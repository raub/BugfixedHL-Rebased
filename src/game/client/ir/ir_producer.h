// hl-cl-producer: observes the local player and sends IR Ingress frames (IR Bot project).
//
// While the game is playable for the local player, the producer keeps an ingress session open
// with the IR service and sends one frame per sample (20 per second): the player's own state,
// the entities the server sent, the sounds of others, and the controls in effect. It can also
// record the frames into an .irin file. hl-cl-consumer accepts controls for the same session.
// Rules: doc/spec/hl-producer_v1.md (§13 for this producer) and
// doc/spec/ir-ingress-protocol_v1.md in https://github.com/raub/ir-bot.
//
// The rules themselves live in ir-bot's hl/common, shared with hl-dem-producer. When this
// repository is built on its own (not as ir-bot's submodule), BHL_IRBOT is not defined and the
// hooks below do nothing.
#ifndef IR_PRODUCER_H
#define IR_PRODUCER_H

struct usercmd_s;
struct ref_params_s;
struct clientdata_s;
struct event_args_s;
struct SvcClientFuncs;

namespace ir_producer
{

using EventFn = void (*)(struct event_args_s *);

#ifdef BHL_IRBOT

// CSvcMessages::Init: adds the handlers for sounds, the resource list, and the level start to
// the table of server message hooks.
void InstallSvcHooks(SvcClientFuncs &funcs);

// Game_HookEvents: returns the function to register for an event script instead of `fn`. It
// reports the event to the producer and then calls `fn`.
EventFn WrapEvent(const char *name, EventFn fn);

// HUD user message dispatch, before the HUD's own handler.
void UserMessage(const char *name, int size, void *buf);

// HUD_TxferLocalOverrides: the local player's state as the server sent it (once per packet).
void ClientData(const struct clientdata_s *client);

// End of CL_CreateMove, after hl-cl-consumer: the command that goes to the server.
void CreateMove(const struct usercmd_s *cmd, int active);

// End of V_CalcRefdef: the view of this client frame. Opens and closes the session and emits
// the frames that are due.
void View(const struct ref_params_s *params);

// HUD_Frame: closes the session when the game stopped rendering (disconnect).
void Frame();

// HUD_Shutdown.
void Shutdown();

// The ingress session id of the local player, or -1 while no session is open with the IR
// service. hl-cl-consumer accepts control channels for this session only.
int SessionId();

// The speed that movement 1.0 stands for; hl-cl-consumer multiplies by the same value.
float MaxSpeed();

// For `ir_status`.
void PrintStatus();

#else

inline void InstallSvcHooks(SvcClientFuncs &) {}
inline EventFn WrapEvent(const char *, EventFn fn) { return fn; }
inline void UserMessage(const char *, int, void *) {}
inline void ClientData(const struct clientdata_s *) {}
inline void CreateMove(const struct usercmd_s *, int) {}
inline void View(const struct ref_params_s *) {}
inline void Frame() {}
inline void Shutdown() {}
inline int SessionId() { return -1; }
inline float MaxSpeed() { return 0; }
inline void PrintStatus() {}

#endif

}

#endif
