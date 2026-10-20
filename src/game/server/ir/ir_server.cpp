// hl-sv-consumer: fake players on a Half-Life server, moved by IR Control. See ir_server.h.
//
// Specifications (in ir-bot): doc/spec/ir-control_v1.md §6 and §8; doc/spec/hl-producer_v1.md §4
// (angles), §7.1 (weapon ids), §11 (movement normalization), §14.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "client.h"

#include "ir_server.h"

// ir-bot's shared headers (hl/common), last because they need the sockets API.
#ifdef _WIN32
#include <winsani_in.h>
#include <winsock2.h>
#include <winsani_out.h>
#endif
#include "common/control_net.hpp"

namespace
{

cvar_t ir_control_port = { "ir_control_port", "47702", FCVAR_SERVER };
cvar_t ir_bot_name = { "ir_bot_name", "ir_bot", FCVAR_SERVER };

struct FakePlayer
{
	edict_t *edict = nullptr;
	std::uint16_t session = 0; // the IR Control session that moves it
	bool handsOff = true;
};

hl::ControlServer s_Server;
int s_iListenPort = 0;
std::vector<FakePlayer> s_Players;

std::uint32_t NowMs()
{
	using namespace std::chrono;
	return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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

FakePlayer *Find(std::uint16_t session)
{
	for (FakePlayer &one : s_Players)
		if (one.session == session)
			return &one;
	return nullptr;
}

std::uint16_t FreeSession()
{
	std::uint16_t session = 1;
	while (Find(session))
		session++;
	return session;
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

// `ir_bot_add [name]`: a fake player joins; its control session is the lowest free number.
void CommandAdd()
{
	const std::uint16_t session = FreeSession();
	std::string name = CMD_ARGC() > 1 ? CMD_ARGV(1) : std::string(ir_bot_name.string) + std::to_string(session);
	edict_t *edict = g_engfuncs.pfnCreateFakeClient(name.c_str());
	if (FNullEnt(edict))
	{
		ALERT(at_console, "ir-bot: the server is full, no fake player added\n");
		return;
	}
	char reject[128] = {};
	if (!ClientConnect(edict, name.c_str(), "127.0.0.1", reject))
	{
		ALERT(at_console, "ir-bot: %s was refused: %s\n", name.c_str(), reject);
		return;
	}
	ClientPutInServer(edict);
	edict->v.flags |= FL_FAKECLIENT;
	FakePlayer one;
	one.edict = edict;
	one.session = session;
	s_Players.push_back(one);
	ALERT(at_console, "ir-bot: %s joined, moved by IR Control session %d\n", name.c_str(), (int)session);
}

// `ir_bot_status`: the fake players, their sessions, and whether a control moves them.
// Printed by the engine's ServerPrint, so that rcon gets the answer too.
void CommandStatus()
{
	char line[256];
	std::snprintf(line, sizeof(line), "ir-bot: %d fake players, IR Control on port %d\n", (int)s_Players.size(), s_iListenPort);
	g_engfuncs.pfnServerPrint(line);
	for (const FakePlayer &one : s_Players)
	{
		const Vector &at = one.edict->v.origin;
		std::snprintf(line, sizeof(line), "  session %d  %s  %s  health %.0f  at %.0f %.0f %.0f  yaw %.0f\n", (int)one.session,
		    STRING(one.edict->v.netname), s_Server.channel(one.session) ? (one.handsOff ? "channel open, hands-off" : "controlled") : "no channel",
		    one.edict->v.health, at.x, at.y, at.z, one.edict->v.v_angle.y);
		g_engfuncs.pfnServerPrint(line);
	}
}

// One server frame of a fake player: the control in effect, as hl-cl-consumer applies it to the
// local player (ir-control §6, §8).
void Move(FakePlayer &one, float seconds)
{
	edict_t *edict = one.edict;
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
	const int msec = static_cast<int>(seconds * 1000.0f + 0.5f);
	g_engfuncs.pfnRunPlayerMove(edict, angles, forward, side, up, buttons, impulse, static_cast<byte>(clamp(msec, 1, 255)));
}

}

void ir_server::Init()
{
	CVAR_REGISTER(&ir_control_port);
	CVAR_REGISTER(&ir_bot_name);
	g_engfuncs.pfnAddServerCommand("ir_bot_add", CommandAdd);
	g_engfuncs.pfnAddServerCommand("ir_bot_status", CommandStatus);
}

void ir_server::Frame()
{
	if (s_Players.empty() && !s_Server.running())
		return;
	Listen();
	s_Server.poll(NowMs());
	for (FakePlayer &one : s_Players)
		Move(one, gpGlobals->frametime);
}

void ir_server::Disconnect(edict_t *player)
{
	for (auto it = s_Players.begin(); it != s_Players.end(); ++it)
	{
		if (it->edict == player)
		{
			s_Server.closeChannel(it->session);
			s_Players.erase(it);
			return;
		}
	}
}
