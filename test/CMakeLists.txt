# Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sml_mqtt_cli_test)

# Add test source files
target_sources(app PRIVATE
	src/main.cpp
	src/test_basic_connection.cpp
	src/test_publish_subscribe.cpp
	src/test_qos_levels.cpp
	src/test_multiple_clients.cpp
)

# Ensure we can find the header
zephyr_include_directories(../include)
