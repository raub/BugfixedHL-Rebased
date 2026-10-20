// hl-sv-consumer: fake players on a Half-Life server, moved by IR Control (IR Bot project).
//
// The server creates fake players (`ir_bot_add`) and, in every server frame, moves each of them by
// the control of its IR Control session: view angles, movement, buttons, weapon selection
// (doc/spec/ir-control_v1.md §6 and §8 in https://github.com/raub/ir-bot). Without an open control
// channel a fake player stands still. hl-sv-producer, which gives each fake player an ingress
// session, comes next; until then a fake player's session id is its number (1, 2, ...).
//
// The protocol code and its rules live in ir-bot's hl/common, shared with hl-cl-consumer. When this
// repository is built on its own (not as ir-bot's submodule), BHL_IRBOT is not defined and the
// hooks below do nothing.
#ifndef IR_SERVER_H
#define IR_SERVER_H

namespace ir_server
{

#ifdef BHL_IRBOT

// GameDLLInit: registers the settings and commands.
void Init();

// StartFrame, before the game rules think: reads arriving controls and moves the fake players.
void Frame();

// ClientDisconnect: forgets a fake player that left (kicked, or the server shuts down).
void Disconnect(struct edict_s *player);

#else

inline void Init() {}
inline void Frame() {}
inline void Disconnect(struct edict_s *) {}

#endif

}

#endif
