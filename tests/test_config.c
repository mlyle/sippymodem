// SPDX-License-Identifier: BSD-3-Clause

#include <config.h>

#include "test.h"

#include <stdlib.h>
#include <string.h>

static const char *const config_environment[] = {
	"SIP_LOGIN",       "SIP_USER",        "SIP_PASSWORD",
	"SIP_DOMAIN",      "SIP_SERVER",      "SIP_PUBLIC_ADDR",
	"SIP_BOUND_ADDR",  "SIP_ID",          "SIP_REG_URI",
	"SIP_PROXY",       "SIP_REALM",       "SIP_TARGET",
	"SIP_PORT",        "SIP_LOG_LEVEL",   "SIP_REGISTER",
	"SIP_AUTO_ANSWER", "SIP_CODEC",       "SIP_RTP_PORT",
	"SIP_JB_MS",       "SIP_FRAME_MS",    "SIP_REMOTE_CLOCK",
	"SIP_GAIN",        "SIP_TX_GAIN",     "SIP_RX_GAIN",
	"SIP_RECORD_PREFIX", "SIP_TRACE", "SIP_PCM_CODEC_GRID",
};

static void clear_environment(void)
{
	for (size_t index = 0; index < ARRAY_LENGTH(config_environment); ++index)
		unsetenv(config_environment[index]);
}

static bool test_defaults(void)
{
	struct app_config config;

	config_init(&config);
	CHECK(config.sip.listen_port == 5060, "wrong default SIP port");
	CHECK(config.sip.log_level == 4, "wrong default SIP log level");
	CHECK(!config.sip.trace_enabled, "trace should be opt-in");
	CHECK(config.sip.registration_enabled,
	      "SIP registration should be enabled by default");
	CHECK(!config.sip.automatic_answer,
	      "automatic answer should be disabled by default");
	CHECK(config.media.codec == MEDIA_CODEC_PCMU,
	      "wrong default media codec");
	CHECK(strcmp(media_codec_name(config.media.codec),
		     "PCMU/8000/1") == 0,
	      "wrong canonical codec name");
	CHECK(config.media.rtp_listen_port == 0,
	      "RTP should use an automatic port by default");
	CHECK(config.media.jitter_buffer_ms == 60,
	      "wrong default jitter buffer");
	CHECK(config.media.frame_duration_ms == 10,
	      "wrong default RTP frame duration");
	CHECK(!config.media.pcm_codec_grid, "physical PCM timing should be the default");
	CHECK(config.media.remote_clock, "remote clock should be enabled");
	CHECK(config.media.transmit_gain == 1.0 &&
		      config.media.receive_gain == 1.0,
	      "wrong default media gain");
	return true;
}

static bool set_test_environment(void)
{
	return setenv("SIP_USER", "alice", 1) == 0 &&
	       setenv("SIP_PASSWORD", "secret", 1) == 0 &&
	       setenv("SIP_DOMAIN", "sip.example.net", 1) == 0 &&
	       setenv("SIP_SERVER", "edge.example.net", 1) == 0 &&
	       setenv("SIP_PUBLIC_ADDR", "198.51.100.10", 1) == 0 &&
	       setenv("SIP_BOUND_ADDR", "192.0.2.10", 1) == 0 &&
	       setenv("SIP_ID", "sip:alice@example.net", 1) == 0 &&
	       setenv("SIP_REG_URI", "sip:example.net", 1) == 0 &&
	       setenv("SIP_PROXY", "sip:proxy.example.net;lr", 1) == 0 &&
	       setenv("SIP_REALM", "example.net", 1) == 0 &&
	       setenv("SIP_TARGET", "sip:bob@example.net", 1) == 0 &&
	       setenv("SIP_PORT", "5070", 1) == 0 &&
	       setenv("SIP_LOG_LEVEL", "6", 1) == 0 &&
	       setenv("SIP_TRACE", "yes", 1) == 0 &&
	       setenv("SIP_REGISTER", "yes", 1) == 0 &&
	       setenv("SIP_AUTO_ANSWER", "on", 1) == 0 &&
	       setenv("SIP_CODEC", "CLEARMODE", 1) == 0 &&
	       setenv("SIP_RTP_PORT", "4000", 1) == 0 &&
	       setenv("SIP_JB_MS", "30", 1) == 0 &&
	       setenv("SIP_FRAME_MS", "20", 1) == 0 &&
	       setenv("SIP_REMOTE_CLOCK", "false", 1) == 0 &&
	       setenv("SIP_PCM_CODEC_GRID", "true", 1) == 0 &&
	       setenv("SIP_GAIN", "0.75", 1) == 0 &&
	       setenv("SIP_RX_GAIN", "0.80", 1) == 0 &&
	       setenv("SIP_RECORD_PREFIX", "/tmp/modem-call", 1) == 0;
}

static bool test_environment_overrides(void)
{
	struct app_config config;
	struct config_error error;

	clear_environment();
	CHECK(set_test_environment(), "failed to prepare configuration environment");
	config_init(&config);
	CHECK(config_apply_environment(&config, &error) == 0,
	      "%s: %s", error.setting, error.message);
	clear_environment();

	CHECK(strcmp(config.sip.user, "alice") == 0, "SIP user was not applied");
	CHECK(strcmp(config.sip.password, "secret") == 0,
	      "SIP password was not applied");
	CHECK(strcmp(config.sip.domain, "sip.example.net") == 0,
	      "SIP domain was not applied");
	CHECK(strcmp(config.sip.server, "edge.example.net") == 0,
	      "SIP server was not applied");
	CHECK(strcmp(config.sip.public_address, "198.51.100.10") == 0,
	      "public address was not applied");
	CHECK(strcmp(config.sip.bound_address, "192.0.2.10") == 0,
	      "bound address was not applied");
	CHECK(strcmp(config.sip.identity_uri, "sip:alice@example.net") == 0,
	      "identity URI was not applied");
	CHECK(strcmp(config.sip.registrar_uri, "sip:example.net") == 0,
	      "registrar URI was not applied");
	CHECK(strcmp(config.sip.proxy_uri, "sip:proxy.example.net;lr") == 0,
	      "proxy URI was not applied");
	CHECK(strcmp(config.sip.realm, "example.net") == 0,
	      "realm was not applied");
	CHECK(strcmp(config.sip.target, "sip:bob@example.net") == 0,
	      "target was not applied");
	CHECK(config.sip.listen_port == 5070 && config.sip.log_level == 6,
	      "SIP integer settings were not applied");
	CHECK(config.sip.trace_enabled, "trace setting was not applied");
	CHECK(config.sip.registration_enabled && config.sip.automatic_answer,
	      "SIP boolean settings were not applied");
	CHECK(config.media.codec == MEDIA_CODEC_CLEARMODE,
	      "media codec was not applied");
	CHECK(config.media.rtp_listen_port == 4000 &&
		      config.media.jitter_buffer_ms == 30 &&
		      config.media.frame_duration_ms == 20,
	      "media timing settings were not applied");
	CHECK(config.media.pcm_codec_grid, "explicit codec topology was not applied");
	CHECK(!config.media.remote_clock, "remote clock setting was not applied");
	CHECK(config.media.transmit_gain == 0.75 &&
		      config.media.receive_gain == 0.80,
	      "shared and directional gains have wrong precedence");
	CHECK(strcmp(config.recording.prefix, "/tmp/modem-call") == 0,
	      "recording prefix was not applied");
	return true;
}

static bool expect_invalid_value(const char *name, const char *value)
{
	struct app_config config;
	struct app_config original;
	struct config_error error;

	clear_environment();
	CHECK(setenv(name, value, 1) == 0, "failed to set %s", name);
	config_init(&config);
	original = config;
	CHECK(config_apply_environment(&config, &error) != 0,
	      "%s unexpectedly accepted '%s'", name, value);
	clear_environment();
	CHECK(strcmp(error.setting, name) == 0,
	      "error identified %s instead of %s", error.setting, name);
	CHECK(error.message[0] != '\0', "configuration error has no explanation");
	CHECK(memcmp(&config, &original, sizeof(config)) == 0,
	      "failed environment application partially changed configuration");
	return true;
}

static bool test_invalid_environment(void)
{
	char oversized[APP_CONFIG_TEXT_MAX + 1];

	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	return expect_invalid_value("SIP_PORT", "65536") &&
	       expect_invalid_value("SIP_TRACE", "sometimes") &&
	       expect_invalid_value("SIP_REGISTER", "sometimes") &&
	       expect_invalid_value("SIP_GAIN", "NaN") &&
	       expect_invalid_value("SIP_CODEC", "lossy") &&
	       expect_invalid_value("SIP_USER", oversized);
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "configuration defaults", test_defaults },
		{ "environment overrides", test_environment_overrides },
		{ "invalid environment", test_invalid_environment },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
