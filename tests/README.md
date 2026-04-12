# SML MQTT Client - Test Suite

Two complementary test configurations:

- **Unit tests** (`qemu_riscv32`, `esp32s3_devkitc`): in-process fake MQTT broker.
  No external broker, no network hardware, no Docker required.
- **Integration tests** (`native_sim`): real Mosquitto broker via ETH_NATIVE_TAP +
  optional NAT routing.  Runs on the build machine as a native Linux process.

## Test approach

### Why a fake broker, not Mosquitto?

When the test binary runs under `qemu_riscv32` with
`CONFIG_NET_TEST=y` + `CONFIG_NET_LOOPBACK=y`, Zephyr creates an
entirely in-kernel virtual TCP/IP stack.  Packets never leave the QEMU
process.  `127.0.0.1` resolves to the Zephyr loopback interface, not
the host OS loopback where Mosquitto would be listening.  There is no
bridge and no path to the host network.

This is the same constraint that applies to all Zephyr networking unit
tests - Zephyr's own MQTT tests in `tests/net/lib/mqtt/` use the
identical fake broker pattern for exactly this reason.

### What we are actually testing

sml-mqtt-cli is a consuming application built on top of Zephyr's MQTT
library.  Protocol correctness and TCP reliability are Zephyr's
responsibility, already tested in `tests/net/lib/mqtt/`.

Our tests validate:

- The SML state machine transitions fire in the correct order for each
  MQTT operation (connect, subscribe, publish, receive, disconnect).
- The `publish_received_cb` is called with the correct topic and
  payload when the broker delivers a message.
- Invalid parameters and error paths are handled without crashing.
- The C API wrapper correctly delegates to the C++ implementation.

### How the fake broker works

`src/fake_broker.cpp` opens a TCP server socket on `127.0.0.1:1883`
within the Zephyr kernel.  It is poll-driven - no broker thread.  Each
test function interleaves:

```
fake_broker_process(FAKE_MQTT_PKT_*)   <- broker receives + responds
client_poll_and_input(client)           <- client processes the response
```

This is deterministic: each call processes exactly one MQTT packet
round-trip.

The key mechanism for the loopback publish-subscribe test: when the
broker receives a PUBLISH on a topic that matches the stored
subscription, it echoes the full PUBLISH packet back to the same
connection.  The client's `MQTT_EVT_PUBLISH` handler then fires,
driving the SML receiving submachine and invoking the user callback.

The fake broker speaks real MQTT 3.1.1 wire format for all packet types:

- CONNECT -> CONNACK (return code 0)
- SUBSCRIBE -> stores topic, sends SUBACK (granted QoS = requested)
- UNSUBSCRIBE -> clears topic, sends UNSUBACK
- PUBLISH -> ACK per QoS level; echoes back if topic matches subscription
- PUBREL -> PUBCOMP
- PINGREQ -> PINGRESP
- DISCONNECT -> closes client socket

### native_sim integration tests (optional)

`native_sim` compiles Zephyr as a Linux executable and bridges to the host
network via the ETH_NATIVE_TAP driver, making a real broker reachable.  This
configuration is used for the `sml_mqtt_integration` suite.

| | fake broker (qemu_riscv32) | real broker (native_sim) |
|---|---|---|
| External dependency | None | Mosquitto must be reachable |
| CI setup | Zero | privileged runner needed (ip tuntap) |
| Portable to real hardware targets | Yes | No |
| Timing determinism | Yes | Network jitter |
| twister-compatible | All boards | native_sim only |

Both approaches are maintained: the fake broker matches Zephyr's own CI and
validates state machine logic without any external dependencies; the
integration suite validates real broker interoperability (QoS 0/1/2 confirmed
with Mosquitto, April 3, 2026).

---

## Running Tests

### Recommended: west twister (matches CI)

From the workspace root (`z-workspaces/`):

```bash
source .venv/bin/activate

west twister \
  -T sml-mqtt-cli/tests \
  -p qemu_riscv32 \
  --inline-logs \
  -x "ZEPHYR_MODULES=$PWD/boost-sml;$PWD/sml-mqtt-cli"
```

twister exits 0 on all-pass, non-zero on any failure.  The full JUnit
XML report is written to `twister-out/twister.xml`.

The `-x` flag (`--extra-args`) injects CMake cache entries prefixed with
`-D`, so `-x ZEPHYR_MODULES=...` is equivalent to `-DZEPHYR_MODULES=...`
in a direct cmake invocation.  Note: `--cmake-args` is NOT a valid twister
flag and is silently ignored.

### Alternative: direct west build (faster iteration)

```bash
source .venv/bin/activate

west build -p always -s sml-mqtt-cli/tests -b qemu_riscv32 \
  -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli"

timeout 120 west build -t run
```

### Standalone (fresh checkout)

```bash
git clone https://github.com/d-o/sml-mqtt-cli.git
cd sml-mqtt-cli/tests
west init -l .
west update
west build -b qemu_riscv32
timeout 120 west build -t run
```

### On real hardware: ESP32-S3

The test suite runs unmodified on `esp32s3_devkitc` (and likely any other
ESP32-S3 board).  Board-specific overlay files in `tests/boards/` handle
the two differences from the QEMU target:

- **Stack size**: Xtensa LX7 has larger ABI frames than RISC-V; the TCP
  work queue stack is bumped from 1 KB to 4 KB.
- **Console redirect**: The devkit board file routes `zephyr,console` to
  `uart0` (GPIO43/44 → USB-UART bridge → `ttyUSB0`).  The overlay
  re-enables the built-in USB-JTAG CDC-ACM device and points the console
  at it, so output appears on `ttyACM0` over the same cable used to flash.

```bash
# From the workspace root (z-workspaces/)
source .venv/bin/activate

# Build (hal_espressif must be in the module list)
west build -p always -s sml-mqtt-cli/tests -b esp32s3_devkitc/esp32s3/procpu \
  -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli;$PWD/modules/hal/espressif"

# Flash
west flash

# Monitor (test output appears on the USB-JTAG ACM port)
west espressif monitor --port /dev/ttyACM0
```

Expected output (22/22 pass, ~2 s total):

```
*** Booting Zephyr OS build v4.3.0 ***
Running TESTSUITE sml_mqtt_basic
 PASS - test_c_api_connect        in 0.072 seconds
 PASS - test_c_api_creation       in 0.011 seconds
 PASS - test_client_creation      in 0.011 seconds
 PASS - test_connect_disconnect   in 0.072 seconds
 PASS - test_invalid_connect      in 0.011 seconds
 PASS - test_invalid_init         in 0.011 seconds
TESTSUITE sml_mqtt_basic succeeded
...
SUITE PASS - 100.00% [sml_mqtt_basic]:    pass=6  fail=0  skip=0  total=6
SUITE PASS - 100.00% [sml_mqtt_multiple]: pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_pubsub]:   pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_qos]:      pass=6  fail=0  skip=0  total=6
PROJECT EXECUTION SUCCESSFUL
```

Board-specific files:

| File | Purpose |
|------|---------|
| `boards/esp32s3_devkitc_procpu.conf` | `NET_TCP_WORKQ_STACK_SIZE=4096`, `SERIAL_ESP32_USB=y` |
| `boards/esp32s3_devkitc_procpu.overlay` | Re-enable `&usb_serial`, redirect `zephyr,console` to it |

Other ESP32-S3 boards should work by adding equivalent `boards/<board>.conf`
and `boards/<board>.overlay` files following the same pattern.

---

### Integration tests: native_sim against real Mosquitto

Requires a Mosquitto broker reachable from the build machine.  The
`run_integration_tests.sh` script handles all TAP/routing setup.

```bash
# Build (broker address baked in at compile time)
west build -p always -s sml-mqtt-cli/tests -b native_sim \
  -- -DSML_MQTT_TEST_BROKER_HOST=<broker-ip>

# LAN/remote broker (NAT via host uplink)
sml-mqtt-cli/tests/scripts/run_integration_tests.sh \
  --broker-host <broker-ip> --enable-routing --no-local-broker

# Local broker on build machine
mosquitto -c sml-mqtt-cli/tests/scripts/mosquitto_integration.conf &
sml-mqtt-cli/tests/scripts/run_integration_tests.sh
```

The script:
1. Grants `cap_net_admin` to the binary via `setcap` (binary runs as the
   current user, not root)
2. Launches the binary in the background; it creates the `zeth` TAP interface
3. Polls until `zeth` appears then configures the host side: `192.0.2.1/24`
4. Optionally enables `ip_forward` + `iptables MASQUERADE` for LAN/remote brokers
5. Waits for the binary to exit and propagates the exit code
6. Cleans up all iptables rules and the TAP interface on exit

Expected output (7/7 pass, ~12 s total):

```
*** Booting Zephyr OS build v4.3.0 ***
Running TESTSUITE sml_mqtt_integration
 PASS - test_integ_connect_disconnect     in 2.560 seconds
 PASS - test_integ_keepalive              in 3.520 seconds
 PASS - test_integ_publish_qos0          in 0.550 seconds
 PASS - test_integ_publish_qos1          in 1.370 seconds
 PASS - test_integ_publish_qos2          in 2.380 seconds
 PASS - test_integ_subscribe_qos_levels  in 0.700 seconds
 PASS - test_integ_subscribe_receive     in 0.900 seconds
TESTSUITE sml_mqtt_integration succeeded
SUITE PASS - 100.00% [sml_mqtt_integration]: pass=7 fail=0 skip=0 total=7
PROJECT EXECUTION SUCCESSFUL
```

---

### Code coverage

Code coverage via gcov instrumentation is **not currently supported** on
`qemu_riscv32`.  The gcov call-stack overhead overflows several Zephyr
networking thread stacks (`tcp_work`, `rx_q`) that are fixed at 1-2 KB
by the kernel configuration.  Bumping individual stacks turns into
whack-a-mole because the instrumentation touches the entire network
subsystem, which we do not own.

The correct solution is `native_sim` (unlimited stack, same Zephyr net
subsystem).  The integration test suite already uses `native_sim` with
`CONFIG_ETH_NATIVE_TAP`; adding gcov instrumentation there is feasible
but has not been done yet.  See STATUS.md for roadmap.

---

## Test suites

### `sml_mqtt_basic` (test_basic_connection.cpp)

| Test | What it validates |
|------|------------------|
| `test_client_creation` | Object construction, initial Disconnected state |
| `test_invalid_init` | NULL and over-length client ID rejected |
| `test_connect_disconnect` | Full connect/disconnect round-trip via fake broker |
| `test_invalid_connect` | NULL hostname rejected before network call |
| `test_c_api_creation` | C API placement-new + deinit, no heap |
| `test_c_api_connect` | C API connect/disconnect via fake broker |

### `sml_mqtt_pubsub` (test_publish_subscribe.cpp)

| Test | What it validates |
|------|------------------|
| `test_publish_qos0` | QoS 0 publish; SM returns to Connected after send |
| `test_subscribe` | Subscribe; SUBACK drives SM back to Connected; broker stores topic |
| `test_unsubscribe` | Unsubscribe; UNSUBACK received; broker clears stored topic |
| `test_loopback_pubsub` | Subscribe + publish on one client; broker echoes PUBLISH back; `publish_received_cb` fires with correct topic and payload |
| `test_invalid_publish` | NULL topic/payload rejected while connected; -EINVAL returned |

### `sml_mqtt_qos` (test_qos_levels.cpp)

| Test | What it validates |
|------|------------------|
| `test_publish_qos1` | QoS 1 full handshake: PUBLISH -> PUBACK; SM returns to Connected |
| `test_publish_qos2` | QoS 2 full handshake: PUBLISH -> PUBREC -> PUBREL -> PUBCOMP |
| `test_publish_qos1_then_subscribe` | **Regression**: after QoS 1 publish, SM must return to Connected so a subsequent subscribe is not silently dropped |
| `test_publish_qos2_then_subscribe` | **Regression**: same for QoS 2 four-way handshake |
| `test_subscribe_qos_levels` | Subscribe at QoS 0, 1, and 2 in sequence; SUBACK after each |
| `test_retained_message` | retain=1 flag set on publish; subscribe on second client succeeds |

The two regression tests specifically guard against a bug where the Boost.SML composite
state machine outer SM became permanently stuck inside `publishing_sm` after any publish.
See commit d6e1c53 and the comment block at the top of `test_qos_levels.cpp` for details.

### `sml_mqtt_multiple` (test_multiple_clients.cpp)

| Test | What it validates |
|------|------------------|
| `test_multiple_clients` | Three clients connect, publish, and disconnect sequentially through the single-accept broker |
| `test_reconnection` | Connect, disconnect, reinit with new client ID, reconnect and publish |
| `test_mixed_api_usage` | C++ client followed by C API client on same broker socket |
| `test_keepalive` | mqtt_live() called directly; PINGREQ/PINGRESP processed if keepalive timer has elapsed |
| `test_rapid_publish` | Ten QoS 0 messages sent back-to-back; all ten PASS |

### `sml_mqtt_integration` (test_integration_broker.cpp, native_sim only)

| Test | What it validates |
|------|------------------|
| `test_integ_connect_disconnect` | Full TCP + MQTT CONNECT/CONNACK/DISCONNECT with real Mosquitto |
| `test_integ_publish_qos0` | QoS 0 fire-and-forget; client stays connected; real broker receives |
| `test_integ_publish_qos1` | QoS 1 PUBLISH -> real PUBACK from Mosquitto; SM returns to Connected |
| `test_integ_publish_qos2` | QoS 2 four-way handshake: PUBREC -> PUBREL -> PUBCOMP via real broker |
| `test_integ_subscribe_receive` | Two clients; publisher sends, Mosquitto routes to subscriber; callback fires with correct topic+payload |
| `test_integ_subscribe_qos_levels` | Subscribe at QoS 0, 1, 2 in sequence; real SUBACK after each |
| `test_integ_keepalive` | mqtt_live() keeps session alive over 3 s; no unexpected disconnect |

---

## Last test run output

### Unit tests (qemu_riscv32)

Verified April 7, 2026, branch 4-test-with-external-broker, commit d6e1c53,
target qemu_riscv32, Zephyr v4.3.0.

```
Running TESTSUITE sml_mqtt_basic
 PASS - test_c_api_connect        in 0.091 seconds
 PASS - test_c_api_creation       in 0.020 seconds
 PASS - test_client_creation      in 0.020 seconds
 PASS - test_connect_disconnect   in 0.080 seconds
 PASS - test_invalid_connect      in 0.020 seconds
 PASS - test_invalid_init         in 0.020 seconds
TESTSUITE sml_mqtt_basic succeeded
Running TESTSUITE sml_mqtt_multiple
 PASS - test_keepalive            in 0.080 seconds
 PASS - test_mixed_api_usage      in 0.150 seconds
 PASS - test_multiple_clients     in 0.210 seconds
 PASS - test_rapid_publish        in 0.090 seconds
 PASS - test_reconnection         in 0.140 seconds
TESTSUITE sml_mqtt_multiple succeeded
Running TESTSUITE sml_mqtt_pubsub
 PASS - test_invalid_publish      in 0.080 seconds
 PASS - test_loopback_pubsub      in 0.200 seconds
 PASS - test_publish_qos0         in 0.080 seconds
 PASS - test_subscribe            in 0.140 seconds
 PASS - test_unsubscribe          in 0.200 seconds
TESTSUITE sml_mqtt_pubsub succeeded
Running TESTSUITE sml_mqtt_qos
 PASS - test_publish_qos1                  in 0.140 seconds
 PASS - test_publish_qos1_then_subscribe   in 0.200 seconds
 PASS - test_publish_qos2                  in 0.200 seconds
 PASS - test_publish_qos2_then_subscribe   in 0.280 seconds
 PASS - test_retained_message              in 0.200 seconds
 PASS - test_subscribe_qos_levels          in 0.260 seconds
TESTSUITE sml_mqtt_qos succeeded
------ TESTSUITE SUMMARY START ------
SUITE PASS - 100.00% [sml_mqtt_basic]:    pass=6  fail=0  skip=0  total=6
SUITE PASS - 100.00% [sml_mqtt_multiple]: pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_pubsub]:   pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_qos]:      pass=6  fail=0  skip=0  total=6
------ TESTSUITE SUMMARY END ------
PROJECT EXECUTION SUCCESSFUL
```

### Integration tests (native_sim)

Verified April 3, 2026, branch test/loopback-fake-broker,
target native_sim, Zephyr v4.3.0.  Real Mosquitto broker on LAN.

```
Running TESTSUITE sml_mqtt_integration
 PASS - test_integ_connect_disconnect     in 2.560 seconds
 PASS - test_integ_keepalive              in 3.520 seconds
 PASS - test_integ_publish_qos0          in 0.550 seconds
 PASS - test_integ_publish_qos1          in 1.370 seconds
 PASS - test_integ_publish_qos2          in 2.380 seconds
 PASS - test_integ_subscribe_qos_levels  in 0.700 seconds
 PASS - test_integ_subscribe_receive     in 0.900 seconds
TESTSUITE sml_mqtt_integration succeeded
SUITE PASS - 100.00% [sml_mqtt_integration]: pass=7 fail=0 skip=0 total=7 duration=11.980 seconds
PROJECT EXECUTION SUCCESSFUL
```

---

## Writing new tests

Each test suite uses Ztest before/after hooks to init/destroy the fake
broker around every test:

```cpp
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

static void before(void *f) { fake_broker_init(); }
static void after(void *f)  { fake_broker_destroy(); }
ZTEST_SUITE(my_suite, NULL, NULL, before, after, NULL);

ZTEST(my_suite, test_my_feature)
{
    sml_mqtt_cli::mqtt_client client;
    client.init("my_test");

    client.connect("127.0.0.1", FAKE_BROKER_PORT, false);
    fake_broker_process(FAKE_MQTT_PKT_CONNECT);
    client_poll_and_input(client);
    zassert_true(client.is_connected(), "should be connected");

    // ... test logic ...

    client.disconnect();
    fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);
}
```

Each `fake_broker_process()` call processes exactly one packet
round-trip.  Do not use `k_sleep()` for synchronisation - use
`fake_broker_process()` + `client_poll_and_input()` instead.

---

## Configuration

Key `prj.conf` options:

```properties
CONFIG_NET_TCP_TIME_WAIT_DELAY=0   # release port immediately on close
                                   # required for per-test broker restart
CONFIG_ZTEST_STACK_SIZE=8192       # SML state machine needs extra stack
CONFIG_MAIN_STACK_SIZE=8192
```

Buffer sizes via Kconfig:

```properties
CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE=256
CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE=256
CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN=512
CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS=8
```

---

## License

Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
SPDX-License-Identifier: MIT
