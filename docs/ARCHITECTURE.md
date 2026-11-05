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
| `examples/common/fault_injection/BUILD.gn` | `:fault_injector` (core, `src/lib` only) and `:fault_injection` (socket; adds jsoncpp + `src/platform`). |
| `examples/common/InetProjectConfig.h` | Raises INET TCP/UDP endpoint pools 32 → 128 (see below). |
| `examples/common/fault_injection/tests/` | Unit tests (`libFaultInjectionTests`) for FaultInjector and `ProcessFaultRequest()`. |
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
  field. Internally both use one path ID, so lookups cannot mix up
  attribute and command IDs.
- **Endpoints**: `FaultConfig::endpoint` is a `std::optional<EndpointId>`;
  empty means all endpoints. Lookups take a concrete endpoint and prefer an
  endpoint-specific fault over an all-endpoints fault on the same path.
  Nothing in the core is cluster-specific, so new clusters (e.g.
  Temperature Measurement 0x0402) need no FaultInjector changes. `FaultTarget` exists because Matter command IDs of 0 are valid and
  cannot serve as a "not set" sentinel.
- **Reconfiguration**: configuring a fault whose (target, endpoint,
  cluster, path) matches an existing fault replaces it in place; otherwise it
  is appended. Clear uses the same exact identity.
  The log distinguishes "configured" from "updated".
- **Corruption values**: `FaultConfig::corruptValue` is a union tagged by
  `FaultValueType` (int32 / float / bool / null), zero-initialized.
  `ApplyDataCorruption` writes the value into a `TLV::TLVWriter` with the
  tag the caller passes (the hook passes the encoder's tag).
- **Fault types**: `data_corruption` (return bad values), `timing_delay`
  (`delay_ms`), `command_drop` (no response), `invalid_status` (custom
  `Protocols::InteractionModel::Status`).
- **Notifications**: `FaultInjectorDelegate::OnFaultInjected` plus
  `ChipLogProgress(AppServer, ...)` on every configure. (The delegate is
  called on configure; no hook triggers faults yet.)
- **Threading**: not thread-safe. Callers use the Matter event loop or hold
  the Matter stack lock.

## FaultSocket protocol

- **Transport**: POSIX `AF_UNIX` stream socket. One command per connection;
  the server reads up to 4096 bytes, stopping at the first newline, EOF, or
  the 1 s read timeout, parses the JSON, writes one response line, closes.
- **Threading**: `Start()` spawns one accept-loop thread that handles
  connections sequentially and polls the listening socket every 200 ms.
  Each command runs under `DeviceLayer::StackLock`. `Stop()` clears the
  atomic `mRunning`, joins, then closes and unlinks.
- **Parsing**: `ProcessFaultRequest()` (public, unit-tested) parses and
  applies one request. jsoncpp is built with `JSON_USE_EXCEPTION=0`, where a
  wrong-type conversion aborts, so every field is type-checked first.
- **Commands**:

| Action | Effect |
|--------|--------|
| `{"action": "configure", "fault": {...}}` | Adds or replaces one fault |
| `{"action": "clear", "cluster": N, "attribute": N}` | Removes the all-endpoints attribute fault on that path (add `"endpoint": N` for an endpoint-specific one) |
| `{"action": "clear", "scope": "command", "cluster": N, "command": N}` | Removes the command fault on that path |
| `{"action": "clear_all"}` | Removes everything |

- **Fault schema**: `type` (`data_corruption`\|`timing_delay`\|`command_drop`\|
  `invalid_status`), `scope` (`attribute`\|`command`, default attribute),
  `endpoint` (optional, 0–65535), `cluster` (required, non-zero; int or
  `"0x0006"` string), `attribute`, `command`, `delay_ms`, `status` (0–255),
  `corrupt_value` (bool/int32/float/null).
  Required per type: `data_corruption` → `corrupt_value`, `timing_delay` →
  `delay_ms > 0`, `invalid_status` → `status`.
- **Response**: `{"result":"ok"}` or `{"result":"error","detail":"<reason>"}`
  with a fixed reason: `invalid json`, `unknown action`, `invalid fault`,
  `invalid clear`, `fault table full`.

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
