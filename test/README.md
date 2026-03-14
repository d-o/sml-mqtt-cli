# SML MQTT Client - Test Suite

This directory contains comprehensive tests for the sml-mqtt-cli library.

## Building Tests

### Option 1: In an Existing Workspace

If you already have a workspace with zephyr, boost-sml, and sml-mqtt-cli:

```bash
# From the workspace root directory
cd /path/to/workspace
west build -s sml-mqtt-cli/test -b qemu_riscv32 -- -DZEPHYR_MODULES="$PWD/boost-sml;$PWD/sml-mqtt-cli"
west build -t run
```

This uses the existing modules without fetching anything new.

### Option 2: Standalone Test Workspace

To create a standalone workspace just for testing:

```bash
# Clone the repository
git clone https://github.com/d-o/sml-mqtt-cli.git
cd sml-mqtt-cli/test

# Initialize west workspace (creates workspace one level up)
west init -l .

# Fetch dependencies (zephyr, boost-sml, sml-mqtt-cli)
west update

# Build and run tests
west build -b qemu_riscv32
west build -t run
```

This creates a workspace structure:
```
workspace/
├── .west/                    # west configuration
├── app/                      # this test directory (via self: path: app)
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── west.yml             # manifest file
│   └── src/
├── boost-sml/               # fetched by west update
├── sml-mqtt-cli/            # fetched by west update
└── zephyr/                  # fetched by west update
```

## Prerequisites

### MQTT Broker
Tests require an MQTT broker running on `localhost:8883` (or the host specified in test configuration).

#### Using Mosquitto
```bash
# Install Mosquitto
sudo apt install mosquitto mosquitto-clients

# Start broker (default port 1883)
mosquitto -v

# Or for port 8883
mosquitto -p 8883 -v
```

#### Using Docker
```bash
# Eclipse Mosquitto
docker run -it -p 8883:1883 eclipse-mosquitto:latest

# EMQX
docker run -it -p 8883:1883 emqx/emqx:latest
```

### Network Configuration
The test `prj.conf` uses static IP configuration:
- IP: 192.168.1.100
- Netmask: 255.255.255.0
- Gateway: 192.168.1.1

Adjust `prj.conf` if your network differs, or use DHCP:
```properties
CONFIG_NET_DHCPV4=y
CONFIG_NET_CONFIG_MY_IPV4_ADDR=""
```

## Building Tests

### For QEMU (x86)
```bash
cd test
west build -p always -b qemu_x86
west build -t run
```

### For Native POSIX
```bash
cd test
west build -p always -b native_sim
west build -t run
```

### For ESP32-S3
```bash
cd test
west build -p always -b esp32s3_devkitc/esp32s3/procpu
west flash
west espressif monitor
```

## Test Suites

### Basic Connection Tests (`test_basic_connection.cpp`)
- Client creation and initialization
- Connection/disconnection
- Invalid parameter handling
- C API wrapper functionality
- State change callbacks

### Publish/Subscribe Tests (`test_publish_subscribe.cpp`)
- Basic publish (QoS 0)
- Subscribe/unsubscribe
- Publish and receive messages
- Multiple subscriptions
- Subscription limits
- Invalid parameters

### QoS Level Tests (`test_qos_levels.cpp`)
- Publish with QoS 0, 1, 2
- Subscribe with different QoS levels
- Retained messages
- QoS handshake verification

### Multiple Client Tests (`test_multiple_clients.cpp`)
- Multiple simultaneous connections
- Client reconnection
- Mixed C/C++ API usage
- Keepalive mechanism
- Rapid publish sequences

## Test Output

Example successful test output:
```
*** Booting Zephyr OS build v4.3 ***
=== SML MQTT Client Test Suite ===
Broker: localhost:8883 (TLS: no)
Make sure MQTT broker is running!

SUITE: sml_mqtt_basic
Test: Client creation and initialization
PASS - test_client_creation

Test: Connect and disconnect from broker
Connecting to localhost:8883...
Connection status: connected
Current state: Connected
Disconnecting...
PASS - test_connect_disconnect

...

=== Test Suite Complete ===
PROJECT EXECUTION SUCCESSFUL
```

## Troubleshooting

### Connection Failures
- Verify broker is running: `mosquitto_sub -h localhost -p 8883 -t '#' -v`
- Check network connectivity
- Review firewall rules
- Enable debug logging: `CONFIG_MQTT_LOG_LEVEL_DBG=y`

### Build Errors
- Ensure Boost.SML module is available: `west update`
- Check C++17 support: `CONFIG_STD_CPP17=y`
- Verify full C++ library: `CONFIG_REQUIRES_FULL_LIBCPP=y`

### Runtime Errors
- Increase stack size: `CONFIG_MAIN_STACK_SIZE=8192`
- Increase heap: `CONFIG_HEAP_MEM_POOL_SIZE=32768`
- Check buffer sizes in Kconfig

## Configuration Options

Key Kconfig options for testing:

```properties
# Buffer sizes (adjust based on test requirements)
CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE=512
CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE=512
CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN=1024

# Timeouts
CONFIG_SML_MQTT_CLI_CONNECT_TIMEOUT_MS=10000
CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC=60

# Resource limits
CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS=16
```

## Writing New Tests

Example test structure:

```cpp
#include <zephyr/ztest.h>
#include <sml-mqtt-cli.hpp>

ZTEST(sml_mqtt_custom, test_my_feature)
{
    using namespace sml_mqtt_cli;

    mqtt_client client;
    client.init("my_test_client");

    int ret = client.connect("localhost", 8883, false);
    zassert_equal(ret, 0, "Failed to connect");

    // Test logic here

    client.disconnect();
}
```

## Memory Footprint

Typical RAM usage per client instance:
- Base context: ~200 bytes
- RX buffer: CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE
- TX buffer: CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE
- Subscriptions: CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS x 132 bytes
- **Total**: ~1-4 KB depending on configuration

## Performance Notes

- QoS 0: Best throughput, no acknowledgments
- QoS 1: Moderate overhead, at-least-once delivery
- QoS 2: Highest overhead, exactly-once delivery
- Keepalive: Default 60s, tune based on network conditions

## License

Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
SPDX-License-Identifier: MIT
