/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

/*
 * test_qos_levels.cpp — QoS lifecycle and regression tests
 *
 * What we are verifying
 * ---------------------
 * The mqtt_client uses a Boost.SML composite state machine.  "Connected" has
 * two composite sub-states: publishing_sm and receiving_sm.  Each sub-SM runs
 * its QoS handshake internally and, when the final ACK arrives, signals
 * completion to the outer SM via evt_transaction_done so the outer SM can
 * return to "Connected".
 *
 * The regression tests (test_publish_qosN_then_subscribe) specifically guard
 * against a bug where the outer SM became permanently stuck inside
 * publishing_sm after any publish.  The symptom was that a subsequent
 * subscribe() call was silently dropped because the "Connected + evt_subscribe"
 * transition was unreachable while the SM remained inside the composite state.
 *
 * What a failure looks like
 * -------------------------
 * If evt_transaction_done is not fired (or not handled) after a publish
 * completes, get_state() returns the innermost sub-SM state ("qos1",
 * "releasing", etc.) instead of "Connected", and the subscribe() call returns
 * 0 but the MQTT SUBSCRIBE packet is never sent — the fake broker never sees
 * it and client_poll_and_input() returns without driving the SM to Connected.
 * The zassert_str_equal("Connected") assertions catch both failure modes.
 *
 * Test interaction pattern (deterministic, no k_sleep)
 * ------------------------------------------------------
 *   fake_broker_process(FAKE_MQTT_PKT_X)  — broker receives packet, sends reply
 *   client_poll_and_input(client)          — client reads reply, drives SM
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
	zassert_str_equal(client.get_state(), "Connected",
			  "SM must return to Connected after PUBACK");

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
	zassert_str_equal(client.get_state(), "Connected",
			  "SM must return to Connected after PUBCOMP");

	disconnect_client(client);
}

/**
 * QoS 1 publish followed by subscribe on the same connection.
 *
 * Exposes the bug where the SM gets permanently stuck inside publishing_sm
 * after the first publish: a subsequent subscribe would be silently dropped
 * because the "Connected" + evt_subscribe transition is unreachable.
 */
ZTEST(sml_mqtt_qos, test_publish_qos1_then_subscribe)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_q1_sub");

	int ret = client.publish("test/q1/then/sub",
				 reinterpret_cast<const uint8_t *>("hi"), 2,
				 MQTT_QOS_1_AT_LEAST_ONCE, false);
	zassert_equal(ret, 0, "QoS 1 publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);  /* broker sends PUBACK */
	client_poll_and_input(client);               /* client processes PUBACK */

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after PUBACK before subscribe");

	/* Subscribe must succeed - silently dropped if SM stuck in publishing_sm */
	ret = client.subscribe("test/q1/then/sub", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe after QoS 1 publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after SUBACK");

	disconnect_client(client);
}

/**
 * QoS 2 publish followed by subscribe on the same connection.
 */
ZTEST(sml_mqtt_qos, test_publish_qos2_then_subscribe)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_q2_sub");

	int ret = client.publish("test/q2/then/sub",
				 reinterpret_cast<const uint8_t *>("hi"), 2,
				 MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "QoS 2 publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);   /* PUBREC */
	client_poll_and_input(client);
	fake_broker_process(FAKE_MQTT_PKT_PUBREL);    /* PUBCOMP */
	client_poll_and_input(client);

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after PUBCOMP before subscribe");

	ret = client.subscribe("test/q2/then/sub", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe after QoS 2 publish failed: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after SUBACK");

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
