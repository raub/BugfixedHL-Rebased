// hl-cl-consumer: applies IR Control to the local player (IR Bot project).
//
// A player (ir-player, ir-bot, or a test tool) opens a control channel over TCP and sends
// controls over UDP; while the channel is open they replace the local keyboard and mouse.
// Channels are accepted for the ingress session hl-cl-producer has open (ir_producer.h): the
// session is the agent. `ir_session <id>` names a session by hand for tests without it.
// Protocol and rules: doc/spec/ir-control_v1.md in https://github.com/raub/ir-bot.
//
// The protocol code lives in ir-bot's hl/common. When this repository is built on its own
// (not as ir-bot's submodule), BHL_IRBOT is not defined and the hooks below do nothing.
#ifndef IR_CONSUMER_H
#define IR_CONSUMER_H

struct usercmd_s;

namespace ir_consumer
{

#ifdef BHL_IRBOT

// HUD_Frame: opens or closes the listener with the producer's session and polls the sockets.
void Frame();

// HUD_Shutdown: closes the sockets.
void Shutdown();

// Start of CL_CreateMove, before local input is applied.
void PreCreateMove(int active);

// End of CL_CreateMove, after local input filled `cmd`: during takeover, overwrites the
// command and the view angles with the current control.
void CreateMove(struct usercmd_s *cmd, int active);

#else

inline void Frame() {}
inline void Shutdown() {}
inline void PreCreateMove(int) {}
inline void CreateMove(struct usercmd_s *, int) {}

#endif

}

#endif
