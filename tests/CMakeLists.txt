# Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sml_mqtt_cli_test)

# Export compile_commands.json for IDE tooling (clangd, VS Code IntelliSense)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add test source files.
#
# Two mutually exclusive build configurations:
#
#   Default (qemu_riscv32, ESP32, any target without ETH_NATIVE_TAP):
#     Compiles the fake MQTT broker and all 20 loopback unit tests.
#     Network is entirely in-kernel (CONFIG_NET_TEST + CONFIG_NET_LOOPBACK).
#     No external broker or network hardware required.
#
#   native_sim (CONFIG_ETH_NATIVE_TAP=y):
#     Compiles only the integration test suite (sml_mqtt_integration).
#     Tests connect to a real MQTT broker via a TAP virtual Ethernet interface.
#     See tests/scripts/run_integration_tests.sh for setup.
#
#     Broker location is configured at build time via CMake cache variables:
#
#       SML_MQTT_TEST_BROKER_HOST  (default "192.0.2.1" = host-side TAP IP)
#       SML_MQTT_TEST_BROKER_PORT  (default 1883)
#
#     Use case 1 - broker on build machine (default):
#       west build -p always -s sml-mqtt-cli/tests -b native_sim \
#         -- -DZEPHYR_MODULES=...
#
#     Use case 2 - broker on LAN or remote host:
#       west build -p always -s sml-mqtt-cli/tests -b native_sim \
#         -- -DZEPHYR_MODULES=... \
#            -DSML_MQTT_TEST_BROKER_HOST="192.168.1.100" \
#            -DSML_MQTT_TEST_BROKER_PORT=1883
#       sudo tests/scripts/run_integration_tests.sh \
#         --broker-host 192.168.1.100 --enable-routing --no-local-broker

if(CONFIG_ETH_NATIVE_TAP)
  # Broker address injected as a compile-time macro.  Defaults to the host
  # side of the TAP interface (192.0.2.1) for the standard local-broker case.
  # Override with -DSML_MQTT_TEST_BROKER_HOST=<ip> to target a LAN or remote
  # broker without modifying source files.
  set(SML_MQTT_TEST_BROKER_HOST "192.0.2.1" CACHE STRING
      "Broker hostname/IP for integration tests (native_sim)")
  set(SML_MQTT_TEST_BROKER_PORT "1883" CACHE STRING
      "Broker TCP port for integration tests (native_sim)")

  message(STATUS "Integration test broker: "
          "${SML_MQTT_TEST_BROKER_HOST}:${SML_MQTT_TEST_BROKER_PORT}")

  zephyr_compile_definitions(
      SML_MQTT_TEST_BROKER_HOST="${SML_MQTT_TEST_BROKER_HOST}"
      SML_MQTT_TEST_BROKER_PORT=${SML_MQTT_TEST_BROKER_PORT}
  )

  target_sources(app PRIVATE
    src/main.cpp
    src/test_integration_broker.cpp
  )
else()
  target_sources(app PRIVATE
    src/fake_broker.cpp
    src/main.cpp
    src/test_basic_connection.cpp
    src/test_publish_subscribe.cpp
    src/test_qos_levels.cpp
    src/test_multiple_clients.cpp
  )
endif()

# Ensure we can find the header
zephyr_include_directories(../include)
