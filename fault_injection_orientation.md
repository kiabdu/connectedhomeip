# Matter Device Emulator for Interoperability Testing
## Orientation Guide for AI Assistants

### Project Goal
Build a **fault injection testing framework** embedded within a Matter device emulator to test how Matter controllers (Home Assistant, chip-tool, etc.) handle malformed, delayed, or corrupted responses from devices.

### Problem Statement
Current Matter testing tools assume healthy device behavior. We need to test edge cases:
- What happens when a device returns an out-of-range temperature value?
- How do controllers handle delayed responses past the timeout threshold?
- Do controllers properly validate boundary conditions?

### Solution Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Python Test Harness (external)                                 │
│  - Controls fault injection via JSON API                        │
│  - Sends commands to Matter app                                 │
│  - Collects metrics on controller behavior                      │
└─────────────────┬───────────────────────────────────────────────┘
                  │
                  │ Unix Domain Socket (IPC)
                  │
┌─────────────────▼───────────────────────────────────────────────┐
│  Matter Device Emulator (chip-lighting-app)                     │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Fault Control Interface                                  │  │
│  │  - JSON parsing via Unix socket                           │  │
│  │  - Runtime fault configuration                            │  │
│  └───────────────────┬───────────────────────────────────────┘  │
│                      │                                          │
│  ┌───────────────────▼───────────────────────────────────────┐  │
│  │  Fault Injection Points                                   │  │
│  │  1. Attribute Access (read/write)                         │  │
│  │  2. Command Handler (invoke/response)                     │  │
│  │  3. Exchange Layer (timing)                               │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### Project Scope (What We're Building)

| Component | File Location | Purpose |
|-----------|---------------|---------|
| FaultInjector | `examples/common/fault_injection/FaultInjector.*` | Fault storage and matching (no JSON, no sockets) |
| FaultSocket | `examples/common/fault_injection/FaultSocket.*` | Unix socket server, JSON parsing and dispatch |
| Attribute Hook | `src/app/data-model-provider/` (see Key Files) | Intercept attribute reads/writes |
| Command Hook | `src/app/data-model-provider/` (see Key Files) | Intercept command execution |
| Integration | `examples/lighting-app/linux/` | Wire everything together |

### Fault Types Supported

1. **DATA_CORRUPTION** - Return invalid values
   - Out-of-range sensor values
   - Invalid enum states
   - Wrong data types

2. **TIMING_DELAY** - Artificial latency
   - Delayed responses
   - Timeout triggering (>10 seconds)

3. **COMMAND_DROP** - Silent failures
   - No response to commands
   - No status responses

4. **INVALID_STATUS** - Error codes
   - Custom IM status codes
   - Protocol-level errors

### Implementation Flow

**Step 1: Fault Injection Control Interface** (done)
- Create Unix Domain Socket server
- JSON schema for fault commands
- Runtime fault configuration

**Step 2: Interception Points** (next)
- Attribute read/write hook
- Command hook
- Timing hook
- See "Hook location for Step 2" below

**Step 3: Integration** (done for chip-lighting-app)
- Wire into `chip-lighting-app`
- Configure clusters/attributes to hook

**Step 4: Python Test Harness**
- Client library for fault injection
- Automated test sequences
- Metrics collection

### Key Files to Modify

```
src/app/data-model-provider/
└── Provider.h                        # ReadAttribute / WriteAttribute / InvokeCommand

examples/common/fault_injection/      # DONE: FaultInjector + FaultSocket
examples/lighting-app/linux/main.cpp  # DONE: starts/stops FaultSocketServer
```

**Hook location for Step 2:** On/Off (`OnOffCluster`) and Level Control
(`LevelControlCluster`) are code-driven clusters (`DefaultServerCluster`
subclasses in `src/app/clusters/`). An `AttributeAccessInterface` hook does
not see their reads. The Interaction Model reaches every cluster through
`DataModel::Provider::ReadAttribute`, `WriteAttribute` and `InvokeCommand`
(`src/app/data-model-provider/Provider.h`). A provider that wraps the app's
provider and consults `FaultInjector` before forwarding covers all clusters
without patching `src/app/` internals. Verify this against the current code
before implementing.

### Testing Clusters (Priority Order)

1. **On/Off (0x0006)** - Simple, well-understood
2. **Level Control (0x0008)** - Range validation
3. **Temperature Measurement (0x0402)** - Boundary values
4. **Color Control (0x0300)** - Multiple parameters

### Expected Behavior

When a fault is configured:
1. Python sends JSON to Unix socket
2. FaultInjector parses and stores configuration
3. When matching cluster/attribute is accessed, fault is triggered
4. Log entry emitted with timestamp and details
5. Modified response sent to controller

### Output Format (planned, not implemented)

Today FaultInjector only emits a plain `ChipLogProgress` line on configure,
e.g. `FaultInjector: configured data_corruption fault on cluster 0x00000006`.
Once the Step 2 hooks exist, triggered faults should be logged as JSON:
```json
{
  "timestamp": 1730668800,
  "event": "FAULT_TRIGGERED",
  "type": "DATA_CORRUPTION",
  "cluster": "0x0006",
  "attribute": "0x0000",
  "details": "returned invalid on-off state"
}
```

### Current Phase: Steps 1 + 3 (COMPLETE), Steps 2 + 4 (Pending)

**Next objective**: Step 2 — make configured faults change device behavior.

**Status: control interface and app integration are done. Up to commit
`[fault-injection] Add architecture documentation` they were build-verified
and commissioning-verified end to end. The later review-fix commit is not
built yet (see "Verification"). Faults are configurable at runtime via the
Unix socket; they do not affect attribute/command handling yet (Step 2).**

**Completed** (committed on `master` and `fault-injection-feature`):
- [x] Created `examples/common/fault_injection/` directory
- [x] `FaultInjector.h` with:
  - `FaultType` enum (kDataCorruption, kTimingDelay, kCommandDrop, kInvalidStatus)
  - `FaultTarget` enum (attribute vs command matching)
  - `FaultValueType` enum (discriminator for the corruptValue union)
  - `FaultConfig` struct for configuration
  - `FaultInjectorDelegate` interface for notifications
  - `FaultInjector` singleton class (max 16 faults, fixed array, no heap)
- [x] `FaultInjector.cpp` with:
  - Fault configuration storage (configure/replace/clear/clear-all)
  - Matching by (attribute|command, endpoint or all endpoints, cluster, ID)
  - `ApplyDataCorruption` (TLV write of int/float/bool/null corrupt values)
  - Delay, command-drop, and injected-status helpers
  - Delegate notifications + ChipLog events
  - Note: JSON parsing is NOT here — it lives in FaultSocket.cpp (transport-agnostic core)
- [x] `FaultSocket.h` + `FaultSocket.cpp` with:
  - POSIX Unix domain socket server, accept loop on its own thread
  - JSON command dispatch (jsoncpp): `configure`, `clear`, `clear_all`
    via `ProcessFaultRequest()` (unit-testable without a socket)
  - JSON response: `{"result":"ok"}` / `{"result":"error","detail":"<fixed reason>"}`
  - JSON fault schema: `type` (data_corruption|timing_delay|command_drop|invalid_status),
    `scope` (attribute|command), `endpoint` (optional), `cluster` (int or "0x0006" string),
    `attribute`/`command`, `delay_ms`, `status`, `corrupt_value` (bool/int/float/null)
- [x] Build wiring: `fault_injection/BUILD.gn`, dep added to
  `examples/lighting-app/linux/BUILD.gn`, tests registered in `src/BUILD.gn`
- [x] Verified: lighting app builds and runs; chip-tool commissions and
  reads/writes OnOff attributes (see docs/how-to.md)

**Files Created**:
```
examples/common/fault_injection/
├── BUILD.gn              # :fault_injector (core, src/lib only)
│                         # :fault_injection (socket, + jsoncpp, src/platform)
├── FaultInjector.h       # Types, config, singleton interface
├── FaultInjector.cpp     # Storage, matching, corruption/status logic
├── FaultSocket.h         # Socket server + ProcessFaultRequest()
├── FaultSocket.cpp       # Accept loop, JSON parsing and dispatch
└── tests/                # pw_unit_test suites (libFaultInjectionTests)
```

**Still Missing**:
- [ ] Step 2: Interception points — faults can be configured but nothing
  consults the FaultInjector yet. Needs attribute read/write, command and
  timing hooks; see "Hook location for Step 2" under Key Files
- [ ] Step 4: Python test harness (client library, test sequences, metrics)
- [ ] Integration test that drives the real socket (Start/Stop, read
  timeout). The JSON handling and FaultInjector are unit tested; the socket
  thread itself is not.
- [ ] Separate read and write faults for attributes. An attribute fault has
  no read/write flag yet; decide when the Step 2 write hook is added.
- [ ] More corrupt value types if needed (uint64, strings, lists, structs).
- [ ] Wire the socket into `examples/all-devices-app` (has a
  `temperature-sensor` device type). Move `--fault-socket` from the lighting
  app's `AppOptions` to a shared place first, so apps do not copy it.

**Code review fixes (2026-11-05, committed, not yet built — see "Verification" below)**:
- [x] Command faults matched only command ID 0. `Matches()` now takes one
  path ID, so attribute and command IDs cannot be swapped.
- [x] Wrongly typed JSON fields aborted the app (jsoncpp is built with
  `JSON_USE_EXCEPTION=0`). Every field is type-checked before conversion;
  bad input returns an error response.
- [x] Error responses embedded unescaped jsoncpp text. All responses are now
  fixed strings (`invalid json`, `unknown action`, `invalid fault`,
  `invalid clear`, `fault table full`).
- [x] A client that never sent a line blocked the server and app shutdown.
  Connections have a 1 s read/write timeout; the accept loop polls every
  200 ms and exits when `Stop()` clears `mRunning` (now `std::atomic<bool>`).
  The listening fd is closed only after the thread has joined.
- [x] FaultInjector was mutated from the socket thread without a lock. The
  socket thread now holds the Matter stack lock (`DeviceLayer::StackLock`)
  while applying a command. Contract: FaultInjector is only used on the
  Matter event loop or with the stack lock held.
- [x] Silent truncation: number strings must parse fully, `status` must be
  0–255, `corrupt_value` must fit int32/float. `data_corruption` requires
  `corrupt_value`, `timing_delay` requires `delay_ms > 0`, `invalid_status`
  requires `status`. The `corruptValue` union is zero-initialized.
- [x] `clear` accepts `"scope": "command"` + `command`.
  API: `ClearFault()` became `ClearAttributeFault()` / `ClearCommandFault()`.
- [x] `ApplyDataCorruption()` takes the TLV tag from the caller.
- [x] Removed the unused, broken `examples/common/BUILD.gn` group.
- [x] Endpoint matching: optional `endpoint` in `FaultConfig` and JSON
  (`configure` and `clear`); empty = all endpoints; endpoint-specific faults
  win over all-endpoints faults. Lookup/clear APIs take an endpoint.
- [x] `null` corrupt values (`FaultValueType::kNull`, `"corrupt_value": null`)
  for nullable attributes such as Temperature Measurement MeasuredValue.
- [x] Unit tests: `examples/common/fault_injection/tests/`
  (`TestFaultInjector.cpp`, `TestFaultSocketRequests.cpp`), registered in
  `src/BUILD.gn` `example_tests` for linux/darwin.

**Verification**: the fixes were written on a macOS checkout without
submodules and have NOT been compiled yet. On the Linux build machine run:
```
scripts/run_in_build_env.sh "./scripts/build/build_examples.py --target linux-x64-tests-clang --quiet gen"
scripts/run_in_build_env.sh "ninja -C out/linux-x64-tests-clang examples/common/fault_injection/tests:libFaultInjectionTests.run"
scripts/run_in_build_env.sh "./scripts/build/build_examples.py --target linux-x64-light --quiet build"
```
Then repeat the socket smoke test in docs/how-to.md section 8.

**Done since Step 1**:
- [x] Step 3: Integration — `FaultSocketServer` starts from
  `ApplicationInit()` in `examples/lighting-app/linux/main.cpp`, stops in
  `ApplicationShutdown()`. Socket path defaults to `/tmp/matter_fault.sock`,
  overridable with `--fault-socket <path>` (option in `AppOptions.cpp`, now
  compiled and registered unconditionally). Verified live: JSON commands
  configure/clear faults, responses and log events work end to end.

### Important Environment Info (read before building)

- **Build layout**: `build_examples.py` for linux targets sets the GN root to
  `examples/<app>/linux` with `chip_root = //third_party/connectedhomeip`
  (a symlink back to the repo root created for the VS Code flow). Do not run
  raw `gn gen` from the repo root for these builds.
- **Endpoint pool fix**: the Linux platform defaults to 32 TCP + 32 UDP
  endpoints; the select system layer sizes its socket pool from those. On
  hosts with many Docker veth/bridge interfaces, mDNS (one socket per address)
  exhausts the pool and the app dies with
  `CHIP Error 0x000000C1: Endpoint pool full`. Fixed by
  `examples/common/InetProjectConfig.h` (128+128), wired via
  `chip_inet_project_config_include` in both
  `examples/lighting-app/linux/args.gni` and `examples/chip-tool/args.gni`.
- **chip-tool java deps 403**: `third_party/java_deps/set_up_java_deps.sh`
  downloads `google-collect-1.0-rc1.jar` from `dist.wso2.org`, which blocks
  curl's default User-Agent with 403. Workaround: download the jar with a
  browser User-Agent into `third_party/java_deps/artifacts/` (the script
  skips files that already exist). Not on Maven Central.
- **chip-tool pairing**: the `pairing ethernet` subcommand no longer exists.
  Use `pairing code <node-id> <payload>` (mDNS discovery based). Verified
  payload: `MT:-24J042C00KA0648G00` (PIN 20202021, discriminator 3840).
- **Factory reset**: `pkill -f chip-lighting-app`, delete
  `/tmp/chip_kvs*`, `/tmp/chip_config.ini`, `/tmp/chip_factory.ini`,
  `/tmp/chip_counters.ini` (device) and `/tmp/chip_tool_*` (controller),
  then restart. The commissioning window only opens once per boot.
- **Non-fatal noise**: `Failed to advertise records ... 0x00000046` in the
  device log is mDNS trying to multicast on Docker interfaces that do not
  support it. Service still publishes on real interfaces.

### Constraints
- Use C++17
- Follow CHIP SDK coding style (CHIP_ERROR, VerifyOrReturnError, etc.)
- Use POSIX Unix domain sockets
- Parse JSON commands and call `FaultInjector::ConfigureFault()`
- Add unit tests with every change (see CLAUDE.md, "Testing")
- jsoncpp is built with `JSON_USE_EXCEPTION=0`: check a value's type before
  any `as*()` / `isMember()` / `operator[]` call, or the app aborts
- FaultInjector is not thread-safe: use it on the Matter event loop or with
  `DeviceLayer::StackLock` held. Step 2 delays must use timers, not sleeps,
  because a sleep on the event loop stalls the whole stack