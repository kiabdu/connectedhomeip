# How-To: Build and Test the Matter Device Emulator

This guide covers everything from a clean tree to commissioning the emulator
with chip-tool: building the emulator, running it, building chip-tool, pairing,
interacting with the device, and the known environment issues on this machine.

## 0. Device credentials (current defaults)

| What | Value |
|------|-------|
| Onboarding payload | `MT:-24J042C00KA0648G00` |
| Manual pairing code | `34970112332` |
| Setup PIN | `20202021` |
| Discriminator | `3840` |
| Commissioning port | `5540` (UDP + TCP) |
| Node ID used in examples | `12344321` |
| Example IP of this host | `192.168.179.24` (enp9s0) |

## 1. Build the emulator (chip-lighting-app)

```bash
scripts/run_in_build_env.sh "./scripts/build/build_examples.py --target linux-x64-light --quiet build"
```

Binary: `out/linux-x64-light/chip-lighting-app`

> **Why no extra flags are needed:** this machine has ~26 Docker veth/bridge
> interfaces. The Linux platform default (32 TCP + 32 UDP endpoints) makes
> mDNS exhaust the socket pool and the app aborts with
> `CHIP Error 0x000000C1: Endpoint pool full`.
> `examples/common/InetProjectConfig.h` raises the pools to 128+128 and is
> wired into the lighting app via `examples/lighting-app/linux/args.gni`
> (`chip_inet_project_config_include`). Both changes are committed.

## 2. Build chip-tool (~10 min, one time)

```bash
scripts/run_in_build_env.sh "./scripts/build/build_examples.py --target linux-x64-chip-tool-clang --quiet build"
```

Binary: `out/linux-x64-chip-tool-clang/chip-tool`

> **Known failure — java deps 403:** the build downloads
> `google-collect-1.0-rc1.jar` from `dist.wso2.org`, which blocks curl's
> default User-Agent (HTTP 403). This artifact is not on Maven Central.
> One-time workaround:
>
> ```bash
> curl --fail --location -A "Mozilla/5.0 (X11; Linux x86_64) Chrome/120.0 Safari/537.36" \
>   "https://dist.wso2.org/maven2/com/google/common/google-collect/1.0-rc1/google-collect-1.0-rc1.jar" \
>   -o third_party/java_deps/artifacts/google-collect-1.0-rc1.jar
> ```
>
> The setup script skips jars that already exist, so the build then passes.
> chip-tool also carries the endpoint-pool fix (wired via
> `examples/chip-tool/args.gni`) — without it, chip-tool itself dies with
> `Endpoint pool full` while bringing up its transport after discovery.

## 3. Run the emulator

```bash
./out/linux-x64-light/chip-lighting-app
```

Startup is healthy when the log shows:

```
SetupQRCode: [MT:-24J042C00KA0648G00]
UDP::Init bound to port=5540
UDP::Init bound to port=5560
CASE Server enabling CASE session setups
mDNS service published: _matterc._udp
```

Optional flags (from the common Linux platform options):

```bash
./out/linux-x64-light/chip-lighting-app --KVS /tmp/chip_kvs_lighting --interface-id enp9s0
```

- `--KVS <filepath>` — separate persistent storage (lets you factory-reset by
  deleting one file).
- `--interface-id <iface>` — restrict mDNS/transports to one interface; useful
  on Docker-heavy hosts as an alternative to the endpoint-pool raise.
- `--fault-socket <path>` — override the fault injection control socket
  (default `/tmp/matter_fault.sock`, see section 8).

> The commissioning window is open for 15 minutes after boot. After that the
> device stops advertising commissionable mode — restart (with a factory
> reset) to re-open it.

## 4. Commission with chip-tool

```bash
./out/linux-x64-chip-tool-clang/chip-tool pairing code 12344321 MT:-24J042C00KA0648G00
```

`12344321` is the node ID you assign (any value). Success looks like:

```
PASE establishment successful
Device commissioning completed with success
```

> `pairing ethernet <ip> <port>` no longer exists in current chip-tool —
> `pairing code <node-id> <payload>` discovers the device via mDNS on the
> network. If discovery fails, wait a few seconds and retry (Docker-interface
> mDNS noise), or check the device is still advertising (`avahi-browse -rt
> _matterc._udp` if avahi-utils is installed).

## 5. Interact with the device (endpoint 1)

```bash
# Read OnOff state
./out/linux-x64-chip-tool-clang/chip-tool onoff read on-off 12344321 1

# Commands
./out/linux-x64-chip-tool-clang/chip-tool onoff on 12344321 1
./out/linux-x64-chip-tool-clang/chip-tool onoff off 12344321 1
./out/linux-x64-chip-tool-clang/chip-tool onoff toggle 12344321 1

# Level control
./out/linux-x64-chip-tool-clang/chip-tool levelcontrol read current-level 12344321 1
./out/linux-x64-chip-tool-clang/chip-tool levelcontrol move-to-level 50 1 0 0 0 12344321 1
```

## 6. Factory reset (re-pairing)

The device only accepts commissioning once per boot.

```bash
pkill -f chip-lighting-app
rm -f /tmp/chip_kvs /tmp/chip_kvs_lighting /tmp/chip_config.ini /tmp/chip_factory.ini /tmp/chip_counters.ini
rm -f /tmp/chip_tool_kvs /tmp/chip_tool_config.ini /tmp/chip_tool_config.alpha.ini   # controller state
./out/linux-x64-light/chip-lighting-app
```

## 7. Troubleshooting

| Symptom | Cause / Fix |
|---------|-------------|
| `chipDie` + `CHIP Error 0x000000C1: Endpoint pool full` | Endpoint pool exhausted by mDNS on many interfaces. Fixed by `examples/common/InetProjectConfig.h` (already wired into both apps). |
| `curl: (22) ... 403` during chip-tool build | `dist.wso2.org` blocks curl's User-Agent. See the workaround in step 2. |
| Pairing times out / "Failed to establish CASE" | Commissioning window expired (15 min) → factory reset + restart. Check the device is listening: `ss -ulnp \| grep 5540`. |
| `Failed to advertise records ... 0x00000046` in device log | mDNS trying to multicast on Docker interfaces. Non-fatal — service still publishes on real interfaces. |
| `Run command failure: ... 0x0000002F` for `pairing ethernet` | Subcommand removed. Use `pairing code <node-id> <payload>`. |

## 8. Fault injection control interface

Unit tests for the fault injector and the JSON handling:

```bash
scripts/run_in_build_env.sh "./scripts/build/build_examples.py --target linux-x64-tests-clang --quiet gen"
scripts/run_in_build_env.sh "ninja -C out/linux-x64-tests-clang examples/common/fault_injection/tests:libFaultInjectionTests.run"
```

The emulator starts a fault injection control socket on
`/tmp/matter_fault.sock` at boot (override with `--fault-socket <path>`).
Startup log line: `FaultSocket: listening on /tmp/matter_fault.sock`.

The socket accepts newline-terminated JSON, one command per connection, and
replies `{"result":"ok"}` or `{"result":"error","detail":"..."}`.

**Quick test with Python:**

```bash
python3 - <<'EOF'
import socket
def send(payload):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect("/tmp/matter_fault.sock")
    s.sendall((payload + "\n").encode())
    print(s.recv(4096).decode().strip())
    s.close()

send('{"action": "configure", "fault": {"type": "data_corruption", "scope": "attribute", "cluster": "0x0006", "attribute": 0, "corrupt_value": true}}')
send('{"action": "configure", "fault": {"type": "timing_delay", "cluster": "0x0008", "delay_ms": 12000}}')
send('{"action": "clear_all"}')
EOF
```

**Actions:**

| Action | Fields | Effect |
|--------|--------|--------|
| `configure` | `fault` object (below) | Adds a fault, or replaces the fault on the same (scope, cluster, attribute/command) |
| `clear` | optional `endpoint`, `cluster`, `attribute` — or `"scope": "command"`, `cluster`, `command` | Removes the fault with exactly that endpoint and path. Without `endpoint`, only the all-endpoints fault is removed. |
| `clear_all` | — | Removes all faults |

At most 16 faults can be active; a 17th `configure` returns an error.

**Fault fields:** `type` (`data_corruption` | `timing_delay` | `command_drop` |
`invalid_status`), `scope` (`attribute` | `command`, default attribute),
`endpoint` (0–65535; omit to match all endpoints), `cluster` (int or
`"0x0006"` string, required, non-zero), `attribute`, `command`, `delay_ms`,
`status` (IM status code, 0–255), `corrupt_value` (bool, int32, float, or
`null`; `25.0` is a float, `25` an int).

A fault for one endpoint and a fault for all endpoints on the same path can
both be active; the endpoint-specific one is used for that endpoint.

Example — report null for MeasuredValue of a temperature sensor on endpoint 2
(Temperature Measurement cluster 0x0402, attribute 0x0000):

```json
{"action": "configure", "fault": {"type": "data_corruption", "endpoint": 2, "cluster": "0x0402", "attribute": 0, "corrupt_value": null}}
```

IDs and numbers accept JSON integers or decimal/`"0x..."` strings. Required
per type: `data_corruption` → `corrupt_value`, `timing_delay` →
`delay_ms > 0`, `invalid_status` → `status`.

**Error details:** `invalid json`, `unknown action`, `invalid fault`
(bad or missing field), `invalid clear`, `fault table full`. The device log
has the CHIP error for a failed `configure`.

Successful configs log on the device, e.g.
`FaultInjector: configured data_corruption fault on cluster 0x00000006`.

> **Status note:** configuring/clearing works end to end, but the interception
> hooks (Step 2 in fault_injection_orientation.md) are not implemented yet — faults
> do not yet affect attribute reads, command handling, or timing.
>
> The server handles one connection at a time. A client must send its line
> within 1 second, otherwise it gets `invalid json` and is disconnected.
