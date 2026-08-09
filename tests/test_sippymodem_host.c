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
	enum sippymodem_sip_signaling_result step_result;
	bool emit_invalid_media_on_step;
	bool emit_inactive_on_step;
	bool defer_hangup_media;
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

static void emit_media(
	struct sippymodem_sip_signaling *signaling,
	enum sippymodem_sip_media_state state, bool compatible);

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_step(
	struct sippymodem_sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	if (signaling == NULL || !signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	if (fake.emit_invalid_media_on_step) {
		fake.emit_invalid_media_on_step = false;
		emit_media(signaling, SIPPYMODEM_SIP_MEDIA_CONFIGURED, false);
	}
	if (fake.emit_inactive_on_step) {
		fake.emit_inactive_on_step = false;
		emit_media(signaling, SIPPYMODEM_SIP_MEDIA_INACTIVE, true);
	}
	return fake.step_result;
}

static void emit_media(
	struct sippymodem_sip_signaling *signaling,
	enum sippymodem_sip_media_state state, bool compatible)
{
	unsigned int payload_type = (unsigned int)
		sippymodem_codec_default_payload_type(
			signaling->config.media.codec);
	const struct sippymodem_sip_media_event event = {
		.state = state,
		.remote_address_family = SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
		.codec = signaling->config.media.codec,
		.payload_type = compatible ? payload_type : payload_type + 1,
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
	emit_media(signaling, SIPPYMODEM_SIP_MEDIA_CONFIGURED, true);
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
	if (!fake.defer_hangup_media)
		emit_media(signaling, SIPPYMODEM_SIP_MEDIA_INACTIVE, true);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

struct core_capture {
	enum sippymodem_modem_core_result result;
	unsigned int calls;
	bool received_silence;
};

enum {
	TEST_RECORDING_CAPACITY = 32,
};

struct recording_capture {
	enum sippymodem_recording_stream streams[TEST_RECORDING_CAPACITY];
	enum sippymodem_recording_format formats[TEST_RECORDING_CAPACITY];
	unsigned int sample_rates[TEST_RECORDING_CAPACITY];
	size_t counts[TEST_RECORDING_CAPACITY];
	size_t calls;
};

static void capture_recording(
	void *context, const struct sippymodem_recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture->calls < TEST_RECORDING_CAPACITY) {
		capture->streams[capture->calls] = block->stream;
		capture->formats[capture->calls] = block->format;
		capture->sample_rates[capture->calls] = block->sample_rate;
		capture->counts[capture->calls] = block->count;
	}
	capture->calls++;
}

static enum sippymodem_modem_core_result process_core(
	void *context, int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	struct core_capture *capture = context;
	bool silence = true;

	for (size_t index = 0; index < count; ++index)
		silence &= receive_samples[index] == 0;
	if (capture != NULL) {
		capture->calls++;
		capture->received_silence = silence;
	}
	memset(transmit_samples, 0, count * sizeof(transmit_samples[0]));
	return capture != NULL ? capture->result : SIPPYMODEM_MODEM_CORE_OK;
}

static struct sippymodem_host *create_host(
	const struct sippymodem_recorder *recorder,
	struct core_capture *core_capture)
{
	struct sippymodem_config config;
	const struct sippymodem_modem_core core = {
		.context = core_capture,
		.process = process_core,
	};

	sippymodem_config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	return sippymodem_host_create(&config, recorder, NULL, &core);
}

static bool test_owns_lifecycle_and_call_control(void)
{
	struct sippymodem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(NULL, NULL);
	CHECK(host != NULL && sippymodem_host_frame_samples(host) == 8 &&
		      !sippymodem_host_is_open(host) &&
		      !sippymodem_host_audio_is_active(host),
	      "host did not construct the complete audio owner");
	CHECK(sippymodem_host_wait_ns(host, 0) == UINT64_MAX &&
		      sippymodem_host_step(host, 0, &datagrams, &frames) ==
			      SIPPYMODEM_HOST_IDLE &&
		      datagrams == 0 && frames == 0,
	      "closed host exposed pending event-loop work");
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

static bool test_steps_recorded_media_on_the_frame_clock(void)
{
	static const enum sippymodem_recording_stream expected_streams[] = {
		SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT,
		SIPPYMODEM_RECORD_RX_LINE_PCM,
		SIPPYMODEM_RECORD_RX_MODEM_PCM,
		SIPPYMODEM_RECORD_TX_MODEM_PCM,
		SIPPYMODEM_RECORD_TX_LINE_PCM,
		SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED,
		SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT,
	};
	static const enum sippymodem_recording_format expected_formats[] = {
		SIPPYMODEM_RECORD_OCTETS,
		SIPPYMODEM_RECORD_S16_NATIVE,
		SIPPYMODEM_RECORD_S16_NATIVE,
		SIPPYMODEM_RECORD_S16_NATIVE,
		SIPPYMODEM_RECORD_S16_NATIVE,
		SIPPYMODEM_RECORD_OCTETS,
		SIPPYMODEM_RECORD_OCTETS,
	};
	struct core_capture core = { 0 };
	struct recording_capture recordings = { 0 };
	const struct sippymodem_recorder recorder = {
		.context = &recordings,
		.write = capture_recording,
	};
	struct sippymodem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(&recorder, &core);
	CHECK(host != NULL &&
		      sippymodem_host_open(host) == SIPPYMODEM_SESSION_OK &&
		      sippymodem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIPPYMODEM_SIP_SIGNALING_OK &&
		      sippymodem_host_wait_ns(host, 1000000000) == 0,
	      "recorded host call did not become immediately runnable");
	CHECK(sippymodem_host_step(host, 1000000000, &datagrams, &frames) ==
		      SIPPYMODEM_HOST_OK &&
		      datagrams == 0 && frames == 1 && core.calls == 1 &&
		      core.received_silence &&
		      recordings.calls == ARRAY_LENGTH(expected_streams) &&
		      memcmp(recordings.streams, expected_streams,
			     sizeof(expected_streams)) == 0 &&
		      memcmp(recordings.formats, expected_formats,
			     sizeof(expected_formats)) == 0 &&
		      sippymodem_host_wait_ns(host, 1000000000) == 1000000,
	      "host did not process and record one complete modem frame");
	for (size_t index = 0; index < recordings.calls; ++index) {
		CHECK(recordings.sample_rates[index] ==
			      SIPPYMODEM_MEDIA_SAMPLE_RATE &&
			      recordings.counts[index] == 8,
		      "recorded host block lost its media dimensions");
	}
	CHECK(sippymodem_host_step(host, 1000500000, NULL, &frames) ==
		      SIPPYMODEM_HOST_IDLE &&
		      frames == 0 && core.calls == 1 &&
		      sippymodem_host_wait_ns(host, 1000500000) == 500000,
	      "host ran audio ahead of the sample clock");

	fake.emit_invalid_media_on_step = true;
	CHECK(sippymodem_host_step(host, 1001000000, NULL, &frames) ==
		      SIPPYMODEM_HOST_MEDIA_BINDING_ERROR &&
		      frames == 1 && core.calls == 2 &&
		      sippymodem_host_audio_is_active(host) &&
		      sippymodem_host_last_session_result(host) ==
			      SIPPYMODEM_SESSION_MEDIA_BINDING_ERROR &&
		      sippymodem_host_last_audio_scheduler_result(host) ==
			      SIPPYMODEM_AUDIO_SCHEDULER_OK &&
		      sippymodem_host_last_audio_path_result(host) ==
			      SIPPYMODEM_AUDIO_PATH_OK,
		      "rejected renegotiation interrupted established audio");
	fake.step_result = SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	CHECK(sippymodem_host_step(host, 1001500000, NULL, &frames) ==
		      SIPPYMODEM_HOST_SESSION_ERROR &&
		      frames == 0 &&
		      sippymodem_host_last_session_result(host) ==
			      SIPPYMODEM_SESSION_SIGNALING_ERROR,
	      "host did not retain a fatal signaling-step result");
	CHECK(sippymodem_host_close(host) == SIPPYMODEM_SESSION_OK,
	      "recorded host did not close");
	sippymodem_host_destroy(host);
	return true;
}

static bool test_audio_failure_waits_for_a_new_media_lifecycle(void)
{
	struct core_capture core = {
		.result = SIPPYMODEM_MODEM_CORE_STOPPED,
	};
	struct sippymodem_host *host;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	fake.defer_hangup_media = true;
	host = create_host(NULL, &core);
	CHECK(host != NULL &&
		      sippymodem_host_open(host) == SIPPYMODEM_SESSION_OK &&
		      sippymodem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIPPYMODEM_SIP_SIGNALING_OK,
	      "failed-audio host call did not start");
	CHECK(sippymodem_host_step(host, 0, NULL, &frames) ==
		      SIPPYMODEM_HOST_MODEM_STOPPED &&
		      frames == 0 && core.calls == 1 && fake.hangups == 1 &&
		      sippymodem_host_media_is_configured(host) &&
		      !sippymodem_host_audio_is_active(host) &&
		      sippymodem_host_wait_ns(host, 0) ==
			      SIPPYMODEM_HOST_SIGNALING_POLL_NS,
	      "stopped modem did not suspend audio and request hangup");
	core.result = SIPPYMODEM_MODEM_CORE_OK;
	CHECK(sippymodem_host_step(host, 1000000, NULL, &frames) ==
		      SIPPYMODEM_HOST_IDLE &&
		      frames == 0 && core.calls == 1,
	      "failed audio restarted before media became inactive");
	fake.emit_inactive_on_step = true;
	CHECK(sippymodem_host_step(host, 2000000, NULL, &frames) ==
		      SIPPYMODEM_HOST_IDLE &&
		      !sippymodem_host_media_is_configured(host) &&
		      sippymodem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIPPYMODEM_SIP_SIGNALING_OK &&
		      sippymodem_host_step(host, 3000000, NULL, &frames) ==
			      SIPPYMODEM_HOST_OK &&
		      frames == 1 && core.calls == 2,
	      "new media lifecycle did not rearm suspended audio");
	core.result = SIPPYMODEM_MODEM_CORE_ERROR;
	CHECK(sippymodem_host_step(host, 4000000, NULL, &frames) ==
		      SIPPYMODEM_HOST_AUDIO_ERROR &&
		      sippymodem_host_last_audio_path_result(host) ==
			      SIPPYMODEM_AUDIO_PATH_MODEM_ERROR &&
		      fake.hangups == 2,
	      "modem failure did not retain its typed audio cause");
	CHECK(sippymodem_host_close(host) == SIPPYMODEM_SESSION_OK,
	      "failed-audio host did not close");
	sippymodem_host_destroy(host);
	return true;
}

static bool test_destroy_closes_an_open_host(void)
{
	struct sippymodem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(NULL, NULL);
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
			      SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT &&
		      sippymodem_host_step(NULL, 0, &datagrams, &frames) ==
			      SIPPYMODEM_HOST_INVALID_ARGUMENT &&
		      datagrams == 0 && frames == 0 &&
		      sippymodem_host_wait_ns(NULL, 0) == UINT64_MAX &&
		      strcmp(sippymodem_host_result_name(
				     SIPPYMODEM_HOST_AUDIO_ERROR),
			     "scheduled audio failed") == 0,
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
		{ "recorded frame clock",
		  test_steps_recorded_media_on_the_frame_clock },
		{ "audio failure lifecycle",
		  test_audio_failure_waits_for_a_new_media_lifecycle },
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
