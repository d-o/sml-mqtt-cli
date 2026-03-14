/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <sml-mqtt-cli.hpp>


using namespace sml_mqtt_cli;

#define TEST_BROKER_HOST "localhost"
#define TEST_BROKER_PORT 8883
#define TEST_USE_TLS false

/**
 * @brief Test multiple simultaneous client connections
 */
ZTEST(sml_mqtt_multiple, test_multiple_clients)
{
	LOG_INF("Test: Multiple simultaneous clients");

	constexpr int NUM_CLIENTS = 3;
	sml_mqtt_cli::mqtt_client clients[NUM_CLIENTS];

	// Initialize and connect all clients
	for (int i = 0; i < NUM_CLIENTS; i++) {
		char client_id[32];
		snprintf(client_id, sizeof(client_id), "test_multi_%d", i);

		LOG_INF("Initializing client %d: %s", i, client_id);
		int ret = clients[i].init(client_id);
		zassert_equal(ret, 0, "Failed to initialize client %d", i);

		ret = clients[i].connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
		zassert_equal(ret, 0, "Failed to connect client %d", i);

		k_sleep(K_SECONDS(1));
		clients[i].input();
	}

	// Verify all are connected
	for (int i = 0; i < NUM_CLIENTS; i++) {
		bool connected = clients[i].is_connected();
		LOG_INF("Client %d connected: %s", i, connected ? "yes" : "no");
		zassert_true(connected, "Client %d should be connected", i);
	}

	// Each client publishes to its own topic
	for (int i = 0; i < NUM_CLIENTS; i++) {
		char topic[64];
		char payload[64];
		snprintf(topic, sizeof(topic), "test/sml/multi/client%d", i);
		snprintf(payload, sizeof(payload), "Message from client %d", i);

		LOG_INF("Client %d publishing to %s", i, topic);
		int ret = clients[i].publish(topic, reinterpret_cast<const uint8_t*>(payload),
		                             strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
		zassert_equal(ret, 0, "Client %d failed to publish", i);

		k_sleep(K_MSEC(500));
		clients[i].live();
	}

	// Disconnect all clients
	for (int i = 0; i < NUM_CLIENTS; i++) {
		LOG_INF("Disconnecting client %d", i);
		clients[i].disconnect();
	}

	LOG_INF("Multiple clients test completed");
}

/**
 * @brief Test client reconnection
 */
ZTEST(sml_mqtt_multiple, test_reconnection)
{
	LOG_INF("Test: Client reconnection");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_reconnect");

	// First connection
	LOG_INF("First connection...");
	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed first connect");

	k_sleep(K_SECONDS(2));
	client.input();
	zassert_true(client.is_connected(), "Should be connected");

	// Disconnect
	LOG_INF("Disconnecting...");
	ret = client.disconnect();
	zassert_equal(ret, 0, "Failed to disconnect");
	k_sleep(K_MSEC(500));
	zassert_false(client.is_connected(), "Should be disconnected");

	// Reconnect
	LOG_INF("Reconnecting...");
	ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to reconnect");

	k_sleep(K_SECONDS(2));
	client.input();
	zassert_true(client.is_connected(), "Should be reconnected");

	// Publish to verify connection works
	const char *topic = "test/sml/reconnect";
	const char *payload = "After reconnection";
	ret = client.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                    strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "Failed to publish after reconnect");

	k_sleep(K_MSEC(500));

	client.disconnect();
	LOG_INF("Reconnection test completed");
}

/**
 * @brief Test using both C and C++ API simultaneously
 */
ZTEST(sml_mqtt_multiple, test_mixed_api_usage)
{
	LOG_INF("Test: Mixed C and C++ API usage");

	// Create C++ client
	sml_mqtt_cli::mqtt_client cpp_client;
	cpp_client.init("test_mixed_cpp");

	int ret = cpp_client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect C++ client");

	k_sleep(K_SECONDS(2));
	cpp_client.input();

	// Create C API client with static storage (no heap)
	static uint8_t c_client_storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));
	sml_mqtt_client_handle_t c_handle = sml_mqtt_client_init_with_storage(c_client_storage, sizeof(c_client_storage));
	zassert_not_null(c_handle, "Failed to create C client");

	ret = sml_mqtt_client_init(c_handle, "test_mixed_c");
	zassert_equal(ret, 0, "Failed to init C client");

	ret = sml_mqtt_client_connect(c_handle, TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect C client");

	k_sleep(K_SECONDS(2));
	sml_mqtt_client_input(c_handle);

	// Both clients publish
	const char *cpp_topic = "test/sml/mixed/cpp";
	const char *c_topic = "test/sml/mixed/c";
	const char *payload = "Mixed API test";

	LOG_INF("C++ client publishing...");
	ret = cpp_client.publish(cpp_topic, reinterpret_cast<const uint8_t*>(payload),
	                        strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "C++ publish failed");

	LOG_INF("C client publishing...");
	ret = sml_mqtt_client_publish(c_handle, c_topic,
	                               reinterpret_cast<const uint8_t*>(payload),
	                               strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "C publish failed");

	k_sleep(K_SECONDS(1));

	// Clean up
	cpp_client.disconnect();
	sml_mqtt_client_disconnect(c_handle);
	sml_mqtt_client_deinit(c_handle);

	LOG_INF("Mixed API test completed");
}

/**
 * @brief Test keepalive mechanism
 */
ZTEST(sml_mqtt_multiple, test_keepalive)
{
	LOG_INF("Test: Keepalive mechanism");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_keepalive");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	zassert_true(client.is_connected(), "Should be connected");

	// Stay idle for longer than half the keepalive interval
	// The library should automatically send PINGREQ
	LOG_INF("Staying idle to test keepalive...");

	for (int i = 0; i < 10; i++) {
		k_sleep(K_SECONDS(3));

		// Call live() to trigger keepalive
		ret = client.live();
		if (ret != 0) {
			LOG_WRN("live() returned %d", ret);
		}

		// Process any PINGRESP
		ret = client.input();
		if (ret == 0) {
			LOG_INF("Processed MQTT input");
		}

		// Check connection is still alive
		if (!client.is_connected()) {
			LOG_ERR("Connection lost during keepalive test");
			break;
		}
	}

	zassert_true(client.is_connected(), "Connection should remain alive");

	client.disconnect();
	LOG_INF("Keepalive test completed");
}

/**
 * @brief Test rapid publish sequence
 */
ZTEST(sml_mqtt_multiple, test_rapid_publish)
{
	LOG_INF("Test: Rapid publish sequence");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_rapid_pub");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	// Publish many messages in quick succession
	const char *topic = "test/sml/rapid";
	constexpr int NUM_MESSAGES = 10;
	int success_count = 0;

	LOG_INF("Publishing %d messages rapidly...", NUM_MESSAGES);

	for (int i = 0; i < NUM_MESSAGES; i++) {
		char payload[64];
		snprintf(payload, sizeof(payload), "Rapid message %d", i);

		ret = client.publish(topic, reinterpret_cast<const uint8_t*>(payload),
		                    strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
		if (ret == 0) {
			success_count++;
		} else {
			LOG_WRN("Publish %d failed: %d", i, ret);
		}

		// Small delay between publishes
		k_sleep(K_MSEC(50));
		client.live();
	}

	LOG_INF("Successfully published %d/%d messages", success_count, NUM_MESSAGES);
	zassert_true(success_count >= NUM_MESSAGES / 2,
	            "Should publish at least half the messages");

	client.disconnect();
}
