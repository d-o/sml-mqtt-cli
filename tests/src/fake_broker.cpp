/*
 * Copyright (c) 2026 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 *
 * fake_broker.cpp - In-process MQTT 3.1.1 fake broker for Zephyr loopback tests.
 *
 * Modelled after zephyr/tests/net/lib/mqtt/v3_1_1/mqtt_client/src/main.c
 *
 * The broker is entirely poll-driven: the caller drives I/O by interleaving
 *   fake_broker_process(EXPECTED_TYPE)  ->  mqtt_input(client)
 * calls.  No threads, no semaphores, no heap.
 */

#include "fake_broker.hpp"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/net/socket.h>
#include <cerrno>
#include <cstring>

/* -------------------------------------------------------------------------
 * Internal constants
 * ------------------------------------------------------------------------- */

#define BROKER_BUF_SIZE    1500   /* max MQTT packet we expect in tests */
#define BROKER_TOPIC_MAX   128    /* max subscription topic length */
#define BROKER_TIMEOUT_MS  100    /* poll timeout per iteration */

/* -------------------------------------------------------------------------
 * Static broker state (one instance, no heap)
 * ------------------------------------------------------------------------- */

static int      s_sock = -1;                       /* server (listen) socket  */
static int      c_sock = -1;                       /* accepted client socket  */
static uint8_t  broker_buf[BROKER_BUF_SIZE];       /* accumulation buffer     */
static size_t   broker_offset = 0;                 /* valid bytes in buf      */
static char     broker_topic[BROKER_TOPIC_MAX];    /* topic from SUBSCRIBE    */

/* -------------------------------------------------------------------------
 * Pre-built fixed-length response packets
 * ------------------------------------------------------------------------- */

/* CONNACK: type=0x20, remaining=2, return_code=0, session_present=0 */
static const uint8_t resp_connack[]  = { 0x20u, 0x02u, 0x00u, 0x00u };

/* PINGRESP: type=0xD0, remaining=0 */
static const uint8_t resp_pingresp[] = { 0xD0u, 0x00u };

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/** Send all bytes in buf to the connected client socket. */
static void broker_send(const uint8_t *data, size_t len)
{
	while (len > 0u) {
		ssize_t n = zsock_send(c_sock, data, len, 0);
		zassert_true(n > 0, "fake_broker: send failed (errno %d)", errno);
		data += n;
		len  -= (size_t)n;
	}
}

/**
 * Encode and send an MQTT fixed header.
 *
 * @param type_byte     Full first byte (type|flags) already formed by caller.
 * @param remaining_len Value to encode in the variable-length field.
 */
static void send_fixed_header(uint8_t type_byte, uint32_t remaining_len)
{
	uint8_t buf[5];
	size_t  pos = 0u;

	buf[pos++] = type_byte;

	/* Variable-length remaining length encoding (MQTT 3.1.1 §2.2.3) */
	uint32_t rl = remaining_len;
	do {
		uint8_t enc = (uint8_t)(rl & 0x7Fu);
		rl >>= 7;
		if (rl > 0u) {
			enc |= 0x80u;
		}
		buf[pos++] = enc;
	} while (rl > 0u);

	broker_send(buf, pos);
}

/* -------------------------------------------------------------------------
 * Packet dispatcher
 *
 * Called once a full packet has been decoded from broker_buf.
 * var_hdr points to the variable header (first byte after fixed header).
 * remaining_len is the remaining length field value.
 * type_byte is the full first byte of the fixed header.
 * ------------------------------------------------------------------------- */

static void dispatch_packet(uint8_t type_byte,
			    const uint8_t *var_hdr,
			    uint32_t remaining_len)
{
	uint8_t type  = type_byte & 0xF0u;
	uint8_t flags = type_byte & 0x0Fu;

	switch (type) {

	/* ---- CONNECT (0x10) ------------------------------------------- */
	case 0x10u:
		broker_send(resp_connack, sizeof(resp_connack));
		break;

	/* ---- PUBLISH (0x30) ------------------------------------------- */
	case 0x30u: {
		uint8_t  qos       = (flags >> 1u) & 0x03u;
		uint16_t topic_len = ((uint16_t)var_hdr[0] << 8u) | var_hdr[1];

		/* Packet identifier is present for QoS 1 and QoS 2 only. */
		const uint8_t *pkt_id = var_hdr + 2u + topic_len;

		if (qos == 1u) {
			/* PUBACK: type=0x40, remaining=2, packet_id */
			uint8_t puback[4] = {
				0x40u, 0x02u, pkt_id[0], pkt_id[1]
			};
			broker_send(puback, sizeof(puback));
		} else if (qos == 2u) {
			/* PUBREC: type=0x50, remaining=2, packet_id */
			uint8_t pubrec[4] = {
				0x50u, 0x02u, pkt_id[0], pkt_id[1]
			};
			broker_send(pubrec, sizeof(pubrec));
		}

		/* Echo the publish back if topic matches active subscription.
		 * This is the mechanism that makes the loopback pubsub test
		 * work: subscribe + publish on one client, broker echoes back. */
		if (topic_len > 0u &&
		    topic_len < BROKER_TOPIC_MAX &&
		    broker_topic[0] != '\0' &&
		    topic_len == (uint16_t)strlen(broker_topic) &&
		    memcmp(var_hdr + 2u, broker_topic, topic_len) == 0) {
			send_fixed_header(type_byte, remaining_len);
			broker_send(var_hdr, remaining_len);
		}
		break;
	}

	/* ---- PUBREL (0x62 on wire, type nibble = 0x60) ----------------- */
	case 0x60u: {
		/* PUBCOMP: type=0x70, remaining=2, echo packet_id */
		uint8_t pubcomp[4] = {
			0x70u, 0x02u, var_hdr[0], var_hdr[1]
		};
		broker_send(pubcomp, sizeof(pubcomp));
		break;
	}

	/* ---- SUBSCRIBE (0x82 on wire, type nibble = 0x80) -------------- */
	case 0x80u: {
		/* Variable header: [pkt_id_hi, pkt_id_lo]
		 * Payload per topic: [topic_len_hi, topic_len_lo, topic..., qos] */
		uint16_t topic_len = ((uint16_t)var_hdr[2] << 8u) | var_hdr[3];
		uint8_t  granted   = (topic_len < BROKER_TOPIC_MAX)
					? var_hdr[4u + topic_len] : 0u;

		if (topic_len > 0u && topic_len < BROKER_TOPIC_MAX) {
			memcpy(broker_topic, var_hdr + 4u, topic_len);
			broker_topic[topic_len] = '\0';
		}

		/* SUBACK: type=0x90, remaining=3, packet_id, granted_qos */
		uint8_t suback[5] = {
			0x90u, 0x03u,
			var_hdr[0], var_hdr[1],
			granted
		};
		broker_send(suback, sizeof(suback));
		break;
	}

	/* ---- UNSUBSCRIBE (0xA2 on wire, type nibble = 0xA0) ------------ */
	case 0xA0u: {
		memset(broker_topic, 0, sizeof(broker_topic));

		/* UNSUBACK: type=0xB0, remaining=2, echo packet_id */
		uint8_t unsuback[4] = {
			0xB0u, 0x02u, var_hdr[0], var_hdr[1]
		};
		broker_send(unsuback, sizeof(unsuback));
		break;
	}

	/* ---- PINGREQ (0xC0) ------------------------------------------- */
	case 0xC0u:
		broker_send(resp_pingresp, sizeof(resp_pingresp));
		break;

	/* ---- DISCONNECT (0xE0) ---------------------------------------- */
	case 0xE0u:
		zsock_close(c_sock);
		c_sock = -1;
		break;

	default:
		zassert_unreachable("fake_broker: unhandled packet type 0x%02x",
				    type);
		break;
	}
}

/* -------------------------------------------------------------------------
 * Packet receiver
 *
 * Attempts a non-blocking read from c_sock into broker_buf, then tries to
 * decode and dispatch one complete MQTT packet.
 *
 * Returns:
 *   0       - one packet successfully processed
 *  -EAGAIN  - not enough data yet (caller should poll and retry)
 * ------------------------------------------------------------------------- */

static int broker_receive(uint8_t expected_type)
{
	zassert_false(broker_offset == sizeof(broker_buf),
		      "fake_broker: receive buffer full");

	ssize_t n = zsock_recv(c_sock,
			       broker_buf + broker_offset,
			       sizeof(broker_buf) - broker_offset,
			       ZSOCK_MSG_DONTWAIT);

	if (n > 0) {
		broker_offset += (size_t)n;
	} else if (n == 0) {
		/* Connection closed by client (e.g. after DISCONNECT). */
		return -EAGAIN;
	} else if (n < 0 && errno != EAGAIN) {
		zassert_unreachable("fake_broker: recv error (errno %d)", errno);
	}

	/* Need at least the first byte (type+flags) and one length byte. */
	if (broker_offset < 2u) {
		return -EAGAIN;
	}

	/* Decode variable-length remaining length (MQTT 3.1.1 §2.2.3).
	 * Each byte: lower 7 bits = value, bit 7 = more bytes follow. */
	uint32_t remaining_len = 0u;
	uint32_t multiplier    = 1u;
	size_t   hdr_end       = 1u;  /* index of first byte past fixed header */

	do {
		if (hdr_end >= broker_offset) {
			return -EAGAIN;  /* partial header */
		}

		uint8_t enc = broker_buf[hdr_end++];

		remaining_len += (uint32_t)(enc & 0x7Fu) * multiplier;
		multiplier    <<= 7;

		if (multiplier > (128u * 128u * 128u)) {
			zassert_unreachable(
				"fake_broker: malformed remaining-length field");
		}

		if (!(enc & 0x80u)) {
			break;  /* this was the last length byte */
		}
	} while (true);

	/* Require the full packet body to be present. */
	size_t total = hdr_end + (size_t)remaining_len;

	if (broker_offset < total) {
		return -EAGAIN;  /* partial packet body */
	}

	/* Validate packet type (upper nibble matches expected). */
	uint8_t type_byte     = broker_buf[0];
	uint8_t received_type = type_byte & 0xF0u;

	zassert_equal(received_type, expected_type & 0xF0u,
		      "fake_broker: expected packet type 0x%02x, got 0x%02x",
		      (unsigned)(expected_type & 0xF0u),
		      (unsigned)received_type);

	/* Dispatch - var_hdr points into broker_buf immediately after the
	 * fixed header.  The buffer is not shifted until after dispatch so
	 * that the echo path in PUBLISH can reference the original bytes. */
	dispatch_packet(type_byte, broker_buf + hdr_end, remaining_len);

	/* Consume the processed packet from the buffer. */
	broker_offset -= total;
	if (broker_offset > 0u) {
		memmove(broker_buf, broker_buf + total, broker_offset);
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void fake_broker_init(void)
{
	struct sockaddr_in bind_addr = {};
	int opt = 1;
	int ret;

	memset(broker_topic, 0, sizeof(broker_topic));
	broker_offset = 0;

	bind_addr.sin_family      = AF_INET;
	bind_addr.sin_port        = htons(FAKE_BROKER_PORT);
	/* sin_addr.s_addr = INADDR_ANY (0) - zero-init above */

	s_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(s_sock >= 0,
		     "fake_broker_init: socket() failed (errno %d)", errno);

	ret = zsock_setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR,
			       &opt, sizeof(opt));
	zassert_ok(ret,
		   "fake_broker_init: SO_REUSEADDR failed (errno %d)", errno);

	ret = zsock_bind(s_sock, (struct sockaddr *)&bind_addr,
			 sizeof(bind_addr));
	zassert_ok(ret,
		   "fake_broker_init: bind() failed (errno %d)", errno);

	ret = zsock_listen(s_sock, 1);
	zassert_ok(ret,
		   "fake_broker_init: listen() failed (errno %d)", errno);
}

void fake_broker_destroy(void)
{
	if (c_sock >= 0) {
		zsock_close(c_sock);
		c_sock = -1;
	}
	if (s_sock >= 0) {
		zsock_close(s_sock);
		s_sock = -1;
	}
	memset(broker_topic, 0, sizeof(broker_topic));
	broker_offset = 0;

	/* Allow the TCP stack to release TIME_WAIT resources. */
	k_msleep(10);
}

void fake_broker_process(uint8_t expected_type)
{
	struct zsock_pollfd fds[2] = {
		{ .fd = s_sock, .events = ZSOCK_POLLIN, .revents = 0 },
		{ .fd = c_sock, .events = ZSOCK_POLLIN, .revents = 0 },
	};

	/* Fast path: already have buffered data and a live connection. */
	if (c_sock >= 0 && broker_offset > 0u) {
		if (broker_receive(expected_type) == 0) {
			return;
		}
	}

	while (true) {
		fds[0].revents = 0;
		fds[1].revents = 0;
		fds[1].fd      = c_sock;  /* refresh in case accept() ran */

		int ret = zsock_poll(fds, ARRAY_SIZE(fds), BROKER_TIMEOUT_MS);

		zassert_true(ret > 0,
			     "fake_broker_process: poll timeout "
			     "waiting for packet type 0x%02x",
			     (unsigned)expected_type);

		for (int i = 0; i < 2; i++) {
			if (fds[i].fd < 0 ||
			    !(fds[i].revents & ZSOCK_POLLIN)) {
				continue;
			}

			if (i == 0) {
				/* New TCP connection from client. */
				zassert_equal(c_sock, -1,
					      "fake_broker_process: "
					      "unexpected second connection");
				c_sock = zsock_accept(s_sock, NULL, NULL);
				zassert_true(
					c_sock >= 0,
					"fake_broker_process: accept() failed "
					"(errno %d)", errno);
				fds[1].fd = c_sock;
			} else {
				/* Data from the connected client. */
				if (broker_receive(expected_type) == 0) {
					return;
				}
			}
		}
	}
}
