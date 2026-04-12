/*
 * Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 *
 * test_integration_broker.cpp - Integration tests against a real MQTT broker.
 *
 * These tests are compiled ONLY for native_sim (CONFIG_ETH_NATIVE_TAP=y).
 * The Zephyr app runs as a native Linux process.  The ETH_NATIVE_TAP driver
 * creates a virtual zeth0 Ethernet interface on the build host.  The Zephyr
 * app gets static IP 192.0.2.2/24; the host TAP interface gets 192.0.2.1/24.
 * The default gateway for the app is 192.0.2.1, so with IP forwarding and
 * MASQUERADE enabled on the host the app can reach any broker the host can.
 *
 * Three supported use cases (see also run_integration_tests.sh --help):
 *
 *   1. Broker on build machine (default)
 *      Mosquitto binds to 0.0.0.0:1883 on the host.  The app connects to
 *      the TAP interface IP (192.0.2.1).  No routing changes needed.
 *
 *      west build -p always -s sml-mqtt-cli/tests -b native_sim \
 *        -- -DZEPHYR_MODULES="..."
 *      sudo tests/scripts/run_integration_tests.sh
 *
 *   2. Broker on the same local network
 *      Enable IP forwarding and MASQUERADE on the host (--enable-routing).
 *      Build with the real broker IP so the app connects directly.
 *
 *      west build ... -DSML_MQTT_TEST_BROKER_HOST="192.168.1.50"
 *      sudo tests/scripts/run_integration_tests.sh \
 *        --broker-host 192.168.1.50 --enable-routing --no-local-broker
 *
 *   3. Broker on a remote host (internet)
 *      Same as LAN - IP forwarding + MASQUERADE lets 192.0.2.2 route via the
 *      host's uplink.  Use the remote hostname or IP as the broker address.
 *
 *      west build ... -DSML_MQTT_TEST_BROKER_HOST="mqtt.example.com"
 *      sudo tests/scripts/run_integration_tests.sh \
 *        --broker-host mqtt.example.com --enable-routing --no-local-broker
 *
 * Broker address defaults (build-time, set by CMakeLists.txt):
 *   SML_MQTT_TEST_BROKER_HOST  "192.0.2.1"   (host-side TAP IP)
 *   SML_MQTT_TEST_BROKER_PORT  1883
 *
 * Key differences from the loopback (fake broker) tests:
 *   - No fake_broker_process() calls.  The real broker handles all CONNACK,
 *     PUBACK, SUBACK etc. automatically and asynchronously.
 *   - Polling helpers use k_uptime_get() deadline loops instead of
 *     relying on synchronous interleaving.
 *   - mqtt_live() is called in every poll iteration so keepalive timers
 *     and QoS retry timers fire correctly.
 */

#ifdef CONFIG_ETH_NATIVE_TAP

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/util.h>
#include <sml-mqtt-cli.hpp>
/* sml-mqtt-cli.hpp already emits LOG_MODULE_DECLARE(sml_mqtt_cli, ...) so
 * this TU can use LOG_INF/LOG_DBG etc. directly without a second declaration. */

/* Declared in main.cpp - drives one round of socket I/O. */
extern void client_poll_and_input(sml_mqtt_cli::mqtt_client &client);

/* -------------------------------------------------------------------------
 * Broker address - injected by CMakeLists.txt at build time.
 *
 * Default: 192.0.2.1:1883 (host-side TAP IP, local Mosquitto).
 * Override at build time: west build ... -DSML_MQTT_TEST_BROKER_HOST=<ip>
 * ------------------------------------------------------------------------- */

#ifndef SML_MQTT_TEST_BROKER_HOST
#define SML_MQTT_TEST_BROKER_HOST "192.0.2.1"
#endif

#ifndef SML_MQTT_TEST_BROKER_PORT
#define SML_MQTT_TEST_BROKER_PORT 1883
#endif

/*
 * Generous timeout for real-network operations.  A local broker typically
 * responds in < 10 ms; 5 000 ms accommodates LAN brokers and CI jitter.
 * For remote (internet) brokers you may wish to increase this.
 */
#define INTEG_TIMEOUT_MS   5000

/* -------------------------------------------------------------------------
 * Shared receive tracking - reset in the suite before hook.
 * ------------------------------------------------------------------------- */

static int     g_rx_count;
static char    g_rx_topic[CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN];
static uint8_t g_rx_payload[CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN];
static size_t  g_rx_payload_len;

static void on_message_received(void *user_data, const char *topic,
				const uint8_t *payload, size_t len,
				enum mqtt_qos qos)
{
	ARG_UNUSED(user_data);
	ARG_UNUSED(qos);

	g_rx_count++;

	strncpy(g_rx_topic, topic ? topic : "", sizeof(g_rx_topic) - 1);
	g_rx_topic[sizeof(g_rx_topic) - 1] = '\0';

	g_rx_payload_len = MIN(len, sizeof(g_rx_payload));
	memcpy(g_rx_payload, payload, g_rx_payload_len);
}

static void reset_rx(void)
{
	g_rx_count       = 0;
	g_rx_payload_len = 0;
	memset(g_rx_topic,   0, sizeof(g_rx_topic));
	memset(g_rx_payload, 0, sizeof(g_rx_payload));
}

/* -------------------------------------------------------------------------
 * Polling helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Drive the MQTT event loop for a fixed duration.
 *
 * Interleaves mqtt_live() (keepalive / QoS retry timers) with
 * client_poll_and_input() (incoming data -> mqtt_input()).  Use this after
 * fire-and-forget operations (QoS 0 publish) and as the inner loop for
 * condition-based wait helpers below.
 */
static void poll_loop(sml_mqtt_cli::mqtt_client &client, int duration_ms)
{
	int64_t deadline = k_uptime_get() + duration_ms;

	while (k_uptime_get() < deadline) {
		mqtt_live(&client.get_context().client);
		client_poll_and_input(client);
		k_sleep(K_MSEC(10));
	}
}

/**
 * @brief Poll until the SM reaches @p state or the timeout expires.
 *
 * Reliable for states in the main SM (e.g. "Connected", "Subscribing",
 * "Unsubscribing", "Connecting", "Disconnected").  Not suitable for
 * sub-state names inside the publishing or receiving submachines.
 *
 * @return true  if @p state was reached before the deadline.
 */
static bool wait_for_state(sml_mqtt_cli::mqtt_client &client, const char *state,
			   int timeout_ms = INTEG_TIMEOUT_MS)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		mqtt_live(&client.get_context().client);
		client_poll_and_input(client);
		if (strcmp(client.get_state(), state) == 0) {
			return true;
		}
		k_sleep(K_MSEC(10));
	}
	return false;
}

/**
 * @brief Poll until g_rx_count reaches @p expected or the timeout expires.
 *
 * Drives both the subscriber's mqtt_live() and client_poll_and_input() while
 * waiting.
 *
 * @return true  if the expected receive count was reached in time.
 */
static bool wait_rx(sml_mqtt_cli::mqtt_client &subscriber, int expected,
		    int timeout_ms = INTEG_TIMEOUT_MS)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (g_rx_count < expected && k_uptime_get() < deadline) {
		mqtt_live(&subscriber.get_context().client);
		client_poll_and_input(subscriber);
		k_sleep(K_MSEC(10));
	}
	return g_rx_count >= expected;
}

/* -------------------------------------------------------------------------
 * Per-test connect / disconnect helpers
 * ------------------------------------------------------------------------- */

static void connect_client(sml_mqtt_cli::mqtt_client &client, const char *id)
{
	int ret = client.init(id);
	zassert_equal(ret, 0, "init(%s) failed: %d", id, ret);

	ret = client.connect(SML_MQTT_TEST_BROKER_HOST, SML_MQTT_TEST_BROKER_PORT, false);
	zassert_equal(ret, 0, "connect(%s) TCP failed: %d", id, ret);

	bool ok = wait_for_state(client, "Connected");
	zassert_true(ok, "%s: timed out waiting for CONNACK from Mosquitto", id);
}

static void disconnect_client(sml_mqtt_cli::mqtt_client &client)
{
	(void)client.disconnect();
	/* Allow Mosquitto to close the session cleanly before the next test. */
	k_sleep(K_MSEC(150));
}

/* -------------------------------------------------------------------------
 * Suite setup: wait for the TAP interface to be assigned its static IP.
 *
 * net_config_init_app() is called automatically by Zephyr during system
 * init when CONFIG_NET_CONFIG_SETTINGS=y.  We just need to wait for it.
 * ------------------------------------------------------------------------- */

static void *integ_suite_setup(void)
{
	LOG_INF("Waiting for network interface...");

	struct net_if *iface = net_if_get_default();

	int64_t deadline = k_uptime_get() + (CONFIG_NET_CONFIG_INIT_TIMEOUT * MSEC_PER_SEC);

	while (!net_if_is_up(iface) && k_uptime_get() < deadline) {
		k_sleep(K_MSEC(100));
	}

	zassert_true(net_if_is_up(iface),
		     "TAP interface did not come up within %d s - "
		     "run tests/scripts/run_integration_tests.sh first",
		     CONFIG_NET_CONFIG_INIT_TIMEOUT);

	LOG_INF("[integ] Network interface up.  Broker: %s:%d",
		SML_MQTT_TEST_BROKER_HOST, SML_MQTT_TEST_BROKER_PORT);

	return NULL;
}

static void integ_before(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_rx();
}

/* -------------------------------------------------------------------------
 * Integration tests
 * ------------------------------------------------------------------------- */

/**
 * Connect and disconnect against the real Mosquitto broker.
 *
 * Validates the full TCP + MQTT CONNECT / CONNACK / DISCONNECT handshake
 * with a real broker.
 */
ZTEST(sml_mqtt_integration, test_integ_connect_disconnect)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_connect");

	zassert_true(client.is_connected(), "must be connected after CONNACK");

	disconnect_client(client);
	zassert_false(client.is_connected(), "must be disconnected after DISCONNECT");
}

/**
 * QoS 0 publish - fire-and-forget; no ACK from broker.
 *
 * Verifies that a QoS 0 PUBLISH is accepted without error and the client
 * remains connected afterwards.
 */
ZTEST(sml_mqtt_integration, test_integ_publish_qos0)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_qos0");

	const char *topic   = "sml/integ/qos0";
	const char *payload = "qos0-from-zephyr";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "QoS 0 publish failed: %d", ret);

	/* No ACK expected; allow the packet to drain. */
	poll_loop(client, 200);

	zassert_true(client.is_connected(), "still connected after QoS 0 publish");

	disconnect_client(client);
}

/**
 * QoS 1 publish - Mosquitto sends PUBACK; SM must return to Connected.
 *
 * Waits for the state machine to return to Connected (PUBACK processed) and
 * verifies no error state or unexpected disconnect occurred.
 */
ZTEST(sml_mqtt_integration, test_integ_publish_qos1)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_qos1");

	const char *topic   = "sml/integ/qos1";
	const char *payload = "qos1-from-zephyr";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_1_AT_LEAST_ONCE, false);
	zassert_equal(ret, 0, "QoS 1 publish failed: %d", ret);

	/* publishing_sm reaches sml::X on PUBACK; outer SM returns to Connected. */
	bool ok = wait_for_state(client, "Connected");
	zassert_true(ok, "timed out waiting for PUBACK from Mosquitto");

	zassert_true(client.is_connected(), "still connected after QoS 1 PUBACK");

	disconnect_client(client);
}

/**
 * QoS 2 publish - four-way handshake with Mosquitto.
 *
 * Sequence: PUBLISH -> PUBREC -> PUBREL -> PUBCOMP.
 * The SML event handler sends PUBREL automatically on PUBREC; the SM
 * transitions through qos2 -> releasing -> idle in the publishing submachine.
 */
ZTEST(sml_mqtt_integration, test_integ_publish_qos2)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_qos2");

	const char *topic   = "sml/integ/qos2";
	const char *payload = "qos2-from-zephyr";

	int ret = client.publish(topic,
				 reinterpret_cast<const uint8_t *>(payload),
				 strlen(payload),
				 MQTT_QOS_2_EXACTLY_ONCE, false);
	zassert_equal(ret, 0, "QoS 2 publish failed: %d", ret);

	/* publishing_sm reaches sml::X on PUBCOMP; outer SM returns to Connected. */
	bool ok = wait_for_state(client, "Connected");
	zassert_true(ok, "timed out waiting for PUBCOMP from Mosquitto");

	zassert_true(client.is_connected(), "still connected after QoS 2 PUBCOMP");

	disconnect_client(client);
}

/**
 * Subscribe then receive a message routed by Mosquitto.
 *
 * One client subscribes, a second publishes to the same topic.  Mosquitto
 * routes the PUBLISH to the subscriber.  Validates end-to-end message
 * delivery with the real broker, including topic and payload correctness.
 */
ZTEST(sml_mqtt_integration, test_integ_subscribe_receive)
{
	const char *topic   = "sml/integ/loopback";
	const char *payload = "hello-from-mosquitto";

	/* -- Subscriber -- */
	sml_mqtt_cli::mqtt_client subscriber;
	subscriber.set_publish_received_callback(on_message_received, nullptr);
	connect_client(subscriber, "sml_integ_sub");

	int ret = subscriber.subscribe(topic, MQTT_QOS_0_AT_MOST_ONCE);
	zassert_equal(ret, 0, "subscribe() failed: %d", ret);

	/* Wait for Subscribing -> Connected transition (SUBACK received). */
	bool ok = wait_for_state(subscriber, "Connected", 2000);
	zassert_true(ok, "timed out waiting for SUBACK from Mosquitto");

	/* -- Publisher -- */
	sml_mqtt_cli::mqtt_client publisher;
	connect_client(publisher, "sml_integ_pub");

	ret = publisher.publish(topic,
				reinterpret_cast<const uint8_t *>(payload),
				strlen(payload),
				MQTT_QOS_0_AT_MOST_ONCE, false);
	zassert_equal(ret, 0, "publish() failed: %d", ret);

	/* Drive subscriber's event loop until the message arrives. */
	ok = wait_rx(subscriber, 1);
	zassert_true(ok, "timed out waiting for published message");

	zassert_equal(g_rx_count, 1, "callback must fire exactly once");
	zassert_str_equal(g_rx_topic, topic, "received topic mismatch");
	zassert_equal(g_rx_payload_len, strlen(payload),
		      "received payload length mismatch");
	zassert_mem_equal(g_rx_payload, payload, strlen(payload),
			  "received payload content mismatch");

	disconnect_client(publisher);
	disconnect_client(subscriber);
}

/**
 * Subscribe at all three QoS levels; verify SUBACK accepted each.
 *
 * Mosquitto grants the requested QoS for each subscription; the SML client
 * must return to Connected after each SUBACK.
 */
ZTEST(sml_mqtt_integration, test_integ_subscribe_qos_levels)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_sub_qos");

	static const struct {
		const char   *topic;
		enum mqtt_qos qos;
	} subs[] = {
		{ "sml/integ/sub/qos0", MQTT_QOS_0_AT_MOST_ONCE  },
		{ "sml/integ/sub/qos1", MQTT_QOS_1_AT_LEAST_ONCE },
		{ "sml/integ/sub/qos2", MQTT_QOS_2_EXACTLY_ONCE  },
	};

	for (size_t i = 0; i < ARRAY_SIZE(subs); i++) {
		int ret = client.subscribe(subs[i].topic, subs[i].qos);
		zassert_equal(ret, 0, "subscribe[%zu] failed: %d", i, ret);

		bool ok = wait_for_state(client, "Connected", 2000);
		zassert_true(ok, "timed out waiting for SUBACK[%zu]", i);
	}

	disconnect_client(client);
}

/**
 * Keepalive: client stays connected while driving the MQTT event loop.
 *
 * Verifies that mqtt_live() + client_poll_and_input() keep the session alive
 * over a 3 s window.  The default keepalive is 60 s so no PINGREQ is expected
 * to fire here; the test simply confirms no unexpected disconnect occurs.
 */
ZTEST(sml_mqtt_integration, test_integ_keepalive)
{
	sml_mqtt_cli::mqtt_client client;
	connect_client(client, "sml_integ_keepalive");

	/* Stay connected for 3 s driving the MQTT event loop. */
	poll_loop(client, 3000);

	zassert_true(client.is_connected(),
		     "client disconnected unexpectedly during keepalive period");

	disconnect_client(client);
}

/* -------------------------------------------------------------------------
 * Suite registration
 * ------------------------------------------------------------------------- */

/*
 * integ_suite_setup (third parameter) waits for the TAP interface to be up
 * before any test runs - avoids connect timeouts if the interface is still
 * being configured.
 */
ZTEST_SUITE(sml_mqtt_integration, NULL, integ_suite_setup,
	    integ_before, NULL, NULL);

#endif /* CONFIG_ETH_NATIVE_TAP */
