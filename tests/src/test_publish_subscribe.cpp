/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

extern void client_poll_and_input(sml_mqtt_cli::mqtt_client &client);

/* -------------------------------------------------------------------------
 * Shared callback tracking - reset before each test that uses it.
 * ------------------------------------------------------------------------- */

static int     rx_count;
static char    rx_topic[CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN];
static uint8_t rx_payload[CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN];
static size_t  rx_payload_len;

static void on_publish_received(void *user_data, const char *topic,
				const uint8_t *payload, size_t len,
				enum mqtt_qos qos)
{
	ARG_UNUSED(user_data);
	ARG_UNUSED(qos);

	rx_count++;
	strncpy(rx_topic, topic ? topic : "", sizeof(rx_topic) - 1);
	rx_topic[sizeof(rx_topic) - 1] = '\0';

	rx_payload_len = (len < sizeof(rx_payload)) ? len : sizeof(rx_payload);
	memcpy(rx_payload, payload, rx_payload_len);
}

static void reset_rx(void)
{
	rx_count       = 0;
	rx_payload_len = 0;
	memset(rx_topic,   0, sizeof(rx_topic));
	memset(rx_payload, 0, sizeof(rx_payload));
}

/* -------------------------------------------------------------------------
 * Helper: connect a client through the fake broker.
 * ------------------------------------------------------------------------- */

static void connect_client(sml_mqtt_cli::mqtt_client &client, const char *id)
{
	int ret = client.init(id);
	zassert_equal(ret, 0, "init failed for %s: %d", id, ret);

	ret = client.connect("127.0.0.1", FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "connect failed for %s: %d", id, ret);

	fake_broker_process(FAKE_MQTT_PKT_CONNECT);
	client_poll_and_input(client);
	zassert_true(client.is_connected(), "%s should be connected", id);
}

static void disconnect_client(sml_mqtt_cli::mqtt_client &client)
{
	client.disconnect();
	fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/**
 * QoS 0 publish - SM should return to Connected after send.
 */
ZTEST(sml_mqtt_pubsub, test_publish_qos0)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_pub_qos0");

	const char *topic   = "test/sml/qos0";
	const char *payload = "hello";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "publish returned: %d", ret);

	/* QoS 0 - broker receives PUBLISH but sends no ACK; no input needed. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);

	disconnect_client(client);
}

/**
 * Subscribe - verify SUBACK drives SM back to Connected.
 */
ZTEST(sml_mqtt_pubsub, test_subscribe)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_subscribe");

	int ret = client.subscribe("test/sml/sub", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe returned: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);   /* processes SUBACK */

	zassert_true(client.is_connected(), "should be Connected after SUBACK");

	disconnect_client(client);
}

/**
 * Unsubscribe - verify UNSUBACK drives SM back to Connected.
 */
ZTEST(sml_mqtt_pubsub, test_unsubscribe)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_unsub");

	const char *topic = "test/sml/unsub";

	int ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe returned: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);

	ret = client.unsubscribe(topic);
	zassert_equal(ret, 0, "unsubscribe returned: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_UNSUBSCRIBE);
	client_poll_and_input(client);   /* processes UNSUBACK */

	zassert_true(client.is_connected(),
		     "should be Connected after UNSUBACK");

	disconnect_client(client);
}

/**
 * @brief Loopback publish-subscribe: subscribe + publish + receive on one client.
 *
 * This is the primary end-to-end validation of the SML state machine:
 *
 *   1. Connect  -> CONNACK  -> SM: Disconnected -> Connected
 *   2. Subscribe-> SUBACK   -> SM: Connected -> Subscribing -> Connected
 *                              fake broker stores topic
 *   3. Publish  -> (no ACK for QoS 0)
 *                              fake broker echoes PUBLISH back (topic matches)
 *   4. Poll     -> MQTT_EVT_PUBLISH -> receiving_sm fires -> callback invoked
 *
 * Asserts that the callback received the correct topic and payload.
 */
ZTEST(sml_mqtt_pubsub, test_loopback_pubsub)
{
	reset_rx();

	sml_mqtt_cli::mqtt_client client;
	client.set_publish_received_callback(on_publish_received, nullptr);
	connect_client(client, "test_loopback");

	const char *topic   = "test/loop/qos0";
	const char *payload = "loopback-payload";

	/* Subscribe so the broker stores the topic for echo. */
	int ret = client.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe returned: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);
	zassert_true(client.is_connected(), "connected after SUBACK");

	/* Publish - broker receives, echoes back on same socket. */
	ret = client.publish(topic,
			     reinterpret_cast<const uint8_t *>(payload),
			     strlen(payload),
			     MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "publish returned: %d", ret);

	/*
	 * fake_broker_process(PUBLISH) sends no ACK for QoS 0 but does
	 * echo the packet back because the topic matches the subscription.
	 * client_poll_and_input() then reads that echo, fires
	 * MQTT_EVT_PUBLISH, which drives the receiving_sm and calls
	 * on_publish_received().
	 */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	client_poll_and_input(client);

	zassert_equal(rx_count, 1, "callback must fire exactly once");
	zassert_str_equal(rx_topic, topic, "received topic mismatch");
	zassert_equal(rx_payload_len, strlen(payload),
		      "received payload length mismatch");
	zassert_mem_equal(rx_payload, payload, strlen(payload),
			  "received payload content mismatch");

	disconnect_client(client);
}

/**
 * Null publish parameters must be rejected while connected.
 */
ZTEST(sml_mqtt_pubsub, test_invalid_publish)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_inv_pub");

	const char *payload = "test";

	zassert_not_equal(
		client.publish(nullptr,
			       reinterpret_cast<const uint8_t *>(payload),
			       strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false),
		0, "NULL topic must fail");

	zassert_not_equal(
		client.publish("test/topic", nullptr, 4,
			       MQTT_QOS_0_AT_MOST_ONCE, false),
		0, "NULL payload must fail");

	disconnect_client(client);
}
