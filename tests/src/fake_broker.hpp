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
#include <stddef.h>

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

/**
 * @brief Receive one packet from the client but suppress the normal response.
 *
 * Identical to fake_broker_process() except no reply is sent back.  Use this
 * to simulate a broker that receives a PUBLISH/PUBREL/PUBREC but never ACKs,
 * leaving the client's QoS state machine waiting indefinitely — useful for
 * testing the per-operation timeout path.
 *
 * @param expected_type  One of the FAKE_MQTT_PKT_* constants.
 */
void fake_broker_absorb(uint8_t expected_type);

/**
 * @brief Send a QoS 0 PUBLISH packet from the broker to the client.
 *
 * The client's next mqtt_input() call will process the packet, invoke the
 * publish_received callback, and fire evt_receive_done (no ACK required for
 * QoS 0 — the client sends nothing back to the broker).
 *
 * @param topic      Topic string (null-terminated).
 * @param payload    Payload bytes (may be NULL if len == 0).
 * @param len        Payload length in bytes.
 */
void fake_broker_send_qos0_publish(const char *topic, const uint8_t *payload,
                                   size_t len);

/**
 * @brief Send a QoS 1 PUBLISH packet from the broker to the client.
 *
 * The client's next mqtt_input() call will process the packet, send PUBACK
 * back to the broker, invoke the publish_received callback, and fire
 * evt_receive_done.  The broker must read the PUBACK from its socket
 * (e.g. via fake_broker_absorb(FAKE_MQTT_PKT_PUBACK)).
 *
 * @param topic      Topic string (null-terminated).
 * @param payload    Payload bytes (may be NULL if len == 0).
 * @param len        Payload length in bytes.
 * @param msg_id     Packet identifier (must be non-zero for QoS 1).
 */
void fake_broker_send_qos1_publish(const char *topic, const uint8_t *payload,
                                   size_t len, uint16_t msg_id);

/**
 * @brief Send a QoS 2 PUBLISH packet from the broker to the client.
 *
 * Builds and transmits a well-formed MQTT 3.1.1 PUBLISH packet with QoS 2
 * directly to the connected client socket.  The client's next mqtt_input()
 * call will process the packet, send PUBREC, and enter the waiting_rel state.
 *
 * @param topic      Topic string (null-terminated).
 * @param payload    Payload bytes (may be NULL if len == 0).
 * @param len        Payload length in bytes.
 * @param msg_id     Packet identifier (must be non-zero for QoS 2).
 */
void fake_broker_send_qos2_publish(const char *topic, const uint8_t *payload,
                                   size_t len, uint16_t msg_id);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_BROKER_HPP */
