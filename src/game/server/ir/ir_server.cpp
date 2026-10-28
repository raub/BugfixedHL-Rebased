// hl-sv-consumer and hl-sv-producer: fake players on a Half-Life server, moved by IR Control and
// observed into IR Ingress. See ir_server.h.
//
// Specifications (in ir-bot): doc/spec/ir-control_v1.md §6 and §8; doc/spec/hl-producer_v1.md §4
// (angles), §5 and §6 (sessions, sampling), §7 to §11 (what a frame holds), §14 (the server
// components); doc/spec/ir-ingress-protocol_v1.md.
//
// Where each part of a fake player's frame comes from:
//
//   AGENT     the player's edict (view origin, angles, velocity, ground, water, health) and the
//             HUD user messages the game sends to that player (weapon, ammunition, armor, zoom,
//             damage), caught on their way to the engine
//   ENTITY[]  what the engine would send to that player as a client: the game's own
//             SetupVisibility and AddToFullPack, asked with the fake player as the host
//   SOUND[]   the game's sounds (EmitSound, EmitAmbientSound, the footsteps of player movement)
//             and other players' weapon events (PlaybackEvent), caught on their way to the engine
//   CONTROL   the control applied to the player in this server frame
//
// All rules (what is reported, boxes, identities, sound lifetime, weapon selection, sampling)
// are ir-bot's hl/common, the same code hl-cl-producer and hl-dem-producer run.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "client.h"
#include "entity_state.h"
#include "event_flags.h"
#include "pm_defs.h"
#include "usercmd.h"

#include "ir_server.h"

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
#include "common/control_net.hpp"
#include "common/coords.hpp"
#include "common/entities.hpp"
#include "common/frame.hpp"
#include "common/hud.hpp"
#include "common/ingress.hpp"
#include "common/irmap.hpp"
#include "common/live.hpp"
#include "common/sampling.hpp"
#include "common/sounds.hpp"

namespace
{

cvar_t ir_control_port = { "ir_control_port", "47702", FCVAR_SERVER };
cvar_t ir_bot_name = { "ir_bot_name", "ir_bot", FCVAR_SERVER };
cvar_t ir_bots = { "ir_bots", "0", FCVAR_SERVER };
cvar_t ir_produce = { "ir_produce", "1", 0 };
cvar_t ir_service = { "ir_service", "127.0.0.1:47700", 0 };
cvar_t ir_record = { "ir_record", "0", 0 };
cvar_t ir_record_dir = { "ir_record_dir", "", 0 };
cvar_t ir_config = { "ir_config", "", 0 };
cvar_t ir_game = { "ir_game", "hl", 0 };
cvar_t ir_kills = { "ir_kills", "", 0 };
// Whether the fake players switch to a weapon they pick up, as cl_autowepswitch of a client:
// 0 never, 1 always (the game's default), 2 unless firing. Off, every switch is the bot's own.
cvar_t ir_autoswitch = { "ir_autoswitch", "1", 0 };

constexpr double RETRY_SECONDS = 5.0; // between attempts to open a session that failed

// The user messages HudState reads; the others are not kept.
const std::set<std::string> HUD_MESSAGES = { "CurWeapon", "WeapPickup", "Battery", "AmmoX", "WeaponList", "SetFOV", "Damage" };

// Everything that belongs to one level (one map load), shared by all fake players.
struct Level
{
	std::string mapId;
	bool configTried = false;
	bool configLoaded = false;
	hl::Irmap irmap;
	std::vector<hl::BrushBox> brushBoxes;
	std::map<std::string, int> soundAssets; // sound name -> a number for the tracker (stop, replace)
	std::map<std::string, bool> wavLoops; // loop marker of the game's .wav files, when asked
	std::vector<hl::SoundMessage> staticSounds; // ambient sounds spawned with the level
	std::vector<std::string> staticNames;
	std::set<std::string> unknownModels;
	std::set<std::string> unknownSounds;
};

// What hl-sv-producer keeps for one fake player: the state of its session.
struct Producer
{
	hl::HudState hud;
	hl::SoundTracker sounds;
	hl::ButtonTracker buttons;
	hl::IntervalLatch command;
	hl::LiveSampler sampler;
	hl::ingress::Client client;
	hl::ingress::IrinWriter writer;
	std::string recordPath;
	hl::SelfState self;
	double retryAt = 0; // game time of the next attempt to open a session
	std::uint32_t framesSent = 0;
	std::uint32_t framesInvalid = 0;
	std::size_t lastEntities = 0, lastSounds = 0;
	std::uint16_t lastIndex = 0; // frame_index of the last frame sent
};

// A player with a session: a fake player, or a human who plays on the server.
struct Player
{
	edict_t *edict = nullptr;
	bool fake = true; // a fake player, moved by IR Control; a human is only observed
	int number = 0; // fake players: 1, 2, ...: the name, and the control session without ingress
	std::uint16_t session = 0; // the IR Control session that moves it
	bool handsOff = true;
	float forwardMove = 0, sideMove = 0; // the movement applied in the last server frame
	std::unique_ptr<Producer> producer;
};

enginefuncs_t s_Engine; // the engine's own functions, before the hooks below replaced them
bool s_bHooked = false;

hl::ControlServer s_Server;
int s_iListenPort = 0;
std::vector<Player> s_Players;
Level s_Level;
std::string s_LevelName; // gpGlobals->mapname of s_Level
std::map<int, std::string> s_UserMessages; // user message id -> name
std::map<unsigned short, std::string> s_EventNames; // event index -> script name, normalized
std::string s_Settings; // the settings the sessions were opened with

// The user message being written by the game, when it is one HudState reads.
struct Message
{
	bool kept = false;
	int dest = 0;
	std::string name;
	edict_t *to = nullptr;
	std::vector<std::uint8_t> data;
} s_Message;

std::uint32_t NowMs()
{
	using namespace std::chrono;
	return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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

// The same folders as hl-cl-producer's.
std::string ConfigDir()
{
	if (ir_config.string && ir_config.string[0])
		return ir_config.string;
	const std::string env = EnvOr("IRBOT_CONFIG", "");
	if (!env.empty())
		return env;
	const std::string data = EnvOr("IRBOT_DATA", "");
	return data.empty() ? std::string("config") : data + "/../config";
}

std::string RecordDir(std::uint32_t rate)
{
	if (ir_record_dir.string && ir_record_dir.string[0])
		return ir_record_dir.string;
	const std::string folder = "/irin/" + std::to_string(rate) + "hz";
	const std::string data = EnvOr("IRBOT_DATA", "");
	if (!data.empty())
		return data + folder;
	char game[256] = "";
	GET_GAME_DIR(game);
	return std::string(game) + folder;
}

std::string TimeStamp()
{
	const std::time_t now = std::time(nullptr);
	char text[32] = "";
	if (const std::tm *local = std::localtime(&now))
		std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", local);
	return text;
}

std::string CurrentSettings()
{
	return std::string(ir_produce.string) + "|" + ir_service.string + "|" + ir_record.string + "|" + ir_record_dir.string + "|" + ir_config.string + "|" + ir_game.string;
}

// IR angle code -> Half-Life degrees (hl-producer §4): pitch changes sign, yaw does not.
float AngleDegrees(std::int16_t code)
{
	return static_cast<float>(hl::controlAngleToRadians(code) * 180.0 / 3.14159265358979323846);
}

// hl-producer §7.1: the weapon ids of IR, as the game names the weapons.
const char *WeaponName(int id)
{
	switch (id)
	{
	case 1: return "weapon_crowbar";
	case 2: return "weapon_9mmhandgun";
	case 3: return "weapon_357";
	case 4: return "weapon_9mmAR";
	case 6: return "weapon_crossbow";
	case 7: return "weapon_shotgun";
	case 8: return "weapon_rpg";
	case 9: return "weapon_gauss";
	case 10: return "weapon_egon";
	case 11: return "weapon_hornetgun";
	case 12: return "weapon_handgrenade";
	case 13: return "weapon_tripmine";
	case 14: return "weapon_satchel";
	case 15: return "weapon_snark";
	default: return nullptr;
	}
}

// The speed that movement 1.0 stands for, as hl-cl-consumer reckons it.
float MaxSpeed(const edict_t *player)
{
	float speed = CVAR_GET_FLOAT("sv_maxspeed");
	if (speed <= 0)
		speed = 270.0f;
	if (player->v.maxspeed > 0 && player->v.maxspeed < speed)
		speed = player->v.maxspeed;
	return speed;
}

// The fake player an IR Control session moves; humans are not moved.
Player *Find(std::uint16_t session)
{
	for (Player &one : s_Players)
		if (one.fake && one.session != 0 && one.session == session)
			return &one;
	return nullptr;
}

Player *FindEdict(const edict_t *edict)
{
	for (Player &one : s_Players)
		if (one.edict == edict)
			return &one;
	return nullptr;
}

int FakeCount()
{
	int count = 0;
	for (const Player &one : s_Players)
		count += one.fake ? 1 : 0;
	return count;
}

int FreeNumber()
{
	int number = 1;
	for (bool taken = true; taken; number++)
	{
		taken = false;
		for (const Player &one : s_Players)
			taken = taken || one.number == number;
		if (!taken)
			return number;
	}
	return number;
}

void Listen()
{
	const int port = static_cast<int>(ir_control_port.value);
	if (port == s_iListenPort)
		return;
	s_iListenPort = port;
	if (s_Server.running())
		s_Server.stop();
	s_Server.setLog([](const std::string &line) { ALERT(at_console, "%s\n", line.c_str()); });
	s_Server.setSessionCheck([](std::uint16_t id) { return Find(id) != nullptr; });
	if (port > 0 && port <= 65535 && s_Server.start(static_cast<std::uint16_t>(port)))
		ALERT(at_console, "ir-control: listening on 127.0.0.1:%d for the fake players\n", port);
	else
		ALERT(at_console, "ir-control: cannot listen on port %d\n", port);
}

// --- The level ------------------------------------------------------------------------------

std::string LevelPath()
{
	return std::string("maps/") + STRING(gpGlobals->mapname) + ".bsp";
}

void CloseSession(Player &one, const char *why);

// A new level: nothing of the old one is valid any more.
void NewLevel()
{
	for (Player &one : s_Players)
	{
		CloseSession(one, "level change");
		one.producer->hud.newLevel();
	}
	s_Level = Level();
	s_LevelName = STRING(gpGlobals->mapname);
	s_Level.mapId = hl::Irmap::normalizeName(s_LevelName);
}

// Loads the map configuration and the brush boxes of the level, once. Without the
// configuration nothing is produced.
bool EnsureConfig()
{
	if (s_Level.configTried)
		return s_Level.configLoaded;
	if (s_Level.mapId.empty())
		return false;
	s_Level.configTried = true;
	try
	{
		s_Level.irmap = hl::Irmap::loadFor(ConfigDir(), ir_game.string, s_Level.mapId);
		s_Level.configLoaded = true;
	}
	catch (const std::exception &e)
	{
		ALERT(at_console, "ir-ingress: no map configuration, nothing is produced on this map: %s\n", e.what());
		return false;
	}
	const std::string path = LevelPath();
	int length = 0;
	byte *data = LOAD_FILE_FOR_ME(const_cast<char *>(path.c_str()), &length);
	if (!data || length <= 0)
	{
		ALERT(at_console, "ir-ingress: cannot read %s: brush entities are not reported\n", path.c_str());
		return true;
	}
	try
	{
		const hl::Bsp bsp = hl::Bsp::parse(std::vector<std::uint8_t>(data, data + length));
		s_Level.brushBoxes = hl::brushBoxesFromBsp(bsp);
	}
	catch (const std::exception &e)
	{
		ALERT(at_console, "ir-ingress: %s: %s: brush entities are not reported\n", path.c_str(), e.what());
	}
	FREE_FILE(data);
	return true;
}

// Whether a sound loops (hl-producer §9.3), as hl-cl-producer decides it.
bool IsLoop(const std::string &name)
{
	if (!s_Level.irmap.loops().empty())
		return s_Level.irmap.isLoop(name);
	const auto known = s_Level.wavLoops.find(name);
	if (known != s_Level.wavLoops.end())
		return known->second;
	bool loop = false;
	int length = 0;
	const std::string path = "sound/" + name;
	if (byte *data = LOAD_FILE_FOR_ME(const_cast<char *>(path.c_str()), &length))
	{
		loop = length > 0 && hl::wavHasLoopMarker(data, static_cast<std::size_t>(length));
		FREE_FILE(data);
	}
	s_Level.wavLoops[name] = loop;
	return loop;
}

std::uint16_t SoundIdFor(const std::string &name)
{
	const std::uint16_t id = s_Level.irmap.soundId(name);
	if (id == 0 && s_Level.unknownSounds.insert(name).second)
		ALERT(at_console, "ir-ingress: unknown sound '%s': not in the map configuration, not reported\n", name.c_str());
	return id;
}

int SoundAsset(const std::string &name)
{
	const auto found = s_Level.soundAssets.find(name);
	if (found != s_Level.soundAssets.end())
		return found->second;
	const int asset = static_cast<int>(s_Level.soundAssets.size()) + 1;
	s_Level.soundAssets[name] = asset;
	return asset;
}

// --- Sessions and frames --------------------------------------------------------------------

bool InGame(const edict_t *edict)
{
	return !(edict->v.flags & FL_SPECTATOR) && edict->v.iuser1 == 0;
}

void CloseSession(Player &one, const char *why)
{
	Producer &p = *one.producer;
	if (!p.sampler.open())
		return;
	p.sampler.end();
	if (p.client.isOpen())
		p.client.close();
	if (p.writer.isOpen())
	{
		p.writer.close();
		ALERT(at_console, "ir-ingress: wrote %s\n", p.recordPath.c_str());
	}
	ALERT(at_console, "ir-ingress: %s: session closed (%s), %u frames\n", STRING(one.edict->v.netname), why, (unsigned)p.framesSent);
}

// The control session of a fake player is its ingress session, the session the IR service
// lists and a bot subscribes to; without one it is the player's number. A human's session is
// only its ingress session (0: none).
void UpdateControlSession(Player &one)
{
	const std::uint16_t session = one.producer->client.isOpen() ? one.producer->client.sessionId() : static_cast<std::uint16_t>(one.number);
	if (session == one.session)
		return;
	s_Server.closeChannel(one.session);
	one.session = session;
}

void ApplySound(Producer &p, int self, const hl::SoundMessage &s, const std::string &name)
{
	hl::applyServerSound(p.sounds, s, self, name, !name.empty() && IsLoop(name), SoundIdFor);
}

void OpenSession(Player &one, double now)
{
	Producer &p = *one.producer;
	p.retryAt = now + RETRY_SECONDS;
	if (!EnsureConfig())
		return;
	const std::string name = STRING(one.edict->v.netname);
	const std::uint32_t rate = s_Level.irmap.rate >= 1 && s_Level.irmap.rate <= hl::ingress::kMaxSampleRateHz ? s_Level.irmap.rate : 20;
	bool hasSink = false;

	const std::string service = ir_service.string;
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
		if (port > 0 && port <= 65535 && p.client.open(host, static_cast<std::uint16_t>(port), ir_game.string, s_Level.mapId, rate, &error))
		{
			ALERT(at_console, "ir-ingress: %s: session %u opened with the IR service at %s (%s/%s, %u Hz)\n", name.c_str(),
			    (unsigned)p.client.sessionId(), service.c_str(), ir_game.string, s_Level.mapId.c_str(), (unsigned)rate);
			hasSink = true;
		}
		else
		{
			ALERT(at_console, "ir-ingress: %s: no session with the IR service at %s: %s\n", name.c_str(), service.c_str(), error.empty() ? "bad address" : error.c_str());
		}
	}

	if (ir_record.value != 0)
	{
		const std::string dir = RecordDir(rate);
		std::error_code ignored;
		std::filesystem::create_directories(dir, ignored);
		p.recordPath = dir + "/" + s_Level.mapId + "-" + TimeStamp() + "-" + name + ".irin";
		if (p.writer.open(p.recordPath, ir_game.string, s_Level.mapId, rate))
		{
			ALERT(at_console, "ir-ingress: %s: recording %s\n", name.c_str(), p.recordPath.c_str());
			hasSink = true;
		}
		else
		{
			ALERT(at_console, "ir-ingress: %s: cannot create %s\n", name.c_str(), p.recordPath.c_str());
		}
	}

	if (!hasSink)
		return;
	p.sampler.begin(now, static_cast<double>(rate));
	p.hud.beginSession();
	p.buttons.reset();
	p.command.take();
	p.framesSent = p.framesInvalid = 0;
	// The ambient sounds of the level play from its start; a session hears them from its own.
	p.sounds.clear();
	const int self = ENTINDEX(one.edict);
	for (std::size_t i = 0; i < s_Level.staticSounds.size(); i++)
		ApplySound(p, self, s_Level.staticSounds[i], s_Level.staticNames[i]);
}

void EmitFrame(Player &one)
{
	Producer &p = *one.producer;
	edict_t *host = one.edict;
	const int self = ENTINDEX(host);

	hl::ingress::Frame frame;
	frame.sessionId = p.client.isOpen() ? p.client.sessionId() : 0;
	frame.frameIndex = p.sampler.nextFrameIndex();
	p.lastIndex = frame.frameIndex;
	hl::fillAgent(p.self, p.hud, frame.agent);

	// ENTITY[] (hl-producer §8): what the engine would send to this player as a client.
	unsigned char *pvs = nullptr, *pas = nullptr;
	SetupVisibility(nullptr, host, &pvs, &pas);
	std::vector<char> present(static_cast<std::size_t>(gpGlobals->maxEntities), 0);
	for (int e = 1; e < gpGlobals->maxEntities; e++)
	{
		edict_t *ent = INDEXENT(e);
		if (!ent || ent->free)
			continue;
		entity_state_t state;
		const bool player = e <= gpGlobals->maxClients;
		if (!AddToFullPack(&state, e, ent, host, 1, player ? 1 : 0, pvs))
			continue;
		present[e] = 1;
		if (state.entityType & ENTITY_BEAM)
			continue;
		hl::EntityInput in;
		in.isSelf = e == self;
		in.isPlayer = player;
		in.alive = state.solid == hl::kSolidSlideBox;
		in.model = STRING(ent->v.model);
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
				ALERT(at_console, "ir-ingress: unknown model '%s': not in the map configuration, not reported\n", name.c_str());
		}
	}
	hl::keepNearestEntities(frame.entities, frame.agent.viewPosition);

	// SOUND[] (§9): an entity's sound is heard while that entity is sent to the player.
	frame.sounds = p.sounds.sample(p.self.viewOrigin, hl::viewYawDegrees(p.self), [&](int index, hl::Vec3 &origin) {
		if (index <= 0 || index >= gpGlobals->maxEntities || !present[index])
			return false;
		const edict_t *ent = INDEXENT(index);
		origin = hl::soundOrigin(ToVec3(ent->v.origin), STRING(ent->v.model), s_Level.brushBoxes);
		return true;
	});

	hl::fillControl(p.self, p.buttons, p.hud, p.command.take(), frame.control);

	std::vector<std::uint8_t> bytes;
	std::string error;
	if (!hl::encodeChecked(frame, bytes, &error))
	{
		if (p.framesInvalid++ == 0)
			ALERT(at_console, "ir-ingress: %s: frame %u is invalid and was dropped: %s\n", STRING(host->v.netname), (unsigned)frame.frameIndex, error.c_str());
		return;
	}
	p.framesSent++;
	p.lastEntities = frame.entities.size();
	p.lastSounds = frame.sounds.size();
	if (p.client.isOpen())
		p.client.send(bytes);
	if (p.writer.isOpen())
	{
		p.writer.write(bytes);
		p.writer.flush(); // a server is usually stopped by ending its process
	}
}

// The player's state after this server frame's move.
void Observe(Player &one)
{
	const edict_t *edict = one.edict;
	hl::SelfState &s = one.producer->self;
	s.viewOrigin = ToVec3(edict->v.origin) + ToVec3(edict->v.view_ofs);
	s.velocity = ToVec3(edict->v.velocity);
	s.commandPitch = edict->v.v_angle.x;
	s.commandYaw = edict->v.v_angle.y;
	s.punchPitch = edict->v.punchangle.x;
	s.punchYaw = edict->v.punchangle.y;
	s.onGround = (edict->v.flags & FL_ONGROUND) != 0;
	s.ducking = (edict->v.flags & FL_DUCKING) != 0;
	s.onLadder = edict->v.movetype == MOVETYPE_FLY;
	s.waterLevel = edict->v.waterlevel;
	s.health = edict->v.health;
	s.forwardMove = one.forwardMove;
	s.sideMove = one.sideMove;
	s.maxSpeed = MaxSpeed(edict);
}

void Produce(Player &one, double now)
{
	Producer &p = *one.producer;
	// Weapon changes are judged once the messages of a whole server frame are in (§11).
	p.hud.endPacket(now);
	Observe(one);
	const bool wanted = ir_produce.value != 0 && !s_Level.mapId.empty() && InGame(one.edict);
	if (!wanted)
		CloseSession(one, "not playable");
	else if (!p.sampler.open() && now >= p.retryAt)
		OpenSession(one, now);
	UpdateControlSession(one);
	for (int due = p.sampler.due(now); due > 0; due--)
		EmitFrame(one);
}

// --- Engine hooks: what the game sends to the players ---------------------------------------
//
// The game calls the engine through its copy of the engine's functions (g_engfuncs). A few of
// them are replaced: each replacement notes what the fake players get, then calls the engine.

int HookRegUserMsg(const char *name, int size)
{
	const int id = s_Engine.pfnRegUserMsg(name, size);
	if (name)
		s_UserMessages[id] = name;
	return id;
}

void HookMessageBegin(int dest, int type, const float *origin, edict_t *ed)
{
	s_Message.kept = false;
	if (!s_Players.empty())
	{
		const auto found = s_UserMessages.find(type);
		if (found != s_UserMessages.end() && HUD_MESSAGES.count(found->second))
		{
			s_Message.kept = true;
			s_Message.dest = dest;
			s_Message.name = found->second;
			s_Message.to = ed;
			s_Message.data.clear();
		}
	}
	s_Engine.pfnMessageBegin(dest, type, origin, ed);
}

void Put(int value, int bytes)
{
	if (!s_Message.kept)
		return;
	for (int i = 0; i < bytes; i++)
		s_Message.data.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
}

void HookWriteByte(int value)
{
	Put(value, 1);
	s_Engine.pfnWriteByte(value);
}

void HookWriteChar(int value)
{
	Put(value, 1);
	s_Engine.pfnWriteChar(value);
}

void HookWriteShort(int value)
{
	Put(value, 2);
	s_Engine.pfnWriteShort(value);
}

void HookWriteLong(int value)
{
	Put(value, 4);
	s_Engine.pfnWriteLong(value);
}

// As the engine encodes them: an angle in a byte, a coordinate in eighths of a unit.
void HookWriteAngle(float value)
{
	Put(static_cast<int>(value * 256.0f / 360.0f), 1);
	s_Engine.pfnWriteAngle(value);
}

void HookWriteCoord(float value)
{
	Put(static_cast<int>(value * 8.0f), 2);
	s_Engine.pfnWriteCoord(value);
}

void HookWriteString(const char *value)
{
	if (s_Message.kept)
	{
		for (const char *c = value ? value : ""; *c; c++)
			s_Message.data.push_back(static_cast<std::uint8_t>(*c));
		s_Message.data.push_back(0);
	}
	s_Engine.pfnWriteString(value);
}

void HookWriteEntity(int value)
{
	Put(value, 2);
	s_Engine.pfnWriteEntity(value);
}

void HookMessageEnd()
{
	s_Engine.pfnMessageEnd();
	if (!s_Message.kept)
		return;
	s_Message.kept = false;
	const bool toOne = s_Message.dest == MSG_ONE || s_Message.dest == MSG_ONE_UNRELIABLE;
	const bool toAll = s_Message.dest == MSG_ALL || s_Message.dest == MSG_BROADCAST;
	for (Player &one : s_Players)
	{
		if (!(toAll || (toOne && s_Message.to == one.edict)))
			continue;
		hl::HudState::Player player;
		player.origin = ToVec3(one.edict->v.origin);
		player.viewPitch = one.edict->v.v_angle.x;
		player.viewYaw = one.edict->v.v_angle.y;
		one.producer->hud.userMessage(s_Message.name, s_Message.data.data(), s_Message.data.size(), gpGlobals->time, player);
	}
}

// A sound as the engine would send it to the clients (svc_sound, svc_spawnstaticsound).
void OnSound(edict_t *entity, const float *position, int channel, const char *sample, float volume, float attenuation, int flags, bool ambient)
{
	if (s_Players.empty() || !sample || !EnsureConfig())
		return;
	std::string name = sample;
	hl::SoundMessage s;
	s.flags = static_cast<std::uint16_t>(flags & (hl::kSndStop | hl::kSndChangeVol | hl::kSndChangePitch | hl::kSndSpawning));
	if (name[0] == '!' || name[0] == '#')
		s.flags |= hl::kSndSentence;
	if (name[0] == '*')
		name.erase(0, 1); // a streamed sound
	name = hl::Irmap::normalizeName(name);
	s.volume = clamp(volume, 0.0f, 1.0f) * 255.0f;
	s.attenuation = static_cast<int>(attenuation * 64.0f) / 64.0;
	s.channel = ambient ? hl::kChanStatic : channel;
	s.entity = entity ? ENTINDEX(entity) : 0;
	s.soundIndex = SoundAsset(name);
	s.hasOrigin = true;
	if (position)
		s.origin = ToVec3(position);
	else if (entity)
		s.origin = ToVec3(entity->v.origin) + (ToVec3(entity->v.mins) + ToVec3(entity->v.maxs)) * 0.5;
	s.isStatic = ambient && (flags & hl::kSndSpawning);
	if (s.isStatic)
	{
		s_Level.staticSounds.push_back(s);
		s_Level.staticNames.push_back(name);
	}
	// SPEC-QUESTION: doc/notes/m6-server-bots.md#sound-reach: every player hears every sound
	// here; the engine sends most of them only to the players whose PAS holds the sound.
	for (Player &one : s_Players)
		if (one.producer->sampler.open())
			ApplySound(*one.producer, ENTINDEX(one.edict), s, name);
}

void HookEmitSound(edict_t *entity, int channel, const char *sample, float volume, float attenuation, int flags, int pitch)
{
	OnSound(entity, nullptr, channel, sample, volume, attenuation, flags, false);
	s_Engine.pfnEmitSound(entity, channel, sample, volume, attenuation, flags, pitch);
}

void HookEmitAmbientSound(edict_t *entity, float *position, const char *sample, float volume, float attenuation, int flags, int pitch)
{
	OnSound(entity, position, hl::kChanStatic, sample, volume, attenuation, flags, true);
	s_Engine.pfnEmitAmbientSound(entity, position, sample, volume, attenuation, flags, pitch);
}

unsigned short HookPrecacheEvent(int type, const char *name)
{
	const unsigned short index = s_Engine.pfnPrecacheEvent(type, name);
	if (name)
		s_EventNames[index] = hl::Irmap::normalizeName(name);
	return index;
}

// Another player's weapon event (hl-producer §9.2).
void HookPlaybackEvent(int flags, const edict_t *invoker, unsigned short index, float delay, float *origin, float *angles,
    float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2)
{
	if (!s_Players.empty() && !(flags & FEV_HOSTONLY) && EnsureConfig())
	{
		const int entity = invoker ? ENTINDEX(const_cast<edict_t *>(invoker)) : 0;
		const hl::Vec3 at = origin ? ToVec3(origin) : hl::Vec3();
		const auto found = s_EventNames.find(index);
		const std::string name = found == s_EventNames.end() ? std::string() : found->second;
		for (Player &one : s_Players)
		{
			if (!one.producer->sampler.open())
				continue;
			hl::applyWeaponEvent(one.producer->sounds, index, entity, !hl::isZero(at), at, ENTINDEX(one.edict), [&]() {
				return name.empty() ? std::uint16_t(0) : SoundIdFor(name);
			});
		}
	}
	s_Engine.pfnPlaybackEvent(flags, invoker, index, delay, origin, angles, fparam1, fparam2, iparam1, iparam2, bparam1, bparam2);
}

// --- Kills and deaths ------------------------------------------------------------------------
//
// The game logs every kill and suicide (UTIL_LogPrintf, through the engine's AlertMessage). Each
// one that concerns a player with a session is noted in a file, one JSON object per line, as
// ir-train's KillNotes notes the kills of a bot in a game client: the time, the frame index of
// the player's session at that moment, what (kill, death, suicide), the other player, the
// weapon, and the player's name, which also ends the name of its recording. So the trainer
// knows which lives of a recording went well.

// <IRBOT_DATA>/kills.jsonl, else kills.jsonl in the game folder.
std::string KillsPath()
{
	if (ir_kills.string && ir_kills.string[0])
		return ir_kills.string;
	const std::string data = EnvOr("IRBOT_DATA", "");
	if (!data.empty())
		return data + "/kills.jsonl";
	char game[256] = "";
	GET_GAME_DIR(game);
	return std::string(game) + "/kills.jsonl";
}

std::string JsonText(const std::string &text)
{
	std::string out = "\"";
	for (const char c : text)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		if (static_cast<unsigned char>(c) >= 0x20)
			out += c;
	}
	return out + "\"";
}

Player *FindUserId(int userId)
{
	for (Player &one : s_Players)
		if (GETPLAYERUSERID(one.edict) == userId)
			return &one;
	return nullptr;
}

void NoteKill(Player *one, const char *what, const std::string &other, const std::string &weapon)
{
	if (!one || !one->producer->sampler.open())
		return;
	using namespace std::chrono;
	const double now = duration<double>(system_clock::now().time_since_epoch()).count();
	char time[32];
	std::snprintf(time, sizeof(time), "%.2f", now);
	std::ofstream file(KillsPath(), std::ios::app);
	file << "{\"time\": " << time << ", \"frame\": " << one->producer->lastIndex << ", \"what\": \"" << what
	     << "\", \"other\": " << JsonText(other) << ", \"weapon\": " << JsonText(weapon)
	     << ", \"player\": " << JsonText(STRING(one->edict->v.netname)) << "}\n";
}

// One line the game logs: "<name><uid><auth><team>" killed "<name><uid><auth><team>" with "<weapon>",
// or "<name><uid><auth><team>" committed suicide with "<weapon>".
void OnLogged(const std::string &line)
{
	static const std::regex killed(R"re("(.*)<(-?\d+)><[^>]*><[^>]*>" killed "(.*)<(-?\d+)><[^>]*><[^>]*>" with "([^"]*)")re");
	static const std::regex suicide(R"re("(.*)<(-?\d+)><[^>]*><[^>]*>" committed suicide with "([^"]*)")re");
	std::smatch m;
	if (std::regex_search(line, m, killed))
	{
		NoteKill(FindUserId(std::atoi(m[2].str().c_str())), "kill", m[3].str(), m[5].str());
		NoteKill(FindUserId(std::atoi(m[4].str().c_str())), "death", m[1].str(), m[5].str());
	}
	else if (std::regex_search(line, m, suicide))
	{
		NoteKill(FindUserId(std::atoi(m[2].str().c_str())), "suicide", "", m[3].str());
	}
}

void HookAlertMessage(ALERT_TYPE type, char *format, ...)
{
	char text[2048];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (type == at_logged && !s_Players.empty() && (std::strstr(text, "\" killed \"") || std::strstr(text, "\" committed suicide with \"")))
		OnLogged(text);
	s_Engine.pfnAlertMessage(type, const_cast<char *>("%s"), text);
}

// The footsteps and other sounds of player movement, which the engine plays for the moving
// player's entity.
int s_iMovingPlayer = 0;
void (*s_pfnMoveSound)(int, const char *, float, float, int, int) = nullptr;

void HookMoveSound(int channel, const char *sample, float volume, float attenuation, int flags, int pitch)
{
	edict_t *entity = s_iMovingPlayer > 0 ? INDEXENT(s_iMovingPlayer) : nullptr;
	OnSound(entity, nullptr, channel, sample, volume, attenuation, flags, false);
	if (s_pfnMoveSound)
		s_pfnMoveSound(channel, sample, volume, attenuation, flags, pitch);
}

void InstallHooks()
{
	if (s_bHooked)
		return;
	s_bHooked = true;
	s_Engine = g_engfuncs;
	g_engfuncs.pfnRegUserMsg = HookRegUserMsg;
	g_engfuncs.pfnMessageBegin = HookMessageBegin;
	g_engfuncs.pfnMessageEnd = HookMessageEnd;
	g_engfuncs.pfnWriteByte = HookWriteByte;
	g_engfuncs.pfnWriteChar = HookWriteChar;
	g_engfuncs.pfnWriteShort = HookWriteShort;
	g_engfuncs.pfnWriteLong = HookWriteLong;
	g_engfuncs.pfnWriteAngle = HookWriteAngle;
	g_engfuncs.pfnWriteCoord = HookWriteCoord;
	g_engfuncs.pfnWriteString = HookWriteString;
	g_engfuncs.pfnWriteEntity = HookWriteEntity;
	g_engfuncs.pfnEmitSound = HookEmitSound;
	g_engfuncs.pfnEmitAmbientSound = HookEmitAmbientSound;
	g_engfuncs.pfnPrecacheEvent = HookPrecacheEvent;
	g_engfuncs.pfnPlaybackEvent = HookPlaybackEvent;
	g_engfuncs.pfnAlertMessage = HookAlertMessage;
}

// --- Commands -------------------------------------------------------------------------------

// A fake player joins; without a name it is called by ir_bot_name and its number.
void AddPlayer(std::string name)
{
	const int number = FreeNumber();
	if (name.empty())
		name = std::string(ir_bot_name.string) + std::to_string(number);
	edict_t *edict = g_engfuncs.pfnCreateFakeClient(name.c_str());
	if (FNullEnt(edict))
	{
		ALERT(at_console, "ir-bot: the server is full, no fake player added\n");
		return;
	}
	// Known before it connects: the game sends it the weapon list while it is put in the server.
	Player added;
	added.edict = edict;
	added.number = number;
	added.session = static_cast<std::uint16_t>(number);
	added.producer = std::make_unique<Producer>();
	s_Players.push_back(std::move(added));
	// The game reads the preference from the player's settings (SetPrefsFromUserinfo) whenever
	// they change, and sets it to 1 as the player is put in the server: both are given.
	const std::string autoswitch = std::to_string(static_cast<int>(ir_autoswitch.value));
	g_engfuncs.pfnSetClientKeyValue(ENTINDEX(edict), g_engfuncs.pfnGetInfoKeyBuffer(edict), "cl_autowepswitch", autoswitch.c_str());
	char reject[128] = {};
	if (!ClientConnect(edict, name.c_str(), "127.0.0.1", reject))
	{
		ALERT(at_console, "ir-bot: %s was refused: %s\n", name.c_str(), reject);
		s_Players.pop_back();
		return;
	}
	ClientPutInServer(edict);
	edict->v.flags |= FL_FAKECLIENT;
	if (CBasePlayer *player = GetClassPtr(reinterpret_cast<CBasePlayer *>(&edict->v)))
		player->m_iAutoWepSwitch = static_cast<int>(ir_autoswitch.value);
	ALERT(at_console, "ir-bot: %s joined\n", name.c_str());
}

// `ir_bot_add [name]`
void CommandAdd()
{
	AddPlayer(CMD_ARGC() > 1 ? CMD_ARGV(1) : "");
}

// `ir_bot_status`: the fake players, their sessions, and whether a control moves them.
// Printed by the engine's ServerPrint, so that rcon gets the answer too.
void CommandStatus()
{
	char line[512];
	std::snprintf(line, sizeof(line), "ir-bot: %d fake players, %d humans, IR Control on port %d, map %s%s\n", FakeCount(),
	    (int)s_Players.size() - FakeCount(), s_iListenPort,
	    s_Level.mapId.c_str(), s_Level.configLoaded ? "" : " (no map configuration loaded)");
	g_engfuncs.pfnServerPrint(line);
	for (const Player &one : s_Players)
	{
		const Producer &p = *one.producer;
		const Vector &at = one.edict->v.origin;
		std::snprintf(line, sizeof(line), "  session %d  %s  %s  health %.0f  at %.0f %.0f %.0f  yaw %.0f\n", (int)one.session,
		    STRING(one.edict->v.netname),
		    !one.fake ? "human, observed" : s_Server.channel(one.session) ? (one.handsOff ? "channel open, hands-off" : "controlled") : "no channel",
		    one.edict->v.health, at.x, at.y, at.z, one.edict->v.v_angle.y);
		g_engfuncs.pfnServerPrint(line);
		if (p.sampler.open())
			std::snprintf(line, sizeof(line), "    ingress: %s%s%s, %u frames (%u invalid); last frame: %u entities, %u sounds\n",
			    p.client.isOpen() ? "IR service" : "", p.client.isOpen() && p.writer.isOpen() ? " and " : "", p.writer.isOpen() ? p.recordPath.c_str() : "",
			    (unsigned)p.framesSent, (unsigned)p.framesInvalid, (unsigned)p.lastEntities, (unsigned)p.lastSounds);
		else
			std::snprintf(line, sizeof(line), "    ingress: no session\n");
		g_engfuncs.pfnServerPrint(line);
	}
}

// One server frame of a fake player: the control in effect, as hl-cl-consumer applies it to the
// local player (ir-control §6, §8).
void Move(Player &one, float seconds)
{
	edict_t *edict = one.edict;
	Producer &p = *one.producer;
	Vector angles = edict->v.v_angle;
	float forward = 0, side = 0, up = 0;
	unsigned short buttons = 0;
	byte impulse = 0;

	hl::ControlChannel *channel = s_Server.channel(one.session);
	if (channel)
	{
		const hl::ControlCommand control = channel->command(NowMs());
		one.handsOff = control.handsOff;
		if (control.setView)
		{
			angles.x = clamp(-AngleDegrees(control.viewPitch), -89.0f, 89.0f);
			angles.y = AngleDegrees(control.viewYaw);
			angles.z = 0;
		}
		const float maxSpeed = MaxSpeed(edict);
		forward = static_cast<float>(hl::controlMoveToUnit(control.moveForward)) * maxSpeed;
		side = static_cast<float>(hl::controlMoveToUnit(control.moveSide)) * maxSpeed;
		if (control.buttons & hl::kButtonPrimaryAttack)
			buttons |= IN_ATTACK;
		if (control.buttons & hl::kButtonSecondaryAttack)
			buttons |= IN_ATTACK2;
		if (control.buttons & hl::kButtonJump)
			buttons |= IN_JUMP;
		if (control.buttons & hl::kButtonCrouch)
			buttons |= IN_DUCK;
		if (control.buttons & hl::kButtonUse)
			buttons |= IN_USE;
		if (control.buttons & hl::kButtonReload)
			buttons |= IN_RELOAD;
		if (control.moveForward > 0)
			buttons |= IN_FORWARD;
		else if (control.moveForward < 0)
			buttons |= IN_BACK;
		if (control.moveSide > 0)
			buttons |= IN_MOVERIGHT;
		else if (control.moveSide < 0)
			buttons |= IN_MOVELEFT;
		// Swimming up: IR has no upward movement; jump held in water means full upmove (§8).
		if ((control.buttons & hl::kButtonJump) && edict->v.waterlevel >= 2)
			up = maxSpeed;
		impulse = control.commandId;
		if (control.weaponSelect != 0)
		{
			CBasePlayer *player = static_cast<CBasePlayer *>(CBaseEntity::Instance(edict));
			const char *name = WeaponName(control.weaponSelect);
			if (player && name)
				player->SelectItem(name);
		}
	}
	else
	{
		one.handsOff = true;
	}

	edict->v.v_angle = angles;
	edict->v.angles.x = -angles.x / 3.0f; // the model's pitch, as the game sets it for players
	edict->v.angles.y = angles.y;
	edict->v.angles.z = 0;
	one.forwardMove = forward;
	one.sideMove = side;
	p.buttons.update(hl::hlButtonsToIr(buttons));
	if (hl::isGameplayImpulse(impulse))
		p.command.set(impulse);
	const int msec = static_cast<int>(seconds * 1000.0f + 0.5f);
	g_engfuncs.pfnRunPlayerMove(edict, angles, forward, side, up, buttons, impulse, static_cast<byte>(clamp(msec, 1, 255)));
}

}

void ir_server::Init()
{
	CVAR_REGISTER(&ir_control_port);
	CVAR_REGISTER(&ir_bot_name);
	CVAR_REGISTER(&ir_bots);
	CVAR_REGISTER(&ir_produce);
	CVAR_REGISTER(&ir_service);
	CVAR_REGISTER(&ir_record);
	CVAR_REGISTER(&ir_record_dir);
	CVAR_REGISTER(&ir_config);
	CVAR_REGISTER(&ir_game);
	CVAR_REGISTER(&ir_kills);
	CVAR_REGISTER(&ir_autoswitch);
	g_engfuncs.pfnAddServerCommand("ir_bot_add", CommandAdd);
	g_engfuncs.pfnAddServerCommand("ir_bot_status", CommandStatus);
	InstallHooks();
}

void ir_server::Frame()
{
	if (s_LevelName != STRING(gpGlobals->mapname))
		NewLevel();
	// `ir_bots <n>`: fake players join, one a frame, until there are so many.
	if (FakeCount() < static_cast<int>(ir_bots.value) && gpGlobals->time > 1.0f)
		AddPlayer("");
	if (s_Players.empty() && !s_Server.running())
		return;
	if (s_Settings != CurrentSettings())
	{
		// New settings take effect with new sessions; the map configuration is read again.
		for (Player &one : s_Players)
		{
			CloseSession(one, "settings changed");
			one.producer->retryAt = 0;
		}
		s_Level.configTried = s_Level.configLoaded = false;
		s_Settings = CurrentSettings();
	}
	Listen();
	s_Server.poll(NowMs());
	for (Player &one : s_Players)
		if (one.fake)
			Move(one, gpGlobals->frametime);
	for (Player &one : s_Players)
		Produce(one, gpGlobals->time);
}

void ir_server::Disconnect(edict_t *player)
{
	for (auto it = s_Players.begin(); it != s_Players.end(); ++it)
	{
		if (it->edict == player)
		{
			CloseSession(*it, "left the server");
			s_Server.closeChannel(it->session);
			s_Players.erase(it);
			return;
		}
	}
}

void ir_server::PutInServer(edict_t *player)
{
	// Fake players of the server library are known before they are put in; those of other bots
	// are not observed.
	if (!player || FindEdict(player) || (player->v.flags & FL_FAKECLIENT))
		return;
	Player human;
	human.edict = player;
	human.fake = false;
	human.producer = std::make_unique<Producer>();
	s_Players.push_back(std::move(human));
}

void ir_server::Command(const edict_t *player, const usercmd_s *cmd)
{
	Player *one = FindEdict(player);
	if (!one || one->fake || !cmd)
		return;
	// A human's control: the commands its client sends, each one in turn (hl-producer §11).
	one->forwardMove = cmd->forwardmove;
	one->sideMove = cmd->sidemove;
	one->producer->buttons.update(hl::hlButtonsToIr(cmd->buttons));
	if (hl::isGameplayImpulse(cmd->impulse))
		one->producer->command.set(cmd->impulse);
}

void ir_server::PlayerMove(playermove_s *pmove, int server, void (*move)(playermove_s *, int))
{
	if (!server || s_Players.empty() || !pmove)
	{
		move(pmove, server);
		return;
	}
	s_iMovingPlayer = pmove->player_index + 1;
	s_pfnMoveSound = pmove->PM_PlaySound;
	pmove->PM_PlaySound = HookMoveSound;
	move(pmove, server);
	pmove->PM_PlaySound = s_pfnMoveSound;
	s_iMovingPlayer = 0;
}
