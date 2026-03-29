/*
 * Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 *
 * fake_broker.hpp - In-process MQTT 3.1.1 fake broker for Zephyr loopback tests.
 *
 * Modelled after zephyr/tests/net/lib/mqtt/v3_1_1/mqtt_client/src/main.c
 *
 * Approach: poll-driven, no broker thread.  The test function interleaves
 *   fake_broker_process(EXPECTED_TYPE)  ->  client_poll_and_input()
 * for each protocol round-trip.  This is the same pattern used by Zephyr's
 * own CI for the upstream mqtt_client tests.
 *
 * Constraints:
 *   - IPv4 / 127.0.0.1:FAKE_BROKER_PORT only
 *   - Single simultaneous client connection
 *   - Static storage, no heap
 */

#ifndef FAKE_BROKER_HPP
#define FAKE_BROKER_HPP

#include <cstdint>

/* -------------------------------------------------------------------------
 * MQTT 3.1.1 wire-format packet type constants (upper nibble only).
 * Defined locally to avoid including private Zephyr MQTT internals.
 * Values match MQTT 3.1.1 specification table 2.1.
 * ------------------------------------------------------------------------- */
#define FAKE_MQTT_PKT_CONNECT     UINT8_C(0x10)
#define FAKE_MQTT_PKT_CONNACK     UINT8_C(0x20)
#define FAKE_MQTT_PKT_PUBLISH     UINT8_C(0x30)
#define FAKE_MQTT_PKT_PUBACK      UINT8_C(0x40)
#define FAKE_MQTT_PKT_PUBREC      UINT8_C(0x50)
#define FAKE_MQTT_PKT_PUBREL      UINT8_C(0x60)
#define FAKE_MQTT_PKT_PUBCOMP     UINT8_C(0x70)
#define FAKE_MQTT_PKT_SUBSCRIBE   UINT8_C(0x80)
#define FAKE_MQTT_PKT_SUBACK      UINT8_C(0x90)
#define FAKE_MQTT_PKT_UNSUBSCRIBE UINT8_C(0xA0)
#define FAKE_MQTT_PKT_UNSUBACK    UINT8_C(0xB0)
#define FAKE_MQTT_PKT_PINGREQ     UINT8_C(0xC0)
#define FAKE_MQTT_PKT_PINGRSP     UINT8_C(0xD0)
#define FAKE_MQTT_PKT_DISCONNECT  UINT8_C(0xE0)

/** TCP port the fake broker listens on. */
#define FAKE_BROKER_PORT          1883

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the fake broker.
 *
 * Creates a TCP server socket, binds to 0.0.0.0:FAKE_BROKER_PORT, and
 * calls listen().  The accept() is deferred to the first
 * fake_broker_process() call.
 *
 * Call once per test (e.g. in a ztest before hook).
 * Triggers a zassert failure on any socket error.
 */
void fake_broker_init(void);

/**
 * @brief Destroy the fake broker.
 *
 * Closes both the server and client sockets and resets all internal state.
 * Call at the end of each test (e.g. in a ztest after hook).
 */
void fake_broker_destroy(void);

/**
 * @brief Drive one round of broker I/O.
 *
 * Blocks (via zsock_poll) until one complete MQTT packet arrives from the
 * client, validates its type, then responds per the MQTT 3.1.1 protocol:
 *
 *   CONNECT     -> sends CONNACK (return code 0, no session present)
 *   SUBSCRIBE   -> stores topic, sends SUBACK (granted QoS = requested)
 *   UNSUBSCRIBE -> clears stored topic, sends UNSUBACK
 *   PUBLISH     -> sends PUBACK (QoS 1) or PUBREC (QoS 2) if applicable;
 *                  if topic matches the stored subscription, echoes the
 *                  full PUBLISH packet back to the client
 *   PUBREL      -> sends PUBCOMP
 *   PINGREQ     -> sends PINGRESP
 *   DISCONNECT  -> closes client socket
 *
 * @param expected_type  One of the FAKE_MQTT_PKT_* constants.
 *                       Triggers a zassert failure if the received packet
 *                       type (upper nibble) does not match.
 */
void fake_broker_process(uint8_t expected_type);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_BROKER_HPP */
