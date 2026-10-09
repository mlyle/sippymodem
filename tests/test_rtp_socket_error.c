// SPDX-License-Identifier: BSD-3-Clause
#define _POSIX_C_SOURCE 200809L
#include "rtp_endpoint.h"
#include "g711.h"
#include "test.h"

#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static unsigned packet_events;
static void capture_packet(void *context, const struct recording_packet *packet)
{
	(void)context;
	if (packet->event == RECORD_PACKET_SEND_FAILED)
		packet_events++;
	errno = EDOM;
#ifdef _WIN32
	WSASetLastError(WSAEINVAL);
#endif
}

static bool test_send_error(void)
{
	struct rtp_stream stream;
	struct rtp_endpoint endpoint;
	struct rtp_jitter_slot slots[16];
	uint8_t workspace[8], packet[64];
	const struct rtp_stream_config config = {
		.codec = MEDIA_CODEC_PCMU, .payload_type = 0,
		.recorder = { .packet = capture_packet },
	};
	const struct rtp_endpoint_config endpoint_config = { 0 };
	struct sockaddr_in address = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	CHECK(rtp_stream_init(&stream, &config, slots, ARRAY_LENGTH(slots),
		      workspace, sizeof(workspace)) &&
	      rtp_endpoint_init(&endpoint, &stream, &endpoint_config) &&
	      rtp_endpoint_open(&endpoint, &address) == RTP_ENDPOINT_OK,
	      "RTP startup failed");
	address.sin_port = htons(9);
	CHECK(rtp_endpoint_set_remote(&endpoint, &address) == RTP_ENDPOINT_OK,
	      "remote setup failed");
	/* Invalidate the OS socket while retaining the owner's startup reference. */
#ifdef _WIN32
	CHECK(closesocket(endpoint.socket_fd) == 0, "test close failed");
	const int expected_error = WSAENOTSOCK;
#else
	CHECK(close(endpoint.socket_fd) == 0, "test close failed");
	const int expected_error = EBADF;
#endif
	const uint8_t codes[] = { 0xff, 0x7f, 0xdf, 0x5f };
	int16_t samples[ARRAY_LENGTH(codes)];
	for (size_t i = 0; i < ARRAY_LENGTH(codes); ++i)
		samples[i] = g711_decode(G711_LAW_PCMU, codes[i]);
	CHECK(rtp_endpoint_send_g711(&endpoint, codes, samples, ARRAY_LENGTH(codes),
		      packet, sizeof(packet)) == RTP_ENDPOINT_SYSTEM_ERROR &&
	      endpoint.last_system_error == expected_error && packet_events == 1,
	      "recorder replaced socket failure: got=%d expected=%d events=%u",
	      endpoint.last_system_error, expected_error, packet_events);
	/* Close also balances the owned Winsock reference after this injected failure. */
	rtp_endpoint_close(&endpoint);
	return true;
}

int main(void)
{
	return test_send_error() ? 0 : 1;
}
