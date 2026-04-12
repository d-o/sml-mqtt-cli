# SML MQTT Client - Quick Reference

## Installation

```yaml
# west.yml
projects:
  - name: boost-sml
    url: https://github.com/boost-ext/sml.git
    revision: zephyr_module
  - name: sml-mqtt-cli
    url: <your-repo-url>
    revision: main
```

```properties
# prj.conf
CONFIG_CPP=y
CONFIG_STD_CPP17=y
CONFIG_REQUIRES_FULL_LIBCPP=y
CONFIG_LIB_BOOST_SML=y
CONFIG_MQTT_LIB=y
CONFIG_LIB_SML_MQTT_CLI=y
```

## C++ API Quick Reference

```cpp
#include <sml-mqtt-cli.hpp>
using namespace sml_mqtt_cli;

// Create & initialize
mqtt_client client;
int ret = client.init("client_id");

// Connect
ret = client.connect("broker.example.com", 1883, false);  // host, port, tls

// Publish
ret = client.publish(topic, payload, len, MQTT_QOS_0_AT_MOST_ONCE, false);

// Subscribe
ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);

// Unsubscribe
ret = client.unsubscribe(topic);

// Process (call in loop)
ret = client.input();  // Handle incoming messages
ret = client.live();   // Send keepalive

// Status
bool connected = client.is_connected();
const char *state = client.get_state();

// Disconnect
ret = client.disconnect();
```

## C API Quick Reference

```c
#include <sml-mqtt-cli.hpp>

// Get required storage size
size_t size = sml_mqtt_client_get_size();

// Allocate storage (static or stack - no heap)
static uint8_t client_storage[sml_mqtt_client_get_size()] __attribute__((aligned(8)));

// Initialize with storage
sml_mqtt_client_handle_t client = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));

// Initialize client
sml_mqtt_client_init(client, "client_id");

// Connect
sml_mqtt_client_connect(client, "broker.example.com", 1883, false);

// Publish
sml_mqtt_client_publish(client, topic, payload, len, MQTT_QOS_0_AT_MOST_ONCE, false);

// Subscribe
sml_mqtt_client_subscribe(client, topic, MQTT_QOS_0_AT_MOST_ONCE);

// Unsubscribe
sml_mqtt_client_unsubscribe(client, topic);

// Process (call in loop)
sml_mqtt_client_input(client);
sml_mqtt_client_live(client);

// Status
bool connected = sml_mqtt_client_is_connected(client);

// Disconnect & deinitialize (does not free storage)
sml_mqtt_client_disconnect(client);
sml_mqtt_client_deinit(client);
```

## Callbacks

```cpp
// C++ API
client.set_publish_received_callback(
    [](void *user_data, const char *topic, const uint8_t *payload,
       size_t len, enum mqtt_qos qos) {
        // Handle message
    },
    nullptr
);

client.set_state_change_callback(
    [](void *user_data, const char *old_state, const char *new_state) {
        // Handle state change
    },
    nullptr
);
```

```c
// C API
void on_message(void *user_data, const char *topic, const uint8_t *payload,
                size_t len, enum mqtt_qos qos) {
    // Handle message
}

void on_state_change(void *user_data, const char *old_state, const char *new_state) {
    // Handle state change
}

sml_mqtt_client_set_publish_received_callback(client, on_message, NULL);
sml_mqtt_client_set_state_change_callback(client, on_state_change, NULL);
```

## QoS Levels

```cpp
MQTT_QOS_0_AT_MOST_ONCE   // Fire and forget, no ack
MQTT_QOS_1_AT_LEAST_ONCE  // Acknowledged, may duplicate
MQTT_QOS_2_EXACTLY_ONCE   // Exactly once, 4-way handshake
```

## Configuration Options

```properties
CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN=128          # Max topic length
CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN=512        # Max payload size
CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN=64       # Max client ID
CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE=256         # RX buffer size
CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE=256         # TX buffer size
CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS=8        # Max subscriptions
CONFIG_SML_MQTT_CLI_CONNECT_TIMEOUT_MS=5000    # Connection timeout
CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC=60           # Keepalive interval
```

## Error Codes

```c
0           // Success
-EINVAL     // Invalid parameter
-ENOTCONN   // Not connected
-EHOSTUNREACH // Cannot reach broker
-EMSGSIZE   // Message too large
-ENOMEM     // Out of resources
-ENOENT     // Not found
-ETIMEDOUT  // Timeout
```

## Main Loop Pattern

```cpp
// C++
while (client.is_connected()) {
    client.input();  // Process incoming
    client.live();   // Send keepalive

    // Your application logic

    k_sleep(K_MSEC(100));
}
```

```c
// C - static storage allocation (no heap)
static uint8_t client_storage[sml_mqtt_client_get_size()] __attribute__((aligned(8)));
sml_mqtt_client_handle_t client = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));

    sml_mqtt_client_input(client);
    sml_mqtt_client_live(client);

    // Your application logic

    k_sleep(K_MSEC(100));
}
```

## Memory Usage

Default config: ~2.4 KB per client
- Base: 200 bytes
- RX buffer: 256 bytes
- TX buffer: 256 bytes
- Topic/payload: 640 bytes
- Subscriptions: 1056 bytes

Minimal config: ~1.1 KB per client

## States

**Main State Machine:**
- **Disconnected** - No connection
- **Connecting** - Waiting for CONNACK
- **Connected** - Ready for operations, transitions to submachines
- **Subscribing** - Waiting for SUBACK
- **Unsubscribing** - Waiting for UNSUBACK

**Publishing Submachine (publishing_sm):**
- **idle** - Ready to publish
- **qos0** - QoS 0 publish in progress
- **qos1** - QoS 1 publish, waiting for PUBACK
- **qos2** - QoS 2 publish, waiting for PUBREC
- **releasing** - QoS 2 release phase, waiting for PUBCOMP

**Receiving Submachine (receiving_sm):**
- **idle** - Ready to receive (initial state)
- **waiting_rel** - QoS 2, waiting for PUBREL from broker (QoS 0/1 complete immediately at sml::X)

## Error Handling

- All functions return errno codes (0 = success)
- Protocol send errors trigger automatic state recovery
- Failed protocol sends (PUBACK, PUBREC, PUBREL, PUBCOMP) terminate the sub-SM (sml::X);
  the outer SM receives evt_transaction_done and returns to Connected
- Connection errors return to Disconnected state
- Error recovery validated by unit tests (fake broker) and integration tests (real Mosquitto)

## Testing

```bash
# Unit tests - in QEMU, no broker required
west build -p always -s sml-mqtt-cli/tests -b qemu_riscv32 \
  -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli"
timeout 120 west build -t run

# Integration tests - native_sim against a real Mosquitto broker
west build -p always -s sml-mqtt-cli/tests -b native_sim \
  -- -DSML_MQTT_TEST_BROKER_HOST=<broker-ip>
sml-mqtt-cli/tests/scripts/run_integration_tests.sh \
  --broker-host <broker-ip> --enable-routing --no-local-broker

# Local broker only (no routing needed)
mosquitto -p 1883 -v
west build -p always -s sml-mqtt-cli/tests -b native_sim
sml-mqtt-cli/tests/scripts/run_integration_tests.sh
```

## Examples

See:
- `example_usage.cpp` - Standalone example
- `tests/src/test_*.cpp` - Test suite examples
- `README.md` - Full documentation

## Support

- Zephyr RTOS 4.3+
- C++17 required
- MQTT v3.1.1 only
- Thread: Use one client per thread
- TLS: Supported via CONFIG_MQTT_LIB_TLS
