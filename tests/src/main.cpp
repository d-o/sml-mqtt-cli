/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <sml-mqtt-cli.hpp>

/*
 * fake_broker.hpp is only compiled in the loopback build (qemu / ESP32).
 * For native_sim integration builds (CONFIG_ETH_NATIVE_TAP), the fake broker
 * is not linked and its header must not be included.
 */
#if !defined(CONFIG_ETH_NATIVE_TAP)
#include "fake_broker.hpp"
#endif

/**
 * @brief Poll the client socket then call mqtt_input().
 *
 * Works for both loopback (fake broker) and real-network (native_sim) builds.
 * The caller should loop this function with appropriate timeouts; it returns
 * after no further data arrives within the current poll window.
 *
 * For loopback tests: call after every fake_broker_process() to let the SML
 * client process the response the broker just sent.
 *
 * For integration tests: call in a loop alongside mqtt_live() until the
 * expected state transition completes.
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
	 * Zephyr's TCP layer can deliver a multi-byte packet in multiple
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
 * Loopback suite registrations (qemu_riscv32 / ESP32 only).
 *
 * Each loopback test gets a clean fake broker instance via before/after hooks.
 * These are omitted for native_sim integration builds where fake_broker.cpp
 * is not compiled; the integration suite registers itself in
 * test_integration_broker.cpp.
 * ------------------------------------------------------------------------- */

#if !defined(CONFIG_ETH_NATIVE_TAP)

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

ZTEST_SUITE(sml_mqtt_basic,       NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_pubsub,      NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_qos,         NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_multiple,    NULL, NULL, broker_before, broker_after, NULL);
ZTEST_SUITE(sml_mqtt_concurrent,  NULL, NULL, broker_before, broker_after, NULL);

#endif /* !CONFIG_ETH_NATIVE_TAP */
