/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 *
 * Example application demonstrating sml-mqtt-cli usage
 * This can be used as a template for your own MQTT applications
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <sml-mqtt-cli.hpp>

LOG_MODULE_REGISTER(mqtt_example, LOG_LEVEL_INF);

using namespace sml_mqtt_cli;

// MQTT broker configuration
#define MQTT_BROKER_HOST "mqtt.example.com"
#define MQTT_BROKER_PORT 1883
#define MQTT_USE_TLS false

// Topics
#define PUBLISH_TOPIC "devices/my_device/data"
#define SUBSCRIBE_TOPIC "devices/my_device/commands"

// Application state
static mqtt_client *g_client = nullptr;
static bool running = true;

// Callback for received messages
static void on_message_received(void *user_data, const char *topic,
                                const uint8_t *payload, size_t len,
                                enum mqtt_qos qos)
{
	LOG_INF("Received message on '%s' (QoS %d):", topic, qos);
	LOG_INF("  Payload: %.*s", (int)len, payload);

	// Handle commands
	if (strncmp((const char*)payload, "stop", 4) == 0) {
		LOG_WRN("Received stop command");
		running = false;
	} else if (strncmp((const char*)payload, "ping", 4) == 0) {
		LOG_INF("Received ping, sending pong");
		if (g_client && g_client->is_connected()) {
			const char *response = "pong";
			g_client->publish("devices/my_device/responses",
			                 (const uint8_t*)response, strlen(response),
			                 MQTT_QOS_0_AT_MOST_ONCE, false);
		}
	}
}

// Callback for state changes
static void on_state_change(void *user_data, const char *old_state, const char *new_state)
{
	LOG_INF("State transition: %s -> %s", old_state, new_state);
}

int main(void)
{
	LOG_INF("=== SML MQTT Client Example ===");
	LOG_INF("Broker: %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

	// Wait for network to be ready
	LOG_INF("Waiting for network...");
	k_sleep(K_SECONDS(2));

	// Create and initialize MQTT client
	mqtt_client client;
	g_client = &client;

	int ret = client.init("my_device_001");
	if (ret != 0) {
		LOG_ERR("Failed to initialize client: %d", ret);
		return ret;
	}

	// Set up callbacks
	client.set_publish_received_callback(on_message_received, nullptr);
	client.set_state_change_callback(on_state_change, nullptr);

	// Connect to broker
	LOG_INF("Connecting to broker...");
	ret = client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USE_TLS);
	if (ret != 0) {
		LOG_ERR("Failed to connect: %d", ret);
		return ret;
	}

	// Wait for connection to establish
	for (int i = 0; i < 20; i++) {
		k_sleep(K_MSEC(100));
		client.input();
		if (client.is_connected()) {
			break;
		}
	}

	if (!client.is_connected()) {
		LOG_ERR("Connection timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("Connected! Current state: %s", client.get_state());

	// Subscribe to command topic
	LOG_INF("Subscribing to %s", SUBSCRIBE_TOPIC);
	ret = client.subscribe(SUBSCRIBE_TOPIC, MQTT_QOS_1_AT_LEAST_ONCE);
	if (ret != 0) {
		LOG_ERR("Failed to subscribe: %d", ret);
	} else {
		k_sleep(K_SECONDS(1));
		client.input();  // Process SUBACK
	}

	// Main application loop
	LOG_INF("Entering main loop...");
	int message_count = 0;

	while (running && client.is_connected()) {
		// Publish periodic status message
		if (message_count % 10 == 0) {
			char payload[128];
			snprintf(payload, sizeof(payload),
			        "{\"uptime\":%lld,\"count\":%d}",
			        k_uptime_get() / 1000, message_count);

			LOG_INF("Publishing: %s", payload);
			ret = client.publish(PUBLISH_TOPIC, (const uint8_t*)payload,
			                    strlen(payload), MQTT_QOS_0_AT_MOST_ONCE, false);
			if (ret != 0) {
				LOG_WRN("Publish failed: %d", ret);
			}
		}

		message_count++;

		// Process incoming MQTT messages
		ret = client.input();
		if (ret < 0 && ret != -EAGAIN && ret != -ENOTCONN) {
			LOG_WRN("Input error: %d", ret);
		}

		// Send keepalive if needed
		ret = client.live();
		if (ret != 0 && ret != -EAGAIN) {
			LOG_WRN("Keepalive error: %d", ret);
		}

		// Check connection status periodically
		if (message_count % 100 == 0) {
			if (!client.is_connected()) {
				LOG_ERR("Lost connection to broker");
				break;
			}
		}

		k_sleep(K_SECONDS(1));
	}

	// Clean shutdown
	LOG_INF("Shutting down...");
	client.disconnect();

	LOG_INF("=== Example Complete ===");
	return 0;
}
