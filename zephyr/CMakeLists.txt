# Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
# SPDX-License-Identifier: MIT

if (CONFIG_LIB_SML_MQTT_CLI)
  zephyr_include_directories(../include)

  # Header-only library - no sources to compile
  # All implementation is in the header file
endif()
