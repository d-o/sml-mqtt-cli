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
| `test_publish_qos0` | QoS 0 publish, SM returns to Connected |
| `test_subscribe` | Subscribe, SUBACK received, SM to Connected |
| `test_unsubscribe` | Unsubscribe, UNSUBACK received |
| `test_loopback_pubsub` | Subscribe + publish + receive on same client |

### `sml_mqtt_qos` (test_qos_levels.cpp)

QoS 1 and QoS 2 handshake sequences: PUBACK, PUBREC/PUBREL/PUBCOMP,
and the receiving submachine mirror of each.

### `sml_mqtt_multiple` (test_multiple_clients.cpp)

Independent client instances, reconnection, keepalive ping cycle.
Tests requiring two simultaneous broker connections are currently
skipped (single-connection fake broker).

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
