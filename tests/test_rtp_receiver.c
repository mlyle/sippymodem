// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_receiver.h>

#include "test.h"
#include "rtp_state_fixture.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOT_COUNT = 8,
	TEST_PAYLOAD_TYPE = 0,
	TEST_SILENCE = 0xff,
};

static struct rtp_packet_view packet_view(
	uint32_t ssrc, uint16_t sequence, uint32_t timestamp,
	const uint8_t *payload, size_t count)
{
	struct rtp_packet_view packet = {
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

static bool init_receiver(struct rtp_receiver *receiver,
			  struct rtp_jitter_slot *slots,
			  size_t prefill_samples)
{
	const struct rtp_receiver_config config = {
		.payload_type = TEST_PAYLOAD_TYPE,
		.silence_octet = TEST_SILENCE,
		.prefill_samples = prefill_samples,
	};

	return rtp_receiver_init(receiver, &config, slots,
					    TEST_SLOT_COUNT);
}

static bool test_prefill_and_concealment(void)
{
	static const uint8_t payload[] = { 1, 2, 3, 4 };
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	struct rtp_packet_view packet =
		packet_view(0x11223344, 1, 100, payload,
			    ARRAY_LENGTH(payload));
	struct rtp_playout_report report;
	uint8_t output[4];

	CHECK(init_receiver(&receiver, slots, 2), "receiver init failed");
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "receiver rejected its first packet");
	CHECK(rtp_receiver_play(&receiver, output, NULL, 2, &report),
	      "prefill playout failed");
	CHECK(output[0] == TEST_SILENCE && output[1] == TEST_SILENCE &&
		      report.prefill_samples == 2 &&
		      report.first_timestamp == 100,
	      "prefill did not delay the RTP timeline");
	CHECK(rtp_receiver_play(&receiver, output, NULL, 4, &report),
	      "payload playout failed");
	CHECK(memcmp(output, payload, sizeof(payload)) == 0 &&
		      report.received_samples == 4 &&
		      report.concealed_samples == 0,
	      "buffered payload was not played intact");
	CHECK(rtp_receiver_play(&receiver, output, NULL, 2, &report),
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

static bool test_received_sample_provenance(void)
{
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	struct rtp_playout_report report;
	uint8_t output[6];
	bool received[6] = { true, true, true, true, true, true };
	const uint8_t zeros[] = { 0xff, 0x7f };
	struct rtp_packet_view packet = packet_view(1, 1, UINT32_MAX - 1u, zeros, 2);

	CHECK(init_receiver(&receiver, slots, 1) &&
	      rtp_receiver_play(&receiver, output, received, 6, &report), "idle playout failed");
	CHECK(memcmp(received, (bool[6]){ false }, sizeof(received)) == 0 && !report.stream_active,
	      "idle silence was described as received media");
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED, "zero packet failed");
	packet = packet_view(1, 3, 1u, zeros, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED, "future packet failed");
	struct rtp_receiver saved = receiver;
	CHECK(!rtp_receiver_play(&receiver, output, (bool *)output, 6, &report) &&
	      rtp_receiver_states_equal(&receiver, &saved), "overlapping mask advanced playout");
	CHECK(rtp_receiver_play(&receiver, output, received, 6, &report) &&
	      memcmp(received, (bool[]){ false, true, true, false, true, false }, sizeof(received)) == 0 &&
	      memcmp(output, (uint8_t[]){ 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff }, sizeof(output)) == 0 &&
	      report.prefill_samples == 1 && report.received_samples == 3 && report.concealed_samples == 2,
	      "received zeros, prefill and holes lost their identity across timestamp wrap");
	return true;
}

static bool test_reordering_and_window_bounds(void)
{
	static const uint8_t first[] = { 1, 2 };
	static const uint8_t middle[] = { 3, 4 };
	static const uint8_t last[] = { 5, 6 };
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	struct rtp_packet_view packet;
	uint8_t output[6];

	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	packet = packet_view(1, 10, 100, first, ARRAY_LENGTH(first));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "first packet failed");
	packet = packet_view(1, 12, 104, last, ARRAY_LENGTH(last));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "future packet failed");
	packet = packet_view(1, 11, 102, middle, ARRAY_LENGTH(middle));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "reordered packet failed");
	packet = packet_view(1, 12, 104, last, ARRAY_LENGTH(last));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED &&
		      receiver.stats.duplicate_samples == 2,
	      "duplicate samples were not retained safely");
	CHECK(receiver.stats.sequence_gaps == 1 &&
		      receiver.stats.sequence_out_of_order == 1 &&
		      receiver.stats.sequence_duplicates == 1,
	      "sequence continuity statistics are wrong");
	CHECK(rtp_receiver_play(&receiver, output, NULL, sizeof(output),
				    NULL) &&
		      memcmp(output, (uint8_t[]){ 1, 2, 3, 4, 5, 6 },
			     sizeof(output)) == 0,
	      "timestamp ordering failed");

	packet = packet_view(1, 9, 104, first, ARRAY_LENGTH(first));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_LATE,
	      "late packet was accepted");
	packet = packet_view(1, 13, 114, first, ARRAY_LENGTH(first));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_TOO_EARLY,
	      "packet beyond the bounded window was accepted");
	return true;
}

static bool test_ssrc_restart_and_wrap(void)
{
	static const uint8_t old_payload[] = { 1, 2 };
	static const uint8_t new_payload[] = { 9, 8, 7, 6 };
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	struct rtp_packet_view packet;
	struct rtp_playout_report report;
	uint8_t output[4];

	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	packet = packet_view(1, 1, 100, old_payload, ARRAY_LENGTH(old_payload));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "old stream failed");
	packet = packet_view(2, UINT16_MAX, UINT32_MAX - 1, new_payload,
			     ARRAY_LENGTH(new_payload));
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "replacement stream failed");
	packet = packet_view(2, 0, 2, NULL, 0);
	CHECK(rtp_receiver_push(&receiver, &packet) ==
		      RTP_RECEIVE_ACCEPTED,
	      "wrapped sequence number failed");
	CHECK(rtp_receiver_play(&receiver, output, NULL, sizeof(output),
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
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	struct rtp_receiver_config config = {
		.payload_type = 128,
	};
	struct rtp_playout_report report;
	struct rtp_header header = {
		.payload_type = TEST_PAYLOAD_TYPE,
	};
	uint8_t packet[16];
	uint8_t output[2] = { 0, 0 };
	size_t packet_size;
	enum rtp_result packet_result;

	CHECK(!rtp_receiver_init(&receiver, &config, slots,
					     TEST_SLOT_COUNT),
	      "receiver accepted an invalid payload type");
	CHECK(init_receiver(&receiver, slots, 0), "receiver init failed");
	CHECK(rtp_receiver_play(&receiver, output, NULL, sizeof(output),
				    &report),
	      "idle playout failed");
	CHECK(!report.stream_active && output[0] == TEST_SILENCE &&
		      output[1] == TEST_SILENCE &&
		      receiver.stats.samples_idle == 2,
	      "idle playout was not identified");
	CHECK(rtp_packet_write(packet, sizeof(packet), &header,
					   output, sizeof(output),
					   &packet_size) == RTP_OK,
	      "failed to build receiver input");
	CHECK(rtp_receiver_receive(&receiver, packet, packet_size,
				       &packet_result) ==
		      RTP_RECEIVE_ACCEPTED &&
		      packet_result == RTP_OK,
	      "receiver did not accept a serialized packet");
	packet[1] = 8;
	CHECK(rtp_receiver_receive(&receiver, packet, packet_size,
				       &packet_result) ==
		      RTP_RECEIVE_UNEXPECTED_PAYLOAD &&
		      packet_result == RTP_OK &&
		      receiver.stats.unexpected_payloads == 1,
	      "receiver did not filter an unexpected payload type");
	CHECK(rtp_receiver_receive(&receiver, packet, 4,
				       &packet_result) ==
		      RTP_RECEIVE_MALFORMED_PACKET &&
		      packet_result == RTP_PACKET_TOO_SHORT &&
		      receiver.stats.malformed_packets == 1,
	      "receiver did not report malformed wire input");
	CHECK(!rtp_receiver_play(&receiver, NULL, NULL, 2, &report),
	      "playout accepted a null output");
	rtp_receiver_reset(&receiver);
	CHECK(!receiver.stream_active && receiver.stats.samples_idle == 0,
	      "receiver reset left call state behind");
	return true;
}

static bool test_remote_clock_tracks_timestamp_extent(void)
{
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	const uint8_t payload[] = { 1, 2, 3, 4 };
	struct rtp_packet_view packet;

	CHECK(init_receiver(&receiver, slots, 0), "clock receiver init failed");
	packet = packet_view(1, 1, UINT32_MAX - 1, payload, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 3) == 0 &&
		      rtp_receiver_take_clock_frames(&receiver, 3) == 0,
	      "a partial clock frame was emitted or forgotten");
	packet = packet_view(1, 3, 0, payload, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 3) == 1,
	      "clock gap, timestamp wrap, or retained partial frame failed");
	packet = packet_view(1, 2, UINT32_MAX, payload, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 1) == 0 &&
		      rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 1) == 0,
	      "reordering or duplication advanced the remote clock twice");
	packet = packet_view(1, 4, 6, payload, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_TOO_EARLY &&
		      rtp_receiver_take_clock_frames(&receiver, 1) == 0,
	      "out-of-window timestamp supplied clock credit");
	packet = packet_view(1, 4, 5, payload, sizeof(payload));
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 2) == 2 &&
		      receiver.clock_pending_samples == 1,
	      "partially buffered packet clocked its rejected tail");
	rtp_receiver_restart(&receiver);
	CHECK(rtp_receiver_take_clock_frames(&receiver, 1) == 0,
	      "stream restart retained partial clock credit");
	packet = packet_view(1, 1, 100, payload, 1);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 2) == 0,
	      "new stream did not retain its partial frame");
	packet = packet_view(2, 1, 200, payload, 2);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 2) == 1 &&
		      receiver.clock_pending_samples == 0,
	      "SSRC restart combined old and new partial clock frames");
	packet = packet_view(2, 2, 204, NULL, 0);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, 1) == 0 &&
		      rtp_receiver_take_clock_frames(NULL, 1) == 0 &&
		      rtp_receiver_take_clock_frames(&receiver, 0) == 0,
	      "empty packet or invalid clock request supplied credit");
	return true;
}

static bool test_local_playout_consumes_clock_time(void)
{
	const uint32_t starts[] = { 100, UINT32_MAX - 5u };
	const uint8_t payload[] = { 1, 2, 3, 4 };
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	uint8_t output[sizeof(payload)];

	for (size_t index = 0; index < ARRAY_LENGTH(starts); ++index) {
		struct rtp_packet_view packet = packet_view(1, 1, starts[index], payload, 2);
		CHECK(init_receiver(&receiver, slots, 0) &&
		      rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, sizeof(output)) == 0,
		      "initial partial clock failed");
		for (unsigned int frame = 0; frame < 4; ++frame)
			CHECK(rtp_receiver_play(&receiver, output, NULL, sizeof(output), NULL),
			      "local playout across quiet media failed");
		CHECK(rtp_receiver_take_clock_frames(&receiver, 1) == 0,
		      "already-played partial audio retained clock credit");
		/* Sequence remains consecutive through silence suppression, including
		 * a timestamp wrap. Only the resumed packet is new clock time. */
		packet = packet_view(1, 2, starts[index] + 4 * sizeof(output),
			payload, sizeof(payload));
		CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
		      rtp_receiver_take_clock_frames(&receiver, sizeof(output)) == 1 &&
		      rtp_receiver_take_clock_frames(&receiver, sizeof(output)) == 0 &&
		      rtp_receiver_play(&receiver, output, NULL, sizeof(output), NULL) &&
		      memcmp(output, payload, sizeof(output)) == 0,
		      "resumed media replayed silence as remote credit or lost its payload");
	}
	return true;
}

static bool test_remote_clock_preserves_time_through_repeated_loss(void)
{
	enum { FRAME_SAMPLES = 80, FRAME_COUNT = 240, WINDOW_SAMPLES = 640 };
	const struct rtp_receiver_config config = {
		.payload_type = TEST_PAYLOAD_TYPE,
		.silence_octet = TEST_SILENCE,
	};
	struct rtp_jitter_slot slots[WINDOW_SAMPLES];
	struct rtp_receiver receiver;
	uint8_t payload[FRAME_SAMPLES];
	uint8_t played[FRAME_SAMPLES];
	size_t clocked = 0;

	memset(payload, 0x80, sizeof(payload));
	CHECK(rtp_receiver_init(&receiver, &config, slots, ARRAY_LENGTH(slots)),
	      "loss clock receiver init failed");
	for (unsigned int frame = 0; frame < FRAME_COUNT; ++frame) {
		struct rtp_packet_view packet = packet_view(
			1, (uint16_t)frame, frame * FRAME_SAMPLES,
			payload, sizeof(payload));
		size_t due;

		if (frame % 12 == 5)
			continue;
		CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED,
		      "post-loss packet %u was rejected", frame);
		due = rtp_receiver_take_clock_frames(&receiver, FRAME_SAMPLES);
		CHECK(due == (frame % 12 == 6 ? 2u : 1u),
		      "packet %u lost or duplicated a clock interval", frame);
		clocked += due;
		for (size_t index = 0; index < due; ++index)
			CHECK(rtp_receiver_play(&receiver, played, NULL, sizeof(played), NULL),
			      "post-loss playout failed");
		CHECK(memcmp(played, payload, sizeof(payload)) == 0,
		      "packet %u remained delayed behind its missing predecessor", frame);
	}
	CHECK(clocked == FRAME_COUNT && receiver.stats.samples_concealed == 1600 &&
		      receiver.play_timestamp == FRAME_COUNT * FRAME_SAMPLES,
	      "twenty missing packets changed elapsed RTP time");
	/* Unconsumed clock credit may not grow beyond the owned receive window. */
	for (unsigned int frame = FRAME_COUNT; frame < FRAME_COUNT + 20; ++frame) {
		struct rtp_packet_view packet = packet_view(
			1, (uint16_t)frame, frame * FRAME_SAMPLES,
			payload, sizeof(payload));

		CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
			      rtp_receiver_play(&receiver, played, NULL, sizeof(played), NULL),
		      "unused clock credit test failed");
	}
	CHECK(rtp_receiver_take_clock_frames(&receiver, FRAME_SAMPLES) ==
		      WINDOW_SAMPLES / FRAME_SAMPLES,
	      "unused remote clock credit exceeded the receive window");
	return true;
}

static bool test_playout_gap_cursor(void)
{
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	const uint8_t payload[] = { 1, 2 };
	uint8_t output[2];
	struct rtp_packet_view packet = packet_view(7, 1, UINT32_MAX - 1u,
		payload, sizeof(payload));
	struct rtp_playout_status status;

	CHECK(init_receiver(&receiver, slots, 0) &&
	      !rtp_receiver_playout_status(&receiver, 2).stream_active,
	      "idle receiver invented a playout gap");
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED,
	      "initial gap-cursor packet failed");
	packet = packet_view(7, 3, 2, payload, sizeof(payload));
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED,
	      "future gap-cursor packet failed");
	struct rtp_receiver saved = receiver;
	status = rtp_receiver_playout_status(&receiver, 2);
	CHECK(status.stream_active && !status.gap && status.ssrc == 7 &&
	      status.timestamp == UINT32_MAX - 1u &&
	      rtp_receiver_states_equal(&receiver, &saved) &&
	      rtp_receiver_play(&receiver, output, NULL, sizeof(output), NULL),
	      "contiguous query changed the receiver or held received audio");
	status = rtp_receiver_playout_status(&receiver, 2);
	CHECK(status.gap && status.timestamp == 0 && status.ssrc == 7,
	      "timestamp wrap hid a real gap");
	packet = packet_view(7, 2, 0, payload, sizeof(payload));
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
	      !rtp_receiver_playout_status(&receiver, 2).gap &&
	      rtp_receiver_play(&receiver, output, NULL, sizeof(output), NULL) &&
	      rtp_receiver_play(&receiver, output, NULL, sizeof(output), NULL) &&
	      !rtp_receiver_playout_status(&receiver, 2).gap,
	      "filled gap or time after the received tail remained blocked");
	packet = packet_view(8, 1, 100, payload, sizeof(payload));
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
	      rtp_receiver_playout_status(&receiver, 2).ssrc == 8 &&
	      rtp_receiver_playout_status(&receiver, 2).timestamp == 100 &&
	      !rtp_receiver_playout_status(&receiver, TEST_SLOT_COUNT + 1).stream_active &&
	      !rtp_receiver_playout_status(NULL, 2).stream_active,
	      "stream restart or query bounds lost cursor identity");
	return true;
}

static bool test_partial_clock_padding_preserves_provenance(void)
{
	struct rtp_jitter_slot slots[TEST_SLOT_COUNT];
	struct rtp_receiver receiver;
	const uint8_t payload[] = { 1, 2, 3, 4 };
	uint8_t output[4];
	bool mask[4];
	struct rtp_packet_view packet = packet_view(1, 1, UINT32_MAX - 1u, payload, 2);

	CHECK(init_receiver(&receiver, slots, 0) &&
	      rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_ACCEPTED &&
	      rtp_receiver_take_clock_frames(&receiver, 4) == 0 &&
	      rtp_receiver_playout_status(&receiver, 4).partial_samples == 2 &&
	      rtp_receiver_pad_clock_frame(&receiver, 4) &&
	      !rtp_receiver_playout_status(&receiver, 4).gap &&
	      rtp_receiver_play(&receiver, output, mask, sizeof(output), NULL) &&
	      memcmp(mask, (bool[]){ true, true, false, false }, sizeof(mask)) == 0 &&
	      memcmp(output, (uint8_t[]){ 1, 2, TEST_SILENCE, TEST_SILENCE }, sizeof(output)) == 0 &&
	      receiver.play_timestamp == 2 && rtp_receiver_take_clock_frames(&receiver, 4) == 0,
	      "padding changed received provenance or doubled credit across timestamp wrap");
	packet = packet_view(1, 2, 0, payload, 2);
	CHECK(rtp_receiver_push(&receiver, &packet) == RTP_RECEIVE_LATE &&
	      rtp_receiver_take_clock_frames(&receiver, 4) == 0,
	      "late padded tail supplied new clock credit");
	struct rtp_receiver saved = receiver;
	CHECK(!rtp_receiver_pad_clock_frame(&receiver, 4) &&
	      rtp_receiver_states_equal(&receiver, &saved),
	      "padding an empty clock mutated receiver state");
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
		{ "received sample provenance", test_received_sample_provenance },
		{ "reordering and window bounds",
		  test_reordering_and_window_bounds },
		{ "SSRC restart and timestamp wrap",
		  test_ssrc_restart_and_wrap },
		{ "validation and idle", test_validation_and_idle },
		{ "remote timestamp clock",
		  test_remote_clock_tracks_timestamp_extent },
		{ "local playout clock handoff", test_local_playout_consumes_clock_time },
		{ "remote clock repeated loss",
		  test_remote_clock_preserves_time_through_repeated_loss },
		{ "playout gap cursor", test_playout_gap_cursor },
		{ "partial clock padding", test_partial_clock_padding_preserves_provenance },
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
