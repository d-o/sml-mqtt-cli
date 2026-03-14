/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sml_mqtt_cli, CONFIG_MQTT_LOG_LEVEL);

// Test broker configuration - assumes broker on localhost:8883
#define TEST_BROKER_HOST "localhost"
#define TEST_BROKER_PORT 8883
#define TEST_USE_TLS false

// Test setup and teardown
static void *suite_setup(void)
{
	LOG_INF("=== SML MQTT Client Test Suite ===");
	LOG_INF("Broker: %s:%d (TLS: %s)", TEST_BROKER_HOST, TEST_BROKER_PORT,
	        TEST_USE_TLS ? "yes" : "no");
	LOG_INF("Make sure MQTT broker is running!");

	// Wait for network to be ready
	k_sleep(K_SECONDS(2));

	return NULL;
}

static void suite_teardown(void *fixture)
{
	LOG_INF("=== Test Suite Complete ===");
}

ZTEST_SUITE(sml_mqtt_basic, NULL, suite_setup, NULL, NULL, suite_teardown);
ZTEST_SUITE(sml_mqtt_pubsub, NULL, suite_setup, NULL, NULL, suite_teardown);
ZTEST_SUITE(sml_mqtt_qos, NULL, suite_setup, NULL, NULL, suite_teardown);
ZTEST_SUITE(sml_mqtt_multiple, NULL, suite_setup, NULL, NULL, suite_teardown);
