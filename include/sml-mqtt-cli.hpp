/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#ifndef SML_MQTT_CLI_HPP
#define SML_MQTT_CLI_HPP

#include <boost/sml.hpp>
#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

LOG_MODULE_DECLARE(sml_mqtt_cli, CONFIG_MQTT_LOG_LEVEL);

namespace sml = boost::sml;

namespace sml_mqtt_cli {

// ============================================================================
// Configuration from Kconfig (defaults provided for when not using Kconfig)
// ============================================================================

#ifndef CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN
#define CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN 128
#endif

#ifndef CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN
#define CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN 512
#endif

#ifndef CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN
#define CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN 64
#endif

#ifndef CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE
#define CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE 256
#endif

#ifndef CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE
#define CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE 256
#endif

#ifndef CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS
#define CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS 8
#endif

#ifndef CONFIG_SML_MQTT_CLI_CONNECT_TIMEOUT_MS
#define CONFIG_SML_MQTT_CLI_CONNECT_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC
#define CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC 60
#endif

#ifndef CONFIG_SML_MQTT_CLI_PUBLISH_TIMEOUT_MS
#define CONFIG_SML_MQTT_CLI_PUBLISH_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS
#define CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS 1
#endif

// ============================================================================
// Forward declarations and types
// ============================================================================

class mqtt_client;

// Callback types for C API compatibility
using state_change_cb_t = void (*)(void *user_data, const char *old_state, const char *new_state);
using publish_received_cb_t = void (*)(void *user_data, const char *topic, const uint8_t *payload, size_t len, enum mqtt_qos qos);

// ============================================================================
// TLS configuration (only available when CONFIG_MQTT_LIB_TLS is enabled)
// ============================================================================

#if defined(CONFIG_MQTT_LIB_TLS)
/**
 * @brief TLS credentials for an MQTT client instance.
 *
 * Security tags must be pre-registered with Zephyr's TLS credential store
 * via tls_credential_add() before calling init().  The library copies the
 * tag IDs into its own context so the caller's array does not need to
 * outlive the init() call.
 */
struct tls_config {
	const sec_tag_t *sec_tag_list;  ///< Array of registered security tags
	size_t           sec_tag_count; ///< Number of tags (max CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS)
	int              peer_verify;   ///< TLS_PEER_VERIFY_REQUIRED or TLS_PEER_VERIFY_NONE
};
#endif /* CONFIG_MQTT_LIB_TLS */

// ============================================================================
// Events for state machine
// ============================================================================

struct evt_connect {
	const char *broker_hostname;
	uint16_t broker_port;
	bool use_tls;
} __attribute__((packed));

struct evt_connack {
	int result_code;
	bool session_present;
} __attribute__((packed));

struct evt_disconnect {
	int reason;
} __attribute__((packed));

struct evt_timeout {} __attribute__((packed));

struct evt_publish {
	const char *topic;
	const uint8_t *payload;
	size_t payload_len;
	enum mqtt_qos qos;
	bool retain;
} __attribute__((packed));

struct evt_publish_sent {} __attribute__((packed));

struct evt_puback {
	uint16_t message_id;
} __attribute__((packed));

struct evt_pubrec {
	uint16_t message_id;
} __attribute__((packed));

struct evt_pubrel {
	uint16_t message_id;
} __attribute__((packed));

struct evt_pubcomp {
	uint16_t message_id;
} __attribute__((packed));

struct evt_subscribe {
	const char *topic;
	enum mqtt_qos qos;
} __attribute__((packed));

struct evt_suback {
	uint16_t message_id;
	enum mqtt_qos granted_qos;
} __attribute__((packed));

struct evt_unsubscribe {
	const char *topic;
} __attribute__((packed));

struct evt_unsuback {
	uint16_t message_id;
} __attribute__((packed));

struct evt_send_error {
	int error_code;
	const char *operation;
} __attribute__((packed));

struct evt_publish_received {
	uint16_t message_id;
	const char *topic;
	size_t payload_len;
	enum mqtt_qos qos;
} __attribute__((packed));

struct evt_pingresp {} __attribute__((packed));

// Synthetic completion signals fired by mqtt_evt_handler after a sub-SM
// transaction reaches its terminal step (sml::X).  The outer SM listens for
// each on its respective composite state to transition back to "Connected".
// Separate types prevent an incoming-receive completion from prematurely
// exiting an in-progress publish handshake and vice versa.
// These are internal events — never fire them directly from application code.
struct evt_publish_done {} __attribute__((packed));
struct evt_receive_done {} __attribute__((packed));

// ============================================================================
// Context - holds all client state and buffers
// ============================================================================

struct mqtt_context {
	// Zephyr MQTT client (from global namespace, not our class)
	::mqtt_client client;

	// Static buffers (embedded-friendly)
	std::array<uint8_t, CONFIG_SML_MQTT_CLI_RX_BUFFER_SIZE> rx_buffer;
	std::array<uint8_t, CONFIG_SML_MQTT_CLI_TX_BUFFER_SIZE> tx_buffer;
	std::array<char, CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN> client_id;
	std::array<char, CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN> current_topic;
	std::array<uint8_t, CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN> current_payload;

	// Broker details
	struct sockaddr_storage broker_addr;

#if defined(CONFIG_MQTT_LIB_TLS)
	// TLS credentials (copied from tls_config at init time)
	sec_tag_t tls_sec_tags[CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS];
	size_t    tls_sec_tag_count;
	int       tls_peer_verify;
	bool      tls_enabled;
	// Hostname copy — Zephyr's mqtt_sec_config.hostname is a const char*
	// that must remain valid for the lifetime of the connection.  We
	// copy it here so callers do not need to keep their string alive.
	char      tls_hostname[CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN];
#endif

	// Subscription tracking
	struct subscription {
		char topic[CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN];
		enum mqtt_qos qos;
		bool active;
	};
	std::array<subscription, CONFIG_SML_MQTT_CLI_MAX_SUBSCRIPTIONS> subscriptions;

	// State tracking
	bool connected;
	uint16_t next_message_id;
	uint16_t pending_message_id;
	size_t current_payload_len;
	int64_t last_activity_ms;
	int64_t connect_start_ms;

	// Per-operation QoS timeouts — one field per sub-SM so live() can fire
	// the correct typed completion event even if both paths are active.
	int64_t publish_op_start_ms;  // 0 = no active outgoing QoS 1/2 operation
	int64_t receive_op_start_ms;  // 0 = no active incoming QoS 2 PUBREL wait
	int64_t qos_timeout_ms;       // shared threshold for both paths; 0 = disabled

	// Callbacks for C API
	state_change_cb_t state_change_cb;
	publish_received_cb_t publish_received_cb;
	void *user_data;

	// Parent client reference
	mqtt_client *parent;

	mqtt_context() noexcept
		: client{}, rx_buffer{}, tx_buffer{}, client_id{}, current_topic{}, current_payload{},
		  broker_addr{}
#if defined(CONFIG_MQTT_LIB_TLS)
		  , tls_sec_tags{}, tls_sec_tag_count(0),
		  tls_peer_verify(TLS_PEER_VERIFY_REQUIRED), tls_enabled(false),
		  tls_hostname{}
#endif
		  , subscriptions{}, connected(false), next_message_id(1),
		  pending_message_id(0), current_payload_len(0), last_activity_ms(0),
		  connect_start_ms(0), publish_op_start_ms(0), receive_op_start_ms(0),
		  qos_timeout_ms(CONFIG_SML_MQTT_CLI_PUBLISH_TIMEOUT_MS),
		  state_change_cb(nullptr), publish_received_cb(nullptr),
		  user_data(nullptr), parent(nullptr)
	{}

	~mqtt_context() = default;

	// Prevent copying
	mqtt_context(const mqtt_context&) = delete;
	mqtt_context& operator=(const mqtt_context&) = delete;
};

// ============================================================================
// State Machine Definition - Hierarchical/Composite Architecture
// ============================================================================

// ============================================================================
// Publishing sub-machine
//
// Encapsulates the full outgoing publish lifecycle for all three QoS levels.
// Each terminal transition goes to sml::X.  The MQTT event handler (or the
// publish() method for QoS 0) fires evt_transaction_done immediately after
// the terminal process_event() call.  At that point the inner SM is at X and
// cannot handle any events, so evt_transaction_done falls through to the outer
// SM which transitions back to "Connected".
//
// QoS 0:  idle ──evt_publish──► qos0 ──evt_publish_sent──► X
// QoS 1:  idle ──evt_publish──► qos1 ──evt_puback────────► X
// QoS 2:  idle ──evt_publish──► qos2 ──evt_pubrec─────► releasing ──evt_pubcomp──► X
// Any state ──evt_send_error──► X
//
// After each ──► X the caller fires: sm_.process_event(evt_transaction_done{})
// ============================================================================
struct publishing_sm {
	auto operator()() const noexcept {
		using namespace sml;

		auto is_qos0 = [](const evt_publish& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_0_AT_MOST_ONCE;
		};
		auto is_qos1 = [](const evt_publish& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_1_AT_LEAST_ONCE;
		};
		auto is_qos2 = [](const evt_publish& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_2_EXACTLY_ONCE;
		};

		auto send_publish = [](mqtt_context& ctx, const evt_publish&) noexcept {
			ctx.pending_message_id = ctx.next_message_id++;
		};

		// QoS 0 complete: packet accepted by the stack.
		auto on_publish_sent = [](mqtt_context& ctx) noexcept {
			ctx.pending_message_id = 0;
		};

		// QoS 1 complete: broker acknowledged delivery.
		auto handle_puback = [](mqtt_context& ctx, const evt_puback&) noexcept {
			ctx.pending_message_id = 0;
		};

		// QoS 2 intermediate: PUBREC received.  The actual PUBREL wire send is
		// done in the MQTT event handler before this SM transition fires.
		auto send_pubrel = [](mqtt_context&, const evt_pubrec&) noexcept {};

		// QoS 2 complete: broker confirmed exactly-once delivery.
		auto handle_pubcomp = [](mqtt_context& ctx, const evt_pubcomp&) noexcept {
			ctx.pending_message_id = 0;
		};

		// Error in any publish state.  Caller fires evt_transaction_done after.
		auto log_error = [](mqtt_context& ctx, const evt_send_error& evt) noexcept {
			LOG_ERR("Publish send error: %d (%s)",
			        evt.error_code, evt.operation ? evt.operation : "unknown");
			ctx.pending_message_id = 0;
		};

		// clang-format off
		return make_transition_table(
			*"idle"_s      + event<evt_publish> [is_qos0] / send_publish      = "qos0"_s,
			 "qos0"_s      + event<evt_publish_sent>      / on_publish_sent   = sml::X,
			 "qos0"_s      + event<evt_send_error>        / log_error         = sml::X,

			 "idle"_s      + event<evt_publish> [is_qos1] / send_publish      = "qos1"_s,
			 "qos1"_s      + event<evt_puback>            / handle_puback     = sml::X,
			 "qos1"_s      + event<evt_send_error>        / log_error         = sml::X,

			 "idle"_s      + event<evt_publish> [is_qos2] / send_publish      = "qos2"_s,
			 "qos2"_s      + event<evt_pubrec>            / send_pubrel       = "releasing"_s,
			 "qos2"_s      + event<evt_send_error>        / log_error         = sml::X,
			 "releasing"_s + event<evt_pubcomp>           / handle_pubcomp    = sml::X,
			 "releasing"_s + event<evt_send_error>        / log_error         = sml::X
		);
		// clang-format on
	}
};

// ============================================================================
// Receiving sub-machine
//
// Encapsulates the full incoming publish lifecycle for all three QoS levels.
// The actual wire-level ACK sends (PUBACK, PUBREC, PUBCOMP) are performed
// directly in the MQTT event handler; the SM actions handle bookkeeping only.
// Each terminal transition goes to sml::X; the event handler fires
// evt_transaction_done after the terminal process_event() call.
//
// QoS 0:  idle ──evt_publish_received──► X          [no ACK required]
// QoS 1:  idle ──evt_publish_received──► X          [PUBACK sent by event handler]
// QoS 2:  idle ──evt_publish_received──► waiting_rel ──evt_pubrel──► X
//                                             └──evt_send_error───► X
//
// After each ──► X the caller fires: sm_.process_event(evt_transaction_done{})
// ============================================================================
struct receiving_sm {
	auto operator()() const noexcept {
		using namespace sml;

		auto is_qos0 = [](const evt_publish_received& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_0_AT_MOST_ONCE;
		};
		auto is_qos1 = [](const evt_publish_received& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_1_AT_LEAST_ONCE;
		};
		auto is_qos2 = [](const evt_publish_received& evt) noexcept -> bool {
			return evt.qos == MQTT_QOS_2_EXACTLY_ONCE;
		};

		// Store incoming message context.  Shared by all QoS levels.
		// For QoS 0/1: caller fires evt_transaction_done right after this.
		// For QoS 1:   PUBACK was already sent by the event handler before
		//              process_event(pub_evt) was called.
		// For QoS 2:   PUBREC was already sent; we wait for evt_pubrel next.
		auto handle_message = [](mqtt_context& ctx,
		                         const evt_publish_received& evt) noexcept {
			if (evt.topic) {
				strncpy(ctx.current_topic.data(), evt.topic,
				        ctx.current_topic.size() - 1);
				ctx.current_topic[ctx.current_topic.size() - 1] = '\0';
			}
			ctx.pending_message_id = evt.message_id;
		};

		// QoS 2 complete: PUBREL received; PUBCOMP sent by event handler after.
		auto on_pubrel = [](mqtt_context& ctx, const evt_pubrel&) noexcept {
			ctx.pending_message_id = 0;
		};

		// Error in any receive state.  Caller fires evt_transaction_done after.
		auto log_error = [](mqtt_context&, const evt_send_error& evt) noexcept {
			LOG_ERR("Receive error: %d (%s)",
			        evt.error_code, evt.operation ? evt.operation : "unknown");
		};

		// clang-format off
		return make_transition_table(
			*"idle"_s        + event<evt_publish_received> [is_qos0] / handle_message = sml::X,
			 "idle"_s        + event<evt_publish_received> [is_qos1] / handle_message = sml::X,
			 "idle"_s        + event<evt_publish_received> [is_qos2] / handle_message = "waiting_rel"_s,
			 "waiting_rel"_s + event<evt_pubrel>                     / on_pubrel       = sml::X,
			 "waiting_rel"_s + event<evt_send_error>                 / log_error       = sml::X
		);
		// clang-format on
	}
};

// Main connection state machine
struct mqtt_state_machine {
	auto operator()() const noexcept {
		using namespace sml;

		// Guards
		auto is_success = [](const evt_connack& evt) noexcept -> bool {
			return evt.result_code == 0;
		};

		auto is_timeout = [](mqtt_context& ctx) noexcept -> bool {
			int64_t now = k_uptime_get();
			return (now - ctx.connect_start_ms) > CONFIG_SML_MQTT_CLI_CONNECT_TIMEOUT_MS;
		};

		// Actions
		auto init_connect = [](mqtt_context& ctx, const evt_connect& evt) noexcept {
			ctx.connect_start_ms = k_uptime_get();
			ctx.connected = false;
		};

		auto send_connect = [](mqtt_context& ctx, const evt_connect& evt) noexcept {
			// This will be called from actual connect method
		};

		auto on_connected = [](mqtt_context& ctx) noexcept {
			ctx.connected = true;
			ctx.last_activity_ms = k_uptime_get();
		};

		auto on_connect_failed = [](mqtt_context& ctx, const evt_connack& evt) noexcept {
			ctx.connected = false;
		};

		auto on_disconnected = [](mqtt_context& ctx) noexcept {
			ctx.connected = false;
		};

		auto cleanup_resources = [](mqtt_context& ctx) noexcept {
			// Clear subscriptions
			for (auto& sub : ctx.subscriptions) {
				sub.active = false;
			}
		};

		auto send_subscribe = [](mqtt_context& ctx, const evt_subscribe& evt) noexcept {
			ctx.pending_message_id = ctx.next_message_id++;
		};

		auto on_subscribed = [](mqtt_context& ctx, const evt_suback& evt) noexcept {
			// Find subscription and mark as active
			for (auto& sub : ctx.subscriptions) {
				if (!sub.active && sub.topic[0] != '\0') {
					sub.active = true;
					break;
				}
			}
			ctx.pending_message_id = 0;
		};

		auto send_unsubscribe = [](mqtt_context& ctx, const evt_unsubscribe& evt) noexcept {
			ctx.pending_message_id = ctx.next_message_id++;
		};

		auto on_unsubscribed = [](mqtt_context& ctx, const evt_unsuback& evt) noexcept {
			ctx.pending_message_id = 0;
		};

		auto start_keepalive = [](mqtt_context& ctx) noexcept {
			ctx.last_activity_ms = k_uptime_get();
		};

		auto log_error = [](mqtt_context& ctx, const evt_send_error& evt) noexcept {
			LOG_ERR("Send error in operation '%s': %d",
			        evt.operation ? evt.operation : "unknown", evt.error_code);
			ctx.pending_message_id = 0;
		};

		auto on_keepalive = [](mqtt_context& ctx, const evt_pingresp& evt) noexcept {
			ctx.last_activity_ms = k_uptime_get();
		};

		// clang-format off
		return make_transition_table(
			// Connection lifecycle
		   *"Disconnected"_s + event<evt_connect> / init_connect = "Connecting"_s,
			"Connecting"_s + event<evt_connack> [is_success] / on_connected = "Connected"_s,
			"Connecting"_s + event<evt_connack> [!is_success] / on_connect_failed = "Disconnected"_s,
			"Connecting"_s + event<evt_timeout> [is_timeout] / cleanup_resources = "Disconnected"_s,
			"Connected"_s + event<evt_disconnect> / on_disconnected = "Disconnected"_s,

			// Publishing: enter composite sub-SM on publish.
			// mqtt_evt_handler fires evt_publish_done when the QoS lifecycle
			// completes; the outer SM listens here to exit back to Connected.
			// Using a typed event (not shared evt_transaction_done) means an
			// incoming receive completion cannot prematurely exit this state.
			"Connected"_s + event<evt_publish>                                     = sml::state<publishing_sm>,
			sml::state<publishing_sm> + event<evt_publish_done>                   = "Connected"_s,
			sml::state<publishing_sm> + event<evt_disconnect> / on_disconnected    = "Disconnected"_s,

			// Receiving: enter composite sub-SM on any incoming publish.
			// mqtt_evt_handler fires evt_receive_done when the QoS lifecycle
			// completes (immediately for QoS 0/1, after PUBREL for QoS 2).
			"Connected"_s + event<evt_publish_received>                            = sml::state<receiving_sm>,
			sml::state<receiving_sm>  + event<evt_receive_done>                   = "Connected"_s,
			sml::state<receiving_sm>  + event<evt_disconnect> / on_disconnected    = "Disconnected"_s,

			// Subscribing
			"Connected"_s + event<evt_subscribe> / send_subscribe = "Subscribing"_s,
			"Subscribing"_s + event<evt_suback> / on_subscribed = "Connected"_s,
			"Subscribing"_s + event<evt_send_error> / log_error = "Connected"_s,
			"Subscribing"_s + event<evt_disconnect> / on_disconnected = "Disconnected"_s,

			// Unsubscribing
			"Connected"_s + event<evt_unsubscribe> / send_unsubscribe = "Unsubscribing"_s,
			"Unsubscribing"_s + event<evt_unsuback> / on_unsubscribed = "Connected"_s,
			"Unsubscribing"_s + event<evt_send_error> / log_error = "Connected"_s,
			"Unsubscribing"_s + event<evt_disconnect> / on_disconnected = "Disconnected"_s,

			// Keepalive
			"Connected"_s + event<evt_pingresp> / on_keepalive,

			// Entry/Exit actions
			"Connected"_s + sml::on_entry<_> / start_keepalive,
			"Disconnected"_s + sml::on_entry<_> / cleanup_resources
		);
		// clang-format on
	}
};

// ============================================================================
// MQTT Client Class
// ============================================================================

class mqtt_client {
public:
	mqtt_client() noexcept : ctx_(), sm_(ctx_) {
		ctx_.parent = this;
	}

	~mqtt_client() noexcept {
		if (ctx_.connected) {
			disconnect();
		}
		/* Always call mqtt_abort() to release the TCP socket.
		 * If the Zephyr MQTT state is already IDLE (properly disconnected),
		 * mqtt_abort() is a no-op.  This handles the case where the client
		 * goes out of scope mid-connect (e.g. after a test assertion), which
		 * would otherwise leak the socket file descriptor. */
		mqtt_abort(&ctx_.client);
	}

	// Prevent copying
	mqtt_client(const mqtt_client&) = delete;
	mqtt_client& operator=(const mqtt_client&) = delete;

	// Initialize client with ID (and optional TLS credentials)
#if defined(CONFIG_MQTT_LIB_TLS)
	int init(const char *client_id, const tls_config *tls = nullptr) noexcept {
#else
	int init(const char *client_id) noexcept {
#endif
		if (!client_id || strlen(client_id) >= CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN) {
			return -EINVAL;
		}

		strncpy(ctx_.client_id.data(), client_id, ctx_.client_id.size() - 1);
		ctx_.client_id[ctx_.client_id.size() - 1] = '\0';

		mqtt_client_init(&ctx_.client);

		ctx_.client.broker = &ctx_.broker_addr;
		ctx_.client.evt_cb = reinterpret_cast<mqtt_evt_cb_t>(mqtt_evt_handler_static);
		ctx_.client.user_data = this;
		ctx_.client.client_id.utf8 = reinterpret_cast<uint8_t*>(ctx_.client_id.data());
		ctx_.client.client_id.size = strlen(ctx_.client_id.data());
		ctx_.client.rx_buf = ctx_.rx_buffer.data();
		ctx_.client.rx_buf_size = ctx_.rx_buffer.size();
		ctx_.client.tx_buf = ctx_.tx_buffer.data();
		ctx_.client.tx_buf_size = ctx_.tx_buffer.size();
		ctx_.client.keepalive = CONFIG_SML_MQTT_CLI_KEEPALIVE_SEC;
		ctx_.client.protocol_version = MQTT_VERSION_3_1_1;
		ctx_.client.user_data = this;

#if defined(CONFIG_MQTT_LIB_TLS)
		if (tls) {
			if (!tls->sec_tag_list || tls->sec_tag_count == 0 ||
			    tls->sec_tag_count > CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS) {
				return -EINVAL;
			}
			memcpy(ctx_.tls_sec_tags, tls->sec_tag_list,
			       tls->sec_tag_count * sizeof(sec_tag_t));
			ctx_.tls_sec_tag_count = tls->sec_tag_count;
			ctx_.tls_peer_verify   = tls->peer_verify;
			ctx_.tls_enabled       = true;
		}
#endif

		return 0;
	}

	// Connect to broker
	int connect(const char *hostname, uint16_t port, bool use_tls = false) noexcept {
		if (!hostname) {
			return -EINVAL;
		}

		// Validate TLS credentials before doing any network work
		if (use_tls) {
#if defined(CONFIG_MQTT_LIB_TLS)
			if (!ctx_.tls_enabled) {
				return -EINVAL;
			}
#else
			return -ENOTSUP;
#endif
		}

		// Resolve hostname
		struct zsock_addrinfo hints = {}, *res;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		char port_str[6];
		snprintf(port_str, sizeof(port_str), "%u", port);

		int ret = zsock_getaddrinfo(hostname, port_str, &hints, &res);
		if (ret != 0) {
			return -EHOSTUNREACH;
		}

		memcpy(&ctx_.broker_addr, res->ai_addr, res->ai_addrlen);
		zsock_freeaddrinfo(res);

		// Configure transport
		if (use_tls) {
#if defined(CONFIG_MQTT_LIB_TLS)
			ctx_.client.transport.type = MQTT_TRANSPORT_SECURE;
			ctx_.client.transport.tls.config.sec_tag_list  = ctx_.tls_sec_tags;
			ctx_.client.transport.tls.config.sec_tag_count = ctx_.tls_sec_tag_count;
			ctx_.client.transport.tls.config.peer_verify   = ctx_.tls_peer_verify;
			strncpy(ctx_.tls_hostname, hostname, sizeof(ctx_.tls_hostname) - 1);
			ctx_.tls_hostname[sizeof(ctx_.tls_hostname) - 1] = '\0';
			ctx_.client.transport.tls.config.hostname      = ctx_.tls_hostname;
#endif
		} else {
			ctx_.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
		}

		// Send connect event to state machine
		evt_connect evt = {hostname, port, use_tls};
		sm_.process_event(evt);

		// Perform actual MQTT connect
		ret = mqtt_connect(&ctx_.client);
		if (ret != 0) {
			evt_connack connack = {ret, false};
			sm_.process_event(connack);
			return ret;
		}

		return 0;
	}

	// Disconnect from broker
	int disconnect() noexcept {
		int ret = mqtt_disconnect(&ctx_.client, nullptr);
		evt_disconnect evt = {0};
		sm_.process_event(evt);
		return ret;
	}

	// Publish message
	int publish(const char *topic, const uint8_t *payload, size_t payload_len,
	            enum mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE, bool retain = false) noexcept {
		if (!topic || !payload || !ctx_.connected) {
			return -EINVAL;
		}

		if (strlen(topic) >= CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN ||
		    payload_len > CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN) {
			return -EMSGSIZE;
		}

		struct mqtt_publish_param param = {};
		struct mqtt_topic pub_topic = {
			.topic = {
				.utf8 = reinterpret_cast<const uint8_t*>(topic),
				.size = strlen(topic)
			},
			.qos = qos
		};

		param.message.topic = pub_topic;
		param.message.payload.data = const_cast<uint8_t*>(payload);
		param.message.payload.len = payload_len;
		param.message_id = ctx_.next_message_id;
		param.dup_flag = 0;
		param.retain_flag = retain ? 1 : 0;

		// Start the per-operation timer for QoS 1/2 before firing the event.
		// The timer is cleared by the event handler on normal completion or
		// by live() on timeout; it is not set for QoS 0 (completes inline).
		if (qos != MQTT_QOS_0_AT_MOST_ONCE) {
			ctx_.publish_op_start_ms = k_uptime_get();
		}

		evt_publish evt = {topic, payload, payload_len, qos, retain};
		sm_.process_event(evt);

		int ret = mqtt_publish(&ctx_.client, &param);
		if (ret == 0 && qos == MQTT_QOS_0_AT_MOST_ONCE) {
			sm_.process_event(evt_publish_sent{});
			sm_.process_event(evt_publish_done{});
		}

		return ret;
	}

	// Subscribe to topic
	int subscribe(const char *topic, enum mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE) noexcept {
		if (!topic || !ctx_.connected) {
			return -EINVAL;
		}

		if (strlen(topic) >= CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN) {
			return -EMSGSIZE;
		}

		// Find free subscription slot
		int slot = -1;
		for (size_t i = 0; i < ctx_.subscriptions.size(); i++) {
			if (!ctx_.subscriptions[i].active) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			return -ENOMEM;
		}

		strncpy(ctx_.subscriptions[slot].topic, topic, CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN - 1);
		ctx_.subscriptions[slot].topic[CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN - 1] = '\0';
		ctx_.subscriptions[slot].qos = qos;

		struct mqtt_topic sub_topic = {
			.topic = {
				.utf8 = reinterpret_cast<uint8_t*>(ctx_.subscriptions[slot].topic),
				.size = strlen(ctx_.subscriptions[slot].topic)
			},
			.qos = qos
		};

		struct mqtt_subscription_list sub_list = {
			.list = &sub_topic,
			.list_count = 1,
			.message_id = ctx_.next_message_id
		};

		evt_subscribe evt = {topic, qos};
		sm_.process_event(evt);

		return mqtt_subscribe(&ctx_.client, &sub_list);
	}

	// Unsubscribe from topic
	int unsubscribe(const char *topic) noexcept {
		if (!topic || !ctx_.connected) {
			return -EINVAL;
		}

		// Find subscription
		int slot = -1;
		for (size_t i = 0; i < ctx_.subscriptions.size(); i++) {
			if (ctx_.subscriptions[i].active &&
			    strcmp(ctx_.subscriptions[i].topic, topic) == 0) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			return -ENOENT;
		}

		struct mqtt_topic unsub_topic = {
			.topic = {
				.utf8 = reinterpret_cast<uint8_t*>(ctx_.subscriptions[slot].topic),
				.size = strlen(ctx_.subscriptions[slot].topic)
			},
			.qos = MQTT_QOS_0_AT_MOST_ONCE
		};

		struct mqtt_subscription_list unsub_list = {
			.list = &unsub_topic,
			.list_count = 1,
			.message_id = ctx_.next_message_id
		};

		evt_unsubscribe evt = {topic};
		sm_.process_event(evt);

		int ret = mqtt_unsubscribe(&ctx_.client, &unsub_list);
		if (ret == 0) {
			ctx_.subscriptions[slot].active = false;
		}

		return ret;
	}

	// Process incoming data (call periodically)
	int input() noexcept {
		if (!ctx_.connected) {
			return -ENOTCONN;
		}
		return mqtt_input(&ctx_.client);
	}

	// Keep connection alive (call periodically).
	// Also drives per-operation QoS timeouts: if a QoS 1/2 handshake has been
	// waiting longer than qos_timeout_ms, the in-flight operation is abandoned
	// and the SM returns to Connected so new operations can proceed.
	int live() noexcept {
		if (!ctx_.connected) {
			return -ENOTCONN;
		}
		if (ctx_.qos_timeout_ms > 0) {
			int64_t now = k_uptime_get();
			if (ctx_.publish_op_start_ms != 0 &&
			    (now - ctx_.publish_op_start_ms) >= ctx_.qos_timeout_ms) {
				LOG_WRN("QoS publish operation timed out");
				ctx_.publish_op_start_ms = 0;
				ctx_.pending_message_id  = 0;
				sm_.process_event(evt_publish_done{});
			}
			if (ctx_.receive_op_start_ms != 0 &&
			    (now - ctx_.receive_op_start_ms) >= ctx_.qos_timeout_ms) {
				LOG_WRN("QoS receive operation timed out");
				ctx_.receive_op_start_ms = 0;
				sm_.process_event(evt_receive_done{});
			}
		}
		return mqtt_live(&ctx_.client);
	}

	// Set per-operation QoS timeout (applies to both publish and receive paths).
	// Pass 0 to disable.
	void set_qos_timeout_ms(int64_t ms) noexcept {
		ctx_.qos_timeout_ms = ms;
	}

	// Check if connected
	bool is_connected() const noexcept {
		return ctx_.connected;
	}

	// Get current state name
	const char* get_state() const noexcept {
		const char* state_name = "Unknown";
		sm_.visit_current_states([&state_name](auto state) {
			state_name = state.c_str();
		});
		return state_name;
	}

	// Set callbacks for C API
	void set_state_change_callback(state_change_cb_t cb, void *user_data = nullptr) noexcept {
		ctx_.state_change_cb = cb;
		ctx_.user_data = user_data;
	}

	void set_publish_received_callback(publish_received_cb_t cb, void *user_data = nullptr) noexcept {
		ctx_.publish_received_cb = cb;
		ctx_.user_data = user_data;
	}

	// Get context (for testing/debugging)
	mqtt_context& get_context() noexcept {
		return ctx_;
	}

private:
	mqtt_context ctx_;
	sml::sm<mqtt_state_machine> sm_;

	// Static MQTT event handler wrapper
	static void mqtt_evt_handler_static(::mqtt_client *client, const struct mqtt_evt *evt) {
		if (!client || !client->user_data) {
			return;
		}

		mqtt_client *self = static_cast<mqtt_client*>(client->user_data);
		self->mqtt_evt_handler(evt);
	}

	// MQTT event handler
	void mqtt_evt_handler(const struct mqtt_evt *evt) noexcept {
		switch (evt->type) {
		case MQTT_EVT_CONNACK: {
			evt_connack connack = {evt->result, false};
			sm_.process_event(connack);
			break;
		}
		case MQTT_EVT_DISCONNECT: {
			evt_disconnect disc = {evt->result};
			sm_.process_event(disc);
			break;
		}
		case MQTT_EVT_PUBLISH: {
			// Read the topic
			const struct mqtt_publish_param *p = &evt->param.publish;
			size_t topic_len = p->message.topic.topic.size;
			if (topic_len >= CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN) {
				topic_len = CONFIG_SML_MQTT_CLI_MAX_TOPIC_LEN - 1;
			}

			memcpy(ctx_.current_topic.data(), p->message.topic.topic.utf8, topic_len);
			ctx_.current_topic[topic_len] = '\0';

			// Read payload
			size_t payload_len = p->message.payload.len;
			if (payload_len > CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN) {
				payload_len = CONFIG_SML_MQTT_CLI_MAX_PAYLOAD_LEN;
			}

			int ret = mqtt_read_publish_payload(&ctx_.client, ctx_.current_payload.data(), payload_len);
			if (ret > 0) {
				ctx_.current_payload_len = ret;
			}

			evt_publish_received pub_evt = {
				p->message_id,
				ctx_.current_topic.data(),
				ctx_.current_payload_len,
				static_cast<mqtt_qos>(p->message.topic.qos)
			};
			sm_.process_event(pub_evt);

			// QoS 0/1: receiving_sm reached X; signal completion to outer SM.
			// QoS 2: inner SM is at "waiting_rel"; start the PUBREL timeout timer.
			if (p->message.topic.qos != MQTT_QOS_2_EXACTLY_ONCE) {
				sm_.process_event(evt_receive_done{});
			} else {
				ctx_.receive_op_start_ms = k_uptime_get();
			}

			// Call user callback
			if (ctx_.publish_received_cb) {
				ctx_.publish_received_cb(ctx_.user_data, ctx_.current_topic.data(),
				                        ctx_.current_payload.data(), ctx_.current_payload_len,
				                        static_cast<mqtt_qos>(p->message.topic.qos));
			}

			// Send acknowledgment
			int ack_ret = 0;
			if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
				struct mqtt_puback_param ack = {p->message_id};
				ack_ret = mqtt_publish_qos1_ack(&ctx_.client, &ack);
				if (ack_ret < 0) {
					LOG_ERR("Failed to send PUBACK: %d", ack_ret);
					/* SM already returned to Connected; evt_send_error is dropped. */
				}
			} else if (p->message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
				struct mqtt_pubrec_param rec = {p->message_id};
				ack_ret = mqtt_publish_qos2_receive(&ctx_.client, &rec);
				if (ack_ret < 0) {
					LOG_ERR("Failed to send PUBREC: %d", ack_ret);
					ctx_.receive_op_start_ms = 0;
					evt_send_error err = {ack_ret, "pubrec"};
					sm_.process_event(err);
					sm_.process_event(evt_receive_done{});
				}
			}
			break;
		}
		case MQTT_EVT_PUBACK: {
			evt_puback ack = {evt->param.puback.message_id};
			sm_.process_event(ack);
			ctx_.publish_op_start_ms = 0;  /* QoS 1 complete; clear timeout timer */
			sm_.process_event(evt_publish_done{});
			break;
		}
		case MQTT_EVT_PUBREC: {
			evt_pubrec rec = {evt->param.pubrec.message_id};
			sm_.process_event(rec);

			// Send PUBREL
			struct mqtt_pubrel_param rel = {evt->param.pubrec.message_id};
			int ret = mqtt_publish_qos2_release(&ctx_.client, &rel);
			if (ret < 0) {
				LOG_ERR("Failed to send PUBREL: %d", ret);
				ctx_.publish_op_start_ms = 0;  /* abort; clear timer */
				evt_send_error err = {ret, "pubrel"};
				sm_.process_event(err);
				sm_.process_event(evt_publish_done{});
			} else {
				/* Restart timer: broker has a fresh window to send PUBCOMP. */
				ctx_.publish_op_start_ms = k_uptime_get();
			}
			break;
		}
		case MQTT_EVT_PUBREL: {
			evt_pubrel rel = {evt->param.pubrel.message_id};
			sm_.process_event(rel);
			ctx_.receive_op_start_ms = 0;  /* incoming QoS 2 complete; clear timer */
			// receiving_sm reached X; signal completion before sending PUBCOMP.
			sm_.process_event(evt_receive_done{});

			// Send PUBCOMP (SM is now at Connected; a send failure is logged only).
			struct mqtt_pubcomp_param comp = {evt->param.pubrel.message_id};
			int ret = mqtt_publish_qos2_complete(&ctx_.client, &comp);
			if (ret < 0) {
				LOG_ERR("Failed to send PUBCOMP: %d", ret);
			}
			break;
		}
		case MQTT_EVT_PUBCOMP: {
			evt_pubcomp comp = {evt->param.pubcomp.message_id};
			sm_.process_event(comp);
			ctx_.publish_op_start_ms = 0;  /* QoS 2 complete; clear timeout timer */
			sm_.process_event(evt_publish_done{});
			break;
		}
		case MQTT_EVT_SUBACK: {
			evt_suback ack = {evt->param.suback.message_id, MQTT_QOS_0_AT_MOST_ONCE};
			sm_.process_event(ack);
			break;
		}
		case MQTT_EVT_UNSUBACK: {
			evt_unsuback ack = {evt->param.unsuback.message_id};
			sm_.process_event(ack);
			break;
		}
		case MQTT_EVT_PINGRESP: {
			evt_pingresp ping = {};
			sm_.process_event(ping);
			break;
		}
		default:
			break;
		}
	}
};

} // namespace sml_mqtt_cli

// ============================================================================
// C API for use from C applications
// ============================================================================

extern "C" {

typedef void* sml_mqtt_client_handle_t;

/**
 * @brief Size macro for MQTT client storage (for static allocation)
 * Use this macro for static array declarations to avoid VLA issues
 */
#define SML_MQTT_CLIENT_SIZE sizeof(sml_mqtt_cli::mqtt_client)

/**
 * @brief Get the size required for MQTT client storage
 * @return Size in bytes needed for sml_mqtt_client_init_with_storage()
 *
 * Use this to allocate storage for the client:
 * @code
 * static uint8_t client_storage[SML_MQTT_CLIENT_SIZE];
 * @endcode
 */
static inline size_t sml_mqtt_client_get_size(void) {
	return SML_MQTT_CLIENT_SIZE;
}

/**
 * @brief Initialize a new MQTT client instance using user-provided storage
 * @param storage Pointer to storage buffer (must be at least sml_mqtt_client_get_size() bytes)
 * @param storage_size Size of storage buffer
 * @return Handle to the client or NULL on failure
 *
 * @note Storage must remain valid for the lifetime of the client
 * @note Storage must be properly aligned (alignof(mqtt_client))
 *
 * Example:
 * @code
 * static uint8_t client_storage[sml_mqtt_client_get_size()] __attribute__((aligned(8)));
 * sml_mqtt_client_handle_t client = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));
 * @endcode
 */
static inline sml_mqtt_client_handle_t sml_mqtt_client_init_with_storage(void *storage, size_t storage_size) {
	if (!storage || storage_size < sizeof(sml_mqtt_cli::mqtt_client)) {
		return nullptr;
	}

	// Check alignment
	if (reinterpret_cast<uintptr_t>(storage) % alignof(sml_mqtt_cli::mqtt_client) != 0) {
		return nullptr;
	}

	// Placement new - construct object in user-provided memory
	return new (storage) sml_mqtt_cli::mqtt_client();
}

/**
 * @brief Deinitialize an MQTT client instance (does not free storage)
 * @param handle Client handle
 *
 * @note This calls the destructor but does not free memory.
 *       Storage remains owned by the caller.
 */
static inline void sml_mqtt_client_deinit(sml_mqtt_client_handle_t handle) {
	if (handle) {
		// Call destructor explicitly
		static_cast<sml_mqtt_cli::mqtt_client*>(handle)->~mqtt_client();
		// Storage is not freed - user owns it
	}
}

/**
 * @brief Initialize MQTT client
 * @param handle Client handle
 * @param client_id Client identifier string
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_init(sml_mqtt_client_handle_t handle, const char *client_id) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->init(client_id);
}

#if defined(CONFIG_MQTT_LIB_TLS)
/**
 * @brief TLS configuration for C API consumers
 *
 * Security tags must be pre-registered via tls_credential_add() before
 * calling sml_mqtt_client_init_tls().
 */
typedef struct {
	const sec_tag_t *sec_tag_list;  /**< Array of registered security tags */
	size_t           sec_tag_count; /**< Number of tags */
	int              peer_verify;   /**< TLS_PEER_VERIFY_REQUIRED or TLS_PEER_VERIFY_NONE */
} sml_mqtt_tls_config_t;

/**
 * @brief Initialize MQTT client with TLS credentials
 * @param handle Client handle
 * @param client_id Client identifier string
 * @param tls TLS configuration (sec_tag_list, peer_verify)
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_init_tls(sml_mqtt_client_handle_t handle, const char *client_id,
                                            const sml_mqtt_tls_config_t *tls) {
	if (!handle || !tls) {
		return -EINVAL;
	}
	sml_mqtt_cli::tls_config cpp_tls = {
		.sec_tag_list  = tls->sec_tag_list,
		.sec_tag_count = tls->sec_tag_count,
		.peer_verify   = tls->peer_verify,
	};
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->init(client_id, &cpp_tls);
}
#endif /* CONFIG_MQTT_LIB_TLS */

/**
 * @brief Connect to MQTT broker
 * @param handle Client handle
 * @param hostname Broker hostname
 * @param port Broker port
 * @param use_tls Use TLS/SSL
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_connect(sml_mqtt_client_handle_t handle, const char *hostname,
                             uint16_t port, bool use_tls) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->connect(hostname, port, use_tls);
}

/**
 * @brief Disconnect from MQTT broker
 * @param handle Client handle
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_disconnect(sml_mqtt_client_handle_t handle) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->disconnect();
}

/**
 * @brief Publish a message
 * @param handle Client handle
 * @param topic Topic string
 * @param payload Message payload
 * @param payload_len Payload length in bytes
 * @param qos Quality of Service level
 * @param retain Retain flag
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_publish(sml_mqtt_client_handle_t handle, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             enum mqtt_qos qos, bool retain) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->publish(topic, payload, payload_len, qos, retain);
}

/**
 * @brief Subscribe to a topic
 * @param handle Client handle
 * @param topic Topic string
 * @param qos Quality of Service level
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_subscribe(sml_mqtt_client_handle_t handle, const char *topic, enum mqtt_qos qos) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->subscribe(topic, qos);
}

/**
 * @brief Unsubscribe from a topic
 * @param handle Client handle
 * @param topic Topic string
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_unsubscribe(sml_mqtt_client_handle_t handle, const char *topic) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->unsubscribe(topic);
}

/**
 * @brief Process incoming MQTT data
 * @param handle Client handle
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_input(sml_mqtt_client_handle_t handle) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->input();
}

/**
 * @brief Keep connection alive (call periodically)
 * @param handle Client handle
 * @return 0 on success, negative error code on failure
 */
static inline int sml_mqtt_client_live(sml_mqtt_client_handle_t handle) {
	if (!handle) {
		return -EINVAL;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->live();
}

/**
 * @brief Check if client is connected
 * @param handle Client handle
 * @return true if connected, false otherwise
 */
static inline bool sml_mqtt_client_is_connected(sml_mqtt_client_handle_t handle) {
	if (!handle) {
		return false;
	}
	return static_cast<sml_mqtt_cli::mqtt_client*>(handle)->is_connected();
}

/**
 * @brief Set state change callback
 * @param handle Client handle
 * @param cb Callback function
 * @param user_data User data passed to callback
 */
static inline void sml_mqtt_client_set_state_change_callback(sml_mqtt_client_handle_t handle,
                                                sml_mqtt_cli::state_change_cb_t cb,
                                                void *user_data) {
	if (!handle) {
		return;
	}
	static_cast<sml_mqtt_cli::mqtt_client*>(handle)->set_state_change_callback(cb, user_data);
}

/**
 * @brief Set publish received callback
 * @param handle Client handle
 * @param cb Callback function
 * @param user_data User data passed to callback
 */
static inline void sml_mqtt_client_set_publish_received_callback(sml_mqtt_client_handle_t handle,
                                                    sml_mqtt_cli::publish_received_cb_t cb,
                                                    void *user_data) {
	if (!handle) {
		return;
	}
	static_cast<sml_mqtt_cli::mqtt_client*>(handle)->set_publish_received_callback(cb, user_data);
}

/**
 * @brief Set the per-operation QoS timeout (applies to both publish and receive paths)
 * @param handle Client handle
 * @param ms Timeout in milliseconds; 0 to disable
 *
 * If live() is called and a QoS 1/2 acknowledgment has not arrived within
 * this time, the in-flight operation is abandoned and the SM returns to
 * Connected.  Defaults to CONFIG_SML_MQTT_CLI_PUBLISH_TIMEOUT_MS.
 */
static inline void sml_mqtt_client_set_qos_timeout_ms(sml_mqtt_client_handle_t handle,
                                                       int64_t ms) {
	if (!handle) {
		return;
	}
	static_cast<sml_mqtt_cli::mqtt_client*>(handle)->set_qos_timeout_ms(ms);
}

} // extern "C"

#endif // SML_MQTT_CLI_HPP
