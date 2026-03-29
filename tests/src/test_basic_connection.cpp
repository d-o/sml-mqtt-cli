/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

/* Helpers declared in main.cpp */
extern void client_poll_and_input(sml_mqtt_cli::mqtt_client &client);

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/**
 * Client construction and init - no network needed.
 */
ZTEST(sml_mqtt_basic, test_client_creation)
{
	sml_mqtt_cli::mqtt_client client;

	int ret = client.init("test_client_basic");
	zassert_equal(ret, 0, "init failed: %d", ret);
	zassert_false(client.is_connected(), "should start disconnected");
	zassert_not_null(client.get_state(), "state must not be NULL");
}

/**
 * Null and over-length client IDs must be rejected before any network call.
 */
ZTEST(sml_mqtt_basic, test_invalid_init)
{
	sml_mqtt_cli::mqtt_client client;

	zassert_not_equal(client.init(nullptr), 0,
			  "NULL client ID must fail");

	char long_id[CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN + 10];
	memset(long_id, 'A', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';
	zassert_not_equal(client.init(long_id), 0,
			  "over-length client ID must fail");
}

/**
 * Full connect/disconnect round-trip via fake broker.
 *
 * Sequence:
 *   client.connect()
 *   -> broker accepts TCP, receives CONNECT, sends CONNACK
 *   client_poll_and_input() -> MQTT_EVT_CONNACK -> SM: Connecting -> Connected
 *   client.disconnect()
 *   -> broker receives DISCONNECT, closes socket
 */
ZTEST(sml_mqtt_basic, test_connect_disconnect)
{
	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("test_connect_001");
	zassert_equal(ret, 0, "init failed");

	ret = client.connect("127.0.0.1", FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "connect returned: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_CONNECT);
	client_poll_and_input(client);

	zassert_true(client.is_connected(), "should be connected after CONNACK");

	ret = client.disconnect();
	zassert_equal(ret, 0, "disconnect returned: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);

	zassert_false(client.is_connected(), "should be disconnected");
}

/**
 * Null hostname must be rejected before any socket call.
 */
ZTEST(sml_mqtt_basic, test_invalid_connect)
{
	sml_mqtt_cli::mqtt_client client;
	client.init("test_invalid_conn");

	int ret = client.connect(nullptr, FAKE_BROKER_PORT, false);
	zassert_not_equal(ret, 0, "NULL hostname must fail");
}

/**
 * C API: placement-new into static storage, init, deinit - no heap.
 */
ZTEST(sml_mqtt_basic, test_c_api_creation)
{
	static uint8_t storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));

	sml_mqtt_client_handle_t h =
		sml_mqtt_client_init_with_storage(storage, sizeof(storage));
	zassert_not_null(h, "handle must not be NULL");

	int ret = sml_mqtt_client_init(h, "test_c_api_001");
	zassert_equal(ret, 0, "C API init failed: %d", ret);
	zassert_false(sml_mqtt_client_is_connected(h),
		      "should start disconnected");

	sml_mqtt_client_deinit(h);
}

/**
 * C API connect/disconnect via fake broker.
 */
ZTEST(sml_mqtt_basic, test_c_api_connect)
{
	static uint8_t storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));

	sml_mqtt_client_handle_t h =
		sml_mqtt_client_init_with_storage(storage, sizeof(storage));
	zassert_not_null(h, "handle must not be NULL");

	int ret = sml_mqtt_client_init(h, "test_c_api_conn");
	zassert_equal(ret, 0, "C API init failed");

	ret = sml_mqtt_client_connect(h, "127.0.0.1", FAKE_BROKER_PORT, false);
	zassert_equal(ret, 0, "C API connect returned: %d", ret);

	fake_broker_process(FAKE_MQTT_PKT_CONNECT);

	/* Poll/input via the C++ object underlying the handle. */
	sml_mqtt_cli::mqtt_client *cpp =
		static_cast<sml_mqtt_cli::mqtt_client *>(h);
	client_poll_and_input(*cpp);

	zassert_true(sml_mqtt_client_is_connected(h),
		     "C API: should be connected");

	ret = sml_mqtt_client_disconnect(h);
	zassert_equal(ret, 0, "C API disconnect returned: %d", ret);
	fake_broker_process(FAKE_MQTT_PKT_DISCONNECT);

	sml_mqtt_client_deinit(h);
}
