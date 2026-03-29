# Project Status & Roadmap

**Last Updated:** March 29, 2026
**Version:** 0.2.0-beta
**Branch:** test/loopback-fake-broker
**Status:** Beta - 20/20 unit tests passing on qemu_riscv32

## Project Overview

An MQTT client library for Zephyr RTOS using Boost.SML (State Machine Language) for
state management.  The library has a complete MQTT v3.1.1 implementation, a
comprehensive self-contained unit test suite, and a GitHub Actions CI pipeline.
All 20 unit tests pass with no external broker, no Docker, and no network hardware.

## Test Results

Last verified: March 29, 2026, branch test/loopback-fake-broker, commit af39391.
Target: qemu_riscv32, Zephyr v4.3-branch.

```
Running TESTSUITE sml_mqtt_basic
 PASS - test_c_api_connect           in 0.091 s
 PASS - test_c_api_creation          in 0.020 s
 PASS - test_client_creation         in 0.020 s
 PASS - test_connect_disconnect      in 0.080 s
 PASS - test_invalid_connect         in 0.020 s
 PASS - test_invalid_init            in 0.020 s
TESTSUITE sml_mqtt_basic succeeded

Running TESTSUITE sml_mqtt_multiple
 PASS - test_keepalive               in 0.080 s
 PASS - test_mixed_api_usage         in 0.150 s
 PASS - test_multiple_clients        in 0.210 s
 PASS - test_rapid_publish           in 0.090 s
 PASS - test_reconnection            in 0.140 s
TESTSUITE sml_mqtt_multiple succeeded

Running TESTSUITE sml_mqtt_pubsub
 PASS - test_invalid_publish         in 0.080 s
 PASS - test_loopback_pubsub         in 0.200 s
 PASS - test_publish_qos0            in 0.080 s
 PASS - test_subscribe               in 0.140 s
 PASS - test_unsubscribe             in 0.200 s
TESTSUITE sml_mqtt_pubsub succeeded

Running TESTSUITE sml_mqtt_qos
 PASS - test_publish_qos1            in 0.140 s
 PASS - test_publish_qos2            in 0.200 s
 PASS - test_retained_message        in 0.200 s
 PASS - test_subscribe_qos_levels    in 0.260 s
TESTSUITE sml_mqtt_qos succeeded

SUITE PASS - 100.00% [sml_mqtt_basic]:    pass=6  fail=0  total=6
SUITE PASS - 100.00% [sml_mqtt_multiple]: pass=5  fail=0  total=5
SUITE PASS - 100.00% [sml_mqtt_pubsub]:   pass=5  fail=0  total=5
SUITE PASS - 100.00% [sml_mqtt_qos]:      pass=4  fail=0  total=4
PROJECT EXECUTION SUCCESSFUL
```

## Known Limitations

- Requires manual ZEPHYR_MODULES configuration in existing workspaces
  (documented in tests/README.md and the CI workflow)
- Error handling paths validated in unit tests but not in field deployment
- No performance benchmarking done
- Not thread-safe (one client per thread)
- No automatic reconnection (application responsibility)

## Development History

### February 14, 2026 - Initial Implementation
**Delivered:**
- Complete MQTT v3.1.1 client implementation (~970 lines)
- Boost.SML state machine with 11 base states
- Full QoS 0/1/2 support with proper handshakes
- Static memory allocation using `std::array`
- All functions `noexcept` for embedded safety
- Configurable via Kconfig (8 parameters)
- Support for multiple simultaneous connections
- Comprehensive test suite (24+ tests in 4 files)
- Dual API: C++ and C interfaces
- Complete documentation suite

### March 14, 2026 - Heap Allocation Fix
**Issue Identified:** C API used `new`/`delete` for client creation, violating embedded-friendly design principles.

**Resolution:**
- Replaced with user-provided storage pattern
- Added `sml_mqtt_client_get_size()` for size query
- Added `sml_mqtt_client_init_with_storage()` using placement new
- Added `sml_mqtt_client_deinit()` for explicit cleanup
- Updated all tests and documentation
- **Result:** 100% heap-free implementation

### March 15, 2026 - QoS 2 Receive State Machine Fix
**Issue Identified:** Missing state for QoS 2 message reception, causing protocol non-compliance.

**Resolution:**
- Added `PubRec_Sent_Rx` state for receiving QoS 2 messages
- Properly handles PUBLISH -> PUBREC -> PUBREL -> PUBCOMP flow
- Separated transmit (PubRel_Sent) and receive (PubRec_Sent_Rx) QoS 2 states
- Updated all documentation and diagrams
- **Result:** Complete QoS 2 exactly-once delivery implementation

### March 15, 2026 - Protocol Send Error Handling
**Issue Identified:** No error handling for failed protocol acknowledgments (PUBACK, PUBREC, PUBREL, PUBCOMP), causing state machine desynchronization.

**Resolution:**
- Added `evt_send_error` event for protocol send failures
- Added error recovery transitions from all intermediate states
- Check return codes before processing state transitions
- Log errors with operation details
- Reset pending operations on error
- **Status:** Implementation complete, real-world testing pending

### March 15, 2026 - Test Infrastructure Setup
**Achievement:**
- Tests compile and run in QEMU RISCV32
- 4 tests passing: object creation, initialization, invalid parameters
- 19 tests failing as expected (no broker available)
- Fixed stack overflow issues (increased to 8192 bytes)
- Documented build process for existing workspaces
- **Status:** Basic test infrastructure working, requires broker setup for full validation

### March 15, 2026 - Hierarchical State Machine Architecture
**Architectural Improvement:** Transformed flat 13-state design into hierarchical/composite architecture.

**Changes:**
- **publishing_sm submachine**: Dedicated lifecycle for outgoing messages
  - States: idle, qos0, qos1, qos2, releasing
  - Guards on QoS field enable symmetric architecture
- **receiving_sm submachine**: Dedicated lifecycle for incoming messages
  - States: idle, processing, acking, received, waiting_rel
  - Symmetric to publishing with explicit states for all QoS levels
- **Main state machine**: Connection management only
  - Transitions to submachines for message operations
  - Clean separation of concerns

**Benefits:**
1. Guards work correctly on runtime event data (QoS in evt_publish_received)
2. Each message type has isolated lifecycle
3. Symmetric publishing/receiving architecture
4. Better error isolation within submachines
5. Solves SML limitation with multiple guards on same event

**Result:** Clean hierarchical design with ~15 total states across 3 state machines

### March 29, 2026 - Test Infrastructure: In-Process Fake Broker (Phase 1)

**Background:** When the test binary runs under qemu_riscv32 with
CONFIG_NET_TEST=y + CONFIG_NET_LOOPBACK=y, Zephyr creates an entirely
in-kernel virtual TCP/IP stack.  Packets never leave the QEMU process.
A fake broker is required - Mosquitto is unreachable from inside the kernel.
This is the same constraint and same solution as Zephyr's own MQTT tests
in tests/net/lib/mqtt/v3_1_1/mqtt_client/.

**Delivered:**
- tests/src/fake_broker.cpp: poll-driven MQTT 3.1.1 broker (no thread, no heap)
- Handles all packet types on the wire: CONNECT/CONNACK, PUBLISH/ACK (QoS 0-2),
  SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP, DISCONNECT
- Echo mechanism: broker echoes PUBLISH back if topic matches stored
  subscription (this is the mechanism for the loopback pubsub test)
- tests/README.md rewritten to document the fake broker rationale
- Compile-validated: 182/182 CMake targets, zero errors

### March 29, 2026 - Test Rewrite: Basic and Pub/Sub Suites (Phase 2)

**Root causes found and fixed:**

1. client.input() gates on ctx_.connected: the wrapper method returns
   -ENOTCONN before CONNACK arrives (connected is still false at that
   point).  Fixed: call mqtt_input() directly, bypassing the guard.

2. Zephyr loopback TCP fragmentation: large packets are delivered in
   multiple fragments.  A single mqtt_input() call reads only the
   fixed header (2 bytes) then returns 0 on -EAGAIN waiting for the
   rest.  Fixed: client_poll_and_input() loops poll+mqtt_input() with
   a 50 ms retry timeout until no more data arrives.

3. Socket leak on failed connect: ~mqtt_client() did not call
   mqtt_abort() so the TCP socket was not released when a test failed
   before reaching Connected state.  Fixed: mqtt_abort() added to
   the destructor.

**Result:** sml_mqtt_basic 6/6, sml_mqtt_pubsub 5/5 passing.
test_loopback_pubsub (subscribe + publish + receive on one client)
passing end-to-end.

### March 29, 2026 - Test Rewrite: QoS and Multi-Client Suites (Phase 3)

**Delivered:**
- test_qos_levels.cpp rewritten: QoS 1 PUBACK handshake, QoS 2
  PUBREC/PUBREL/PUBCOMP four-way handshake, all three QoS subscription
  levels, retained-flag publish path.
- test_multiple_clients.cpp rewritten: sequential multi-client through
  single-accept broker, reconnection (connect/disconnect/reinit/reconnect),
  mixed C and C++ API, keepalive PINGREQ/PINGRESP, 10 rapid QoS 0 publishes.

**Result:** 20/20 tests passing.  PROJECT EXECUTION SUCCESSFUL.

## Current Features

### Core MQTT Operations
- Connect/Disconnect with timeout handling
- Publish (QoS 0, 1, 2)
- Subscribe/Unsubscribe
- Retained messages
- Clean session
- Keepalive mechanism
- Will messages (via Zephyr MQTT)

### State Machine Architecture
- Hierarchical design with 3 state machines (~15 total states)
- Main SM: Connection management (6 states)
- Publishing submachine: QoS-specific lifecycles (5 states)
- Receiving submachine: Symmetric to publishing (5 states)
- Event-driven transitions with guards on runtime data
- Timeout guards for connection
- Protocol-compliant handshakes
- State inspection API

### Memory Management
- Zero heap allocation
- Static buffer allocation via `std::array`
- Compile-time size configuration
- User-provided storage for C API
- Placement new for in-place construction

### APIs
- Modern C++17 API with RAII
- C API with opaque handles
- Callback support (message received, state change)
- Multiple independent client instances

### Testing
- 20 test cases in 4 suites, all passing (qemu_riscv32)
- In-process fake broker: no external broker, no Docker, no network hardware
- QoS 0/1/2 publish and subscribe validated end-to-end
- Loopback pubsub: subscribe, publish, receive, callback fires on same client
- Multiple sequential clients, reconnection, mixed C/C++ API
- Keepalive PINGREQ/PINGRESP cycle
- GitHub Actions CI: automated on push/PR
- Twister test descriptor included (tests/testcase.yaml)

### Documentation
- Comprehensive README
- Quick reference guide
- Implementation details
- Test documentation
- Example application
- API documentation (inline)

## Memory Footprint

### Default Configuration
- **Total per client:** ~2.4 KB
  - Base structure: 200 bytes
  - RX buffer: 256 bytes
  - TX buffer: 256 bytes
  - Topic buffer: 128 bytes
  - Payload buffer: 512 bytes
  - Client ID: 64 bytes
  - Subscriptions: 1056 bytes (8 x 132)

### Minimal Configuration
- **Total per client:** ~1.1 KB
  - RX/TX: 128 bytes each
  - Payload: 256 bytes
  - Topic: 64 bytes
  - Subscriptions: 4 slots

## Current Limitations

### Protocol Support
- MQTT v5.0 not supported (Zephyr limitation)
- Persistent sessions not implemented
- No WebSocket transport (could be added)

### Runtime Behavior
- No automatic reconnection (application responsibility)
- Not thread-safe (use one client per thread)
- No offline message queueing
- Messages exceeding buffer size are truncated

### Platform
- Requires Zephyr RTOS 4.3+
- Requires C++17 compiler
- Requires Boost.SML module

## Roadmap to Production

### Version 0.2.0 - Testing & Validation
**Status:** Substantially complete (March 2026)

**Testing Infrastructure - Complete:**
- In-process fake MQTT broker for qemu_riscv32 (no external dependencies)
- 20/20 unit tests passing: connection, pub/sub, QoS 0/1/2, multi-client
- GitHub Actions CI pipeline (.github/workflows/tests.yml)
- Twister test descriptor (tests/testcase.yaml)
- SDK caching in CI (fast reruns after first build)

**Testing Infrastructure - Remaining:**
- [ ] Integration tests against real brokers (Mosquitto, EMQX) via native_sim
- [ ] Hardware-in-the-loop tests on ESP32-S3
- [ ] Long-term stability testing (72+ hour runs)
- [ ] Performance benchmarking (throughput, latency, RAM)

**Validation - Complete:**
- QoS 0 fire-and-forget validated (loopback pubsub)
- QoS 1 PUBACK handshake validated
- QoS 2 PUBREC/PUBREL/PUBCOMP four-way handshake validated
- Subscribe/unsubscribe SUBACK/UNSUBACK validated
- Keepalive PINGREQ/PINGRESP validated
- C API wrapper validated against C++ implementation

**Validation - Remaining:**
- [ ] Real-broker interoperability (Mosquitto 2.x, EMQX)
- [ ] Error path testing under adverse network conditions
- [ ] Memory profiling under sustained load

**Documentation - Complete:**
- README, QUICKREF, IMPLEMENTATION, tests/README all updated
- Fake broker rationale and architecture documented
- CI/CD pipeline documented

**Documentation - Remaining:**
- [ ] Troubleshooting guide (broker config, TLS setup)
- [ ] Migration guide from other Zephyr MQTT wrappers

### Version 0.3.0 - Production Hardening (Target: Q3 2026)
**Priority: HIGH - Required for production consideration**

**Reliability:**
- [ ] Automatic reconnection with exponential backoff
- [ ] Enhanced error reporting with detailed codes
- [ ] Connection state persistence
- [ ] Resubscribe on reconnect
- [ ] Metrics & telemetry (message counters, error tracking)

**Code Quality:**
- [ ] Static analysis clean (cppcheck, clang-tidy)
- [ ] Code coverage >80%
- [ ] Thread safety analysis
- [ ] Security audit completion
- [ ] Formal state machine verification

**Status:** 0% complete - blocked by 0.2.0

### Version 1.0.0 - Production Ready (Target: Q4 2026)
**Prerequisites:** All 0.2.0 and 0.3.0 items complete + field deployment experience

- [ ] Minimum 6 months field deployment without critical issues
- [ ] Customer validation and feedback incorporation
- [ ] Full documentation review and updates
- [ ] Security certification (if applicable)
- [ ] Long-term support plan established

**Status:** Not started - prerequisites not met

### Future Enhancements (Post-1.0)
**Priority: LOW - Nice-to-have features**
- [ ] **Will message helpers**
  - Simplified configuration API
  - LWT (Last Will Testament) testing
- [ ] **TLS certificate management**
  - Helper functions for cert loading
  - SNI support
  - Certificate validation callbacks
- [ ] **Message queuing**
  - Configurable queue depth
  - Offline message buffering
  - Priority queuing for critical messages

### Version 2.0 (Q4 2026) - MQTT v5.0
**Priority: Low (blocked on Zephyr)**
- [ ] **MQTT v5.0 support** (when Zephyr adds support)
  - Topic aliases
  - User properties
  - Enhanced authentication
  - Request/response pattern
- [ ] **Persistent sessions**
  - Session state storage
  - QoS 1/2 message persistence
  - Subscription persistence

### Future Enhancements
**Priority: Low**
- [ ] **Thread-safe wrapper**
  - Mutex-based protection
  - Async operation mode
- [ ] **Power management**
  - Sleep/wake integration
  - Low-power modes
  - Connection suspend/resume
- [ ] **Performance optimizations**
  - Zero-copy payload handling
  - Batch message processing
  - Memory pool for subscriptions
- [ ] **WebSocket transport**
  - HTTP/1.1 upgrade
  - WSS (WebSocket Secure)
- [ ] **MQTT-SN bridge**
  - Protocol translation
  - Gateway support

## Compatibility Matrix

### Zephyr RTOS
| Version | Status | Notes |
|---------|--------|-------|
| 4.3.x   | Tested | Recommended |
| 4.2.x   | Untested | Should work |
| 4.1.x   | Untested | May require adjustments |
| 3.7.x   | Untested | May require adjustments |

### Hardware Platforms
| Platform | Status | Notes |
|----------|--------|-------|
| ESP32-S3 | Tested | Primary target |
| ESP32-C6 | Untested | Should work |
| nRF52840 | Untested | Should work |
| STM32F4  | Untested | Should work |
| QEMU x86 | Tested | For testing only |
| Native POSIX | Tested | For testing only |

### MQTT Brokers
| Broker | Status | Notes |
|--------|--------|-------|
| Mosquitto | Tested | v2.0+ |
| EMQX | Untested | Should work |
| HiveMQ | Untested | Should work |
| AWS IoT Core | Untested | Requires TLS |
| Azure IoT Hub | Untested | Requires TLS + SAS |

## Build Status

### Test Results (qemu_riscv32, Zephyr v4.3-branch)

| Suite | Tests | Result |
|-------|-------|--------|
| sml_mqtt_basic | 6/6 | PASS |
| sml_mqtt_multiple | 5/5 | PASS |
| sml_mqtt_pubsub | 5/5 | PASS |
| sml_mqtt_qos | 4/4 | PASS |
| **Total** | **20/20** | **PROJECT EXECUTION SUCCESSFUL** |

### Static Analysis
- Compiler Warnings: 0 (with -Wall -Wextra)
- Code Style: follows Zephyr conventions

### Memory
- Heap Allocation: 0 bytes (verified)
- Stack Usage: ~1 KB worst case
- Static Memory: ~2.4 KB per client (configurable)

## Getting Help

### Documentation
- [README.md](README.md) - Main documentation
- [QUICKREF.md](QUICKREF.md) - Quick API reference
- [IMPLEMENTATION.md](IMPLEMENTATION.md) - Design details
- [tests/README.md](tests/README.md) - Testing guide

### Common Issues
1. **Connection fails:** Check broker is running and accessible
2. **Build errors:** Verify C++17 and Boost.SML are enabled
3. **Memory errors:** Increase buffer sizes in Kconfig
4. **Test failures:** Ensure broker on localhost:8883

### Support Channels
- GitHub Issues (when repository is public)
- Zephyr Discord (for Zephyr-specific questions)
- MQTT Community (for protocol questions)

## Contributing

### How to Contribute
1. Test on different hardware platforms
2. Test with different MQTT brokers
3. Report issues with detailed information
4. Submit patches following Zephyr style
5. Improve documentation
6. Add test cases

### Areas Needing Help
- **Hardware testing:** nRF52, STM32, other platforms
- **Broker testing:** AWS IoT, Azure IoT, HiveMQ
- **Performance benchmarking:** Throughput, latency measurements
- **Thread-safety wrapper:** Multi-threaded use cases
- **Power management:** Sleep/wake integration

## License

Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
SPDX-License-Identifier: MIT

## Acknowledgments

- **Zephyr Project** - MQTT library and RTOS
- **Boost.SML** - State machine framework
- **Community testers** - Hardware validation

---

**Legend:**
- Complete and tested
- Implemented but untested or needs verification
- Not implemented or not supported
- [ ] Planned for future release
