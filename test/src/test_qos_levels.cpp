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
 * @brief Test publish with QoS 1
 */
ZTEST(sml_mqtt_qos, test_publish_qos1)
{
	LOG_INF("Test: Publish with QoS 1");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_pub_qos1");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	zassert_true(client.is_connected(), "Client should be connected");

	// Publish with QoS 1
	const char *topic = "test/sml/qos1";
	const char *payload = "Hello, MQTT QoS 1!";

	LOG_INF("Publishing with QoS 1 to '%s'...", topic);
	ret = client.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                     strlen(payload), MQTT_QOS_1_AT_LEAST_ONCE, false);
	zassert_equal(ret, 0, "Failed to publish: %d", ret);

	// Wait for PUBACK
	k_sleep(K_SECONDS(1));
	client.input();

	LOG_INF("QoS 1 publish completed");

	client.disconnect();
}

/**
 * @brief Test publish with QoS 2
 */
ZTEST(sml_mqtt_qos, test_publish_qos2)
{
	LOG_INF("Test: Publish with QoS 2");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_pub_qos2");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	zassert_true(client.is_connected(), "Client should be connected");

	// Publish with QoS 2
	const char *topic = "test/sml/qos2";
	const char *payload = "Hello, MQTT QoS 2!";

	LOG_INF("Publishing with QoS 2 to '%s'...", topic);
	ret = client.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                     strlen(payload), MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "Failed to publish: %d", ret);

	// Wait for PUBREC, PUBREL, PUBCOMP sequence
	for (int i = 0; i < 3; i++) {
		k_sleep(K_SECONDS(1));
		client.input();
	}

	LOG_INF("QoS 2 publish completed");

	client.disconnect();
}

/**
 * @brief Test subscribe with different QoS levels
 */
ZTEST(sml_mqtt_qos, test_subscribe_qos_levels)
{
	LOG_INF("Test: Subscribe with different QoS levels");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_sub_qos");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	// Subscribe with QoS 0
	LOG_INF("Subscribing with QoS 0...");
	ret = client.subscribe("test/sml/qos/0", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe with QoS 0");

	k_sleep(K_SECONDS(1));
	client.input();

	// Subscribe with QoS 1
	LOG_INF("Subscribing with QoS 1...");
	ret = client.subscribe("test/sml/qos/1", MQTT_QOS_1_AT_LEAST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe with QoS 1");

	k_sleep(K_SECONDS(1));
	client.input();

	// Subscribe with QoS 2
	LOG_INF("Subscribing with QoS 2...");
	ret = client.subscribe("test/sml/qos/2", MQTT_QOS_2_EXACTLY_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe with QoS 2");

	k_sleep(K_SECONDS(1));
	client.input();

	LOG_INF("QoS subscription test completed");

	client.disconnect();
}

/**
 * @brief Test retained messages
 */
ZTEST(sml_mqtt_qos, test_retained_message)
{
	LOG_INF("Test: Retained messages");

	sml_mqtt_cli::mqtt_client publisher;
	publisher.init("test_retain_pub");

	int ret = publisher.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect publisher");

	k_sleep(K_SECONDS(2));
	publisher.input();

	// Publish retained message
	const char *topic = "test/sml/retained";
	const char *payload = "This is a retained message";

	LOG_INF("Publishing retained message...");
	ret = publisher.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                       strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, true);
	zassert_equal(ret, 0, "Failed to publish retained message");

	k_sleep(K_SECONDS(1));
	publisher.disconnect();

	// Now subscribe with a new client - should receive retained message
	sml_mqtt_cli::mqtt_client subscriber;
	subscriber.init("test_retain_sub");

	ret = subscriber.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect subscriber");

	k_sleep(K_SECONDS(2));
	subscriber.input();

	LOG_INF("Subscribing to topic with retained message...");
	ret = subscriber.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe");

	// Wait for retained message
	k_sleep(K_SECONDS(2));
	subscriber.input();

	LOG_INF("Retained message test completed");

	// Clean up - publish empty retained message
	sml_mqtt_cli::mqtt_client cleaner;
	cleaner.init("test_retain_clean");
	cleaner.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	k_sleep(K_SECONDS(2));
	cleaner.input();
	cleaner.publish(topic, reinterpret_cast<const uint8_t*>(""), 0,
	               MQTT_QOS_0_AT_MOST_ONCE, true);
	k_sleep(K_MSEC(500));
	cleaner.disconnect();

	subscriber.disconnect();
}
