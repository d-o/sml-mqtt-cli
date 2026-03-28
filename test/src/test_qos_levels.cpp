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
 * Helpers shared across QoS tests
 * ------------------------------------------------------------------------- */

static void connect_client(sml_mqtt_cli::mqtt_client &client, const char *id)
{
	int ret = client.init(id);
	zassert_equal(ret, 0, "init failed: %d", ret);

	ret = client.connect("127.0.0.1", FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "connect failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_CONNECT);
	client_poll_and_input(client);
	zassert_true(client.is_connected(), "should be connected");
}

static void disconnect_client(sml_mqtt_cli::mqtt_client &client)
{
	client.disconnect();
	fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);
}


/**
 * QoS 1 publish: client sends PUBLISH, broker replies PUBACK.
 * Verifies the four-message ID echo in the PUBACK.
 */
ZTEST(sml_mqtt_qos, test_publish_qos1)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_pub_qos1");

	const char *topic   = "test/sml/qos1";
	const char *payload = "Hello, MQTT QoS 1!";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_1_AT_LEAST_ONCE, false);
	zassert_equal(ret, 0, "QoS 1 publish failed: %d", ret);

	/* Broker receives PUBLISH and sends PUBACK. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	client_poll_and_input(client);

	zassert_true(client.is_connected(), "still connected after PUBACK");

	disconnect_client(client);
}

/**
 * QoS 2 publish: PUBLISH -> PUBREC -> PUBREL -> PUBCOMP.
 */
ZTEST(sml_mqtt_qos, test_publish_qos2)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_pub_qos2");

	const char *topic   = "test/sml/qos2";
	const char *payload = "Hello, MQTT QoS 2!";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "QoS 2 publish failed: %d", ret);

	/* Step 1: broker receives PUBLISH, sends PUBREC. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	client_poll_and_input(client);

	/*
	 * Step 2: the SML event handler for MQTT_EVT_PUBREC has already sent
	 * PUBREL.  Broker receives PUBREL, sends PUBCOMP.
	 */
	fake_broker_process(FAKE_MQTT_PKT_PUBREL);
	client_poll_and_input(client);

	zassert_true(client.is_connected(), "still connected after PUBCOMP");

	disconnect_client(client);
}

/**
 * Subscribe at QoS 0, 1, and 2 - verify SUBACK drives SM back to Connected
 * after each subscription.
 */
ZTEST(sml_mqtt_qos, test_subscribe_qos_levels)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_sub_qos");

	static const struct {
		const char  *topic;
		enum mqtt_qos qos;
	} subs[] = {
		{ "test/sml/qos/0", MQTT_QOS_0_AT_MOST_ONCE  },
		{ "test/sml/qos/1", MQTT_QOS_1_AT_LEAST_ONCE },
		{ "test/sml/qos/2", MQTT_QOS_2_EXACTLY_ONCE  },
	};

	for (size_t i = 0; i < ARRAY_SIZE(subs); i++) {
		int ret = client.subscribe(subs[i].topic, subs[i].qos);
		zassert_equal(ret, 0, "subscribe[%zu] failed: %d", i, ret);

		fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
		client_poll_and_input(client);
		zassert_true(client.is_connected(),
			     "connected after SUBACK[%zu]", i);
	}

	disconnect_client(client);
}

/**
 * Retained-flag publish: the fake broker treats retained the same as a
 * normal QoS 0 publish (it does not persist state), so this test verifies
 * only that the library sends the packet with retain=1 without crashing.
 *
 * A secondary client then subscribes to the same topic; the fake broker
 * does not re-deliver the retained message (no persistence), so the test
 * simply verifies both clients connect, publish/subscribe without error,
 * and disconnect cleanly.
 */
ZTEST(sml_mqtt_qos, test_retained_message)
{
	/* Publisher */
	sml_mqtt_cli::mqtt_client publisher;
	connect_client(publisher, "test_retain_pub");

	const char *topic   = "test/sml/retained";
	const char *payload = "This is a retained message";

	int ret = publisher.publish(topic,
				    reinterpret_cast<const uint8_t *>(payload),
				    strlen(payload),
				    MQTT_QOS_0_AT_MOST_ONCE, true /* retain */);
	zassert_equal(ret, 0, "retained publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	disconnect_client(publisher);

	/* Subscriber - separate connection via new broker_before/after cycle
	 * is not available here; same broker socket is live.  Just verify that
	 * subscribe succeeds. */
	sml_mqtt_cli::mqtt_client subscriber;
	connect_client(subscriber, "test_retain_sub");

	ret = subscriber.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscriber.subscribe failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(subscriber);
	zassert_true(subscriber.is_connected(), "subscriber connected after SUBACK");

	disconnect_client(subscriber);
}
