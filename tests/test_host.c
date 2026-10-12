// SPDX-License-Identifier: BSD-3-Clause

#include <host.h>

#include "test.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fake_signaling_control {
	unsigned int creates;
	unsigned int destroys;
	unsigned int opens;
	unsigned int closes;
	unsigned int dials;
	unsigned int answers;
	unsigned int hangups;
	enum sip_signaling_result step_result;
	bool emit_invalid_media_on_step;
	bool emit_inactive_on_step;
	bool defer_hangup_media;
	bool early_media;
	bool connect_on_step;
	uint16_t remote_port;
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
	if (signaling->opened)
		return SIP_SIGNALING_BUSY;
	signaling->opened = true;
	fake.opens++;
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_close(
	struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	signaling->opened = false;
	fake.closes++;
	return SIP_SIGNALING_OK;
}

static void emit_media(
	struct sip_signaling *signaling,
	enum sip_media_state state, bool compatible);

static void emit_call(struct sip_signaling *signaling, enum sip_call_state state)
{
	const struct sip_call_event event = { .state = state };
	if (signaling->callbacks.call_changed != NULL)
		signaling->callbacks.call_changed(signaling->callbacks.context, &event);
}

enum sip_signaling_result sip_signaling_step(
	struct sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	if (signaling == NULL || !signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	if (fake.connect_on_step) {
		fake.connect_on_step = false;
		emit_call(signaling, SIP_CALL_CONNECTED);
	}
	if (fake.emit_invalid_media_on_step) {
		fake.emit_invalid_media_on_step = false;
		emit_media(signaling, SIP_MEDIA_CONFIGURED, false);
	}
	if (fake.emit_inactive_on_step) {
		fake.emit_inactive_on_step = false;
		emit_media(signaling, SIP_MEDIA_INACTIVE, true);
	}
	return fake.step_result;
}

static void emit_media(
	struct sip_signaling *signaling,
	enum sip_media_state state, bool compatible)
{
	unsigned int payload_type = (unsigned int)
		media_codec_default_payload_type(
			signaling->config.media.codec);
	const struct sip_media_event event = {
		.state = state,
		.remote_address_family = SIP_MEDIA_ADDRESS_IPV4,
		.codec = signaling->config.media.codec,
		.payload_type = compatible ? payload_type : payload_type + 1,
		.sample_rate = MEDIA_SAMPLE_RATE,
		.channels = 1,
		.local_port = signaling->config.media.rtp_listen_port,
		.remote_address = "127.0.0.1",
		.remote_port = fake.remote_port != 0 ?
			       fake.remote_port :
			       42000,
	};

	if (signaling->callbacks.media_changed != NULL)
		signaling->callbacks.media_changed(
			signaling->callbacks.context, &event);
}

enum sip_signaling_result sip_signaling_set_endpoint(struct sip_signaling *signaling,
	struct sip_endpoint *endpoint)
{
	return signaling == NULL || endpoint == NULL ? SIP_SIGNALING_INVALID_ARGUMENT :
		!signaling->opened ? SIP_SIGNALING_NOT_OPEN : SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target)
{
	if (signaling == NULL || target == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	fake.dials++;
	emit_call(signaling, fake.early_media ? SIP_CALL_EARLY : SIP_CALL_CONNECTED);
	emit_media(signaling, SIP_MEDIA_CONFIGURED, true);
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
	if (!fake.defer_hangup_media)
		emit_media(signaling, SIP_MEDIA_INACTIVE, true);
	return SIP_SIGNALING_OK;
}

struct core_capture {
	unsigned int stop_after;
	struct modem_host *hangup_host;
	enum modem_core_result result;
	unsigned int calls;
	bool received_silence;
};

enum {
	TEST_RECORDING_CAPACITY = 32,
};

struct recording_capture {
	enum recording_stream streams[TEST_RECORDING_CAPACITY];
	enum recording_format formats[TEST_RECORDING_CAPACITY];
	unsigned int sample_rates[TEST_RECORDING_CAPACITY];
	size_t counts[TEST_RECORDING_CAPACITY];
	size_t calls;
};

static void capture_recording(
	void *context, const struct recording_block *block)
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

static enum modem_core_result process_core(
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
		if (capture->stop_after != 0 && capture->calls >= capture->stop_after) {
			if (capture->hangup_host != NULL)
				(void)modem_host_hangup(capture->hangup_host);
			return MODEM_CORE_STOPPED;
		}
	}
	memset(transmit_samples, 0, count * sizeof(transmit_samples[0]));
	return capture != NULL ? capture->result : MODEM_CORE_OK;
}

static struct modem_host *create_host(
	const struct media_recorder *recorder,
	struct core_capture *core_capture)
{
	struct app_config config;
	const struct modem_core core = {
		.context = core_capture,
		.process = process_core,
	};

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	config.media.remote_clock = false;
	return modem_host_create(&config, recorder, NULL, &core);
}

static struct modem_host *create_remote_host(
	struct core_capture *core_capture,
	const struct sip_signaling_callbacks *callbacks)
{
	struct app_config config;
	const struct modem_core core = {
		.context = core_capture,
		.process = process_core,
	};

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	config.media.remote_clock = true;
	config.sip.trace_enabled = callbacks != NULL && callbacks->trace != NULL;
	return modem_host_create(&config, NULL, callbacks, &core);
}

enum { CLOCK_FRAME_SAMPLES = 160, CLOCK_PACKET_COUNT = 64 };

struct clock_capture {
	unsigned int calls;
	size_t received_count;
	size_t generated_count;
	uint8_t received[CLOCK_FRAME_SAMPLES * CLOCK_PACKET_COUNT];
};

struct clock_fixture {
	struct clock_capture capture;
	struct modem_host *host;
	struct sockaddr_in address;
	int peer;
};

static enum modem_core_result capture_clock_audio(
	void *context, enum g711_law law, const uint8_t *received,
	int16_t *receive_samples, const bool *mask, uint8_t *transmitted,
	int16_t *transmit_samples, size_t count, bool *exact_output)
{
	struct clock_capture *capture = context;
	(void)law;
	(void)receive_samples;
	(void)transmitted;
	(void)exact_output;

	capture->calls++;
	for (size_t index = 0; index < count; ++index) {
		if (mask[index]) {
			if (capture->received_count >= sizeof(capture->received))
				return MODEM_CORE_ERROR;
			capture->received[capture->received_count++] = received[index];
		} else {
			capture->generated_count++;
		}
	}
	memset(transmit_samples, 0, count * sizeof(*transmit_samples));
	return MODEM_CORE_OK;
}

static bool clock_fixture_open_call(struct clock_fixture *fixture,
	unsigned int jitter_ms, bool early_media)
{
	struct app_config config;
	struct sockaddr_in peer = {
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t size = sizeof(peer);
	const struct modem_core core = {
		.context = &fixture->capture,
		.process = process_core, .process_g711 = capture_clock_audio,
	};

	memset(&fake, 0, sizeof(fake));
	fake.early_media = early_media;
	memset(fixture, 0, sizeof(*fixture));
	fixture->peer = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(fixture->peer >= 0 && bind(fixture->peer,
	      (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
	      getsockname(fixture->peer, (struct sockaddr *)&peer, &size) == 0,
	      "clock fixture peer failed");
	fake.remote_port = ntohs(peer.sin_port);
	config_init(&config);
	config.media.jitter_buffer_ms = jitter_ms;
	config.media.frame_duration_ms = 20;
	fixture->host = modem_host_create(&config, NULL, NULL, &core);
	size = sizeof(fixture->address);
	CHECK(fixture->host != NULL &&
	      modem_host_open(fixture->host) == SIP_SESSION_OK &&
	      modem_host_dial(fixture->host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK &&
	      getsockname(modem_host_media_poll_fd(fixture->host),
	      (struct sockaddr *)&fixture->address, &size) == 0,
	      "clock fixture host failed");
	return true;
}

static bool clock_fixture_open(struct clock_fixture *fixture, unsigned int jitter_ms)
{
	return clock_fixture_open_call(fixture, jitter_ms, false);
}

static bool clock_fixture_send_part(struct clock_fixture *fixture,
	uint32_t ssrc, unsigned int frame, unsigned int offset, size_t count)
{
	uint8_t payload[CLOCK_FRAME_SAMPLES];
	uint8_t packet[RTP_HEADER_SIZE + sizeof(payload)];
	const struct rtp_header header = {
		.ssrc = ssrc, .sequence = (uint16_t)(frame * 2 + (offset != 0)),
		.timestamp = frame * CLOCK_FRAME_SAMPLES + offset,
	};
	size_t size;

	memset(payload, (int)(frame + 1u), sizeof(payload));
	CHECK(count <= sizeof(payload) && rtp_packet_write(packet, sizeof(packet), &header, payload,
	      count, &size) == RTP_OK &&
	      sendto(fixture->peer, packet, size, 0, (struct sockaddr *)&fixture->address,
	      sizeof(fixture->address)) == (ssize_t)size, "clock fixture send failed");
	return true;
}

static bool clock_fixture_send(struct clock_fixture *fixture, unsigned int frame)
{
	return clock_fixture_send_part(fixture, 1, frame, 0, CLOCK_FRAME_SAMPLES);
}

static void clock_fixture_close(struct clock_fixture *fixture)
{
	modem_host_destroy(fixture->host);
	close(fixture->peer);
}

static bool test_early_media_clock_handoff(void)
{
	struct clock_fixture fixture;
	size_t frames, datagrams;
	const uint64_t start = UINT64_C(1000000000);
	const uint64_t interval = UINT64_C(20000000);

	CHECK(clock_fixture_open_call(&fixture, 0, true) &&
	      clock_fixture_send(&fixture, 0), "early-media fixture failed");
	CHECK(modem_host_step(fixture.host, start, &datagrams, &frames) == MODEM_HOST_OK &&
	      datagrams == 1 && frames == 1 &&
	      fixture.capture.received_count == CLOCK_FRAME_SAMPLES,
	      "early media did not play its first packet");
	/* Ringback can suppress RTP through silence. Timers and the receive cursor
	 * must keep advancing without an established-call watchdog or extra credit. */
	for (unsigned int frame = 1; frame < 400; ++frame) {
		uint64_t now = start + frame * interval;
		CHECK(modem_host_wait_ns(fixture.host, now) == 0 &&
		      modem_host_step(fixture.host, now, &datagrams, &frames) == MODEM_HOST_OK &&
		      datagrams == 0 && frames == 1 && fake.hangups == 0,
		      "quiet early media stopped timers, accumulated work or hung up");
	}
	CHECK(clock_fixture_send(&fixture, 400), "resumed early media send failed");
	CHECK(modem_host_step(fixture.host, start + 400 * interval,
	      &datagrams, &frames) == MODEM_HOST_OK && datagrams == 1 && frames == 1 &&
	      fixture.capture.received_count == 2 * CLOCK_FRAME_SAMPLES,
	      "resumed early media was rejected after the quiet timestamp gap");
	fake.connect_on_step = true;
	CHECK(clock_fixture_send(&fixture, 401), "connected media send failed");
	CHECK(modem_host_step(fixture.host, start + 401 * interval,
	      &datagrams, &frames) == MODEM_HOST_OK && datagrams == 1 && frames == 1 &&
	      fixture.capture.calls == 402 &&
	      fixture.capture.received_count == 3 * CLOCK_FRAME_SAMPLES,
	      "clock handoff replayed already-consumed silence or lost the answer packet");
	CHECK(modem_host_step(fixture.host, start + 402 * interval,
	      &datagrams, &frames) == MODEM_HOST_IDLE && frames == 0 &&
	      modem_host_wait_ns(fixture.host, start + 402 * interval) > 0,
	      "established media advanced without remote credit or spun");
	CHECK(modem_host_step(fixture.host, start + 401 * interval +
	      MODEM_HOST_REMOTE_MEDIA_TIMEOUT_NS, &datagrams, &frames) ==
	      MODEM_HOST_MEDIA_TIMEOUT && fake.hangups == 1,
	      "early-media clocking disabled the established-stream watchdog");
	clock_fixture_close(&fixture);
	return true;
}

static bool test_remote_gap_wait_has_no_fixed_prefill(void)
{
	struct clock_fixture fixture;
	size_t frames;

	CHECK(clock_fixture_open(&fixture, 60) &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.generated_count == CLOCK_FRAME_SAMPLES,
	      "default remote host failed to seed without received RTP");
	CHECK(clock_fixture_send(&fixture, 0) &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == CLOCK_FRAME_SAMPLES,
	      "default jitter setting delayed contiguous first audio");
	CHECK(clock_fixture_send(&fixture, 2) &&
	      modem_host_step(fixture.host, 20000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && modem_host_wait_ns(fixture.host, 20000000) == 20000000,
	      "real gap was concealed early or exposed a busy wait");
	CHECK(clock_fixture_send(&fixture, 1) &&
	      modem_host_step(fixture.host, 30000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 2 && fixture.capture.received_count == 3 * CLOCK_FRAME_SAMPLES &&
	      fixture.capture.generated_count == CLOCK_FRAME_SAMPLES,
	      "late predecessor did not release intact retained clock credit");
	for (size_t index = 0; index < fixture.capture.received_count; ++index)
		CHECK(fixture.capture.received[index] == index / CLOCK_FRAME_SAMPLES + 1u,
		      "gap recovery changed sample %zu", index);
	clock_fixture_close(&fixture);
	return true;
}

static bool test_remote_gap_timeout_is_once_per_hole(void)
{
	struct clock_fixture fixture;
	size_t frames;

	CHECK(clock_fixture_open(&fixture, 60) && clock_fixture_send(&fixture, 0) &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.generated_count == 0,
	      "already received startup media was seeded a second time");
	CHECK(clock_fixture_send(&fixture, 3) &&
	      modem_host_step(fixture.host, 40000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 &&
	      modem_host_step(fixture.host, 99999999, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && modem_host_wait_ns(fixture.host, 99999999) == 1,
	      "missing run changed its deadline before expiry");
	CHECK(modem_host_step(fixture.host, 100000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 3 && fixture.capture.generated_count == 2 * CLOCK_FRAME_SAMPLES &&
	      fixture.capture.received_count == 2 * CLOCK_FRAME_SAMPLES,
	      "consecutive lost packets restarted the same hole deadline");
	for (size_t index = CLOCK_FRAME_SAMPLES; index < fixture.capture.received_count; ++index)
		CHECK(fixture.capture.received[index] == 4,
		      "loss concealment changed received provenance");
	clock_fixture_close(&fixture);
	return true;
}

static bool test_remote_partial_frame_is_bounded(void)
{
	struct clock_fixture fixture;
	size_t frames;

	CHECK(clock_fixture_open(&fixture, 0) &&
	      clock_fixture_send_part(&fixture, 1, 0, 0, 40) &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && modem_host_wait_ns(fixture.host, 0) == 20000000 &&
	      clock_fixture_send_part(&fixture, 1, 0, 40, 120) &&
	      modem_host_step(fixture.host, 5000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == CLOCK_FRAME_SAMPLES &&
	      fixture.capture.generated_count == 0,
	      "ordinary partial packets were seeded or flushed before completion");
	CHECK(clock_fixture_send_part(&fixture, 1, 1, 0, 40) &&
	      modem_host_step(fixture.host, 20000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 &&
	      modem_host_step(fixture.host, 39999999, NULL, &frames) == MODEM_HOST_IDLE &&
	      modem_host_wait_ns(fixture.host, 39999999) == 1 &&
	      modem_host_step(fixture.host, 40000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == CLOCK_FRAME_SAMPLES + 40 &&
	      fixture.capture.generated_count == 120,
	      "stalled partial audio failed to release its bounded feedback deadline");
	CHECK(clock_fixture_send_part(&fixture, 1, 1, 40, 120) &&
	      modem_host_step(fixture.host, 40000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && clock_fixture_send(&fixture, 2) &&
	      modem_host_step(fixture.host, 50000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == 2 * CLOCK_FRAME_SAMPLES + 40,
	      "late padded tail supplied a second clock tick or displaced later audio");
	clock_fixture_close(&fixture);
	return true;
}

static bool test_remote_pacing_preserves_credit_and_restart(void)
{
	struct clock_fixture fixture;
	size_t frames;

	CHECK(clock_fixture_open(&fixture, 60), "paced fixture failed");
	for (unsigned int frame = 0; frame < 8; ++frame)
		CHECK(clock_fixture_send(&fixture, frame), "paced burst failed");
	CHECK(modem_host_step(fixture.host, 1000000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 4 &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && modem_host_wait_ns(fixture.host, 0) == 15000000,
	      "same-time or backward host passes exceeded the four-frame burst");
	CHECK(clock_fixture_send_part(&fixture, 2, 0, 0, CLOCK_FRAME_SAMPLES) &&
	      modem_host_step(fixture.host, 1000000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 &&
	      modem_host_step(fixture.host, 1015000000, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == 5 * CLOCK_FRAME_SAMPLES &&
	      fixture.capture.generated_count == 0 &&
	      modem_host_step(fixture.host, 1030000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0,
	      "SSRC restart inherited retired RTP credit or renewed its burst allowance");
	CHECK(modem_host_hangup(fixture.host) == SIP_SIGNALING_OK &&
	      modem_host_step(fixture.host, UINT64_MAX, NULL, &frames) == MODEM_HOST_IDLE &&
	      modem_host_dial(fixture.host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK &&
	      modem_host_step(fixture.host, 0, NULL, &frames) == MODEM_HOST_OK && frames == 1 &&
	      clock_fixture_send(&fixture, 0) &&
	      modem_host_step(fixture.host, 20000000, NULL, &frames) == MODEM_HOST_OK && frames == 1,
	      "idle media observations leaked the previous call's time origin or debt");
	clock_fixture_close(&fixture);
	return true;
}

static bool test_remote_feedback_clock_is_bounded(void)
{
	struct clock_fixture fixture;
	size_t total = 0;

	CHECK(clock_fixture_open(&fixture, 0) &&
	      fcntl(fixture.peer, F_SETFL, O_NONBLOCK) == 0,
	      "feedback fixture failed");
	for (unsigned int tick = 0; tick <= 1200; ++tick) {
		uint64_t now = tick * UINT64_C(1000000);

		for (unsigned int pass = 0; pass < 8; ++pass) {
			size_t frames;
			enum modem_host_result result = modem_host_step(fixture.host, now, NULL, &frames);
			uint8_t packet[RTP_HEADER_SIZE + CLOCK_FRAME_SAMPLES];
			ssize_t size;

			CHECK((result == MODEM_HOST_OK || result == MODEM_HOST_IDLE) && frames <= 4,
			      "feedback host failed or exceeded its per-pass DSP budget");
			total += frames;
			fixture.capture.received_count = 0;
			while ((size = recv(fixture.peer, packet, sizeof(packet), 0)) >= 0)
				CHECK(sendto(fixture.peer, packet, (size_t)size, 0,
				      (struct sockaddr *)&fixture.address, sizeof(fixture.address)) == size,
				      "feedback packet reflection failed");
			if (frames == 0) {
				CHECK(modem_host_wait_ns(fixture.host, now) > 0,
				      "feedback loop exhausted its budget without a wait");
				break;
			}
			CHECK(pass < 7, "same-time feedback exceeded the pacing burst");
		}
	}
	CHECK(total == 84 && fixture.capture.generated_count == CLOCK_FRAME_SAMPLES,
	      "feedback produced %zu frames instead of four burst plus 80 paced, or lost audio", total);
	clock_fixture_close(&fixture);
	return true;
}

static bool test_owns_lifecycle_and_call_control(void)
{
	struct modem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(NULL, NULL);
	CHECK(host != NULL && modem_host_frame_samples(host) == 8 &&
		      !modem_host_is_open(host) &&
		      !modem_host_audio_is_active(host),
	      "host did not construct the complete audio owner");
	CHECK(modem_host_wait_ns(host, 0) == UINT64_MAX &&
		      modem_host_step(host, 0, &datagrams, &frames) ==
			      MODEM_HOST_IDLE &&
		      datagrams == 0 && frames == 0,
	      "closed host exposed pending event-loop work");
	CHECK(modem_host_dial(host, "sip:peer@127.0.0.1") ==
		      SIP_SIGNALING_NOT_OPEN,
	      "closed host accepted a call");
	CHECK(modem_host_open(host) == SIP_SESSION_OK &&
		      modem_host_is_open(host) && fake.opens == 1 &&
		      !modem_host_media_is_configured(host),
	      "host did not open its signaling and RTP session");
	CHECK(modem_host_dial(host, "sip:peer@127.0.0.1") ==
		      SIP_SIGNALING_OK &&
		      modem_host_media_is_configured(host) &&
		      modem_host_audio_is_active(host),
	      "negotiated media did not activate the host audio path");
	CHECK(modem_host_answer(host) == SIP_SIGNALING_OK &&
		      modem_host_hangup(host) ==
			      SIP_SIGNALING_OK &&
		      !modem_host_media_is_configured(host) &&
		      !modem_host_audio_is_active(host) && fake.dials == 1 &&
		      fake.answers == 1 && fake.hangups == 1,
	      "host did not synchronize call control with media state");
	CHECK(modem_host_close(host) == SIP_SESSION_OK &&
		      !modem_host_is_open(host) && fake.closes == 1 &&
		      fake.destroys == 1,
	      "host did not close its complete session");
	modem_host_destroy(host);
	return true;
}

static bool test_creation_validation_is_transactional(void)
{
	struct app_config config;
	struct modem_core core = { .process = process_core };

	config_init(&config);
	CHECK(modem_host_create(NULL, NULL, NULL, &core) == NULL &&
		      modem_host_create(&config, NULL, NULL, NULL) == NULL,
	      "host accepted incomplete ownership");
	config.media.frame_duration_ms = 0;
	CHECK(modem_host_create(&config, NULL, NULL, &core) == NULL,
	      "host retained an invalid media session");
	config_init(&config);
	config.media.receive_gain = NAN;
	CHECK(modem_host_create(&config, NULL, NULL, &core) == NULL,
	      "host retained a session after audio path rejection");
	return true;
}

static bool test_steps_recorded_media_on_the_frame_clock(void)
{
	static const enum recording_stream expected_streams[] = {
		RECORD_RX_RTP_PAYLOAD_PLAYOUT,
		RECORD_RX_LINE_PCM,
		RECORD_RX_MODEM_PCM,
		RECORD_TX_MODEM_PCM,
		RECORD_TX_LINE_PCM,
		RECORD_TX_RTP_PAYLOAD_PREPARED,
		RECORD_TX_RTP_PAYLOAD_SENT,
	};
	static const enum recording_format expected_formats[] = {
		RECORD_OCTETS,
		RECORD_S16_NATIVE,
		RECORD_S16_NATIVE,
		RECORD_S16_NATIVE,
		RECORD_S16_NATIVE,
		RECORD_OCTETS,
		RECORD_OCTETS,
	};
	struct core_capture core = { 0 };
	struct recording_capture recordings = { 0 };
	const struct media_recorder recorder = {
		.context = &recordings,
		.write = capture_recording,
	};
	struct modem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(&recorder, &core);
	CHECK(host != NULL &&
		      modem_host_open(host) == SIP_SESSION_OK &&
		      modem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIP_SIGNALING_OK &&
		      modem_host_wait_ns(host, 1000000000) == 0,
	      "recorded host call did not become immediately runnable");
	CHECK(modem_host_step(host, 1000000000, &datagrams, &frames) ==
		      MODEM_HOST_OK &&
		      datagrams == 0 && frames == 1 && core.calls == 1 &&
		      core.received_silence &&
		      recordings.calls == ARRAY_LENGTH(expected_streams) &&
		      memcmp(recordings.streams, expected_streams,
			     sizeof(expected_streams)) == 0 &&
		      memcmp(recordings.formats, expected_formats,
			     sizeof(expected_formats)) == 0 &&
		      modem_host_wait_ns(host, 1000000000) == 1000000,
	      "host did not process and record one complete modem frame");
	for (size_t index = 0; index < recordings.calls; ++index) {
		CHECK(recordings.sample_rates[index] ==
			      MEDIA_SAMPLE_RATE &&
			      recordings.counts[index] == 8,
		      "recorded host block lost its media dimensions");
	}
	CHECK(modem_host_step(host, 1000500000, NULL, &frames) ==
		      MODEM_HOST_IDLE &&
		      frames == 0 && core.calls == 1 &&
		      modem_host_wait_ns(host, 1000500000) == 500000,
	      "host ran audio ahead of the sample clock");

	fake.emit_invalid_media_on_step = true;
	CHECK(modem_host_step(host, 1001000000, NULL, &frames) ==
		      MODEM_HOST_MEDIA_BINDING_ERROR &&
		      frames == 1 && core.calls == 2 &&
		      modem_host_audio_is_active(host) &&
		      modem_host_last_session_result(host) ==
			      SIP_SESSION_MEDIA_BINDING_ERROR &&
		      modem_host_last_audio_scheduler_result(host) ==
			      AUDIO_SCHEDULER_OK &&
		      modem_host_last_audio_path_result(host) ==
			      AUDIO_PATH_OK,
		      "rejected renegotiation interrupted established audio");
	fake.step_result = SIP_SIGNALING_LIBRARY_ERROR;
	CHECK(modem_host_step(host, 1001500000, NULL, &frames) ==
		      MODEM_HOST_SESSION_ERROR &&
		      frames == 0 &&
		      modem_host_last_session_result(host) ==
			      SIP_SESSION_SIGNALING_ERROR,
	      "host did not retain a fatal signaling-step result");
	CHECK(modem_host_close(host) == SIP_SESSION_OK,
	      "recorded host did not close");
	modem_host_destroy(host);
	return true;
}

static void capture_media_trace(void *context, const struct sip_trace_record *record)
{
	char *text = context;
	if (record->size < 1024) {
		memcpy(text, record->data, record->size);
		text[record->size] = '\0';
	}
}

static bool test_remote_media_clocks_elapsed_time(void)
{
	struct core_capture core = { 0 };
	char trace[1024] = { 0 };
	const struct sip_signaling_callbacks callbacks = {
		.context = trace,
		.trace = capture_media_trace,
	};
	struct modem_host *host;
	struct sockaddr_in peer = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sockaddr_in host_address;
	struct rtp_header rtp_header = {
		.payload_type = 0,
		.sequence = 7,
		.timestamp = 800,
		.ssrc = 1,
	};
	uint8_t packet[RTP_HEADER_SIZE + 8];
	uint8_t payload[8];
	socklen_t address_size = sizeof(peer);
	size_t packet_size = 0;
	size_t datagrams = 99;
	size_t frames = 99;
	int peer_fd;
	int media_fd;

	memset(&fake, 0, sizeof(fake));
	memset(payload, 0xff, sizeof(payload));
	peer_fd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(peer_fd >= 0 && bind(peer_fd, (struct sockaddr *)&peer,
					  sizeof(peer)) == 0 &&
		      getsockname(peer_fd, (struct sockaddr *)&peer,
				  &address_size) == 0,
	      "remote clock peer socket did not open");
	fake.remote_port = ntohs(peer.sin_port);
	host = create_remote_host(&core, &callbacks);
	CHECK(host != NULL && modem_host_open(host) == SIP_SESSION_OK &&
		      modem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIP_SIGNALING_OK &&
		      modem_host_wait_ns(host, 1000000000) == 0,
	      "remote-clock host did not arm its seed frame");
	media_fd = modem_host_media_poll_fd(host);
	address_size = sizeof(host_address);
	CHECK(media_fd >= 0 &&
		      getsockname(media_fd, (struct sockaddr *)&host_address,
				  &address_size) == 0 &&
		      modem_host_step(host, 1000000000, &datagrams, &frames) ==
			      MODEM_HOST_OK &&
		      datagrams == 0 && frames == 1 && core.calls == 1 &&
		      modem_host_wait_ns(host, 1000000000) ==
			      1000000,
	      "remote-clock host did not emit exactly one seed frame");
	CHECK(strstr(trace, "TX=1 RX=0 accepted=0"),
	      "trace did not report the seed packet with no inbound media");
	CHECK(rtp_packet_write(packet, sizeof(packet), &rtp_header,
				       payload, sizeof(payload), &packet_size) ==
		      RTP_OK &&
		      sendto(peer_fd, packet, packet_size, 0,
			     (struct sockaddr *)&host_address,
			     sizeof(host_address)) == (ssize_t)packet_size,
	      "remote clock RTP frame was not sent");
	CHECK(modem_host_step(host, 1000000000, &datagrams, &frames) ==
		      MODEM_HOST_OK &&
		      datagrams == 1 && frames == 1 && core.calls == 2,
	      "one received RTP frame did not clock one modem frame");
	CHECK(modem_host_step(host, 2000000000, &datagrams, &frames) ==
		      MODEM_HOST_IDLE &&
		      datagrams == 0 && frames == 0 && core.calls == 2,
	      "remote-clock host advanced from wall time alone");
	CHECK(strstr(trace, "TX=1 RX=1 accepted=1") &&
	      strstr(trace, "buffered=8 played=8"),
	      "host trace did not expose real received audio");
	CHECK(modem_host_step(host, 3000000000, &datagrams, &frames) ==
	      MODEM_HOST_IDLE && strstr(trace, "TX=0 RX=0 accepted=0"),
	      "stopped remote media clock also stopped diagnostics");
	/* One lost inbound packet must not also remove an outbound clock tick. */
	rtp_header.sequence += 2;
	rtp_header.timestamp += 16;
	memset(payload, 0x00, sizeof(payload));
	CHECK(rtp_packet_write(packet, sizeof(packet), &rtp_header,
			       payload, sizeof(payload), &packet_size) == RTP_OK &&
		      sendto(peer_fd, packet, packet_size, 0,
			     (struct sockaddr *)&host_address,
			     sizeof(host_address)) == (ssize_t)packet_size,
	      "RTP packet following a clock gap was not sent");
	CHECK(modem_host_step(host, 4000000000, &datagrams, &frames) ==
		      MODEM_HOST_OK && datagrams == 1 && frames == 2 &&
		      core.calls == 4 && !core.received_silence,
	      "lost inbound RTP time delayed the modem's transmit clock");
	struct audio_levels levels = modem_host_take_audio_levels(host);
	CHECK(levels.receive_peak == 32124 && levels.transmit_peak == 0,
	      "host did not expose decoded PCMU levels");
	levels = modem_host_take_audio_levels(host);
	CHECK(levels.receive_peak == 0 && levels.transmit_peak == 0,
	      "host retained peaks between intervals");
	/* Packet boundaries need not coincide with the modem's frame boundary. */
	for (unsigned int part = 0; part < 2; ++part) {
		size_t count = part == 0 ? 3 : 5;

		rtp_header.sequence++;
		rtp_header.timestamp += part == 0 ? 8 : 3;
		CHECK(rtp_packet_write(packet, sizeof(packet), &rtp_header,
				       payload, count, &packet_size) == RTP_OK &&
			      sendto(peer_fd, packet, packet_size, 0,
				     (struct sockaddr *)&host_address,
				     sizeof(host_address)) == (ssize_t)packet_size,
		      "partial RTP clock frame was not sent");
		CHECK(modem_host_step(host, 4000000000, &datagrams, &frames) ==
			      (part == 0 ? MODEM_HOST_IDLE : MODEM_HOST_OK) &&
			      datagrams == 1 && frames == part && core.calls == 4 + part,
		      "partial frame credit did not survive separate socket drains");
	}
	/* A burst keeps clock credit but processes at most four frames per pass. */
	for (unsigned int packet_index = 0; packet_index < 12; ++packet_index) {
		rtp_header.sequence++;
		rtp_header.timestamp += packet_index == 0 ? 5 : 8;
		CHECK(rtp_packet_write(packet, sizeof(packet), &rtp_header,
			payload, sizeof(payload), &packet_size) == RTP_OK &&
			sendto(peer_fd, packet, packet_size, 0,
			(struct sockaddr *)&host_address, sizeof(host_address)) == (ssize_t)packet_size,
			"burst RTP packet was not sent");
	}
	CHECK(modem_host_step(host, 4100000000, &datagrams, &frames) == MODEM_HOST_OK &&
	      datagrams == 12 && frames == 4 && core.calls == 9 &&
	      modem_host_step(host, 4100000000, NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && modem_host_wait_ns(host, 4100000000) == 750000,
	      "remote burst exceeded four frames or retried without waiting");
	for (unsigned int pass = 1; pass <= 8; ++pass) {
		uint64_t now = UINT64_C(4100000000) + pass * UINT64_C(750000);

		CHECK(modem_host_step(host, now, &datagrams, &frames) == MODEM_HOST_OK &&
		      datagrams == 0 && frames == 1 && core.calls == 9 + pass,
		      "paced remote continuation lost clock credit on pass %u", pass);
		CHECK(modem_host_wait_ns(host, now) ==
		      (pass < 8 ? 750000 : MODEM_HOST_SIGNALING_POLL_NS),
		      "pending remote clock credit did not retain its pacing deadline");
	}
	/* Missing RTP must eventually release the call even without SIP BYE.
	 * Invalid packets are not clock credit and cannot keep it alive. */
	const uint64_t deadline = UINT64_C(4100000000) +
		MODEM_HOST_REMOTE_MEDIA_TIMEOUT_NS;

	fake.defer_hangup_media = true;
	CHECK(sendto(peer_fd, "bad", 3, 0, (struct sockaddr *)&host_address,
		     sizeof(host_address)) == 3 &&
	      modem_host_step(host, deadline - 1,
		      &datagrams, &frames) == MODEM_HOST_IDLE &&
	      datagrams == 1 && frames == 0 && fake.hangups == 0 &&
	      modem_host_audio_is_active(host),
	      "remote media deadline rejected a short gap or processed malformed audio");
	CHECK(modem_host_step(host, deadline,
		      &datagrams, &frames) == MODEM_HOST_MEDIA_TIMEOUT &&
	      datagrams == 0 && frames == 0 && fake.hangups == 1 &&
	      !modem_host_audio_is_active(host) &&
	      modem_host_media_is_configured(host),
	      "missing RTP did not suspend audio and request hangup at its deadline");
	CHECK(modem_host_step(host, deadline + 1, &datagrams, &frames) == MODEM_HOST_IDLE &&
	      frames == 0 && fake.hangups == 1,
	      "media timeout repeated hangup while waiting for SIP teardown");
	fake.emit_inactive_on_step = true;
	CHECK(modem_host_step(host, deadline + 2, NULL, NULL) == MODEM_HOST_IDLE &&
	      !modem_host_media_is_configured(host),
	      "media timeout retained the old call lifecycle");
	CHECK(modem_host_dial(host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK &&
	      modem_host_step(host, UINT64_MAX - MODEM_HOST_REMOTE_MEDIA_TIMEOUT_NS,
		      NULL, &frames) == MODEM_HOST_OK && frames == 1 &&
	      modem_host_audio_is_active(host) &&
	      modem_host_step(host, UINT64_MAX - 1, NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 &&
	      modem_host_step(host, UINT64_MAX, NULL, &frames) == MODEM_HOST_MEDIA_TIMEOUT &&
	      frames == 0 && fake.hangups == 2,
	      "new media did not reset its bounded, overflow-safe startup deadline");
	CHECK(modem_host_hangup(host) == SIP_SIGNALING_OK,
	      "metered call did not hang up");
	levels = modem_host_take_audio_levels(host);
	CHECK(levels.receive_peak == 0 && levels.transmit_peak == 0,
	      "hangup retained audio peaks for the next call");
	/* A later frame can synchronously retire the credit during DSP. */
	fake.defer_hangup_media = false;
	CHECK(modem_host_hangup(host) == SIP_SIGNALING_OK && !modem_host_media_is_configured(host),
		"deferred old media did not clear");
	core.calls = 0;
	core.stop_after = 3;
	core.hangup_host = host;
	CHECK(modem_host_dial(host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK &&
		modem_host_step(host, UINT64_MAX, NULL, &frames) == MODEM_HOST_OK && frames == 1,
		"stop-burst call did not seed");
	for (unsigned int i = 0; i < 8; ++i) {
		rtp_header.sequence++;
		rtp_header.timestamp += 8;
		CHECK(rtp_packet_write(packet, sizeof(packet), &rtp_header, payload,
			sizeof(payload), &packet_size) == RTP_OK &&
			sendto(peer_fd, packet, packet_size, 0, (struct sockaddr *)&host_address,
			sizeof(host_address)) == (ssize_t)packet_size, "stop-burst packet failed");
	}
	CHECK(modem_host_step(host, UINT64_MAX, &datagrams, &frames) == MODEM_HOST_MODEM_STOPPED &&
		frames == 1 && core.calls == 3 && !modem_host_audio_is_active(host),
		"later frame did not retire the burst safely");
	core.stop_after = 0;
	core.hangup_host = NULL;
	CHECK(modem_host_dial(host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK &&
		modem_host_step(host, UINT64_MAX, NULL, &frames) == MODEM_HOST_OK && frames == 1 &&
		modem_host_wait_ns(host, UINT64_MAX) == 1000000,
		"replacement call inherited retired remote clock credit");
	CHECK(modem_host_close(host) == SIP_SESSION_OK,
	      "remote-clock host did not close");
	modem_host_destroy(host);
	close(peer_fd);
	return true;
}

static bool test_remote_startup_qualifies_a_return_path(void)
{
	struct clock_fixture fixture;
	uint8_t packet[RTP_HEADER_SIZE + CLOCK_FRAME_SAMPLES];
	struct rtp_header previous = { 0 };
	size_t frames;

	CHECK(clock_fixture_open(&fixture, 60), "startup fixture failed");
	/* A symmetric RTP peer may need several sequential packets before it
	 * knows where to send audio. One seed cannot open that return path. */
	for (unsigned int index = 0; index < 5; ++index) {
		const uint64_t now = index * UINT64_C(20000000);
		struct rtp_packet_view view;
		ssize_t count;

		CHECK(sendto(fixture.peer, "bad", 3, 0,
			(struct sockaddr *)&fixture.address, sizeof(fixture.address)) == 3 &&
		      modem_host_step(fixture.host, now, NULL, &frames) == MODEM_HOST_OK &&
		      frames == 1, "startup stopped before source qualification at %u", index);
		count = recv(fixture.peer, packet, sizeof(packet), MSG_DONTWAIT);
		CHECK(count > 0 && rtp_packet_parse(packet, (size_t)count, &view) == RTP_OK &&
		      view.payload_size == CLOCK_FRAME_SAMPLES &&
		      (index == 0 ||
		       (view.header.sequence == (uint16_t)(previous.sequence + 1u) &&
			view.header.timestamp == previous.timestamp + CLOCK_FRAME_SAMPLES)),
		      "startup RTP was not consecutive at %u", index);
		previous = view.header;
		CHECK(modem_host_step(fixture.host, now + UINT64_C(19999999),
			NULL, &frames) == MODEM_HOST_IDLE && frames == 0 &&
		      modem_host_wait_ns(fixture.host, now + UINT64_C(19999999)) == 1,
		      "startup advanced before its nominal frame deadline");
	}
	CHECK(fixture.capture.generated_count == 5 * CLOCK_FRAME_SAMPLES &&
	      clock_fixture_send(&fixture, 0) &&
	      modem_host_step(fixture.host, UINT64_C(100000000), NULL, &frames) == MODEM_HOST_OK &&
	      frames == 1 && fixture.capture.received_count == CLOCK_FRAME_SAMPLES &&
	      fixture.capture.generated_count == 5 * CLOCK_FRAME_SAMPLES,
	      "first peer audio was delayed or also consumed a startup tick");
	CHECK(modem_host_step(fixture.host, UINT64_C(200000000), NULL, &frames) == MODEM_HOST_IDLE &&
	      frames == 0, "established remote clock resumed wall-clock transmission");
	clock_fixture_close(&fixture);
	return true;
}

static bool test_audio_failure_waits_for_a_new_media_lifecycle(void)
{
	struct core_capture core = {
		.result = MODEM_CORE_STOPPED,
	};
	struct modem_host *host;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	fake.defer_hangup_media = true;
	host = create_host(NULL, &core);
	CHECK(host != NULL &&
		      modem_host_open(host) == SIP_SESSION_OK &&
		      modem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIP_SIGNALING_OK,
	      "failed-audio host call did not start");
	CHECK(modem_host_step(host, 0, NULL, &frames) ==
		      MODEM_HOST_MODEM_STOPPED &&
		      frames == 0 && core.calls == 1 && fake.hangups == 1 &&
		      modem_host_media_is_configured(host) &&
		      !modem_host_audio_is_active(host) &&
		      modem_host_wait_ns(host, 0) ==
			      MODEM_HOST_SIGNALING_POLL_NS,
	      "stopped modem did not suspend audio and request hangup");
	core.result = MODEM_CORE_OK;
	CHECK(modem_host_step(host, 1000000, NULL, &frames) ==
		      MODEM_HOST_IDLE &&
		      frames == 0 && core.calls == 1,
	      "failed audio restarted before media became inactive");
	fake.emit_inactive_on_step = true;
	CHECK(modem_host_step(host, 2000000, NULL, &frames) ==
		      MODEM_HOST_IDLE &&
		      !modem_host_media_is_configured(host) &&
		      modem_host_dial(host, "sip:peer@127.0.0.1") ==
			      SIP_SIGNALING_OK &&
		      modem_host_step(host, 3000000, NULL, &frames) ==
			      MODEM_HOST_OK &&
		      frames == 1 && core.calls == 2,
	      "new media lifecycle did not rearm suspended audio");
	core.result = MODEM_CORE_ERROR;
	CHECK(modem_host_step(host, 4000000, NULL, &frames) ==
		      MODEM_HOST_AUDIO_ERROR &&
		      modem_host_last_audio_path_result(host) ==
			      AUDIO_PATH_MODEM_ERROR &&
		      fake.hangups == 2,
	      "modem failure did not retain its typed audio cause");
	CHECK(modem_host_close(host) == SIP_SESSION_OK,
	      "failed-audio host did not close");
	modem_host_destroy(host);
	return true;
}

static bool test_shared_failure_defers_signaling(void)
{
	struct app_config config;
	struct core_capture capture = { .result = MODEM_CORE_ERROR };
	const struct modem_core core = { .context = &capture, .process = process_core };
	/* Fake signaling never dereferences its shared-server identity. */
	struct sip_server *server = (struct sip_server *)&capture;
	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 0;
	config.media.remote_clock = false;
	memset(&fake, 0, sizeof(fake));
	struct modem_host *host = modem_host_create_shared(server, &config, NULL, NULL, &core);
	CHECK(host != NULL && modem_host_open(host) == SIP_SESSION_OK &&
	      modem_host_dial(host, "sip:peer@127.0.0.1") == SIP_SIGNALING_OK,
	      "shared host did not start");
	CHECK(modem_host_prepare_media(host) == SIP_SESSION_OK &&
	      modem_host_step(host, 0, NULL, NULL) == MODEM_HOST_AUDIO_ERROR &&
	      fake.hangups == 0 && !modem_host_audio_is_active(host),
	      "shared audio failure invoked signaling inside media work");
	CHECK(modem_host_finish_media(host) == SIP_SESSION_OK &&
	      modem_host_hangup(host) == SIP_SIGNALING_OK && fake.hangups == 1,
	      "owner did not finish failed media and hang up");
	CHECK(modem_host_close(host) == SIP_SESSION_OK, "shared host did not close");
	modem_host_destroy(host);
	return true;
}

static bool test_destroy_closes_an_open_host(void)
{
	struct modem_host *host;
	size_t datagrams = 99;
	size_t frames = 99;

	memset(&fake, 0, sizeof(fake));
	host = create_host(NULL, NULL);
	CHECK(host != NULL &&
		      modem_host_open(host) == SIP_SESSION_OK,
	      "open destroy fixture failed");
	modem_host_destroy(host);
	CHECK(fake.closes == 1 && fake.destroys == 1,
	      "destroy did not close and release an open host");
	CHECK(modem_host_frame_samples(NULL) == 0 &&
		      modem_host_media_poll_fd(NULL) == -1 &&
		      !modem_host_is_open(NULL) &&
		      !modem_host_media_is_configured(NULL) &&
		      !modem_host_audio_is_active(NULL) &&
		      modem_host_open(NULL) ==
			      SIP_SESSION_INVALID_ARGUMENT &&
		      modem_host_close(NULL) ==
			      SIP_SESSION_INVALID_ARGUMENT &&
		      modem_host_dial(NULL, "sip:peer@127.0.0.1") ==
			      SIP_SIGNALING_INVALID_ARGUMENT &&
		      modem_host_step(NULL, 0, &datagrams, &frames) ==
			      MODEM_HOST_INVALID_ARGUMENT &&
		      datagrams == 0 && frames == 0 &&
		      modem_host_wait_ns(NULL, 0) == UINT64_MAX &&
		      strcmp(modem_host_result_name(
				     MODEM_HOST_AUDIO_ERROR),
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
		{ "remote RTP clock", test_remote_media_clocks_elapsed_time },
		{ "early-media clock handoff", test_early_media_clock_handoff },
		{ "remote startup return path", test_remote_startup_qualifies_a_return_path },
		{ "remote gap without prefill", test_remote_gap_wait_has_no_fixed_prefill },
		{ "remote gap timeout", test_remote_gap_timeout_is_once_per_hole },
		{ "remote partial deadline", test_remote_partial_frame_is_bounded },
		{ "remote pacing and restart", test_remote_pacing_preserves_credit_and_restart },
		{ "remote feedback ceiling", test_remote_feedback_clock_is_bounded },
		{ "audio failure lifecycle",
		  test_audio_failure_waits_for_a_new_media_lifecycle },
		{ "shared failure ownership", test_shared_failure_defers_signaling },
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

struct sip_signaling *sip_signaling_create_shared(struct sip_server *server, const struct app_config *config, const struct sip_signaling_callbacks *callbacks)
{
	(void)server;
	return sip_signaling_create(config, callbacks);
}
