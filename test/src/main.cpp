/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include "fake_broker.hpp"
#include <sml-mqtt-cli.hpp>

/* Broker address used by all tests - matches fake_broker_init() binding. */
#define TEST_BROKER_HOST "127.0.0.1"
#define TEST_BROKER_PORT FAKE_BROKER_PORT
#define TEST_USE_TLS     false

/**
 * @brief Poll the client socket then call mqtt_input().
 *
 * Use this after every fake_broker_process() call to let the SML client
 * process the response the broker just sent.  Replaces k_sleep() - no
 * timing dependency.
 *
 * @param client  Reference to the sml_mqtt_cli::mqtt_client instance.
 */
void client_poll_and_input(sml_mqtt_cli::mqtt_client &client)
{
	int fd = client.get_context().client.transport.tcp.sock;

	if (fd < 0) {
		return;
	}

	struct zsock_pollfd pfd = {
		.fd      = fd,
		.events  = ZSOCK_POLLIN,
		.revents = 0,
	};

	/*
	 * Zephyr's TCP loopback can deliver a multi-byte packet in multiple
	 * fragments.  Loop: wait up to 200 ms for data, call mqtt_input(),
	 * then immediately check if more data has arrived (50 ms timeout).
	 * Stop when no further data comes in.
	 */
	int timeout_ms = 200;

	while (true) {
		pfd.revents = 0;
		int ret = zsock_poll(&pfd, 1, timeout_ms);

		if (ret <= 0 || !(pfd.revents & ZSOCK_POLLIN)) {
			break;
		}

		mqtt_input(&client.get_context().client);

		/* Switch to short timeout for subsequent fragments. */
		timeout_ms = 50;
	}
}

/* -------------------------------------------------------------------------
 * Per-suite before/after hooks - each test gets a clean broker instance.
 * ------------------------------------------------------------------------- */

static void broker_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_broker_init();
}

static void broker_after(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_broker_destroy();
}

/* -------------------------------------------------------------------------
 * Suite registrations
 * ------------------------------------------------------------------------- */

ZTEST_SUITE(sml_mqtt_basic,    NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_pubsub,   NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_qos,      NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_multiple, NULL, NULL, broker_before, broker_after, NULL);
