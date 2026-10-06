// hl-cl-producer: observes the local player and sends IR Ingress frames. See ir_producer.h.
//
// Specifications (in ir-bot): doc/spec/hl-producer_v1.md (§13 lists the hooks used here),
// doc/spec/ir-ingress-protocol_v1.md.
//
// Where each part of a frame comes from:
//
//   AGENT     V_CalcRefdef (view origin and angles, velocity, ground, water, health), the
//             client data of the last server packet (ducking), the local player's entity
//             (ladder), HUD user messages (weapon, ammunition, armor, zoom, damage)
//   ENTITY[]  the client's entity list: every entity that was in the last server packet
//   SOUND[]   svc_sound, svc_spawnstaticsound, svc_stopsound, and the event hooks of other
//             players' weapon events; the engine's own (predicted) sounds are never seen here
//   CONTROL   CL_CreateMove (movement, buttons, impulse), V_CalcRefdef (commanded angles), and
//             the weapon changes seen in HUD user messages
//
// All rules (what is reported, boxes, identities, sound lifetime, weapon selection, sampling)
// are ir-bot's hl/common, the same code hl-dem-producer runs.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hud.h"
#include "cl_util.h"
#include "console.h"
#include "convar.h"
#include "usercmd.h"
#include "ref_params.h"
#include "cl_entity.h"
#include "com_model.h"
#include "entity_state.h"
#include "event_args.h"
#include "pm_defs.h"
#include "pm_movevars.h"
#include "svc_messages.h"
#include "engine_patches.h"

#include "ir_producer.h"

// ir-bot's shared headers (hl/common), last because they need the sockets API.
#ifdef _WIN32
#include <winsani_in.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winsani_out.h>
#endif
// The game headers define min and max as macros; the shared headers use std::min and std::max.
#undef min
#undef max
#define HL_INGRESS_NETWORK
#include "common/bsp.hpp"
#include "common/constants.hpp"
#include "common/coords.hpp"
#include "common/entities.hpp"
#include "common/frame.hpp"
#include "common/hud.hpp"
#include "common/ingress.hpp"
#include "common/irmap.hpp"
#include "common/live.hpp"
#include "common/sampling.hpp"
#include "common/sounds.hpp"
#include "common/svc.hpp"

extern playermove_t *pmove;

namespace
{

ConVar ir_produce("ir_produce", "0", 0, "hl-cl-producer: 0 = off, 1 = send IR Ingress frames while playing, 2 = also while a demo plays (for checks)");
ConVar ir_service("ir_service", "127.0.0.1:47700", 0, "Address of the IR service (empty = do not connect)");
ConVar ir_record("ir_record", "0", 0, "1 = also write every session into an .irin file");
ConVar ir_record_dir("ir_record_dir", "", 0, "Folder for .irin files (default: <IRBOT_DATA>/irin, else <game>/irin)");
ConVar ir_config("ir_config", "", 0, "Folder with the map configuration files (default: IRBOT_CONFIG, else <IRBOT_DATA>/../config)");

ConVar ir_snapshot("ir_snapshot", "", 0, "For checks: \"<from> <to> <every>\" takes a game snapshot at these frame indices, to compare with the IR service's images");

constexpr int MAX_IR_EVENTS = 32;
constexpr double VIEW_TIMEOUT = 2.0; // seconds without a rendered view before the session is closed

// Everything that belongs to one level (one map load).
struct Level
{
	std::string levelName; // as the engine names it: maps/crossfire.bsp
	std::string mapId;
	bool configTried = false;
	bool configLoaded = false;
	hl::Irmap irmap;
	bool bspTried = false;
	std::vector<hl::BrushBox> brushBoxes;
	std::map<int, std::string> soundNames; // precache index -> name, as in the map configuration
	std::map<std::string, bool> wavLoops; // loop marker of the game's .wav files, when asked
	std::set<std::string> unknownModels;
	std::set<std::string> unknownSounds;
	hl::SoundTracker sounds;
	bool openFailed = false; // no session could be opened; retried when the settings change
};

struct EventHook
{
	std::string name;
	ir_producer::EventFn fn = nullptr;
};

Level s_Level;
hl::HudState s_Hud;
hl::ButtonTracker s_Buttons;
hl::IntervalLatch s_Command;
hl::LiveSampler s_Sampler;
hl::ingress::Client s_Client;
hl::ingress::IrinWriter s_Writer;
std::string s_RecordPath;
std::string s_Settings; // the console variables a session was opened (or failed) with

EventHook s_Events[MAX_IR_EVENTS];
int s_iEventCount = 0;
bool s_bSvcHooked = false;

// The latest state seen by the hooks.
hl::SelfState s_Self;
hl::Vec3 s_vecPlayerOrigin;
int s_iSelf = 0; // entity index of the local player, 0 = unknown
int s_iClientFlags = 0;
float s_flForwardMove = 0, s_flSideMove = 0;
std::uint8_t s_iHeldButtons = 0;
double s_flLastViewWall = 0;
double s_flLastAbsTime = 0; // the engine's real time at the last view
double s_flPausedTime = 0; // real time spent paused, taken out of the sample clock

// Counters for `ir_status`.
std::uint32_t s_iFramesSent = 0;
std::uint32_t s_iFramesInvalid = 0;
std::size_t s_iLastEntities = 0, s_iLastSounds = 0;

double WallSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The sample clock: the engine's real time, which is also the clock a demo's frames are
// stamped with, so that hl-dem-producer samples a recording of the same play at the same rate.
// It stands still while the game is paused. (The client's game time, ref_params.time, follows
// the server's clock and is corrected in small steps on a connection with changing latency.)
double SampleTime()
{
	return s_flLastAbsTime - s_flPausedTime;
}

hl::Vec3 ToVec3(const float *v)
{
	return hl::Vec3(v[0], v[1], v[2]);
}

std::string EnvOr(const char *name, const std::string &fallback)
{
	const char *value = std::getenv(name);
	return value && value[0] ? std::string(value) : fallback;
}

// SPEC-QUESTION: doc/notes/stream-f-questions.md#client-folders
std::string ConfigDir()
{
	const char *cvar = ir_config.GetString();
	if (cvar && cvar[0])
		return cvar;
	const std::string env = EnvOr("IRBOT_CONFIG", "");
	if (!env.empty())
		return env;
	const std::string data = EnvOr("IRBOT_DATA", "");
	return data.empty() ? std::string("config") : data + "/../config";
}

std::string RecordDir()
{
	const char *cvar = ir_record_dir.GetString();
	if (cvar && cvar[0])
		return cvar;
	const std::string data = EnvOr("IRBOT_DATA", "");
	if (!data.empty())
		return data + "/irin";
	return std::string(gEngfuncs.pfnGetGameDirectory()) + "/irin";
}

// game_id of the game directory (hl-producer §1).
std::string GameId()
{
	const std::string dir = hl::Irmap::normalizeName(gEngfuncs.pfnGetGameDirectory());
	return dir == "valve" ? std::string("hl") : dir;
}

// maps/crossfire.bsp -> crossfire
std::string MapIdOf(const std::string &levelName)
{
	std::string name = hl::Irmap::normalizeName(levelName);
	const std::size_t slash = name.find_last_of('/');
	if (slash != std::string::npos)
		name.erase(0, slash + 1);
	const std::size_t dot = name.find_last_of('.');
	if (dot != std::string::npos)
		name.erase(dot);
	return name;
}

std::string CurrentSettings()
{
	return std::string(ir_produce.GetString()) + "|" + ir_service.GetString() + "|" + ir_record.GetString() + "|" + ir_record_dir.GetString() + "|" + ir_config.GetString();
}

void CloseSession(const char *why)
{
	if (!s_Sampler.open())
		return;
	s_Sampler.end();
	if (s_Client.isOpen())
		s_Client.close();
	if (s_Writer.isOpen())
	{
		s_Writer.close();
		ConPrintf("ir-ingress: wrote %s\n", s_RecordPath.c_str());
	}
	ConPrintf("ir-ingress: session closed (%s), %u frames\n", why, (unsigned)s_iFramesSent);
}

// A new level starts (svc_serverinfo): nothing of the old one is valid any more.
void NewLevel()
{
	CloseSession("level change");
	s_Level = Level();
	const char *name = gEngfuncs.pfnGetLevelName();
	s_Level.levelName = name ? name : "";
	s_Level.mapId = MapIdOf(s_Level.levelName);
	s_Hud.newLevel();
	s_iSelf = 0;
}

// Loads the map configuration of the current level, once. Without it nothing is produced.
bool EnsureConfig()
{
	if (s_Level.configTried)
		return s_Level.configLoaded;
	if (s_Level.mapId.empty())
		return false; // no level yet
	s_Level.configTried = true;
	try
	{
		s_Level.irmap = hl::Irmap::loadFor(ConfigDir(), GameId(), s_Level.mapId);
		s_Level.configLoaded = true;
	}
	catch (const std::exception &e)
	{
		ConPrintf("ir-ingress: no map configuration, nothing is produced on this map: %s\n", e.what());
	}
	return s_Level.configLoaded;
}

// The boxes of the map's brush models, from the BSP the game itself plays.
void EnsureBsp()
{
	if (s_Level.bspTried)
		return;
	s_Level.bspTried = true;
	int length = 0;
	byte *data = gEngfuncs.COM_LoadFile(s_Level.levelName.c_str(), 5, &length);
	if (!data || length <= 0)
	{
		ConPrintf("ir-ingress: cannot read %s: brush entities are not reported\n", s_Level.levelName.c_str());
		return;
	}
	try
	{
		const hl::Bsp bsp = hl::Bsp::parse(std::vector<std::uint8_t>(data, data + length));
		s_Level.brushBoxes = hl::brushBoxesFromBsp(bsp);
	}
	catch (const std::exception &e)
	{
		ConPrintf("ir-ingress: %s: %s: brush entities are not reported\n", s_Level.levelName.c_str(), e.what());
	}
	gEngfuncs.COM_FreeFile(data);
}

// Whether a sound loops (hl-producer §9.3): the map configuration's `loop` lines decide; a
// configuration without them leaves it to the loop marker of the game's .wav file.
bool IsLoop(const std::string &name)
{
	if (!s_Level.irmap.loops().empty())
		return s_Level.irmap.isLoop(name);
	const auto known = s_Level.wavLoops.find(name);
	if (known != s_Level.wavLoops.end())
		return known->second;
	bool loop = false;
	int length = 0;
	if (byte *data = gEngfuncs.COM_LoadFile(("sound/" + name).c_str(), 5, &length))
	{
		loop = length > 0 && hl::wavHasLoopMarker(data, static_cast<std::size_t>(length));
		gEngfuncs.COM_FreeFile(data);
	}
	s_Level.wavLoops[name] = loop;
	return loop;
}

std::uint16_t SoundIdFor(const std::string &name)
{
	const std::uint16_t id = s_Level.irmap.soundId(name);
	if (id == 0 && s_Level.unknownSounds.insert(name).second)
		ConPrintf("ir-ingress: unknown sound '%s': not in the map configuration, not reported\n", name.c_str());
	return id;
}

int SelfEntity()
{
	if (s_iSelf > 0)
		return s_iSelf;
	cl_entity_t *local = gEngfuncs.GetLocalPlayer();
	return local ? local->index : 0;
}

// --- Server message hooks ------------------------------------------------------------------
//
// Each hook decodes the message from the engine's buffer without consuming it, then lets the
// engine handle it as usual.

const CEnginePatches::EngineMsgBuf &MsgBuf()
{
	return CEnginePatches::Get().GetMsgBuf();
}

const std::uint8_t *MsgData()
{
	return static_cast<const std::uint8_t *>(MsgBuf().GetBuf()) + MsgBuf().GetReadPos();
}

std::size_t MsgSize()
{
	const int left = MsgBuf().GetSize() - MsgBuf().GetReadPos();
	return left > 0 ? static_cast<std::size_t>(left) : 0;
}

void OnSound(const hl::SoundMessage &s)
{
	if (!EnsureConfig())
		return;
	const auto found = s_Level.soundNames.find(s.soundIndex);
	const std::string name = found == s_Level.soundNames.end() ? std::string() : found->second;
	hl::applyServerSound(s_Level.sounds, s, SelfEntity(), name, !name.empty() && IsLoop(name), SoundIdFor);
}

void SvcServerInfo()
{
	CEnginePatches::Get().GetEngineSvcHandlers().pfnSvcServerInfo();
	NewLevel(); // after the engine: it knows the level's name now
}

void SvcResourceList()
{
	try
	{
		std::vector<hl::svc::Resource> list;
		hl::svc::readResourceList(MsgData(), MsgSize(), list);
		for (const hl::svc::Resource &res : list)
		{
			if (res.type == hl::svc::kResourceSound)
				s_Level.soundNames[res.index] = hl::Irmap::normalizeName(res.name);
		}
	}
	catch (const hl::ParseError &e)
	{
		ConPrintf("ir-ingress: cannot read the resource list (%s): sounds are not reported\n", e.what());
	}
	CEnginePatches::Get().GetEngineSvcHandlers().pfnSvcResourceList();
}

void SvcSound()
{
	try
	{
		hl::SoundMessage s;
		hl::svc::readSound(MsgData(), MsgSize(), s);
		OnSound(s);
	}
	catch (const hl::ParseError &)
	{
	}
	CEnginePatches::Get().GetEngineSvcHandlers().pfnSvcSound();
}

void SvcSpawnStaticSound()
{
	try
	{
		hl::SoundMessage s;
		hl::svc::readStaticSound(MsgData(), MsgSize(), s);
		OnSound(s);
	}
	catch (const hl::ParseError &)
	{
	}
	CEnginePatches::Get().GetEngineSvcHandlers().pfnSvcSpawnStaticSound();
}

void SvcStopSound()
{
	try
	{
		int entity = 0;
		hl::svc::readStopSound(MsgData(), MsgSize(), entity);
		s_Level.sounds.stopEntity(entity);
	}
	catch (const hl::ParseError &)
	{
	}
	CEnginePatches::Get().GetEngineSvcHandlers().pfnSvcStopSound();
}

// --- Weapon events ---------------------------------------------------------------------------

void OnEvent(int number, struct event_args_s *args)
{
	if (!args || !EnsureConfig())
		return;
	const hl::Vec3 origin = ToVec3(args->origin);
	hl::applyWeaponEvent(s_Level.sounds, number, args->entindex, !hl::isZero(origin), origin, SelfEntity(), [&]() {
		return SoundIdFor(hl::Irmap::normalizeName(s_Events[number].name));
	});
}

// One registered function per event script, because the engine's callback carries no context.
template <int N>
void EventTrampoline(struct event_args_s *args)
{
	OnEvent(N, args);
	if (s_Events[N].fn)
		s_Events[N].fn(args);
}

template <int... N>
ir_producer::EventFn TrampolineAt(int index, std::integer_sequence<int, N...>)
{
	static const ir_producer::EventFn table[] = { &EventTrampoline<N>... };
	return table[index];
}

// --- Session and frames ----------------------------------------------------------------------

std::string TimeStamp()
{
	const std::time_t now = std::time(nullptr);
	char text[32] = "";
	if (const std::tm *local = std::localtime(&now))
		std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", local);
	return text;
}

bool OpenSession(double now)
{
	s_Settings = CurrentSettings();
	if (!EnsureConfig())
	{
		s_Level.openFailed = true;
		return false;
	}
	EnsureBsp();

	const std::string gameId = GameId();
	bool hasSink = false;

	const std::string service = ir_service.GetString();
	if (!service.empty())
	{
		std::string host = service;
		int port = hl::ingress::kDefaultPort;
		const std::size_t colon = service.find_last_of(':');
		if (colon != std::string::npos)
		{
			host = service.substr(0, colon);
			port = std::atoi(service.c_str() + colon + 1);
		}
		std::string error;
		if (port > 0 && port <= 65535 && s_Client.open(host, static_cast<std::uint16_t>(port), gameId, s_Level.mapId, &error))
		{
			ConPrintf("ir-ingress: session %u opened with the IR service at %s (%s/%s)\n", (unsigned)s_Client.sessionId(), service.c_str(), gameId.c_str(), s_Level.mapId.c_str());
			hasSink = true;
		}
		else
		{
			ConPrintf("ir-ingress: no session with the IR service at %s: %s\n", service.c_str(), error.empty() ? "bad address" : error.c_str());
		}
	}

	if (ir_record.GetBool())
	{
		const std::string dir = RecordDir();
		std::error_code ignored;
		std::filesystem::create_directories(dir, ignored);
		s_RecordPath = dir + "/" + s_Level.mapId + "-" + TimeStamp() + ".irin";
		if (s_Writer.open(s_RecordPath, gameId, s_Level.mapId))
		{
			ConPrintf("ir-ingress: recording %s\n", s_RecordPath.c_str());
			hasSink = true;
		}
		else
		{
			ConPrintf("ir-ingress: cannot create %s\n", s_RecordPath.c_str());
		}
	}

	if (!hasSink)
	{
		ConPrintf("ir-ingress: nothing to send frames to; `ir_connect` tries again\n");
		s_Level.openFailed = true;
		return false;
	}

	s_Sampler.begin(now, s_Level.irmap.rate > 0 ? s_Level.irmap.rate : 20.0);
	s_Hud.beginSession();
	s_Buttons.reset();
	s_Buttons.setHeld(s_iHeldButtons);
	s_Command.take();
	s_iFramesSent = 0;
	s_iFramesInvalid = 0;
	return true;
}

// True for an entity that was in the last server packet. `local` is the local player's entity,
// which every packet contains.
bool IsPresent(const cl_entity_t *ent, const cl_entity_t *local)
{
	return ent && local && ent->curstate.messagenum == local->curstate.messagenum;
}

const char *ModelName(const cl_entity_t *ent)
{
	if (ent->curstate.modelindex == 0)
		return "";
	const model_t *model = gEngfuncs.hudGetModelByIndex(ent->curstate.modelindex);
	return model ? model->name : "";
}

void EmitFrame(const struct ref_params_s *params)
{
	hl::ingress::Frame frame;
	frame.sessionId = s_Client.isOpen() ? s_Client.sessionId() : 0;
	frame.frameIndex = s_Sampler.nextFrameIndex();

	hl::fillAgent(s_Self, s_Hud, frame.agent);

	// ENTITY[] (hl-producer §8): what the server sent with its last packet.
	const cl_entity_t *local = gEngfuncs.GetEntityByIndex(s_iSelf);
	for (int i = 1; i < params->max_entities; i++)
	{
		const cl_entity_t *ent = gEngfuncs.GetEntityByIndex(i);
		if (!IsPresent(ent, local))
			continue;
		const entity_state_t &state = ent->curstate;
		if (state.entityType & ENTITY_BEAM)
			continue;
		hl::EntityInput in;
		in.isSelf = i == s_iSelf;
		in.isPlayer = i <= params->maxclients;
		in.alive = state.solid == hl::kSolidSlideBox;
		in.model = ModelName(ent);
		in.origin = ToVec3(state.origin);
		in.angles = ToVec3(state.angles);
		in.mins = ToVec3(state.mins);
		in.maxs = ToVec3(state.maxs);
		in.effects = state.effects;
		in.rendermode = state.rendermode;
		in.renderamt = state.renderamt;
		hl::ingress::Entity entity;
		const hl::EntityVerdict verdict = hl::makeEntity(in, s_Level.irmap, s_Level.brushBoxes, entity);
		if (verdict == hl::EntityVerdict::Reported)
		{
			frame.entities.push_back(entity);
		}
		else if (verdict == hl::EntityVerdict::UnknownModel)
		{
			const std::string name = hl::Irmap::normalizeName(in.model);
			if (s_Level.unknownModels.insert(name).second)
				ConPrintf("ir-ingress: unknown model '%s': not in the map configuration, not reported\n", name.c_str());
		}
	}
	hl::keepNearestEntities(frame.entities, frame.agent.viewPosition);

	// SOUND[] (§9)
	frame.sounds = s_Level.sounds.sample(s_Self.viewOrigin, hl::viewYawDegrees(s_Self), [&](int index, hl::Vec3 &origin) {
		const cl_entity_t *ent = index < params->max_entities ? gEngfuncs.GetEntityByIndex(index) : nullptr;
		if (!IsPresent(ent, local))
			return false;
		origin = hl::soundOrigin(ToVec3(ent->curstate.origin), ModelName(ent), s_Level.brushBoxes);
		return true;
	});

	hl::fillControl(s_Self, s_Buttons, s_Hud, s_Command.take(), frame.control);

	std::vector<std::uint8_t> bytes;
	std::string error;
	if (!hl::encodeChecked(frame, bytes, &error))
	{
		if (s_iFramesInvalid++ == 0)
			ConPrintf("ir-ingress: frame %u is invalid and was dropped: %s\n", (unsigned)frame.frameIndex, error.c_str());
		return;
	}
	s_iFramesSent++;
	s_iLastEntities = frame.entities.size();
	s_iLastSounds = frame.sounds.size();
	if (s_Client.isOpen())
		s_Client.send(bytes);
	if (s_Writer.isOpen())
		s_Writer.write(bytes);

	// For checks: the game's own picture of the moment this frame describes.
	const char *snapshot = ir_snapshot.GetString();
	if (snapshot && snapshot[0])
	{
		int from = 0, to = 0, every = 1;
		if (std::sscanf(snapshot, "%d %d %d", &from, &to, &every) >= 2 && every > 0 && frame.frameIndex >= from && frame.frameIndex <= to && (frame.frameIndex - from) % every == 0)
		{
			ConPrintf("ir-ingress: snapshot at frame %u\n", (unsigned)frame.frameIndex);
			gEngfuncs.pfnClientCmd("snapshot\n");
		}
	}
}

// The command of this client frame: movement, held buttons with their presses, impulse.
void TakeCommand(const struct usercmd_s *cmd)
{
	s_flForwardMove = cmd->forwardmove;
	s_flSideMove = cmd->sidemove;
	s_iHeldButtons = hl::hlButtonsToIr(cmd->buttons);
	s_Buttons.update(s_iHeldButtons);
	if (hl::isGameplayImpulse(cmd->impulse))
		s_Command.set(cmd->impulse);
}

}

CON_COMMAND(ir_connect, "hl-cl-producer: closes the session and opens a new one (after starting the IR service, for example)")
{
	CloseSession("ir_connect");
	s_Level.openFailed = false;
	s_Level.configTried = false; // the map configuration is read again
	s_Level.configLoaded = false;
}

void ir_producer::InstallSvcHooks(SvcClientFuncs &funcs)
{
	funcs.pfnSvcServerInfo = SvcServerInfo;
	funcs.pfnSvcResourceList = SvcResourceList;
	funcs.pfnSvcSound = SvcSound;
	funcs.pfnSvcSpawnStaticSound = SvcSpawnStaticSound;
	funcs.pfnSvcStopSound = SvcStopSound;
	s_bSvcHooked = true;
}

ir_producer::EventFn ir_producer::WrapEvent(const char *name, EventFn fn)
{
	if (s_iEventCount >= MAX_IR_EVENTS)
		return fn; // more event scripts than slots: this one is not reported
	const int index = s_iEventCount++;
	s_Events[index].name = name;
	s_Events[index].fn = fn;
	return TrampolineAt(index, std::make_integer_sequence<int, MAX_IR_EVENTS>());
}

void ir_producer::UserMessage(const char *name, int size, void *buf)
{
	if (!name || !buf || size < 0)
		return;
	hl::HudState::Player player;
	player.origin = s_vecPlayerOrigin;
	player.viewPitch = s_Self.commandPitch;
	player.viewYaw = s_Self.commandYaw;
	s_Hud.userMessage(name, static_cast<const std::uint8_t *>(buf), static_cast<std::size_t>(size), SampleTime(), player);
}

void ir_producer::ClientData(const struct clientdata_s *client)
{
	if (client)
		s_iClientFlags = client->flags;
}

void ir_producer::CreateMove(const struct usercmd_s *cmd, int active)
{
	if (cmd && active)
		TakeCommand(cmd);
}

void ir_producer::View(const struct ref_params_s *params)
{
	s_flLastViewWall = WallSeconds();
	const double absTime = gEngfuncs.GetAbsoluteTime();
	if (params->paused)
		s_flPausedTime += absTime - s_flLastAbsTime;
	s_flLastAbsTime = absTime;

	// Without the server message hook, a level change is noticed here.
	const char *levelName = gEngfuncs.pfnGetLevelName();
	if (s_Level.levelName != (levelName ? levelName : ""))
		NewLevel();

	const int mode = ir_produce.GetInt();
	const bool wanted = mode >= 1 && (!params->demoplayback || mode >= 2);
	// A session lasts while the game is playable for the player (hl-producer §5).
	const bool playable = wanted && !s_Level.mapId.empty() && !params->intermission && !params->spectator && !g_iUser1;

	if (s_Settings != CurrentSettings())
	{
		// A change of the settings restarts the session and reads the map configuration again:
		// `ir_config` may have been set after the level started.
		CloseSession("settings changed");
		s_Level.openFailed = false;
		s_Level.configTried = false;
		s_Level.configLoaded = false;
		s_Settings = CurrentSettings();
	}
	if (!playable)
		CloseSession(wanted ? "not playable" : "off");

	const double now = SampleTime();
	s_iSelf = params->playernum + 1;
	if (params->demoplayback && params->cmd)
		TakeCommand(params->cmd); // a demo plays: its recorded command stands in for CL_CreateMove

	// Weapon changes are judged once the messages of whole packets are in (hl-producer §11).
	s_Hud.endPacket(now);

	const cl_entity_t *local = gEngfuncs.GetEntityByIndex(s_iSelf);
	s_vecPlayerOrigin = ToVec3(params->simorg);
	s_Self.viewOrigin = ToVec3(params->vieworg);
	s_Self.velocity = ToVec3(params->simvel);
	s_Self.commandPitch = params->cl_viewangles[0];
	s_Self.commandYaw = params->cl_viewangles[1];
	s_Self.punchPitch = params->punchangle[0];
	s_Self.punchYaw = params->punchangle[1];
	s_Self.onGround = params->onground != 0;
	s_Self.ducking = (s_iClientFlags & hl::kFlDucking) != 0;
	s_Self.onLadder = local && local->curstate.movetype == hl::kMoveTypeFly;
	s_Self.waterLevel = params->waterlevel;
	s_Self.health = params->health;
	s_Self.forwardMove = s_flForwardMove;
	s_Self.sideMove = s_flSideMove;
	s_Self.maxSpeed = MaxSpeed();

	if (playable && !s_Sampler.open() && !s_Level.openFailed)
		OpenSession(now);

	if (!params->paused)
	{
		for (int due = s_Sampler.due(now); due > 0; due--)
			EmitFrame(params);
	}
}

void ir_producer::Frame()
{
	// The view is calculated for every rendered game frame. When that stops (disconnect, a
	// level being loaded), the game is not playable.
	if (s_Sampler.open() && WallSeconds() - s_flLastViewWall > VIEW_TIMEOUT)
		CloseSession("the game stopped");
}

void ir_producer::Shutdown()
{
	CloseSession("shutdown");
}

int ir_producer::SessionId()
{
	return s_Sampler.open() && s_Client.isOpen() ? static_cast<int>(s_Client.sessionId()) : -1;
}

// The server's maximum speed (movevars), lowered by the player's own limit when the game sets
// one.
float ir_producer::MaxSpeed()
{
	float speed = 270.0f; // the project demos' sv_maxspeed, used until movevars are known
	if (pmove && pmove->movevars && pmove->movevars->maxspeed > 0)
		speed = pmove->movevars->maxspeed;
	const float client = gEngfuncs.GetClientMaxspeed();
	if (client > 0 && client < speed)
		speed = client;
	return speed;
}

void ir_producer::PrintStatus()
{
	if (ir_produce.GetInt() < 1)
	{
		ConPrintf("ir-ingress: off (ir_produce 1 turns hl-cl-producer on)\n");
		return;
	}
	if (!s_bSvcHooked || !CEnginePatches::Get().GetMsgBuf().IsValid())
		ConPrintf("ir-ingress: the engine's server messages are not hooked: no sounds are reported\n");
	if (!s_Sampler.open())
	{
		ConPrintf("ir-ingress: no session (map '%s'%s)\n", s_Level.mapId.c_str(), s_Level.openFailed ? ", opening failed; ir_connect tries again" : "");
		return;
	}
	if (s_Client.isOpen())
		ConPrintf("ir-ingress: session %u with the IR service at %s\n", (unsigned)s_Client.sessionId(), ir_service.GetString());
	if (s_Writer.isOpen())
		ConPrintf("ir-ingress: recording %s\n", s_RecordPath.c_str());
	ConPrintf("ir-ingress: map %s at %.0f Hz, %u frames (%u invalid, %d clock restarts); last frame: %u entities, %u sounds\n",
	    s_Level.mapId.c_str(), s_Sampler.rate(), (unsigned)s_iFramesSent, (unsigned)s_iFramesInvalid, s_Sampler.restarts(),
	    (unsigned)s_iLastEntities, (unsigned)s_iLastSounds);
	ConPrintf("ir-ingress: %u unknown models, %u unknown sounds on this map\n", (unsigned)s_Level.unknownModels.size(), (unsigned)s_Level.unknownSounds.size());
}
