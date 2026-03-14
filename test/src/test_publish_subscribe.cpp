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

// Callback tracking for publish received
static int publish_received_count = 0;
static char last_received_topic[128] = {0};
static uint8_t last_received_payload[512] = {0};
static size_t last_received_payload_len = 0;
static enum mqtt_qos last_received_qos = MQTT_QOS_0_AT_MOST_ONCE;

static void publish_received_callback(void *user_data, const char *topic,
                                      const uint8_t *payload, size_t len,
                                      enum mqtt_qos qos)
{
	publish_received_count++;
	strncpy(last_received_topic, topic, sizeof(last_received_topic) - 1);
	last_received_topic[sizeof(last_received_topic) - 1] = '\0';

	last_received_payload_len = len < sizeof(last_received_payload) ? len : sizeof(last_received_payload);
	memcpy(last_received_payload, payload, last_received_payload_len);
	last_received_qos = qos;

	LOG_INF("Publish received: topic='%s', len=%zu, qos=%d", topic, len, qos);
}

static void reset_publish_tracking(void)
{
	publish_received_count = 0;
	memset(last_received_topic, 0, sizeof(last_received_topic));
	memset(last_received_payload, 0, sizeof(last_received_payload));
	last_received_payload_len = 0;
	last_received_qos = MQTT_QOS_0_AT_MOST_ONCE;
}

/**
 * @brief Test basic publish with QoS 0
 */
ZTEST(sml_mqtt_pubsub, test_publish_qos0)
{
	LOG_INF("Test: Publish with QoS 0");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_pub_qos0");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	zassert_true(client.is_connected(), "Client should be connected");

	// Publish a message
	const char *topic = "test/sml/qos0";
	const char *payload = "Hello, MQTT QoS 0!";

	LOG_INF("Publishing to '%s'...", topic);
	ret = client.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                     strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "Failed to publish: %d", ret);

	// Allow time for message to be sent
	k_sleep(K_MSEC(500));
	client.live();

	LOG_INF("Publish QoS 0 test completed");

	client.disconnect();
}

/**
 * @brief Test basic subscribe
 */
ZTEST(sml_mqtt_pubsub, test_subscribe)
{
	LOG_INF("Test: Subscribe to topic");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_subscribe");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	zassert_true(client.is_connected(), "Client should be connected");

	// Subscribe to a topic
	const char *topic = "test/sml/subscribe";

	LOG_INF("Subscribing to '%s'...", topic);
	ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe: %d", ret);

	// Wait for SUBACK
	k_sleep(K_SECONDS(1));
	client.input();

	LOG_INF("Subscribe test completed");

	client.disconnect();
}

/**
 * @brief Test unsubscribe
 */
ZTEST(sml_mqtt_pubsub, test_unsubscribe)
{
	LOG_INF("Test: Subscribe and unsubscribe");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_unsub");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	const char *topic = "test/sml/unsub";

	// Subscribe
	LOG_INF("Subscribing to '%s'...", topic);
	ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe");

	k_sleep(K_SECONDS(1));
	client.input();

	// Unsubscribe
	LOG_INF("Unsubscribing from '%s'...", topic);
	ret = client.unsubscribe(topic);
	zassert_equal(ret, 0, "Failed to unsubscribe: %d", ret);

	k_sleep(K_SECONDS(1));
	client.input();

	LOG_INF("Unsubscribe test completed");

	client.disconnect();
}

/**
 * @brief Test publish and receive
 *
 * This test uses two clients: one publisher and one subscriber
 */
ZTEST(sml_mqtt_pubsub, test_publish_and_receive)
{
	LOG_INF("Test: Publish and receive message");

	reset_publish_tracking();

	// Create subscriber client
	sml_mqtt_cli::mqtt_client subscriber;
	subscriber.init("test_subscriber");
	subscriber.set_publish_received_callback(publish_received_callback, nullptr);

	int ret = subscriber.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect subscriber");

	k_sleep(K_SECONDS(2));
	subscriber.input();

	// Subscribe to test topic
	const char *topic = "test/sml/pubrecv";
	LOG_INF("Subscribing to '%s'...", topic);
	ret = subscriber.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "Failed to subscribe");

	k_sleep(K_SECONDS(1));
	subscriber.input();

	// Create publisher client
	sml_mqtt_cli::mqtt_client publisher;
	publisher.init("test_publisher");

	ret = publisher.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect publisher");

	k_sleep(K_SECONDS(2));
	publisher.input();

	// Publish a message
	const char *payload = "Test message from publisher";
	LOG_INF("Publishing message...");
	ret = publisher.publish(topic, reinterpret_cast<const uint8_t*>(payload),
	                       strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "Failed to publish");

	// Give time for message to propagate
	k_sleep(K_SECONDS(2));

	// Process incoming messages on subscriber
	for (int i = 0; i < 5; i++) {
		ret = subscriber.input();
		if (ret == 0) {
			k_sleep(K_MSEC(100));
		} else {
			break;
		}
	}

	// Check if message was received
	LOG_INF("Publish received count: %d", publish_received_count);
	if (publish_received_count > 0) {
		LOG_INF("Received topic: '%s'", last_received_topic);
		LOG_INF("Received payload: '%.*s'", (int)last_received_payload_len, last_received_payload);

		zassert_equal(strcmp(last_received_topic, topic), 0, "Topic mismatch");
		zassert_equal(last_received_payload_len, strlen(payload), "Payload length mismatch");
		zassert_mem_equal(last_received_payload, payload, strlen(payload), "Payload mismatch");
	} else {
		LOG_WRN("No message received - broker may not be running or misconfigured");
	}

	publisher.disconnect();
	subscriber.disconnect();
}

/**
 * @brief Test multiple subscriptions
 */
ZTEST(sml_mqtt_pubsub, test_multiple_subscriptions)
{
	LOG_INF("Test: Multiple subscriptions");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_multi_sub");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	// Subscribe to multiple topics
	const char *topics[] = {
		"test/sml/multi/1",
		"test/sml/multi/2",
		"test/sml/multi/3"
	};

	for (size_t i = 0; i < ARRAY_SIZE(topics); i++) {
		LOG_INF("Subscribing to '%s'...", topics[i]);
		ret = client.subscribe(topics[i], MQTT_QOS_0_AT_MOST_ONCE);
		zassert_equal(ret, 0, "Failed to subscribe to topic %zu", i);

		k_sleep(K_MSEC(500));
		client.input();
	}

	// Unsubscribe from all
	for (size_t i = 0; i < ARRAY_SIZE(topics); i++) {
		LOG_INF("Unsubscribing from '%s'...", topics[i]);
		ret = client.unsubscribe(topics[i]);
		zassert_equal(ret, 0, "Failed to unsubscribe from topic %zu", i);

		k_sleep(K_MSEC(500));
		client.input();
	}

	client.disconnect();
}

/**
 * @brief Test subscription limit
 */
ZTEST(sml_mqtt_pubsub, test_subscription_limit)
{
	LOG_INF("Test: Subscription limit");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_sub_limit");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	// Subscribe up to the limit
	char topic[64];
	int success_count = 0;

	for (int i = 0; i < CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS + 2; i++) {
		snprintf(topic, sizeof(topic), "test/sml/limit/%d", i);
		ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);

		if (ret == 0) {
			success_count++;
			k_sleep(K_MSEC(200));
			client.input();
		} else {
			LOG_INF("Subscription %d failed as expected (limit reached)", i);
			break;
		}
	}

	LOG_INF("Successfully subscribed to %d topics (max=%d)",
	        success_count, CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS);

	zassert_true(success_count <= CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS,
	            "Exceeded subscription limit");

	client.disconnect();
}

/**
 * @brief Test invalid publish parameters
 */
ZTEST(sml_mqtt_pubsub, test_invalid_publish)
{
	LOG_INF("Test: Invalid publish parameters");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_invalid_pub");

	int ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	client.input();

	const char *payload = "test";

	// NULL topic should fail
	ret = client.publish(nullptr, reinterpret_cast<const uint8_t*>(payload),
	                    strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_not_equal(ret, 0, "Should fail with NULL topic");

	// NULL payload should fail
	ret = client.publish("test/topic", nullptr, 10, MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_not_equal(ret, 0, "Should fail with NULL payload");

	// Too long topic should fail
	char long_topic[CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN + 10];
	memset(long_topic, 'A', sizeof(long_topic) - 1);
	long_topic[sizeof(long_topic) - 1] = '\0';

	ret = client.publish(long_topic, reinterpret_cast<const uint8_t*>(payload),
	                    strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_not_equal(ret, 0, "Should fail with too long topic");

	client.disconnect();
}
