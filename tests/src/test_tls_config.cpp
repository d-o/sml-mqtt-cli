/*
 * Copyright (c) 2025 Dean Sellers (dean@sellers.id.au)
 * SPDX-License-Identifier: MIT
 */

/*
 * TLS configuration tests.
 *
 * These tests do not require a broker: they either check fields set by init()
 * directly, or test connect() paths that return before calling mqtt_connect()
 * (i.e. early validation failures).
 *
 * TLS-specific tests are skipped when CONFIG_MQTT_LIB_TLS is not enabled.
 * The -ENOTSUP test is skipped when CONFIG_MQTT_LIB_TLS is enabled (tested
 * via the TLS build instead).
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <sml-mqtt-cli.hpp>

#if !defined(CONFIG_ETH_NATIVE_TAP)

/**
 * Without CONFIG_MQTT_LIB_TLS, connect() with use_tls=true must return
 * -ENOTSUP and must not attempt a network connection.
 */
ZTEST(sml_mqtt_tls, test_connect_tls_no_kconfig_returns_enotsup)
{
#if defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_enotsup");
	zassert_equal(ret, 0, "init failed: %d", ret);

	ret = client.connect("127.0.0.1", 8883, true);
	zassert_equal(ret, -ENOTSUP,
		      "expected -ENOTSUP without CONFIG_MQTT_LIB_TLS, got %d", ret);
#endif
}

/**
 * With CONFIG_MQTT_LIB_TLS, connect() with use_tls=true but no TLS
 * credentials provided at init() must return -EINVAL.
 */
ZTEST(sml_mqtt_tls, test_connect_tls_without_creds_returns_einval)
{
#if !defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_no_creds");
	zassert_equal(ret, 0, "init failed: %d", ret);

	/* No tls_config passed to init(); use_tls=true must be rejected early. */
	ret = client.connect("127.0.0.1", 8883, true);
	zassert_equal(ret, -EINVAL,
		      "expected -EINVAL (no creds), got %d", ret);
#endif
}

/**
 * init() with a valid tls_config must copy sec_tag IDs and peer_verify into
 * the client context so connect() can wire them into the Zephyr transport.
 */
ZTEST(sml_mqtt_tls, test_init_tls_copies_credentials)
{
#if !defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	static const sec_tag_t tags[] = {42};
	sml_mqtt_cli::tls_config tls = {
		.sec_tag_list  = tags,
		.sec_tag_count = 1,
		.peer_verify   = TLS_PEER_VERIFY_REQUIRED,
	};

	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_copy_test", &tls);
	zassert_equal(ret, 0, "init with TLS failed: %d", ret);

	const auto &ctx = client.get_context();
	zassert_true(ctx.tls_enabled, "tls_enabled must be set");
	zassert_equal(ctx.tls_sec_tag_count, 1U, "sec_tag_count mismatch");
	zassert_equal(ctx.tls_sec_tags[0], (sec_tag_t)42, "sec_tag value mismatch");
	zassert_equal(ctx.tls_peer_verify, TLS_PEER_VERIFY_REQUIRED,
		      "peer_verify mismatch");
#endif
}

/**
 * init() with a null sec_tag_list must be rejected.
 */
ZTEST(sml_mqtt_tls, test_init_tls_null_tag_list_returns_einval)
{
#if !defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	sml_mqtt_cli::tls_config tls = {
		.sec_tag_list  = nullptr,
		.sec_tag_count = 1,
		.peer_verify   = TLS_PEER_VERIFY_REQUIRED,
	};

	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_null_tags", &tls);
	zassert_equal(ret, -EINVAL,
		      "expected -EINVAL for null sec_tag_list, got %d", ret);
#endif
}

/**
 * init() with sec_tag_count exceeding CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS
 * must be rejected.
 */
ZTEST(sml_mqtt_tls, test_init_tls_too_many_tags_returns_einval)
{
#if !defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	sec_tag_t tags[CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS + 1];
	memset(tags, 0, sizeof(tags));

	sml_mqtt_cli::tls_config tls = {
		.sec_tag_list  = tags,
		.sec_tag_count = CONFIG_SML_MQTT_CLI_MAX_TLS_SEC_TAGS + 1,
		.peer_verify   = TLS_PEER_VERIFY_REQUIRED,
	};

	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_many_tags", &tls);
	zassert_equal(ret, -EINVAL,
		      "expected -EINVAL for too many tags, got %d", ret);
#endif
}

/**
 * init() without a tls_config (plain non-TLS init) must leave tls_enabled
 * false, so a subsequent connect(..., true) returns -EINVAL.
 */
ZTEST(sml_mqtt_tls, test_init_plain_tls_disabled)
{
#if !defined(CONFIG_MQTT_LIB_TLS)
	ztest_test_skip();
#else
	sml_mqtt_cli::mqtt_client client;
	int ret = client.init("tls_plain_init");
	zassert_equal(ret, 0, "plain init failed: %d", ret);

	const auto &ctx = client.get_context();
	zassert_false(ctx.tls_enabled, "tls_enabled must be false for plain init");
#endif
}

#endif /* !CONFIG_ETH_NATIVE_TAP */
