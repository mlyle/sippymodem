// SPDX-License-Identifier: BSD-3-Clause

#include <session.h>

#include "test.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct fake_signaling_control {
	unsigned int advertised_rtp_port;
	unsigned int creates;
	unsigned int destroys;
	unsigned int dials;
	unsigned int answers;
	unsigned int hangups;
	bool fail_open;
	bool emit_media;
	bool emit_inactive;
};

static struct fake_signaling_control fake;

struct sip_signaling {
	struct app_config config;
	struct sip_signaling_callbacks callbacks;
	bool opened;
};

struct sip_signaling *sip_signaling_create(
	const struct app_config *config,
	const struct sip_signaling_callbacks *callbacks)
{
	struct sip_signaling *signaling;

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

void sip_signaling_destroy(
	struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return;
	fake.destroys++;
	free(signaling);
}

enum sip_signaling_result sip_signaling_open(
	struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (fake.fail_open)
		return SIP_SIGNALING_LIBRARY_ERROR;
	signaling->opened = true;
	if (signaling->callbacks.ready != NULL)
		signaling->callbacks.ready(signaling->callbacks.context);
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_close(
	struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	signaling->opened = false;
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_step(
	struct sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	if (signaling == NULL || !signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	if (fake.emit_media && signaling->callbacks.media_changed != NULL) {
		const struct sip_media_event event = {
			.state = fake.emit_inactive ?
					SIP_MEDIA_INACTIVE :
					SIP_MEDIA_CONFIGURED,
			.remote_address_family =
				SIP_MEDIA_ADDRESS_IPV4,
			.codec = MEDIA_CODEC_PCMU,
			.payload_type = 0,
			.sample_rate = MEDIA_SAMPLE_RATE,
			.channels = 1,
			.local_port = signaling->config.media.rtp_listen_port,
			.remote_address = "127.0.0.1",
			.remote_port = 42000,
		};

		fake.emit_media = false;
		signaling->callbacks.media_changed(
			signaling->callbacks.context, &event);
	}
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target)
{
	if (signaling == NULL || target == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	fake.dials++;
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_answer(
	struct sip_signaling *signaling)
{
	if (signaling == NULL || !signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	fake.answers++;
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_hangup(
	struct sip_signaling *signaling)
{
	if (signaling == NULL || !signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	fake.hangups++;
	return SIP_SIGNALING_OK;
}

struct callback_capture {
	struct sip_session *session;
	enum sip_session_result close_result;
	enum sip_rtp_result binding;
	unsigned int ready_calls;
	unsigned int media_calls;
	bool close_on_media;
};

static void on_ready(void *context)
{
	struct callback_capture *capture = context;

	if (sip_session_is_open(capture->session))
		capture->ready_calls++;
}

static void on_media(
	void *context, const struct sip_media_event *event)
{
	struct callback_capture *capture = context;

	(void)event;
	capture->media_calls++;
	capture->binding =
		sip_session_last_media_binding(capture->session);
	if (capture->close_on_media)
		capture->close_result =
			sip_session_close(capture->session);
}

static struct sip_session *create_session(
	struct callback_capture *capture)
{
	struct app_config config;
	const struct sip_signaling_callbacks callbacks = {
		.context = capture,
		.ready = on_ready,
		.media_changed = on_media,
	};

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	return sip_session_create(&config, NULL, &callbacks);
}

static bool test_owns_transport_lifetimes(void)
{
	struct callback_capture capture = { 0 };
	struct sip_session *session;
	struct rtp_playout_report report;
	uint8_t source_octets[8];
	int16_t samples[8];
	size_t drained = 99;

	memset(&fake, 0, sizeof(fake));
	session = create_session(&capture);
	CHECK(session != NULL, "session creation failed");
	capture.session = session;
	CHECK(sip_session_dial(session, "sip:peer@127.0.0.1") ==
		      SIP_SIGNALING_NOT_OPEN,
	      "closed session accepted call control");
	CHECK(sip_session_frame_samples(session) == 8 &&
		      sip_session_open(session) == SIP_SESSION_OK &&
		      capture.ready_calls == 1 && fake.creates == 1 &&
		      fake.advertised_rtp_port != 0 &&
		      fake.advertised_rtp_port ==
			      sip_session_rtp_local_port(session),
	      "session did not open RTP before signaling");
	CHECK(sip_session_open(session) ==
		      SIP_SESSION_ALREADY_OPEN,
	      "session accepted a duplicate open");

	fake.emit_media = true;
	CHECK(sip_session_step(session, 0, &drained) ==
		      SIP_SESSION_OK &&
		      drained == 0 && capture.media_calls == 1 &&
		      capture.binding == SIP_RTP_OK &&
		      sip_session_media_is_configured(session),
	      "session did not bind negotiated media");
	CHECK(sip_session_dial(session, "sip:peer@127.0.0.1") ==
		      SIP_SIGNALING_OK &&
		      sip_session_answer(session) ==
			      SIP_SIGNALING_OK &&
		      sip_session_hangup(session) ==
			      SIP_SIGNALING_OK &&
		      fake.dials == 1 && fake.answers == 1 && fake.hangups == 1 &&
		      sip_session_last_signaling_result(session) ==
			      SIP_SIGNALING_OK,
	      "session did not preserve typed call control");
	memset(samples, 1, sizeof(samples));
	memset(source_octets, 0, sizeof(source_octets));
	CHECK(sip_session_play(session, samples, source_octets,
				      ARRAY_LENGTH(samples),
				      &report) == MEDIA_OK &&
		      !report.stream_active &&
		      memcmp(samples, (int16_t[8]){ 0 }, sizeof(samples)) == 0 &&
		      memcmp(source_octets,
			     (uint8_t[8]){ 0xff, 0xff, 0xff, 0xff,
					   0xff, 0xff, 0xff, 0xff },
			     sizeof(source_octets)) == 0 &&
		      sip_session_send(session, samples,
					       ARRAY_LENGTH(samples)) ==
			      RTP_ENDPOINT_OK,
	      "session did not expose exact-frame RTP media");
	fake.emit_media = true;
	fake.emit_inactive = true;
	capture.close_on_media = true;
	CHECK(sip_session_step(session, 0, NULL) ==
		      SIP_SESSION_OK &&
		      capture.close_result == SIP_SESSION_OK &&
		      !sip_session_is_open(session) &&
		      !sip_session_media_is_configured(session) &&
		      sip_session_rtp_local_port(session) == 0 &&
		      fake.destroys == 1,
	      "session close was not safe from a forwarded callback");
	CHECK(sip_session_step(session, 0, NULL) ==
		      SIP_SESSION_NOT_OPEN &&
		      sip_session_close(session) ==
			      SIP_SESSION_NOT_OPEN &&
		      sip_session_send(session, samples,
					       ARRAY_LENGTH(samples)) ==
			      RTP_ENDPOINT_NOT_OPEN,
	      "closed session lifecycle is inconsistent");
	sip_session_destroy(session);
	return true;
}

static bool test_open_failure_is_transactional(void)
{
	struct callback_capture capture = { 0 };
	struct sip_session *session;

	memset(&fake, 0, sizeof(fake));
	session = create_session(&capture);
	CHECK(session != NULL, "failure session creation failed");
	capture.session = session;
	fake.fail_open = true;
	CHECK(sip_session_open(session) ==
		      SIP_SESSION_SIGNALING_ERROR &&
		      !sip_session_is_open(session) &&
		      sip_session_rtp_local_port(session) == 0 &&
		      fake.creates == 1 && fake.destroys == 1,
	      "signaling failure retained a partial session");
	fake.fail_open = false;
	CHECK(sip_session_open(session) == SIP_SESSION_OK &&
		      sip_session_close(session) == SIP_SESSION_OK &&
		      fake.creates == 2 && fake.destroys == 2,
	      "session could not reopen after a signaling failure");
	sip_session_destroy(session);
	return true;
}

static bool test_close_clears_media_without_inactive_event(void)
{
	struct callback_capture capture = { 0 };
	struct sip_session *session;

	memset(&fake, 0, sizeof(fake));
	session = create_session(&capture);
	CHECK(session != NULL, "media reset session creation failed");
	capture.session = session;
	CHECK(sip_session_open(session) == SIP_SESSION_OK,
	      "media reset session did not open");
	fake.emit_media = true;
	CHECK(sip_session_step(session, 0, NULL) ==
		      SIP_SESSION_OK &&
		      sip_session_media_is_configured(session),
	      "media reset session did not configure media");
	CHECK(sip_session_close(session) == SIP_SESSION_OK &&
		      !sip_session_media_is_configured(session),
	      "session close retained negotiated media state");
	CHECK(sip_session_open(session) == SIP_SESSION_OK &&
		      !sip_session_media_is_configured(session),
	      "session reopen inherited negotiated media state");
	CHECK(sip_session_close(session) == SIP_SESSION_OK,
	      "reopened media reset session did not close");
	sip_session_destroy(session);
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
		{ "close clears media", test_close_clears_media_without_inactive_event },
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
