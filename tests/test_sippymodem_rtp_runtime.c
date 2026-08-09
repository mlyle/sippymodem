// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_rtp_runtime.h>

#include "test.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct recording_capture {
	enum sippymodem_recording_stream streams[4];
	size_t calls;
};

static void capture_recording(
	void *context, const struct sippymodem_recording_block *block)
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
	struct sippymodem_config config;
	struct recording_capture capture = { 0 };
	const struct sippymodem_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	const struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sippymodem_rtp_runtime *runtime;
	struct sippymodem_rtp_endpoint *endpoint;
	struct sockaddr_in peer_address;
	struct pollfd descriptor;
	uint8_t packet[64];
	int16_t samples[8] = { 0 };
	enum sippymodem_rtp_endpoint_result open_result;
	ssize_t received;
	int peer_socket;

	sippymodem_config_init(&config);
	config.media.frame_duration_ms = 1;
	config.media.jitter_buffer_ms = 2;
	runtime = sippymodem_rtp_runtime_create(&config.media, &recorder);
	CHECK(runtime != NULL, "RTP runtime creation failed");
	endpoint = sippymodem_rtp_runtime_endpoint(runtime);
	CHECK(sippymodem_rtp_runtime_frame_samples(runtime) ==
		      ARRAY_LENGTH(samples) &&
		      endpoint != NULL && endpoint->stream != NULL &&
		      endpoint->stream->receiver.slot_count == 24 &&
		      endpoint->stream->receiver.config.prefill_samples == 16,
	      "RTP runtime storage does not match the media timing");
	open_result = sippymodem_rtp_runtime_open(runtime, &local);
	CHECK(open_result == SIPPYMODEM_RTP_ENDPOINT_OK &&
		      sippymodem_rtp_runtime_local_port(runtime) != 0,
	      "RTP runtime did not open an ephemeral endpoint: %s (%d)",
	      sippymodem_rtp_endpoint_result_name(open_result),
	      endpoint->last_system_error);
	CHECK(open_peer(&peer_socket, &peer_address) &&
		      sippymodem_rtp_endpoint_set_remote(endpoint,
						 &peer_address) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "RTP runtime peer setup failed");

	CHECK(sippymodem_rtp_runtime_send(runtime, samples,
					  ARRAY_LENGTH(samples) - 1) ==
		      SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT &&
		      capture.calls == 0,
	      "RTP runtime accepted a partial media frame");
	CHECK(sippymodem_rtp_runtime_send(runtime, samples,
					  ARRAY_LENGTH(samples)) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "RTP runtime did not send one complete frame");
	descriptor.fd = peer_socket;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	CHECK(poll(&descriptor, 1, 1000) == 1,
	      "RTP runtime packet did not reach its peer");
	received = recv(peer_socket, packet, sizeof(packet), 0);
	CHECK(received == SIPPYMODEM_RTP_HEADER_SIZE +
				   (ssize_t)ARRAY_LENGTH(samples) &&
		      capture.calls == 3 &&
		      capture.streams[0] == SIPPYMODEM_RECORD_TX_LINE_PCM &&
		      capture.streams[1] ==
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      capture.streams[2] ==
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT,
	      "RTP runtime did not preserve the recording lifecycle");

	close(peer_socket);
	sippymodem_rtp_runtime_close(runtime);
	CHECK(sippymodem_rtp_runtime_local_port(runtime) == 0,
	      "RTP runtime retained a closed port");
	sippymodem_rtp_runtime_destroy(runtime);
	return true;
}

static bool test_rejects_unusable_media_config(void)
{
	struct sippymodem_config config;

	sippymodem_config_init(&config);
	config.media.codec = SIPPYMODEM_CODEC_CLEARMODE;
	CHECK(sippymodem_rtp_runtime_create(&config.media, NULL) == NULL,
	      "RTP runtime accepted media without a linear mapping");
	config.media.codec = SIPPYMODEM_CODEC_PCMU;
	config.media.frame_duration_ms = 0;
	CHECK(sippymodem_rtp_runtime_create(&config.media, NULL) == NULL,
	      "RTP runtime accepted a zero-length frame");
	config.media.frame_duration_ms = 255;
	CHECK(sippymodem_rtp_runtime_create(&config.media, NULL) == NULL,
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
