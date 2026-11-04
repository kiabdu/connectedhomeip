# Matter Device Emulator — Architecture

Fault injection framework embedded in the Matter lighting app emulator, for
testing how Matter controllers (Home Assistant, chip-tool) handle malformed,
delayed, or corrupted device behavior.

## Components

```
┌─────────────────────────────────────────────────────────────────┐
│  Python test harness (planned, Step 4)                          │
└─────────────────┬───────────────────────────────────────────────┘
                  │ newline-terminated JSON, one command per
                  │ connection, JSON response line
                  │ Unix domain socket (default /tmp/matter_fault.sock)
┌─────────────────▼───────────────────────────────────────────────┐
│  chip-lighting-app                                               │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  FaultSocketServer (examples/common/fault_injection/)     │  │
│  │  - accept loop on dedicated thread                        │  │
│  │  - JSON parse + dispatch (jsoncpp)                        │  │
│  └───────────────────┬───────────────────────────────────────┘  │
│                      │                                          │
│  ┌───────────────────▼───────────────────────────────────────┐  │
│  │  FaultInjector singleton (examples/common/fault_injection/)│ │
│  │  - fixed array of 16 FaultConfig, no heap                 │  │
│  │  - match by (target, cluster, attribute/command)          │  │
│  └───────────────────┬───────────────────────────────────────┘  │
│                      │ consulted by interception hooks          │
│  ┌───────────────────▼───────────────────────────────────────┐  │
│  │  Interception points (planned, Step 2)                    │  │
│  │  - AttributeAccessInterface (read/write corruption)       │  │
│  │  - InteractionModelEngine (command drop/status)           │  │
│  │  - Exchange layer (timing delay)                          │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Repository layout

| Path | Purpose |
|------|---------|
| `examples/common/fault_injection/FaultInjector.h/.cpp` | Core fault storage, matching, corruption/status helpers. Transport-agnostic — no JSON, no sockets. |
| `examples/common/fault_injection/FaultSocket.h/.cpp` | Unix domain socket server. JSON parsing and dispatch live here, not in the core. |
| `examples/common/fault_injection/BUILD.gn` | `source_set("fault_injection")`, deps on `src/lib` + jsoncpp, exports its include dir. |
| `examples/common/InetProjectConfig.h` | Raises INET TCP/UDP endpoint pools 32 → 128 (see below). |
| `examples/common/BUILD.gn` | Group target for common example components. |
| `examples/lighting-app/linux/main.cpp` | Starts/stops `FaultSocketServer` in `ApplicationInit`/`ApplicationShutdown`. |
| `examples/lighting-app/linux/AppOptions.h/.cpp` | App CLI options, including `--fault-socket <path>`. |
| `FULT_INJECTION_ORIENT.md` | Working orientation/checklist for AI assistants (status may drift). |
| `docs/how-to.md` | Build/run/commission/test instructions. |
| `docs/ARCHITECTURE.md` | This document — the design source of truth. |

## FaultInjector design

- **Storage**: fixed array of 16 `FaultConfig` values — no heap, no dynamic
  containers (embedded-style constraint).
- **Matching**: a fault matches either attribute accesses
  (`FaultTarget::kAttribute` + cluster + attribute) or command invocations
  (`FaultTarget::kCommand` + cluster + command), selected by the `target`
  field. `FaultTarget` exists because Matter command IDs of 0 are valid and
  cannot serve as a "not set" sentinel.
- **Reconfiguration**: configuring a fault whose (target, cluster, path)
  matches an existing fault replaces it in place; otherwise it is appended.
  The log distinguishes "configured" from "updated".
- **Corruption values**: `FaultConfig::corruptValue` is a union tagged by
  `FaultValueType` (int32 / float / bool). `ApplyDataCorruption` writes the
  value into a `TLV::TLVWriter` with an anonymous tag.
- **Fault types**: `data_corruption` (return bad values), `timing_delay`
  (`delay_ms`), `command_drop` (no response), `invalid_status` (custom
  `Protocols::InteractionModel::Status`).
- **Notifications**: `FaultInjectorDelegate::OnFaultInjected` plus
  `ChipLogProgress(AppServer, ...)` on every configure.

## FaultSocket protocol

- **Transport**: POSIX `AF_UNIX` stream socket. One command per connection;
  the server reads up to 4096 bytes, stopping at the first newline or EOF,
  parses the JSON, writes one response line, closes.
- **Threading**: `Start()` spawns one accept-loop thread that handles
  connections sequentially. `Stop()` shuts the socket down to wake `accept()`,
  closes, joins, unlinks the path.
- **Commands**:

| Action | Effect |
|--------|--------|
| `{"action": "configure", "fault": {...}}` | Adds or replaces one fault |
| `{"action": "clear", "cluster": N, "attribute": N}` | Removes matching attribute faults |
| `{"action": "clear_all"}` | Removes everything |

- **Fault schema**: `type` (`data_corruption`\|`timing_delay`\|`command_drop`\|
  `invalid_status`), `scope` (`attribute`\|`command`, default attribute),
  `cluster` (required, non-zero; int or `"0x0006"` string), `attribute`,
  `command`, `delay_ms`, `status`, `corrupt_value` (bool/int/float).
- **Response**: `{"result":"ok"}` or `{"result":"error","detail":"..."}`.
- **Known simplification**: the accept thread mutates `FaultInjector` state
  without synchronization. Harmless while only the socket writes, but the
  Step 2 hooks (event-loop threads) will race with it — add a lock or make
  configure-before-use the contract when hooks land.

## Lighting app integration

- `ApplicationInit()` starts the server; `ApplicationShutdown()` stops it.
- Socket path defaults to `/tmp/matter_fault.sock`, overridden with
  `--fault-socket <path>`.
- `AppOptions` is compiled and registered unconditionally (it was previously
  gated behind `CHIP_EXAMPLE_ENABLE_DIAGNOSTIC_LOGS`, which is off by default).

## Environment configuration

- **Endpoint pools**: Linux platform defaults to 32 TCP + 32 UDP endpoints;
  the select-based system layer sizes its socket watch pool from those. mDNS
  opens a socket per address, so hosts with many Docker veth/bridge
  interfaces exhaust the pool and the app dies with
  `CHIP Error 0x000000C1: Endpoint pool full`.
  `examples/common/InetProjectConfig.h` raises both pools to 128 and is wired
  via `chip_inet_project_config_include` in the lighting app's and chip-tool's
  `args.gni` (the controller hits the same wall during discovery).
- **chip-tool java deps**: `dist.wso2.org` 403s curl's default User-Agent for
  `google-collect-1.0-rc1.jar`; pre-download it with a browser UA into
  `third_party/java_deps/artifacts/` (see docs/how-to.md).

## Design decisions

- **JSON in the transport layer**: FaultInjector has no JSON or socket
  dependency; FaultSocket owns parsing. The core stays unit-testable and
  reusable for other transports (e.g. the Python harness over a different
  channel).
- **No heap in the core**: fixed-capacity storage and fixed buffers; jsoncpp
  allocation is confined to FaultSocket (example-app code, not SDK).
- **Replace semantics**: a fault for the same path replaces rather than
  stacks — the harness always knows the active fault for a path.
- **Status**: Steps 1 (control interface) and 3 (app integration) are done
  and verified; Step 2 (interception hooks) and Step 4 (Python harness) are
  pending. Configured faults do not yet affect device behavior.
