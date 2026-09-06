// SPDX-License-Identifier: BSD-3-Clause

#include <host.h>

#include "test.h"

#include <arpa/inet.h>
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

enum sip_signaling_result sip_signaling_step(
	struct sip_signaling *signaling, unsigned int timeout_ms)
{
	(void)timeout_ms;
	if (signaling == NULL || !signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
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

enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target)
{
	if (signaling == NULL || target == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	fake.dials++;
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
	config.media.remote_clock = true;
	return modem_host_create(&config, NULL, NULL, &core);
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

static bool test_remote_media_clocks_elapsed_time(void)
{
	struct core_capture core = { 0 };
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
	host = create_remote_host(&core);
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
			      MODEM_HOST_SIGNALING_POLL_NS,
	      "remote-clock host did not emit exactly one seed frame");
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
	CHECK(modem_host_step(host, UINT64_MAX, &datagrams, &frames) ==
		      MODEM_HOST_IDLE &&
		      datagrams == 0 && frames == 0 && core.calls == 2,
	      "remote-clock host advanced from wall time alone");
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
	CHECK(modem_host_step(host, UINT64_MAX, &datagrams, &frames) ==
		      MODEM_HOST_OK && datagrams == 1 && frames == 2 &&
		      core.calls == 4 && !core.received_silence,
	      "lost inbound RTP time delayed the modem's transmit clock");
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
		CHECK(modem_host_step(host, UINT64_MAX, &datagrams, &frames) ==
			      (part == 0 ? MODEM_HOST_IDLE : MODEM_HOST_OK) &&
			      datagrams == 1 && frames == part && core.calls == 4 + part,
		      "partial frame credit did not survive separate socket drains");
	}
	CHECK(modem_host_close(host) == SIP_SESSION_OK,
	      "remote-clock host did not close");
	modem_host_destroy(host);
	close(peer_fd);
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
