# SML MQTT Client - Test Suite

Tests for the sml-mqtt-cli library running on `qemu_riscv32` with an
in-process fake MQTT broker.  No external broker, no network hardware,
no Docker required.

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

### Why not native_sim with a real broker?

`native_sim` compiles Zephyr as a Linux executable and can bridge to
the host network via TUN/TAP, making a real broker reachable.  This is
a valid approach for integration testing but has trade-offs:

| | fake broker (qemu_riscv32) | real broker (native_sim) |
|---|---|---|
| External dependency | None | Mosquitto must be running |
| CI setup | Zero | broker install + startup script |
| Portable to real hardware targets | Yes | No |
| Timing determinism | Yes | Network jitter |
| twister-compatible | All boards | native_sim only |

The fake broker approach matches Zephyr's own CI and is the right
choice for a library that targets embedded hardware.

---

## Building Tests

### Option 1: In an Existing Workspace

In this workspace (recommended):

```bash
cd /path/to/workspace          # the z-workspaces root
source .venv/bin/activate

west build -p always -s sml-mqtt-cli/test -b qemu_riscv32 \
  -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli"

west build -t run
```

The `-DZEPHYR_MODULES` override points CMake at the local copies of
boost-sml and sml-mqtt-cli without re-running `west update`.

Standalone (fresh checkout):

```bash
git clone https://github.com/d-o/sml-mqtt-cli.git
cd sml-mqtt-cli/test
west init -l .
west update
west build -b qemu_riscv32
west build -t run
```

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
| `test_subscribe_qos_levels` | Subscribe at QoS 0, 1, and 2 in sequence; SUBACK after each |
| `test_retained_message` | retain=1 flag set on publish; subscribe on second client succeeds |

### `sml_mqtt_multiple` (test_multiple_clients.cpp)

| Test | What it validates |
|------|------------------|
| `test_multiple_clients` | Three clients connect, publish, and disconnect sequentially through the single-accept broker |
| `test_reconnection` | Connect, disconnect, reinit with new client ID, reconnect and publish |
| `test_mixed_api_usage` | C++ client followed by C API client on same broker socket |
| `test_keepalive` | mqtt_live() called directly; PINGREQ/PINGRESP processed if keepalive timer has elapsed |
| `test_rapid_publish` | Ten QoS 0 messages sent back-to-back; all ten PASS |

---

## Last test run output

Verified March 29, 2026, branch test/loopback-fake-broker (commit af39391),
target qemu_riscv32, Zephyr v4.3-branch.

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
 PASS - test_publish_qos1         in 0.140 seconds
 PASS - test_publish_qos2         in 0.200 seconds
 PASS - test_retained_message     in 0.200 seconds
 PASS - test_subscribe_qos_levels in 0.260 seconds
TESTSUITE sml_mqtt_qos succeeded
------ TESTSUITE SUMMARY START ------
SUITE PASS - 100.00% [sml_mqtt_basic]:    pass=6  fail=0  skip=0  total=6
SUITE PASS - 100.00% [sml_mqtt_multiple]: pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_pubsub]:   pass=5  fail=0  skip=0  total=5
SUITE PASS - 100.00% [sml_mqtt_qos]:      pass=4  fail=0  skip=0  total=4
------ TESTSUITE SUMMARY END ------
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
