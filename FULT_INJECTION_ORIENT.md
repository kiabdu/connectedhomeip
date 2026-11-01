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
| FaultInjector | `examples/common/fault_injection/` | Core fault logic, JSON parsing, Unix socket |
| Attribute Hook | `src/app/` | Intercept attribute reads/writes |
| Command Hook | `src/app/` | Intercept command execution |
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

**Step 1: Fault Injection Control Interface** (Current Phase)
- Create Unix Domain Socket server
- JSON schema for fault commands
- Runtime fault configuration

**Step 2: Interception Points**
- Attribute Access Interface hook
- Command Handler hook
- Exchange Layer hook

**Step 3: Integration**
- Wire into `chip-lighting-app`
- Configure clusters/attributes to hook

**Step 4: Python Test Harness**
- Client library for fault injection
- Automated test sequences
- Metrics collection

### Key Files to Modify

```
src/app/
├── AttributeAccessInterface.h       # Add fault hook
├── InteractionModelEngine.cpp       # Add command hook
└── ...

examples/
└── common/
    └── fault_injection/              # NEW: FaultInjector class
        ├── FaultInjector.h
        ├── FaultInjector.cpp
        └── FaultSocket.h

examples/lighting-app/linux/
└── main.cpp                          # Initialize FaultInjector
```

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

### Output Format

Fault events are logged as JSON:
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

### Current Phase: Step 1 (Start Here)

**Objective**: Create the basic fault injection control interface.

**Completed**:
- [x] Created `examples/common/fault_injection/` directory
- [x] Created `FaultInjector.h` with:
  - `FaultType` enum (kDataCorruption, kTimingDelay, kCommandDrop, kInvalidStatus)
  - `FaultConfig` struct for configuration
  - `FaultInjectorDelegate` interface for notifications
  - `FaultInjector` singleton class
- [x] Created `FaultInjector.cpp` with:
  - JSON parsing using jsoncpp
  - Fault configuration storage
  - Matching logic for attribute paths
  - Data corruption, timing delay, and status code hooks

**Files Created**:
```
examples/common/fault_injection/
├── BUILD.gn              # Build configuration
├── FaultInjector.h       # Header with class definitions
├── FaultInjector.cpp     # Implementation with JSON parsing
└── FaultSocket.h         # (next) Unix domain socket server
```

**Next Task**: Create `FaultSocket.h` - Unix Domain Socket server for JSON command interface

**Constraints**:
- Use C++17
- Follow CHIP SDK coding style (CHIP_ERROR, VerifyOrReturnError, etc.)
- Use POSIX Unix domain sockets
- Parse JSON commands and call `FaultInjector::ConfigureFault()`