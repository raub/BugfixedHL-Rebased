// hl-cl-consumer: applies IR Control to the local player. See ir_consumer.h.
//
// Specifications (in ir-bot): doc/spec/ir-control_v1.md §6 and §8, doc/spec/hl-producer_v1.md
// §4 (angles), §7.1 (weapon ids), §11 (movement normalization).
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "hud.h"
#include "cl_util.h"
#include "console.h"
#include "convar.h"
#include "usercmd.h"
#include "in_buttons.h"
#include "pm_defs.h"
#include "pm_movevars.h"
#include "pm_shared.h"

#include "ir_consumer.h"
#include "ir_producer.h"

// ir-bot's shared headers (hl/common), last because they need the sockets API.
#ifdef _WIN32
#include <winsani_in.h>
#include <winsock2.h>
#include <winsani_out.h>
#endif
#include "common/control_net.hpp"

namespace
{

ConVar ir_session("ir_session", "", 0, "For tests without hl-cl-producer: accept control channels for this session id (empty = the producer's session)");
ConVar ir_control_port("ir_control_port", "47702", 0, "TCP port hl-cl-consumer listens on (controls arrive on the same UDP port)");

// The session is the agent (ir-control §2.2): controls are accepted for the ingress session
// hl-cl-producer has open, and only while it is open. `ir_session` names a session by hand
// instead, for tests that run without the producer or the IR service.
int WantedSession()
{
	const char *text = ir_session.GetString();
	if (!text || !text[0])
		return ir_producer::SessionId();
	char *end = nullptr;
	const long value = std::strtol(text, &end, 10);
	if (end == text || *end != '\0' || value < 0 || value > 65535)
		return -1;
	return static_cast<int>(value);
}

hl::ControlServer s_Server;
int s_iSession = -1; // session served now, -1 = none
int s_iListenPort = 0; // port the listener was last started (or failed to start) on
bool s_bTakeover = false; // a control channel is open: local input is replaced
bool s_bHandsOff = true; // no control in effect (none received yet, or timed out)
Vector s_vecViewAngles; // view angles before local input touched them in this frame

std::uint32_t NowMs()
{
	using namespace std::chrono;
	return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Weapon ids are the Half-Life SDK ids (hl-producer §7.1). The game selects a weapon by its
// class name as a server command, plus `weaponselect` in the command for prediction.
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

// IR angle code -> Half-Life degrees: pitch changes sign (HL positive = down), yaw does not.
float AngleDegrees(std::int16_t code)
{
	return static_cast<float>(hl::controlAngleToRadians(code) * 180.0 / 3.14159265358979323846);
}

void StopListening()
{
	if (s_Server.running())
	{
		s_Server.stop();
		ConPrintf("ir-control: listener closed\n");
	}
	s_iListenPort = 0;
}

}

CON_COMMAND(ir_release, "Closes the IR control channel: control returns to keyboard and mouse")
{
	if (s_iSession >= 0 && s_Server.channel(static_cast<std::uint16_t>(s_iSession)))
		s_Server.closeChannel(static_cast<std::uint16_t>(s_iSession));
	else
		ConPrintf("ir-control: no channel is open\n");
}

CON_COMMAND(ir_status, "Prints the state of hl-cl-consumer")
{
	ir_producer::PrintStatus();
	if (!s_Server.running())
	{
		ConPrintf("ir-control: off (no ingress session is open; ir_session <id> names one by hand)\n");
		return;
	}
	ConPrintf("ir-control: session %d, listening on 127.0.0.1:%d (TCP), controls on UDP %d, %s\n",
	    s_iSession, (int)s_Server.tcpPort(), (int)s_Server.udpPort(),
	    !s_bTakeover ? "no channel (local input)" : s_bHandsOff ? "channel open, hands-off" : "channel open, controls applied");
}

void ir_consumer::Frame()
{
	const int session = WantedSession();
	if (session != s_iSession)
	{
		s_iSession = session; // the old session's channel, if any, is closed by poll()
		s_iListenPort = 0;
	}

	if (s_iSession < 0)
	{
		StopListening();
	}
	else
	{
		const int port = ir_control_port.GetInt();
		if (port != s_iListenPort)
		{
			// Also reached after a failed start, but only once the cvars change again.
			s_iListenPort = port;
			s_Server.setLog([](const std::string &line) { ConPrintf("%s\n", line.c_str()); });
			s_Server.setSessionCheck([](std::uint16_t id) { return static_cast<int>(id) == s_iSession; });
			if (port > 0 && port <= 65535 && s_Server.start(static_cast<std::uint16_t>(port)))
				ConPrintf("ir-control: session %d, listening on 127.0.0.1:%d\n", s_iSession, port);
			else
				ConPrintf("ir-control: cannot listen on port %d\n", port);
		}
	}

	s_Server.poll(NowMs());

	const bool takeover = s_iSession >= 0 && s_Server.channel(static_cast<std::uint16_t>(s_iSession)) != nullptr;
	if (takeover != s_bTakeover)
	{
		s_bTakeover = takeover;
		s_bHandsOff = true;
		ConPrintf(takeover ? "ir-control: takeover, local input is replaced (ir_release to stop)\n" : "ir-control: released, local input is back\n");
	}
}

void ir_consumer::Shutdown()
{
	s_Server.stop();
	s_bTakeover = false;
}

void ir_consumer::PreCreateMove(int active)
{
	if (active && s_bTakeover)
		gEngfuncs.GetViewAngles((float *)s_vecViewAngles);
}

void ir_consumer::CreateMove(struct usercmd_s *cmd, int active)
{
	if (!active || !s_bTakeover)
		return;

	hl::ControlChannel *channel = s_Server.channel(static_cast<std::uint16_t>(s_iSession));
	if (!channel)
		return; // closed since the last frame; Frame() reports it

	const hl::ControlCommand control = channel->command(NowMs());
	if (control.handsOff != s_bHandsOff)
	{
		s_bHandsOff = control.handsOff;
		if (s_bHandsOff)
			ConPrintf("ir-control: no control for %d ms, hands-off\n", (int)hl::kControlTimeoutMs);
	}

	// View (ir-control §6.1): set once when a control arrives, then left alone, so that angles
	// set by the server (respawn, teleport) stay. Whatever the mouse and the keyboard did to the
	// view in this frame is undone.
	Vector angles = s_vecViewAngles;
	if (control.setView)
	{
		angles[0] = clamp(-AngleDegrees(control.viewPitch), -89.0f, 89.0f);
		angles[1] = AngleDegrees(control.viewYaw);
		angles[2] = 0;
	}
	gEngfuncs.SetViewAngles((float *)angles);

	// Movement (§6.2, §8): normalized movement times the maximum speed.
	// hl-cl-producer divides by the same speed.
	const float maxSpeed = ir_producer::MaxSpeed();
	cmd->forwardmove = static_cast<float>(hl::controlMoveToUnit(control.moveForward)) * maxSpeed;
	cmd->sidemove = static_cast<float>(hl::controlMoveToUnit(control.moveSide)) * maxSpeed;
	cmd->upmove = 0;

	// Buttons (§6.3; the table is applied by hl::ControlApplier), plus the movement-key bits
	// from the signs of the movement values (§8).
	int buttons = 0;
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
	cmd->buttons = static_cast<unsigned short>(buttons);

	// Swimming up: IR has no upward movement; jump held in water means full upmove (§8).
	if ((control.buttons & hl::kButtonJump) && PM_GetWaterLevel() >= 2)
		cmd->upmove = maxSpeed;

	// One-shot requests (§6.4): the applier reports each exactly once.
	cmd->impulse = control.commandId;
	cmd->weaponselect = 0;
	if (control.weaponSelect != 0)
	{
		if (const char *name = WeaponName(control.weaponSelect))
		{
			gEngfuncs.pfnServerCmd(name);
			cmd->weaponselect = control.weaponSelect;
		}
		else
		{
			ConPrintf("ir-control: unknown weapon id %d\n", (int)control.weaponSelect);
		}
	}
}
