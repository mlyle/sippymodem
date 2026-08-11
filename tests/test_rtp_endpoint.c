// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_endpoint.h>

#include "test.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
	TEST_RECORDINGS = 12,
	TEST_PACKET_SIZE = 64,
};

struct recording_capture {
	enum sippymodem_recording_stream streams[TEST_RECORDINGS];
	size_t counts[TEST_RECORDINGS];
	uint8_t data[TEST_RECORDINGS][16];
	size_t calls;
};

struct endpoint_fixture {
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct sippymodem_rtp_endpoint endpoint;
	struct recording_capture recordings;
	uint8_t receive_workspace[TEST_WORKSPACE];
};

static void capture_recording(
	void *context,
	const struct sippymodem_recording_block *block)
{
	struct recording_capture *capture = context;
	size_t bytes = block->count;

	if (capture->calls >= ARRAY_LENGTH(capture->streams))
		return;
	if (block->format == SIPPYMODEM_RECORD_S16_NATIVE)
		bytes *= sizeof(int16_t);
	if (bytes > sizeof(capture->data[0]))
		bytes = sizeof(capture->data[0]);
	capture->streams[capture->calls] = block->stream;
	capture->counts[capture->calls] = block->count;
	memcpy(capture->data[capture->calls], block->data, bytes);
	capture->calls++;
}

static bool open_fixture(struct endpoint_fixture *fixture, uint32_t seed,
			 bool learn_remote, bool symmetric_rtp)
{
	const struct sippymodem_rtp_stream_config stream_config = {
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
		.transmit_sequence = (uint16_t)seed,
		.transmit_timestamp = seed * 80,
		.transmit_ssrc = 0x10000000u + seed,
		.recorder = {
			.context = &fixture->recordings,
			.write = capture_recording,
		},
	};
	const struct sockaddr_in local_address = {
		.sin_family = AF_INET,
		.sin_port = 0,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	const struct sippymodem_rtp_endpoint_config endpoint_config = {
		.learn_remote = learn_remote,
		.symmetric_rtp = symmetric_rtp,
	};
	enum sippymodem_rtp_endpoint_result open_result;

	memset(fixture, 0, sizeof(*fixture));
	if (!sippymodem_rtp_stream_init(
		    &fixture->stream, &stream_config, fixture->slots,
		    ARRAY_LENGTH(fixture->slots), fixture->receive_workspace,
		    sizeof(fixture->receive_workspace))) {
		fprintf(stderr, "fixture stream init failed\n");
		return false;
	}
	if (!sippymodem_rtp_endpoint_init(&fixture->endpoint,
					   &fixture->stream,
					   &endpoint_config)) {
		fprintf(stderr, "fixture endpoint init failed\n");
		return false;
	}
	open_result = sippymodem_rtp_endpoint_open(&fixture->endpoint,
					    &local_address);
	if (open_result != SIPPYMODEM_RTP_ENDPOINT_OK) {
		fprintf(stderr, "fixture endpoint open failed: %s (%d)\n",
			sippymodem_rtp_endpoint_result_name(open_result),
			fixture->endpoint.last_system_error);
		return false;
	}
	return true;
}

static struct sockaddr_in fixture_address(
	const struct endpoint_fixture *fixture)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	address.sin_port = htons(
		sippymodem_rtp_endpoint_local_port(&fixture->endpoint));
	return address;
}

static bool wait_for_datagram(const struct sippymodem_rtp_endpoint *endpoint)
{
	struct pollfd descriptor = {
		.fd = sippymodem_rtp_endpoint_fd(endpoint),
		.events = POLLIN,
	};

	return poll(&descriptor, 1, 1000) == 1 &&
	       (descriptor.revents & POLLIN) != 0;
}

static bool send_raw(int socket_fd, const struct sockaddr_in *remote,
		     const void *data, size_t size)
{
	return sendto(socket_fd, data, size, 0,
		      (const struct sockaddr *)remote, sizeof(*remote)) ==
	       (ssize_t)size;
}

static bool test_loopback_media_and_recording(void)
{
	static const int16_t transmit[] = { 0, 1000, -1000, 0 };
	static const int16_t expected[] = { 0, 988, -988, 0 };
	static const uint8_t encoded[] = { 0xff, 0xce, 0x4e, 0xff };
	struct endpoint_fixture sender;
	struct endpoint_fixture receiver;
	struct sockaddr_in sender_address;
	struct sockaddr_in receiver_address;
	struct sippymodem_rtp_playout_report report;
	uint8_t packet[TEST_PACKET_SIZE];
	int16_t played[ARRAY_LENGTH(transmit)];
	size_t drained = 0;

	CHECK(open_fixture(&sender, 1, false, false),
	      "failed to open sender endpoint");
	CHECK(open_fixture(&receiver, 2, false, false),
	      "failed to open receiver endpoint");
	sender_address = fixture_address(&sender);
	receiver_address = fixture_address(&receiver);
	CHECK(sippymodem_rtp_endpoint_send(
		      &sender.endpoint, transmit, ARRAY_LENGTH(transmit), packet,
		      sizeof(packet)) == SIPPYMODEM_RTP_ENDPOINT_NO_REMOTE,
	      "send without an SDP remote was not rejected");
	CHECK(sender.recordings.calls == 0 &&
		      sender.endpoint.stats.transmit_no_remote == 1,
	      "rejected send changed media or recording state");
	CHECK(sippymodem_rtp_endpoint_set_remote(&sender.endpoint,
						 &receiver_address) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      sippymodem_rtp_endpoint_set_remote(&receiver.endpoint,
						 &sender_address) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "failed to configure RTP remotes");
	CHECK(sippymodem_rtp_endpoint_send(
		      &sender.endpoint, transmit, ARRAY_LENGTH(transmit), packet,
		      sizeof(packet)) == SIPPYMODEM_RTP_ENDPOINT_OK,
	      "loopback RTP send failed");
	CHECK(sender.recordings.calls == 3 &&
		      sender.recordings.streams[0] ==
			      SIPPYMODEM_RECORD_TX_LINE_PCM &&
		      sender.recordings.streams[1] ==
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      sender.recordings.streams[2] ==
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT &&
		      memcmp(sender.recordings.data[2], encoded,
			     sizeof(encoded)) == 0,
	      "transmit recording lifecycle is wrong");
	CHECK(wait_for_datagram(&receiver.endpoint),
	      "receiver socket did not become readable");
	CHECK(sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK && drained == 1,
	      "receiver did not drain one packet");
	CHECK(receiver.recordings.calls == 1 &&
		      receiver.recordings.streams[0] ==
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL &&
		      memcmp(receiver.recordings.data[0], encoded,
			     sizeof(encoded)) == 0,
	      "arrival recording is missing or wrong");
	CHECK(sippymodem_rtp_stream_play(&receiver.stream, played,
				  ARRAY_LENGTH(played), &report) ==
		      SIPPYMODEM_MEDIA_OK &&
		      memcmp(played, expected, sizeof(expected)) == 0,
	      "loopback media did not decode correctly");
	CHECK(receiver.recordings.calls == 3 &&
		      receiver.recordings.streams[1] ==
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT &&
		      receiver.recordings.streams[2] ==
			      SIPPYMODEM_RECORD_RX_LINE_PCM,
	      "receive recording lifecycle is wrong");
	CHECK(sender.endpoint.stats.transmit_attempts == 1 &&
		      sender.endpoint.stats.transmit_sent == 1 &&
		      receiver.endpoint.stats.receive_datagrams == 1 &&
		      receiver.endpoint.stats.receive_routed == 1,
	      "endpoint statistics are wrong");

	sippymodem_rtp_endpoint_close(&receiver.endpoint);
	sippymodem_rtp_endpoint_close(&sender.endpoint);
	return true;
}

static bool test_source_filter_and_malformed_input(void)
{
	struct endpoint_fixture expected_source;
	struct endpoint_fixture receiver;
	struct sockaddr_in expected_address;
	struct sockaddr_in receiver_address;
	struct sippymodem_rtp_header header = {
		.payload_type = 0,
		.sequence = 1,
		.timestamp = 80,
		.ssrc = 3,
	};
	const uint8_t payload[] = { 0xff, 0xfe };
	uint8_t packet[TEST_PACKET_SIZE];
	uint8_t oversized[SIPPYMODEM_RTP_ENDPOINT_MAX_DATAGRAM + 1] = { 0 };
	size_t packet_size;
	size_t drained;
	int rogue_socket;

	CHECK(open_fixture(&expected_source, 3, false, false),
	      "failed to open expected source");
	CHECK(open_fixture(&receiver, 4, false, false),
	      "failed to open receiver");
	expected_address = fixture_address(&expected_source);
	receiver_address = fixture_address(&receiver);
	CHECK(sippymodem_rtp_endpoint_set_remote(
		      &receiver.endpoint, &expected_address) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "failed to set expected source");
	CHECK(sippymodem_rtp_packet_write(packet, sizeof(packet), &header,
					   payload, sizeof(payload),
					   &packet_size) == SIPPYMODEM_RTP_OK,
	      "failed to build filtered packet");
	rogue_socket = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(rogue_socket >= 0 &&
		      send_raw(rogue_socket, &receiver_address, packet,
			       packet_size),
	      "failed to send rogue packet");
	CHECK(wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      receiver.endpoint.stats.receive_filtered == 1 &&
		      receiver.recordings.calls == 0,
	      "unexpected RTP source was not filtered");
	close(rogue_socket);

	CHECK(send_raw(sippymodem_rtp_endpoint_fd(&expected_source.endpoint),
		       &receiver_address,
		       packet, 4) && wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      receiver.stream.receiver.stats.malformed_packets == 1 &&
		      receiver.recordings.calls == 0,
	      "malformed packet was recorded or not diagnosed");
	CHECK(send_raw(sippymodem_rtp_endpoint_fd(&expected_source.endpoint),
		       &receiver_address, oversized, sizeof(oversized)) &&
		      wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      receiver.endpoint.stats.receive_oversized == 1,
	      "oversized datagram was not discarded");

	sippymodem_rtp_endpoint_close(&receiver.endpoint);
	sippymodem_rtp_endpoint_close(&expected_source.endpoint);
	return true;
}

static bool test_symmetric_remote_learning(void)
{
	static const int16_t samples[] = { 0, 1000 };
	static const int16_t expected[] = { 0, 988 };
	struct endpoint_fixture sender;
	struct endpoint_fixture receiver;
	struct sockaddr_in sender_address;
	struct sockaddr_in receiver_address;
	struct sockaddr_in advertised_address;
	struct sockaddr_in transmit_address;
	struct sippymodem_rtp_header rogue_header = {
		.payload_type = 0,
		.sequence = 90,
		.timestamp = 900,
		.ssrc = 90,
	};
	uint8_t packet[TEST_PACKET_SIZE];
	int16_t played[ARRAY_LENGTH(samples)];
	size_t packet_size;
	size_t drained;
	bool learned = false;
	int rogue_socket;
	uint16_t advertised_port;

	CHECK(open_fixture(&sender, 6, false, false),
	      "failed to open symmetric sender");
	CHECK(open_fixture(&receiver, 7, true, true),
	      "failed to open symmetric receiver");
	sender_address = fixture_address(&sender);
	receiver_address = fixture_address(&receiver);
	advertised_address = sender_address;
	advertised_port = ntohs(advertised_address.sin_port);
	advertised_address.sin_port = htons(advertised_port == UINT16_MAX ?
						 advertised_port - 1 :
						 advertised_port + 1);
	CHECK(sippymodem_rtp_endpoint_set_remote(&sender.endpoint,
						 &receiver_address) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      sippymodem_rtp_endpoint_set_remote(&receiver.endpoint,
						 &advertised_address) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "failed to set symmetric test routes");
	rogue_socket = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(rogue_socket >= 0 &&
		      sippymodem_rtp_packet_write(packet, sizeof(packet),
					     &rogue_header, NULL, 0,
					     &packet_size) == SIPPYMODEM_RTP_OK &&
		      send_raw(rogue_socket, &receiver_address, packet,
			       packet_size) &&
		      wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      receiver.endpoint.stats.receive_filtered == 1 &&
		      receiver.endpoint.stats.remote_learns == 0,
	      "empty RTP packet established a learned route");
	CHECK(sippymodem_rtp_endpoint_send(&sender.endpoint, samples,
				    ARRAY_LENGTH(samples), packet,
				    sizeof(packet)) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK,
	      "receiver did not accept the NAT source");
	CHECK(receiver.endpoint.stats.remote_learns == 1 &&
		      sippymodem_rtp_endpoint_transmit_remote(
			      &receiver.endpoint, &transmit_address, &learned) &&
		      learned && transmit_address.sin_port ==
				 sender_address.sin_port &&
		      transmit_address.sin_addr.s_addr ==
				 sender_address.sin_addr.s_addr,
	      "receiver did not select the learned symmetric route");
	CHECK(sippymodem_rtp_stream_play(&receiver.stream, played,
				  ARRAY_LENGTH(played), NULL) ==
		      SIPPYMODEM_MEDIA_OK &&
		      memcmp(played, expected, sizeof(expected)) == 0,
	      "receiver did not play learned-source media");
	CHECK(sippymodem_rtp_endpoint_send(&receiver.endpoint, samples,
				    ARRAY_LENGTH(samples), packet,
				    sizeof(packet)) ==
		      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      wait_for_datagram(&sender.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&sender.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      sippymodem_rtp_stream_play(&sender.stream, played,
					    ARRAY_LENGTH(played), NULL) ==
			      SIPPYMODEM_MEDIA_OK &&
		      memcmp(played, expected, sizeof(expected)) == 0,
	      "symmetric reply did not reach the learned source");

	CHECK(sippymodem_rtp_packet_write(packet, sizeof(packet), &rogue_header,
					   (const uint8_t[]){ 0xff, 0xff }, 2,
					   &packet_size) == SIPPYMODEM_RTP_OK,
	      "failed to build post-learning packet");
	CHECK(send_raw(rogue_socket, &receiver_address, packet,
			       packet_size) &&
		      wait_for_datagram(&receiver.endpoint) &&
		      sippymodem_rtp_endpoint_drain(&receiver.endpoint, &drained) ==
			      SIPPYMODEM_RTP_ENDPOINT_OK &&
		      receiver.endpoint.stats.receive_filtered == 2 &&
		      receiver.endpoint.stats.remote_learns == 1,
	      "active media allowed an unknown source to relearn the route");
	close(rogue_socket);
	sippymodem_rtp_endpoint_close(&receiver.endpoint);
	sippymodem_rtp_endpoint_close(&sender.endpoint);
	return true;
}

static bool test_lifecycle_validation(void)
{
	struct endpoint_fixture fixture;
	struct sippymodem_rtp_endpoint_config defaults;
	struct sockaddr_in address;
	int status_flags;
	int descriptor_flags;

	sippymodem_rtp_endpoint_config_init(&defaults);
	CHECK(defaults.learn_remote && defaults.symmetric_rtp,
	      "endpoint defaults are not NAT-friendly");
	CHECK(open_fixture(&fixture, 5, false, false),
	      "failed to open endpoint");
	address = fixture_address(&fixture);
	status_flags = fcntl(sippymodem_rtp_endpoint_fd(&fixture.endpoint),
			     F_GETFL, 0);
	descriptor_flags = fcntl(sippymodem_rtp_endpoint_fd(&fixture.endpoint),
				 F_GETFD, 0);
	CHECK(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0 &&
		      descriptor_flags >= 0 &&
		      (descriptor_flags & FD_CLOEXEC) != 0,
	      "endpoint socket flags are wrong");
	CHECK(sippymodem_rtp_endpoint_open(&fixture.endpoint, &address) ==
		      SIPPYMODEM_RTP_ENDPOINT_ALREADY_OPEN,
	      "endpoint allowed a second open");
	address.sin_port = 0;
	CHECK(sippymodem_rtp_endpoint_set_remote(&fixture.endpoint, &address) ==
		      SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT,
	      "endpoint accepted a zero remote port");
	sippymodem_rtp_endpoint_close(&fixture.endpoint);
	CHECK(sippymodem_rtp_endpoint_fd(&fixture.endpoint) == -1 &&
		      sippymodem_rtp_endpoint_local_port(&fixture.endpoint) == 0 &&
		      sippymodem_rtp_endpoint_drain(&fixture.endpoint, NULL) ==
			      SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN,
	      "closed endpoint retained socket state");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "loopback media and recording",
		  test_loopback_media_and_recording },
		{ "source filter and malformed input",
		  test_source_filter_and_malformed_input },
		{ "symmetric remote learning", test_symmetric_remote_learning },
		{ "lifecycle validation", test_lifecycle_validation },
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
