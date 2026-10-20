// hl-sv-consumer and hl-sv-producer: fake players on a Half-Life server (IR Bot project).
//
// The server creates fake players (`ir_bot_add`). In every server frame it moves each of them by
// the control of its IR Control session: view angles, movement, buttons, weapon selection
// (doc/spec/ir-control_v1.md §6 and §8 in https://github.com/raub/ir-bot); without an open control
// channel a fake player stands still. And it observes each of them into an IR Ingress session of
// its own (doc/spec/hl-producer_v1.md §14): what the player sees, its state, what it hears, and
// the control it was given. That session's id, which the IR service hands out, is also the
// player's IR Control session; while no IR service is there, the session is the player's number.
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

// The game's PM_Move export: runs `move`, and notes the sounds of player movement (footsteps).
void PlayerMove(struct playermove_s *pmove, int server, void (*move)(struct playermove_s *, int));

#else

inline void Init() {}
inline void Frame() {}
inline void Disconnect(struct edict_s *) {}
inline void PlayerMove(struct playermove_s *pmove, int server, void (*move)(struct playermove_s *, int)) { move(pmove, server); }

#endif

}

#endif
