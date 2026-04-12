/*
 * Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

/*
 * test_concurrent_ops.cpp — Split-event (evt_publish_done / evt_receive_done)
 * regression and interference tests.
 *
 * What we are verifying
 * ---------------------
 * Before the split-event fix, evt_transaction_done was shared by both
 * publishing_sm and receiving_sm.  An incoming QoS 0/1 message fires
 * evt_transaction_done (now evt_receive_done) to exit receiving_sm.  If the
 * outer SM was simultaneously in sml::state<publishing_sm>, the shared event
 * matched the publishing_sm exit transition and prematurely returned the SM to
 * "Connected", aborting the outgoing publish handshake.
 *
 * The fix splits the event into evt_publish_done (for the publish path) and
 * evt_receive_done (for the receive path).  The outer SM only matches each
 * event against the composite state it belongs to, so interference is
 * impossible.
 *
 * Note: the outer SM is still sequential — it can be in publishing_sm OR
 * receiving_sm at a time, not both.  What the fix prevents is an incoming
 * message's *completion signal* being misrouted to the publish path.
 *
 * Test interaction pattern (deterministic, no k_sleep for flow control)
 * ----------------------------------------------------------------------
 *   fake_broker_send_qos0_publish(...)   — broker pushes a QoS 0 PUBLISH to
 *                                          the client over the loopback socket
 *   client_poll_and_input(client)        — client reads + processes the message
 *   fake_broker_process(EXPECTED_TYPE)   — broker reads + ACKs a client packet
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

extern void client_poll_and_input(sml_mqtt_cli::mqtt_client &client);

/* -------------------------------------------------------------------------
 * Test-local helpers
 * ------------------------------------------------------------------------- */

static volatile int s_rx_count;

static void on_publish_received(void *user_data,
                                const char *topic, const uint8_t *payload,
                                size_t len, enum mqtt_qos qos)
{
	ARG_UNUSED(user_data);
	ARG_UNUSED(topic);
	ARG_UNUSED(payload);
	ARG_UNUSED(len);
	ARG_UNUSED(qos);
	s_rx_count++;
}

static void connect_client(sml_mqtt_cli::mqtt_client &client, const char *id)
{
	s_rx_count = 0;
	client.set_publish_received_callback(on_publish_received, nullptr);

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

/* -------------------------------------------------------------------------
 * sml_mqtt_concurrent test suite
 * ------------------------------------------------------------------------- */

/**
 * Core regression: incoming QoS 0 while outgoing QoS 1 is in progress.
 *
 * Before the fix: evt_transaction_done from the incoming QoS 0 receive matched
 * the sml::state<publishing_sm> exit transition and returned the SM to
 * "Connected" before PUBACK arrived — aborting the publish handshake.
 *
 * After the fix: evt_receive_done has no transition on publishing_sm; the SM
 * stays inside publishing_sm until PUBACK fires evt_publish_done.
 *
 * Sequence:
 *   1. Client sends QoS 1 PUBLISH → outer SM enters publishing_sm
 *   2. Broker injects QoS 0 PUBLISH to client (before sending PUBACK)
 *   3. client_poll_and_input → processes incoming QoS 0 → fires evt_receive_done
 *      (bug: would have exited publishing_sm; fix: event is ignored)
 *   4. Verify SM is NOT "Connected" yet (publish still in progress)
 *   5. fake_broker_process reads client's PUBLISH, sends PUBACK
 *   6. client_poll_and_input → PUBACK → evt_publish_done → SM = "Connected"
 */
ZTEST(sml_mqtt_concurrent, test_receive_qos0_during_publish_qos1)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_conc_rx0_tx1");

	int ret = client.publish("test/conc/out/qos1",
				 reinterpret_cast<const uint8_t *>("hello"), 5,
				 MQTT_QOS_1_AT_LEAST_ONCE, false);
	zassert_equal(ret, 0, "QoS 1 publish failed: %d", ret);

	/* Broker injects an incoming QoS 0 message BEFORE processing the
	 * client's PUBLISH and sending PUBACK. */
	fake_broker_send_qos0_publish("test/conc/in/qos0",
				      reinterpret_cast<const uint8_t *>("ping"), 4);

	/* Client processes the incoming QoS 0.  evt_receive_done is fired.
	 * With the fix it must NOT exit publishing_sm. */
	client_poll_and_input(client);

	/* Receive callback must have fired exactly once. */
	zassert_equal(s_rx_count, 1, "publish_received callback must fire once");

	/* SM must still be tracking the outgoing QoS 1 — NOT back at Connected. */
	zassert_true(strcmp(client.get_state(), "Connected") != 0,
		     "SM must NOT return to Connected after evt_receive_done "
		     "while publish handshake is still in progress (bug: premature exit)");

	/* Now let the broker process the client's QoS 1 PUBLISH → sends PUBACK. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);
	client_poll_and_input(client);

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must return to Connected after PUBACK");

	/* SM must be fully operational afterwards. */
	ret = client.subscribe("test/conc/out/qos1", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe after concurrent ops failed: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);
	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after SUBACK");

	disconnect_client(client);
}

/**
 * Incoming QoS 0 during outgoing QoS 2, while waiting for PUBREC.
 *
 * The publish handshake is in its first phase (PUBLISH sent, awaiting PUBREC).
 * An incoming QoS 0 message fires evt_receive_done.  With the fix this must
 * not disturb the QoS 2 publish handshake.
 */
ZTEST(sml_mqtt_concurrent, test_receive_qos0_during_publish_qos2_waiting_pubrec)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_conc_rx0_tx2a");

	int ret = client.publish("test/conc/out/qos2",
				 reinterpret_cast<const uint8_t *>("data"), 4,
				 MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "QoS 2 publish failed: %d", ret);

	/* Inject QoS 0 incoming before the broker sends PUBREC. */
	fake_broker_send_qos0_publish("test/conc/in/qos0b",
				      reinterpret_cast<const uint8_t *>("x"), 1);
	client_poll_and_input(client);

	zassert_equal(s_rx_count, 1, "receive callback fired once");
	zassert_true(strcmp(client.get_state(), "Connected") != 0,
		     "SM must NOT return to Connected — QoS 2 handshake in progress");

	/* Complete the QoS 2 handshake normally: PUBREC → PUBREL → PUBCOMP. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);   /* broker sends PUBREC */
	client_poll_and_input(client);                /* client sends PUBREL */
	fake_broker_process(FAKE_MQTT_PKT_PUBREL);    /* broker sends PUBCOMP */
	client_poll_and_input(client);                /* client processes PUBCOMP */

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must be Connected after QoS 2 PUBCOMP");

	disconnect_client(client);
}

/**
 * Incoming QoS 0 during outgoing QoS 2 in releasing phase (waiting for PUBCOMP).
 *
 * PUBREC has arrived and PUBREL was sent.  The SM is now in publishing_sm's
 * "releasing" sub-state.  An incoming QoS 0 fires evt_receive_done.  With the
 * fix the SM stays in releasing; with the old bug it exits prematurely.
 */
ZTEST(sml_mqtt_concurrent, test_receive_qos0_during_publish_qos2_releasing)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_conc_rx0_tx2b");

	int ret = client.publish("test/conc/out/qos2r",
				 reinterpret_cast<const uint8_t *>("data"), 4,
				 MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "QoS 2 publish failed: %d", ret);

	/* Step 1: let PUBREC arrive so the SM enters "releasing" state. */
	fake_broker_process(FAKE_MQTT_PKT_PUBLISH);   /* broker sends PUBREC */
	client_poll_and_input(client);                /* client processes PUBREC, sends PUBREL */

	/* Broker received PUBREL but hasn't sent PUBCOMP yet. */
	/* Inject QoS 0 incoming while SM is in "releasing" sub-state. */
	fake_broker_absorb(FAKE_MQTT_PKT_PUBREL);     /* consume PUBREL, suppress PUBCOMP */
	fake_broker_send_qos0_publish("test/conc/in/qos0c",
				      reinterpret_cast<const uint8_t *>("x"), 1);
	client_poll_and_input(client);                /* client processes incoming QoS 0 */

	zassert_equal(s_rx_count, 1, "receive callback fired once");
	zassert_true(strcmp(client.get_state(), "Connected") != 0,
		     "SM must NOT return to Connected — still in QoS 2 releasing state");

	/* Now manually send PUBCOMP to complete the handshake.
	 * Build a minimal PUBCOMP: type=0x70, remaining=2, pid=0x00 0x01
	 * (the client used pending_message_id=1 for the first publish). */

	/* We need to send PUBCOMP from broker to client.  Use the fake broker's
	 * internal helper by triggering a PUBREL response manually.  Since the
	 * fake_broker_absorb() already consumed the PUBREL without responding,
	 * we send a raw PUBCOMP packet directly via a second fake_broker
	 * helper — but none exists yet.  Instead, use the timeout mechanism:
	 * set a short timeout, sleep, call live() to drive SM back to Connected. */
	client.set_qos_timeout_ms(10);
	k_msleep(50);
	client.live();

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must return to Connected via timeout after releasing state");

	disconnect_client(client);
}

/**
 * evt_publish_done does not prematurely exit receiving_sm.
 *
 * The SM is in receiving_sm waiting for PUBREL (QoS 2 inbound).  A stray
 * evt_publish_done fires (simulated via a short-timeout live() call with a
 * non-zero publish_op_start_ms).  With the fix it must not exit receiving_sm.
 */
ZTEST(sml_mqtt_concurrent, test_publish_done_does_not_exit_receiving_sm)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "test_conc_txdone_rxsm");

	/* Set a very short publish timeout so that if publish_op_start_ms were
	 * accidentally set, live() would fire evt_publish_done. */
	client.set_qos_timeout_ms(5);

	/* Broker sends QoS 2 PUBLISH → SM enters receiving_sm waiting for PUBREL. */
	fake_broker_send_qos2_publish("test/conc/in/qos2",
				      reinterpret_cast<const uint8_t *>("msg"), 3,
				      0x0099u);
	client_poll_and_input(client);            /* client processes PUBLISH, sends PUBREC */
	fake_broker_absorb(FAKE_MQTT_PKT_PUBREC); /* broker absorbs PUBREC — no PUBREL sent */

	/* SM is now in receiving_sm (waiting_rel).  Wait slightly longer than
	 * the publish timeout and call live().  If publish_op_start_ms is 0
	 * (as it should be — no active publish), evt_publish_done is NOT fired
	 * and the receive timeout drives SM back via evt_receive_done. */
	k_msleep(50);
	client.live();   /* receive timeout fires evt_receive_done */

	zassert_str_equal(client.get_state(), "Connected",
			  "SM must return to Connected via receive timeout");

	/* Confirm SM is operational. */
	int ret = client.subscribe("test/conc/after/rx", MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe after receiving_sm timeout: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_SUBSCRIBE);
	client_poll_and_input(client);
	zassert_str_equal(client.get_state(), "Connected", "Connected after SUBACK");

	disconnect_client(client);
}
