// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_session.h>

#include "test.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct fake_signaling_control {
	unsigned int advertised_rtp_port;
	unsigned int creates;
	unsigned int destroys;
	bool fail_open;
	bool emit_media;
	bool emit_inactive;
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
	fake.advertised_rtp_port = config->media.rtp_listen_port;
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
	if (fake.fail_open)
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	signaling->opened = true;
	if (signaling->callbacks.ready != NULL)
		signaling->callbacks.ready(signaling->callbacks.context);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_close(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	signaling->opened = false;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_step(
	struct sippymodem_sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	if (signaling == NULL || !signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	if (fake.emit_media && signaling->callbacks.media_changed != NULL) {
		const struct sippymodem_sip_media_event event = {
			.state = fake.emit_inactive ?
					SIPPYMODEM_SIP_MEDIA_INACTIVE :
					SIPPYMODEM_SIP_MEDIA_CONFIGURED,
			.remote_address_family =
				SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
			.codec = SIPPYMODEM_CODEC_PCMU,
			.payload_type = 0,
			.sample_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE,
			.channels = 1,
			.local_port = signaling->config.media.rtp_listen_port,
			.remote_address = "127.0.0.1",
			.remote_port = 42000,
		};

		fake.emit_media = false;
		signaling->callbacks.media_changed(
			signaling->callbacks.context, &event);
	}
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

struct callback_capture {
	struct sippymodem_session *session;
	enum sippymodem_session_result close_result;
	enum sippymodem_sip_rtp_result binding;
	unsigned int ready_calls;
	unsigned int media_calls;
	bool close_on_media;
};

static void on_ready(void *context)
{
	struct callback_capture *capture = context;

	if (sippymodem_session_is_open(capture->session))
		capture->ready_calls++;
}

static void on_media(
	void *context, const struct sippymodem_sip_media_event *event)
{
	struct callback_capture *capture = context;

	(void)event;
	capture->media_calls++;
	capture->binding =
		sippymodem_session_last_media_binding(capture->session);
	if (capture->close_on_media)
		capture->close_result =
			sippymodem_session_close(capture->session);
}

static struct sippymodem_session *create_session(
	struct callback_capture *capture)
{
	struct sippymodem_config config;
	const struct sippymodem_sip_signaling_callbacks callbacks = {
		.context = capture,
		.ready = on_ready,
		.media_changed = on_media,
	};

	sippymodem_config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	return sippymodem_session_create(&config, NULL, &callbacks);
}

static bool test_owns_transport_lifetimes(void)
{
	struct callback_capture capture = { 0 };
	struct sippymodem_session *session;
	size_t drained = 99;

	memset(&fake, 0, sizeof(fake));
	session = create_session(&capture);
	CHECK(session != NULL, "session creation failed");
	capture.session = session;
	CHECK(sippymodem_session_frame_samples(session) == 8 &&
		      sippymodem_session_open(session) == SIPPYMODEM_SESSION_OK &&
		      capture.ready_calls == 1 && fake.creates == 1 &&
		      fake.advertised_rtp_port != 0 &&
		      fake.advertised_rtp_port ==
			      sippymodem_session_rtp_local_port(session),
	      "session did not open RTP before signaling");
	CHECK(sippymodem_session_open(session) ==
		      SIPPYMODEM_SESSION_ALREADY_OPEN,
	      "session accepted a duplicate open");

	fake.emit_media = true;
	CHECK(sippymodem_session_step(session, 0, &drained) ==
		      SIPPYMODEM_SESSION_OK &&
		      drained == 0 && capture.media_calls == 1 &&
		      capture.binding == SIPPYMODEM_SIP_RTP_OK,
	      "session did not bind negotiated media");
	fake.emit_media = true;
	fake.emit_inactive = true;
	capture.close_on_media = true;
	CHECK(sippymodem_session_step(session, 0, NULL) ==
		      SIPPYMODEM_SESSION_OK &&
		      capture.close_result == SIPPYMODEM_SESSION_OK &&
		      !sippymodem_session_is_open(session) &&
		      sippymodem_session_rtp_local_port(session) == 0 &&
		      fake.destroys == 1,
	      "session close was not safe from a forwarded callback");
	CHECK(sippymodem_session_step(session, 0, NULL) ==
		      SIPPYMODEM_SESSION_NOT_OPEN &&
		      sippymodem_session_close(session) ==
			      SIPPYMODEM_SESSION_NOT_OPEN,
	      "closed session lifecycle is inconsistent");
	sippymodem_session_destroy(session);
	return true;
}

static bool test_open_failure_is_transactional(void)
{
	struct callback_capture capture = { 0 };
	struct sippymodem_session *session;

	memset(&fake, 0, sizeof(fake));
	session = create_session(&capture);
	CHECK(session != NULL, "failure session creation failed");
	capture.session = session;
	fake.fail_open = true;
	CHECK(sippymodem_session_open(session) ==
		      SIPPYMODEM_SESSION_SIGNALING_ERROR &&
		      !sippymodem_session_is_open(session) &&
		      sippymodem_session_rtp_local_port(session) == 0 &&
		      fake.creates == 1 && fake.destroys == 1,
	      "signaling failure retained a partial session");
	fake.fail_open = false;
	CHECK(sippymodem_session_open(session) == SIPPYMODEM_SESSION_OK &&
		      sippymodem_session_close(session) == SIPPYMODEM_SESSION_OK &&
		      fake.creates == 2 && fake.destroys == 2,
	      "session could not reopen after a signaling failure");
	sippymodem_session_destroy(session);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "transport lifetimes", test_owns_transport_lifetimes },
		{ "transactional open failure", test_open_failure_is_transactional },
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
