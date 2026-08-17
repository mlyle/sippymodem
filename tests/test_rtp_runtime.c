// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_runtime.h>

#include <g711.h>

#include "test.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct recording_capture {
	enum recording_stream streams[4];
	size_t calls;
};

static void capture_recording(
	void *context, const struct recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture->calls < ARRAY_LENGTH(capture->streams))
		capture->streams[capture->calls] = block->stream;
	capture->calls++;
}

static bool open_peer(int *socket_fd, struct sockaddr_in *address)
{
	socklen_t address_size = sizeof(*address);
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	*socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (*socket_fd < 0 ||
	    bind(*socket_fd, (const struct sockaddr *)&local, sizeof(local)) !=
		    0 ||
	    getsockname(*socket_fd, (struct sockaddr *)address,
			&address_size) != 0) {
		if (*socket_fd >= 0)
			close(*socket_fd);
		*socket_fd = -1;
		return false;
	}
	return true;
}

static bool test_owns_bounded_media_storage(void)
{
	struct app_config config;
	struct recording_capture capture = { 0 };
	const struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	const struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct rtp_runtime *runtime;
	struct rtp_endpoint *endpoint;
	struct sockaddr_in peer_address;
	struct pollfd descriptor;
	uint8_t packet[64];
	int16_t samples[8] = { 0 };
	const uint8_t codewords[8] = {
		0x7f, 0xff, 0x00, 0x80, 0xfe, 0x7e, 0x81, 0x01,
	};
	int16_t decoded[ARRAY_LENGTH(codewords)];
	enum rtp_endpoint_result open_result;
	ssize_t received;
	int peer_socket;

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 2;
	config.media.remote_clock = false;
	runtime = rtp_runtime_create(&config.media, &recorder);
	CHECK(runtime != NULL, "RTP runtime creation failed");
	endpoint = rtp_runtime_endpoint(runtime);
	CHECK(rtp_runtime_frame_samples(runtime) ==
		      ARRAY_LENGTH(samples) &&
		      endpoint != NULL && endpoint->stream != NULL &&
		      endpoint->stream->receiver.slot_count == 56 &&
		      endpoint->stream->receiver.config.prefill_samples == 16,
	      "RTP runtime storage does not match the media timing");
	open_result = rtp_runtime_open(runtime, &local);
	CHECK(open_result == RTP_ENDPOINT_OK &&
		      rtp_runtime_local_port(runtime) != 0,
	      "RTP runtime did not open an ephemeral endpoint: %s (%d)",
	      rtp_endpoint_result_name(open_result),
	      endpoint->last_system_error);
	CHECK(open_peer(&peer_socket, &peer_address) &&
		      rtp_endpoint_set_remote(endpoint,
						 &peer_address) ==
			      RTP_ENDPOINT_OK,
	      "RTP runtime peer setup failed");

	CHECK(rtp_runtime_send(runtime, samples,
					  ARRAY_LENGTH(samples) - 1) ==
		      RTP_ENDPOINT_INVALID_ARGUMENT &&
		      capture.calls == 0,
	      "RTP runtime accepted a partial media frame");
	CHECK(rtp_runtime_send(runtime, samples,
					  ARRAY_LENGTH(samples)) ==
		      RTP_ENDPOINT_OK,
	      "RTP runtime did not send one complete frame");
	descriptor.fd = peer_socket;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	CHECK(poll(&descriptor, 1, 1000) == 1,
	      "RTP runtime packet did not reach its peer");
	received = recv(peer_socket, packet, sizeof(packet), 0);
	CHECK(received == RTP_HEADER_SIZE +
				   (ssize_t)ARRAY_LENGTH(samples) &&
		      capture.calls == 3 &&
		      capture.streams[0] == RECORD_TX_LINE_PCM &&
		      capture.streams[1] ==
			      RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      capture.streams[2] ==
			      RECORD_TX_RTP_PAYLOAD_SENT,
	      "RTP runtime did not preserve the recording lifecycle");

	for (size_t index = 0; index < ARRAY_LENGTH(codewords); ++index)
		decoded[index] = g711_decode_pcmu(codewords[index]);
	capture.calls = 0;
	CHECK(rtp_runtime_send_g711(
		      runtime, codewords, decoded, ARRAY_LENGTH(codewords) - 1) ==
			      RTP_ENDPOINT_INVALID_ARGUMENT &&
		      capture.calls == 0,
	      "RTP runtime accepted a partial exact G.711 frame");
	CHECK(rtp_runtime_send_g711(
		      runtime, codewords, decoded, ARRAY_LENGTH(codewords)) ==
			      RTP_ENDPOINT_OK &&
		      poll(&descriptor, 1, 1000) == 1,
	      "RTP runtime did not send an exact G.711 frame");
	received = recv(peer_socket, packet, sizeof(packet), 0);
	CHECK(received == RTP_HEADER_SIZE + (ssize_t)sizeof(codewords) &&
		      memcmp(packet + RTP_HEADER_SIZE, codewords,
			     sizeof(codewords)) == 0 &&
		      capture.calls == 3 &&
		      capture.streams[0] == RECORD_TX_LINE_PCM &&
		      capture.streams[1] == RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      capture.streams[2] == RECORD_TX_RTP_PAYLOAD_SENT,
	      "RTP runtime changed exact codewords or recording order");

	close(peer_socket);
	rtp_runtime_close(runtime);
	CHECK(rtp_runtime_local_port(runtime) == 0,
	      "RTP runtime retained a closed port");
	rtp_runtime_destroy(runtime);
	return true;
}

static bool test_buffers_scheduler_catchup_burst(void)
{
	struct app_config config;
	struct rtp_runtime *runtime;
	struct rtp_endpoint *endpoint;
	struct rtp_receiver *receiver;
	uint8_t payloads[8][8];
	uint8_t played[8];
	uint8_t expected[7 * 8];
	uint8_t actual[7 * 8];
	size_t actual_count = 0;

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 2;
	config.media.remote_clock = false;
	runtime = rtp_runtime_create(&config.media, NULL);
	CHECK(runtime != NULL, "catch-up RTP runtime creation failed");
	endpoint = rtp_runtime_endpoint(runtime);
	receiver = &endpoint->stream->receiver;
	CHECK(receiver->slot_count ==
		      16 + 8 * (MEDIA_MAX_CATCHUP_FRAMES + 1),
	      "RTP window does not reserve four catch-up frames");

	for (size_t packet_index = 0;
	     packet_index < ARRAY_LENGTH(payloads); ++packet_index) {
		struct rtp_packet_view packet = {
			.header = {
				.payload_type = 0,
				.sequence = (uint16_t)packet_index,
				.timestamp = 1000 + (uint32_t)(packet_index * 8),
				.ssrc = 1,
			},
			.payload = payloads[packet_index],
			.payload_size = ARRAY_LENGTH(payloads[packet_index]),
		};

		memset(payloads[packet_index], (int)(packet_index + 1),
		       sizeof(payloads[packet_index]));
		if (packet_index < 4) {
			CHECK(rtp_receiver_push(receiver, &packet) ==
				      RTP_RECEIVE_ACCEPTED,
			      "initial RTP packet %zu was rejected", packet_index);
			if (packet_index == 2) {
				CHECK(rtp_receiver_play(receiver, played,
							ARRAY_LENGTH(played), NULL) &&
					      rtp_receiver_play(receiver, played,
							ARRAY_LENGTH(played), NULL),
				      "RTP prefill failed");
				CHECK(rtp_receiver_play(receiver, played,
							ARRAY_LENGTH(played), NULL),
				      "first RTP frame did not play");
			}
			continue;
		}

		CHECK(rtp_receiver_push(receiver, &packet) ==
			      RTP_RECEIVE_ACCEPTED,
		      "catch-up RTP packet %zu was rejected", packet_index);
	}

	for (size_t packet_index = 1;
	     packet_index < ARRAY_LENGTH(payloads); ++packet_index) {
		memcpy(expected + (packet_index - 1) * sizeof(payloads[0]),
		       payloads[packet_index], sizeof(payloads[packet_index]));
		CHECK(rtp_receiver_play(receiver, played,
					ARRAY_LENGTH(played), NULL),
		      "buffered catch-up frame did not play");
		memcpy(actual + actual_count, played, sizeof(played));
		actual_count += sizeof(played);
	}
	CHECK(memcmp(actual, expected, sizeof(expected)) == 0 &&
		      receiver->stats.early_samples == 0 &&
		      receiver->stats.samples_concealed == 0,
	      "catch-up burst was dropped or concealed");

	rtp_runtime_destroy(runtime);
	return true;
}

static bool test_buffers_remote_clock_backlog(void)
{
	enum {
		FRAME_SAMPLES = 8,
		FRAME_COUNT = 64,
	};
	struct app_config config;
	struct rtp_runtime *runtime;
	struct rtp_endpoint *endpoint;
	struct rtp_receiver *receiver;
	uint8_t payload[FRAME_SAMPLES];
	uint8_t played[FRAME_SAMPLES];

	config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 2;
	config.media.remote_clock = true;
	runtime = rtp_runtime_create(&config.media, NULL);
	CHECK(runtime != NULL, "remote-clock RTP runtime creation failed");
	endpoint = rtp_runtime_endpoint(runtime);
	receiver = &endpoint->stream->receiver;

	for (size_t frame = 0; frame < FRAME_COUNT; ++frame) {
		struct rtp_packet_view packet = {
			.header = {
				.payload_type = 0,
				.sequence = (uint16_t)frame,
				.timestamp = 1000 +
					(uint32_t)(frame * FRAME_SAMPLES),
				.ssrc = 1,
			},
			.payload = payload,
			.payload_size = sizeof(payload),
		};

		memset(payload, (int)(frame + 1u), sizeof(payload));
		CHECK(rtp_receiver_push(receiver, &packet) ==
			      RTP_RECEIVE_ACCEPTED,
		      "remote-clock frame %zu exceeded its backlog", frame);
	}
	CHECK(receiver->stats.early_samples == 0,
	      "remote-clock backlog discarded future media");
	for (size_t prefill = 0; prefill < 2; ++prefill)
		CHECK(rtp_receiver_play(
			      receiver, played, sizeof(played), NULL),
		      "remote-clock prefill did not play");
	for (size_t frame = 0; frame < FRAME_COUNT; ++frame) {
		CHECK(rtp_receiver_play(
			      receiver, played, sizeof(played), NULL),
		      "remote-clock backlog frame %zu did not play", frame);
		for (size_t sample = 0; sample < sizeof(played); ++sample)
			CHECK(played[sample] == (uint8_t)(frame + 1u),
			      "remote-clock frame %zu sample %zu changed",
			      frame, sample);
	}

	rtp_runtime_destroy(runtime);
	return true;
}

static bool test_rejects_unusable_media_config(void)
{
	struct app_config config;

	config_init(&config);
	config.media.codec = MEDIA_CODEC_CLEARMODE;
	CHECK(rtp_runtime_create(&config.media, NULL) == NULL,
	      "RTP runtime accepted media without a linear mapping");
	config.media.codec = MEDIA_CODEC_PCMU;
	config.media.frame_duration_ms = 0;
	CHECK(rtp_runtime_create(&config.media, NULL) == NULL,
	      "RTP runtime accepted a zero-length frame");
	config.media.frame_duration_ms = 255;
	CHECK(rtp_runtime_create(&config.media, NULL) == NULL,
	      "RTP runtime accepted frames beyond its datagram boundary");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "bounded media storage", test_owns_bounded_media_storage },
		{ "scheduler catch-up burst", test_buffers_scheduler_catchup_burst },
		{ "remote-clock backlog", test_buffers_remote_clock_backlog },
		{ "unusable media config", test_rejects_unusable_media_config },
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
