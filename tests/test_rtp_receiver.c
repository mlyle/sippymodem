// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_receiver.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOT_COUNT = 8,
	TEST_PAYLOAD_TYPE = 0,
	TEST_SILENCE = 0xff,
};

static struct sippymodem_rtp_packet_view packet_view(
	uint32_t ssrc, uint16_t sequence, uint32_t timestamp,
	const uint8_t *payload, size_t count)
{
	struct sippymodem_rtp_packet_view packet = {
		.header = {
			.payload_type = TEST_PAYLOAD_TYPE,
			.sequence = sequence,
			.timestamp = timestamp,
			.ssrc = ssrc,
		},
		.payload = payload,
		.payload_size = count,
	};

	return packet;
}

static bool init_receiver(struct sippymodem_rtp_receiver *receiver,
			  struct sippymodem_rtp_jitter_slot *slots,
			  size_t prefill_samples)
{
	const struct sippymodem_rtp_receiver_config config = {
		.payload_type = TEST_PAYLOAD_TYPE,
		.silence_octet = TEST_SILENCE,
		.prefill_samples = prefill_samples,
	};

	return sippymodem_rtp_receiver_init(receiver, &config, slots,
					    TEST_SLOT_COUNT);
}

static bool test_prefill_and_concealment(void)
{
	static const uint8_t payload[] = { 1, 2, 3, 4 };
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct sippymodem_rtp_receiver receiver;
	struct sippymodem_rtp_packet_view packet =
		packet_view(0x11223344, 1, 100, payload,
			    ARRAY_LENGTH(payload));
	struct sippymodem_rtp_playout_report report;
	uint8_t output[4];

	CHECK(init_receiver(&receiver, slots, 2), "receiver init failed");
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "receiver rejected its first packet");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, 2, &report),
	      "prefill playout failed");
	CHECK(output[0] == TEST_SILENCE && output[1] == TEST_SILENCE &&
		      report.prefill_samples == 2 &&
		      report.first_timestamp == 100,
	      "prefill did not delay the RTP timeline");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, 4, &report),
	      "payload playout failed");
	CHECK(memcmp(output, payload, sizeof(payload)) == 0 &&
		      report.received_samples == 4 &&
		      report.concealed_samples == 0,
	      "buffered payload was not played intact");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, 2, &report),
	      "concealment playout failed");
	CHECK(output[0] == TEST_SILENCE && output[1] == TEST_SILENCE &&
		      report.concealed_samples == 2,
	      "missing samples were not concealed");
	CHECK(receiver.stats.samples_prefilled == 2 &&
		      receiver.stats.samples_played == 4 &&
		      receiver.stats.samples_concealed == 2,
	      "playout statistics are wrong");
	return true;
}

static bool test_reordering_and_window_bounds(void)
{
	static const uint8_t first[] = { 1, 2 };
	static const uint8_t middle[] = { 3, 4 };
	static const uint8_t last[] = { 5, 6 };
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct sippymodem_rtp_receiver receiver;
	struct sippymodem_rtp_packet_view packet;
	uint8_t output[6];

	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	packet = packet_view(1, 10, 100, first, ARRAY_LENGTH(first));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "first packet failed");
	packet = packet_view(1, 12, 104, last, ARRAY_LENGTH(last));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "future packet failed");
	packet = packet_view(1, 11, 102, middle, ARRAY_LENGTH(middle));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "reordered packet failed");
	packet = packet_view(1, 12, 104, last, ARRAY_LENGTH(last));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED &&
		      receiver.stats.duplicate_samples == 2,
	      "duplicate samples were not retained safely");
	CHECK(receiver.stats.sequence_gaps == 1 &&
		      receiver.stats.sequence_out_of_order == 1 &&
		      receiver.stats.sequence_duplicates == 1,
	      "sequence continuity statistics are wrong");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, sizeof(output),
				    NULL) &&
		      memcmp(output, (uint8_t[]){ 1, 2, 3, 4, 5, 6 },
			     sizeof(output)) == 0,
	      "timestamp ordering failed");

	packet = packet_view(1, 9, 104, first, ARRAY_LENGTH(first));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_LATE,
	      "late packet was accepted");
	packet = packet_view(1, 13, 114, first, ARRAY_LENGTH(first));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_TOO_EARLY,
	      "packet beyond the bounded window was accepted");
	return true;
}

static bool test_ssrc_restart_and_wrap(void)
{
	static const uint8_t old_payload[] = { 1, 2 };
	static const uint8_t new_payload[] = { 9, 8, 7, 6 };
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct sippymodem_rtp_receiver receiver;
	struct sippymodem_rtp_packet_view packet;
	struct sippymodem_rtp_playout_report report;
	uint8_t output[4];

	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	packet = packet_view(1, 1, 100, old_payload, ARRAY_LENGTH(old_payload));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "old stream failed");
	packet = packet_view(2, UINT16_MAX, UINT32_MAX - 1, new_payload,
			     ARRAY_LENGTH(new_payload));
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "replacement stream failed");
	packet = packet_view(2, 0, 2, NULL, 0);
	CHECK(sippymodem_rtp_receiver_push(&receiver, &packet) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	      "wrapped sequence number failed");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, sizeof(output),
				    &report) &&
		      memcmp(output, new_payload, sizeof(output)) == 0 &&
		      report.first_timestamp == UINT32_MAX - 1,
	      "SSRC restart or timestamp wrap failed");
	CHECK(receiver.play_timestamp == 2 && receiver.stats.ssrc_restarts == 1,
	      "wrapped playout state is wrong");
	return true;
}

static bool test_validation_and_idle(void)
{
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct sippymodem_rtp_receiver receiver;
	struct sippymodem_rtp_receiver_config config = {
		.payload_type = 128,
	};
	struct sippymodem_rtp_playout_report report;
	struct sippymodem_rtp_header header = {
		.payload_type = TEST_PAYLOAD_TYPE,
	};
	uint8_t packet[16];
	uint8_t output[2] = { 0, 0 };
	size_t packet_size;
	enum sippymodem_rtp_result packet_result;

	CHECK(!sippymodem_rtp_receiver_init(&receiver, &config, slots,
					     TEST_SLOT_COUNT),
	      "receiver accepted an invalid payload type");
	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	CHECK(sippymodem_rtp_receiver_play(&receiver, output, sizeof(output),
				    &report),
	      "idle playout failed");
	CHECK(!report.stream_active && output[0] == TEST_SILENCE &&
		      output[1] == TEST_SILENCE &&
		      receiver.stats.samples_idle == 2,
	      "idle playout was not identified");
	CHECK(sippymodem_rtp_packet_write(packet, sizeof(packet), &header,
					   output, sizeof(output),
					   &packet_size) == SIPPYMODEM_RTP_OK,
	      "failed to build receiver input");
	CHECK(sippymodem_rtp_receiver_receive(&receiver, packet, packet_size,
				       &packet_result) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED &&
		      packet_result == SIPPYMODEM_RTP_OK,
	      "receiver did not accept a serialized packet");
	packet[1] = 8;
	CHECK(sippymodem_rtp_receiver_receive(&receiver, packet, packet_size,
				       &packet_result) ==
		      SIPPYMODEM_RTP_RECEIVE_UNEXPECTED_PAYLOAD &&
		      packet_result == SIPPYMODEM_RTP_OK &&
		      receiver.stats.unexpected_payloads == 1,
	      "receiver did not filter an unexpected payload type");
	CHECK(sippymodem_rtp_receiver_receive(&receiver, packet, 4,
				       &packet_result) ==
		      SIPPYMODEM_RTP_RECEIVE_MALFORMED_PACKET &&
		      packet_result == SIPPYMODEM_RTP_PACKET_TOO_SHORT &&
		      receiver.stats.malformed_packets == 1,
	      "receiver did not report malformed wire input");
	CHECK(!sippymodem_rtp_receiver_play(&receiver, NULL, 2, &report),
	      "playout accepted a null output");
	sippymodem_rtp_receiver_reset(&receiver);
	CHECK(!receiver.stream_active && receiver.stats.samples_idle == 0,
	      "receiver reset left call state behind");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "prefill and concealment", test_prefill_and_concealment },
		{ "reordering and window bounds",
		  test_reordering_and_window_bounds },
		{ "SSRC restart and timestamp wrap",
		  test_ssrc_restart_and_wrap },
		{ "validation and idle", test_validation_and_idle },
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
