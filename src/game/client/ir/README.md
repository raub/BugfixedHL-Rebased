# IR Bot components of the client library

Two components of the IR Bot project (<https://github.com/raub/ir-bot>) live here. Both are compiled
only when this repository is checked out as ir-bot's submodule (`hl/BugfixedHL-Rebased`), where the
shared code `../common` exists; built on its own, the hooks are empty.

| Component | Files | Does |
|---|---|---|
| `hl-cl-producer` | `ir_producer.{h,cpp}` | observes the local player and sends IR Ingress frames to the IR service, 20 per second; can record them into an `.irin` file |
| `hl-cl-consumer` | `ir_consumer.{h,cpp}` | applies IR Control from a player (viewer or bot) to the local player |

Specifications, in ir-bot: `doc/spec/hl-producer_v1.md` (§13), `doc/spec/ir-ingress-protocol_v1.md`,
`doc/spec/ir-control_v1.md`.

## Console

| Name | Default | Meaning |
|---|---|---|
| `ir_produce` | `0` | `1`: produce frames while playing; `2`: also while a demo plays (for checks) |
| `ir_service` | `127.0.0.1:47700` | address of the IR service; empty: do not connect |
| `ir_record` | `0` | `1`: also write each session into an `.irin` file |
| `ir_record_dir` | | folder for `.irin` files; default `<IRBOT_DATA>/irin`, else `<game>/irin` |
| `ir_config` | | folder with the map configurations; default `IRBOT_CONFIG`, else `<IRBOT_DATA>/../config` |
| `ir_connect` | command | closes the session and opens a new one (after starting the IR service) |
| `ir_snapshot` | | `"<from> <to> <every>"`: game snapshots at these frame indices, to compare with the IR service's images |
| `ir_control_port` | `47702` | TCP port the consumer listens on (controls arrive on the same UDP port) |
| `ir_session` | | for tests without the producer: accept control channels for this session id |
| `ir_release` | command | closes the control channel: control returns to keyboard and mouse |
| `ir_status` | command | prints the state of both components |

A session is open while the game is playable for the local player on a map that has a configuration
(`config/hl/<map>.irmap` in ir-bot). The consumer accepts control channels for that session.

## Hooks

`ir_producer.h` and `ir_consumer.h` list every hook with the place it is called from. The calls are
single lines in `cdll_int.cpp`, `input.cpp`, `view.cpp`, `entity.cpp`, `hud.cpp`, `hud/base.h`,
`svc_messages.cpp`, and `hl/hl_events.cpp`.
