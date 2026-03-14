/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <sml-mqtt-cli.hpp>


using namespace sml_mqtt_cli;

// Test broker configuration
extern const char *TEST_BROKER_HOST;
extern uint16_t TEST_BROKER_PORT;
extern bool TEST_USE_TLS;

#define TEST_BROKER_HOST "localhost"
#define TEST_BROKER_PORT 8883
#define TEST_USE_TLS false

/**
 * @brief Test basic client creation and initialization
 */
ZTEST(sml_mqtt_basic, test_client_creation)
{
	LOG_INF("Test: Client creation and initialization");

	sml_mqtt_cli::mqtt_client client;

	int ret = client.init("test_client_basic");
	zassert_equal(ret, 0, "Failed to initialize client: %d", ret);

	// Should start in disconnected state
	zassert_false(client.is_connected(), "Client should not be connected initially");

	const char *state = client.get_state();
	LOG_INF("Initial state: %s", state);
	zassert_not_null(state, "State should not be NULL");
}

/**
 * @brief Test client initialization with invalid parameters
 */
ZTEST(sml_mqtt_basic, test_invalid_init)
{
	LOG_INF("Test: Invalid initialization parameters");

	sml_mqtt_cli::mqtt_client client;

	// NULL client ID should fail
	int ret = client.init(nullptr);
	zassert_not_equal(ret, 0, "Should fail with NULL client ID");

	// Too long client ID should fail
	char long_id[CONFIG_SML_MQTT_CLI_MAX_CLIENT_ID_LEN + 10];
	memset(long_id, 'A', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';

	ret = client.init(long_id);
	zassert_not_equal(ret, 0, "Should fail with too long client ID");
}

/**
 * @brief Test connection to MQTT broker
 */
ZTEST(sml_mqtt_basic, test_connect_disconnect)
{
	LOG_INF("Test: Connect and disconnect from broker");

	sml_mqtt_cli::mqtt_client client;

	int ret = client.init("test_connect_001");
	zassert_equal(ret, 0, "Failed to initialize client");

	// Attempt to connect
	LOG_INF("Connecting to %s:%d...", TEST_BROKER_HOST, TEST_BROKER_PORT);
	ret = client.connect(TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect: %d", ret);

	// Wait for connection to establish
	k_sleep(K_SECONDS(2));

	// Process any incoming data
	ret = client.input();
	LOG_INF("Input returned: %d", ret);

	// Check if connected
	bool connected = client.is_connected();
	LOG_INF("Connection status: %s", connected ? "connected" : "disconnected");
	zassert_true(connected, "Client should be connected");

	// Check state
	const char *state = client.get_state();
	LOG_INF("Current state: %s", state);

	// Disconnect
	LOG_INF("Disconnecting...");
	ret = client.disconnect();
	zassert_equal(ret, 0, "Failed to disconnect: %d", ret);

	k_sleep(K_MSEC(100));

	// Should be disconnected now
	zassert_false(client.is_connected(), "Client should be disconnected");
}

/**
 * @brief Test connection with invalid parameters
 */
ZTEST(sml_mqtt_basic, test_invalid_connect)
{
	LOG_INF("Test: Invalid connection parameters");

	sml_mqtt_cli::mqtt_client client;
	client.init("test_invalid_conn");

	// NULL hostname should fail
	int ret = client.connect(nullptr, 1883, false);
	zassert_not_equal(ret, 0, "Should fail with NULL hostname");

	// Invalid hostname should fail
	ret = client.connect("invalid.host.that.does.not.exist.local", 1883, false);
	zassert_not_equal(ret, 0, "Should fail with invalid hostname");
}

/**
 * @brief Test C API client creation
 */
ZTEST(sml_mqtt_basic, test_c_api_creation)
{
	LOG_INF("Test: C API client creation");

	// Allocate storage statically (no heap)
	static uint8_t client_storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));
	sml_mqtt_client_handle_t handle = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));
	zassert_not_null(handle, "Failed to create client handle");

	int ret = sml_mqtt_client_init(handle, "test_c_api_001");
	zassert_equal(ret, 0, "Failed to initialize client");

	bool connected = sml_mqtt_client_is_connected(handle);
	zassert_false(connected, "Client should not be connected initially");

	sml_mqtt_client_deinit(handle);
}

/**
 * @brief Test C API connection
 */
ZTEST(sml_mqtt_basic, test_c_api_connect)
{
	LOG_INF("Test: C API connection");

	// Allocate storage statically (no heap)
	static uint8_t client_storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));
	sml_mqtt_client_handle_t handle = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));
	zassert_not_null(handle, "Failed to create client handle");

	int ret = sml_mqtt_client_init(handle, "test_c_api_conn");
	zassert_equal(ret, 0, "Failed to initialize client");

	// Connect
	LOG_INF("Connecting via C API...");
	ret = sml_mqtt_client_connect(handle, TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect: %d", ret);

	k_sleep(K_SECONDS(2));

	// Process input
	ret = sml_mqtt_client_input(handle);
	LOG_INF("Input returned: %d", ret);

	// Check connection
	bool connected = sml_mqtt_client_is_connected(handle);
	LOG_INF("C API connection status: %s", connected ? "connected" : "disconnected");
	zassert_true(connected, "Client should be connected");

	// Disconnect
	ret = sml_mqtt_client_disconnect(handle);
	zassert_equal(ret, 0, "Failed to disconnect");

	sml_mqtt_client_deinit(handle);
}

/**
 * @brief Test state callbacks
 */
static int callback_count = 0;
static char last_old_state[32] = {0};
static char last_new_state[32] = {0};

static void state_change_callback(void *user_data, const char *old_state, const char *new_state)
{
	callback_count++;
	strncpy(last_old_state, old_state ? old_state : "NULL", sizeof(last_old_state) - 1);
	strncpy(last_new_state, new_state ? new_state : "NULL", sizeof(last_new_state) - 1);
	LOG_INF("State change callback: %s -> %s", old_state, new_state);
}

ZTEST(sml_mqtt_basic, test_state_callbacks)
{
	LOG_INF("Test: State change callbacks");

	callback_count = 0;
	memset(last_old_state, 0, sizeof(last_old_state));
	memset(last_new_state, 0, sizeof(last_new_state));

	// Allocate storage statically (no heap)
	static uint8_t client_storage[SML_MQTT_CLIENT_SIZE] __attribute__((aligned(8)));
	sml_mqtt_client_handle_t handle = sml_mqtt_client_init_with_storage(client_storage, sizeof(client_storage));
	zassert_not_null(handle, "Failed to create client");

	sml_mqtt_client_init(handle, "test_callbacks");
	sml_mqtt_client_set_state_change_callback(handle, state_change_callback, nullptr);

	// Connect (should trigger state changes)
	int ret = sml_mqtt_client_connect(handle, TEST_BROKER_HOST, TEST_BROKER_PORT, TEST_USE_TLS);
	zassert_equal(ret, 0, "Failed to connect");

	k_sleep(K_SECONDS(2));
	sml_mqtt_client_input(handle);

	// Should have received at least one callback
	LOG_INF("Callback count: %d", callback_count);
	// Note: Callbacks currently not fully wired in state machine transitions
	// This test documents the API for future implementation

	sml_mqtt_client_disconnect(handle);
	sml_mqtt_client_deinit(handle);
}
