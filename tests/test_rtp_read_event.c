// SPDX-License-Identifier: BSD-3-Clause
#include "rtp_endpoint.h"
#include "test.h"

#ifndef _WIN32
#include <poll.h>
#endif

static bool test_read_event(void)
{
	struct rtp_stream stream;
	struct rtp_endpoint endpoint;
	struct rtp_jitter_slot slots[16];
	uint8_t workspace[8];
	const struct rtp_stream_config config = { .codec = MEDIA_CODEC_PCMU, .payload_type = 0 };
	const struct rtp_endpoint_config endpoint_config = { 0 };
	struct sockaddr_in address = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	CHECK(rtp_stream_init(&stream, &config, slots, ARRAY_LENGTH(slots), workspace, sizeof(workspace)) &&
	      rtp_endpoint_init(&endpoint, &stream, &endpoint_config), "RTP initialize failed");
	for (unsigned int pass = 0; pass < 10; ++pass) {
		CHECK(rtp_endpoint_open(&endpoint, &address) == RTP_ENDPOINT_OK, "open failed");
		net_socket sender = net_udp_open();
		CHECK(sender != NET_INVALID_SOCKET, "sender open failed");
#ifdef _WIN32
		HANDLE event = rtp_endpoint_read_handle(&endpoint);
		DWORD flags;
		CHECK(event != NULL && GetHandleInformation(event, &flags) &&
		      !(flags & HANDLE_FLAG_INHERIT) && WaitForSingleObject(event, 0) == WAIT_TIMEOUT,
		      "empty event/ownership differs");
#else
		CHECK(rtp_endpoint_read_handle(&endpoint) == NULL, "POSIX has a Windows event");
#endif
		const unsigned char packet[] = { 0, 1, 2 };
		CHECK(net_socket_send(sender, packet, sizeof(packet), &endpoint.local_address) == sizeof(packet),
		      "send failed");
#ifdef _WIN32
		CHECK(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0 &&
		      WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "wait consumed RTP readiness");
#else
		struct pollfd descriptor = { .fd = endpoint.socket_fd, .events = POLLIN };
		CHECK(poll(&descriptor, 1, 1000) == 1, "RTP did not become readable");
#endif
		uint64_t before = endpoint.stats.receive_datagrams;
		size_t drained;
		CHECK(rtp_endpoint_drain(&endpoint, &drained) == RTP_ENDPOINT_OK && drained == 1 &&
		      endpoint.stats.receive_datagrams == before + 1, "wait lost the datagram");
		/* A reenabling recv can leave one final Windows level notification. */
		CHECK(rtp_endpoint_drain(&endpoint, &drained) == RTP_ENDPOINT_OK && drained == 0,
		      "empty drain failed");
#ifdef _WIN32
		CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "empty socket kept its event signaled");
#endif
		net_socket_close(sender);
		rtp_endpoint_close(&endpoint);
		rtp_endpoint_close(&endpoint);
		CHECK(rtp_endpoint_read_handle(&endpoint) == NULL, "close retained a read event");
	}
	return true;
}

int main(void)
{
	return test_read_event() ? 0 : 1;
}
