// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_host.h>

#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct fake_signaling_control {
	unsigned int creates;
	unsigned int destroys;
	unsigned int opens;
	unsigned int closes;
	unsigned int dials;
	unsigned int answers;
	unsigned int hangups;
};

static struct fake_signaling_control fake;

struct sippymodem_sip_signaling {
	struct sippymodem_config config;
	struct sippymodem_sip_signaling_callbacks callbacks;
	bool opened;
};

struct sippymodem_sip_signaling *sippymodem_sip_signaling_create(
	const struct sippymodem_config *config,
	const struct sippymodem_sip_signaling_callbacks *callbacks)
{
	struct sippymodem_sip_signaling *signaling;

	if (config == NULL || callbacks == NULL)
		return NULL;
	signaling = calloc(1, sizeof(*signaling));
	if (signaling == NULL)
		return NULL;
	signaling->config = *config;
	signaling->callbacks = *callbacks;
	fake.creates++;
	return signaling;
}

void sippymodem_sip_signaling_destroy(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return;
	fake.destroys++;
	free(signaling);
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_open(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_BUSY;
	signaling->opened = true;
	fake.opens++;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_close(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	signaling->opened = false;
	fake.closes++;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_step(
	struct sippymodem_sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	return signaling != NULL && signaling->opened ?
		       SIPPYMODEM_SIP_SIGNALING_OK :
		       SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
}

static void emit_media(
	struct sippymodem_sip_signaling *signaling,
	enum sippymodem_sip_media_state state)
{
	const struct sippymodem_sip_media_event event = {
		.state = state,
		.remote_address_family = SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
		.codec = signaling->config.media.codec,
		.payload_type = (unsigned int)sippymodem_codec_default_payload_type(
			signaling->config.media.codec),
		.sample_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE,
		.channels = 1,
		.local_port = signaling->config.media.rtp_listen_port,
		.remote_address = "127.0.0.1",
		.remote_port = 42000,
	};

	if (signaling->callbacks.media_changed != NULL)
		signaling->callbacks.media_changed(
			signaling->callbacks.context, &event);
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_dial(
	struct sippymodem_sip_signaling *signaling, const char *target)
{
	if (signaling == NULL || target == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	fake.dials++;
	emit_media(signaling, SIPPYMODEM_SIP_MEDIA_CONFIGURED);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_answer(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL || !signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	fake.answers++;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_hangup(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL || !signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	fake.hangups++;
	emit_media(signaling, SIPPYMODEM_SIP_MEDIA_INACTIVE);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static enum sippymodem_modem_core_result process_core(
	void *context, const int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	(void)context;
	(void)receive_samples;
	memset(transmit_samples, 0, count * sizeof(transmit_samples[0]));
	return SIPPYMODEM_MODEM_CORE_OK;
}

static struct sippymodem_host *create_host(void)
{
	struct sippymodem_config config;
	const struct sippymodem_modem_core core = {
		.process = process_core,
	};

	sippymodem_config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	return sippymodem_host_create(&config, NULL, NULL, &core);
}

static bool test_owns_lifecycle_and_call_control(void)
{
	struct sippymodem_host *host;

	memset(&fake, 0, sizeof(fake));
	host = create_host();
	CHECK(host != NULL && sippymodem_host_frame_samples(host) == 8 &&
		      !sippymodem_host_is_open(host) &&
		      !sippymodem_host_audio_is_active(host),
	      "host did not construct the complete audio owner");
	CHECK(sippymodem_host_dial(host, "sip:peer@127.0.0.1") ==
		      SIPPYMODEM_SIP_SIGNALING_NOT_OPEN,
	      "closed host accepted a call");
	CHECK(sippymodem_host_open(host) == SIPPYMODEM_SESSION_OK &&
		      sippymodem_host_is_open(host) && fake.opens == 1 &&
		      !sippymodem_host_media_is_configured(host),
	      "host did not open its signaling and RTP session");
	CHECK(sippymodem_host_dial(host, "sip:peer@127.0.0.1") ==
		      SIPPYMODEM_SIP_SIGNALING_OK &&
		      sippymodem_host_media_is_configured(host) &&
		      sippymodem_host_audio_is_active(host),
	      "negotiated media did not activate the host audio path");
	CHECK(sippymodem_host_answer(host) == SIPPYMODEM_SIP_SIGNALING_OK &&
		      sippymodem_host_hangup(host) ==
			      SIPPYMODEM_SIP_SIGNALING_OK &&
		      !sippymodem_host_media_is_configured(host) &&
		      !sippymodem_host_audio_is_active(host) && fake.dials == 1 &&
		      fake.answers == 1 && fake.hangups == 1,
	      "host did not synchronize call control with media state");
	CHECK(sippymodem_host_close(host) == SIPPYMODEM_SESSION_OK &&
		      !sippymodem_host_is_open(host) && fake.closes == 1 &&
		      fake.destroys == 1,
	      "host did not close its complete session");
	sippymodem_host_destroy(host);
	return true;
}

static bool test_creation_validation_is_transactional(void)
{
	struct sippymodem_config config;
	struct sippymodem_modem_core core = { .process = process_core };

	sippymodem_config_init(&config);
	CHECK(sippymodem_host_create(NULL, NULL, NULL, &core) == NULL &&
		      sippymodem_host_create(&config, NULL, NULL, NULL) == NULL,
	      "host accepted incomplete ownership");
	config.media.frame_duration_ms = 0;
	CHECK(sippymodem_host_create(&config, NULL, NULL, &core) == NULL,
	      "host retained an invalid media session");
	sippymodem_config_init(&config);
	config.media.receive_gain = NAN;
	CHECK(sippymodem_host_create(&config, NULL, NULL, &core) == NULL,
	      "host retained a session after audio path rejection");
	return true;
}

static bool test_destroy_closes_an_open_host(void)
{
	struct sippymodem_host *host;

	memset(&fake, 0, sizeof(fake));
	host = create_host();
	CHECK(host != NULL &&
		      sippymodem_host_open(host) == SIPPYMODEM_SESSION_OK,
	      "open destroy fixture failed");
	sippymodem_host_destroy(host);
	CHECK(fake.closes == 1 && fake.destroys == 1,
	      "destroy did not close and release an open host");
	CHECK(sippymodem_host_frame_samples(NULL) == 0 &&
		      !sippymodem_host_is_open(NULL) &&
		      !sippymodem_host_media_is_configured(NULL) &&
		      !sippymodem_host_audio_is_active(NULL) &&
		      sippymodem_host_open(NULL) ==
			      SIPPYMODEM_SESSION_INVALID_ARGUMENT &&
		      sippymodem_host_close(NULL) ==
			      SIPPYMODEM_SESSION_INVALID_ARGUMENT &&
		      sippymodem_host_dial(NULL, "sip:peer@127.0.0.1") ==
			      SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT,
	      "host null validation is inconsistent");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "lifecycle and call control", test_owns_lifecycle_and_call_control },
		{ "transactional creation",
		  test_creation_validation_is_transactional },
		{ "open destroy", test_destroy_closes_an_open_host },
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
