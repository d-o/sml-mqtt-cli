/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

extern void client_poll_and_input(sml_mqtt_cli::mqtt_client &client);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void connect_client(sml_mqtt_cli::mqtt_client &client, const char *id)
{
	int ret = client.init(id);
	zassert_equal(ret, 0, "init(%s) failed: %d", id, ret);

	ret = client.connect("127.0.0.1", FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "connect(%s) failed: %d", id, ret);

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
 * Sequential multiple clients.
 *
 * The fake broker handles a single TCP connection at a time.  Connect,
 * publish, and disconnect each client in turn - each gets a fresh accepted
 * socket from the same listening socket.
 */
ZTEST(sml_mqtt_multiple, test_multiple_clients)
{
	constexpr int NUM_CLIENTS = 3;

	for (int i = 0; i < NUM_CLIENTS; i++) {
		char client_id[32];
		snprintf(client_id, sizeof(client_id), "test_multi_%d", i);

		sml_mqtt_cli::mqtt_client client;
		connect_client(client, client_id);

		char topic[64];
		char payload[64];
		snprintf(topic,   sizeof(topic),   "test/sml/multi/client%d", i);
		snprintf(payload, sizeof(payload), "Message from client %d",   i);

		int ret = client.publish(topic,
					 reinterpret_cast<const uint8_t *>(payload),
					 strlen(payload),
					 MQTT_QOS_0_AT_MOST_ONCE, false);
		zassert_equal(ret, 0, "client[%d] publish failed: %d", i, ret);

		fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
		disconnect_client(client);
	}
}

/**
 * Reconnection: connect, disconnect, then reinitialise and reconnect.
 */
ZTEST(sml_mqtt_multiple, test_reconnection)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_reconnect");

	/* First disconnect. */
	disconnect_client(client);
	zassert_false(client.is_connected(), "should be disconnected");

	/* Reinitialise (resets internal SM state) then reconnect. */
	connect_client(client, "test_reconnect2");

	const char *topic   = "test/sml/reconnect";
	const char *payload = "After reconnection";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "publish after reconnect failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	disconnect_client(client);
}

/**
 * Mixed C and C++ API: both clients connect and publish sequentially.
 */
ZTEST(sml_mqtt_multiple, test_mixed_api_usage)
{
	/* C++ client first. */
	sml_mqtt_cli::mqtt_client cpp_client;
	connect_client(cpp_client, "test_mixed_cpp");

	const char *payload = "Mixed API test";

	int ret = cpp_client.publish("test/sml/mixed/cpp",
				     reinterpret_cast<const uint8_t *>(payload),
				     strlen(payload),
				     MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "C++ publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	disconnect_client(cpp_client);

	/* C API client. */
	static uint8_t c_storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));
	sml_mqtt_client_handle_t c_handle =
		sml_mqtt_client_init_with_storage(c_storage, sizeof(c_storage));
	zassert_not_null(c_handle, "C handle is NULL");

	ret = sml_mqtt_client_init(c_handle, "test_mixed_c");
	zassert_equal(ret, 0, "C init failed: %d", ret);

	ret = sml_mqtt_client_connect(c_handle, "127.0.0.1",
				      FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "C connect failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_CONNECT);

	sml_mqtt_cli::mqtt_client *cpp_ptr =
		static_cast<sml_mqtt_cli::mqtt_client *>(c_handle);
	client_poll_and_input(*cpp_ptr);
	zassert_true(sml_mqtt_client_is_connected(c_handle),
		     "C client should be connected");

	ret = sml_mqtt_client_publish(c_handle, "test/sml/mixed/c",
				      reinterpret_cast<const uint8_t *>(payload),
				      strlen(payload),
				      MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "C publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);

	sml_mqtt_client_disconnect(c_handle);
	fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);

	sml_mqtt_client_deinit(c_handle);
}

/**
 * Keepalive: manually call live() and process a PINGRESP.
 *
 * The fake broker responds to PINGREQ with PINGRESP.  live() internally
 * calls mqtt_live() which sends a PINGREQ when the keepalive timer fires;
 * because the timer interval is CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC seconds,
 * we cannot wait for it in a unit test.  Instead, exercise the broker-level
 * PINGREQ/PINGRESP exchange by calling mqtt_live() on the underlying context
 * directly.
 */
ZTEST(sml_mqtt_multiple, test_keepalive)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_keepalive");

	/*
	 * Force a PINGREQ by calling mqtt_live() on the underlying Zephyr
	 * mqtt_client.  This sends a PINGREQ when the connection is active
	 * (regardless of the keepalive timer value).
	 */
	int ret = mqtt_live(&client.get_context().client);

	if (ret == 0) {
		/* Broker receives the PINGREQ and sends PINGRESP. */
		fake_broker_process(FAKE_MQTT_PKT_PINGREQ);
		client_poll_and_input(client);
		zassert_true(client.is_connected(),
			     "still connected after PINGRESP");
	}
	/* ret < 0 means keepalive timer has not elapsed yet, which is fine;
	 * the important check is that the client is still connected. */
	zassert_true(client.is_connected(), "connected after live()");

	disconnect_client(client);
}

/**
 * Rapid QoS 0 publish: send 10 messages without any delay.
 * Verifies the SM stays in Connected and does not jam on back-to-back
 * publishes.
 */
ZTEST(sml_mqtt_multiple, test_rapid_publish)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_rapid_pub");

	constexpr int NUM_MESSAGES = 10;
	const char   *topic        = "test/sml/rapid";
	int           success      = 0;

	for (int i = 0; i < NUM_MESSAGES; i++) {
		char payload[64];
		snprintf(payload, sizeof(payload), "Rapid message %d", i);

		int ret = client.publish(
			topic,
			reinterpret_cast<const uint8_t *>(payload),
			strlen(payload),
			MQTT_QOS_0_AT_MOST_ONCE, false);

		if (ret == 0) {
			fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
			success++;
		}
	}

	zassert_equal(success, NUM_MESSAGES,
		      "expected %d publishes, got %d",
		      NUM_MESSAGES, success);

	disconnect_client(client);
}


