// SPDX-License-Identifier: BSD-3-Clause

#include <v90_cp_receiver.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters parameters_for(
	uint8_t count, bool codec_constellations_differ)
{
	struct v90_cp_parameters parameters = {
		.kind = count % 2u != 0 ? V90_CP_TRAINING : V90_CP_DATA,
		.law = count % 2u != 0 ? G711_LAW_PCMU : G711_LAW_PCMA,
		.upstream_rate_mask = 0x1555,
		.trn1d_ratio_q3_13 = 0x2345,
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.selected_rate_index = 17,
		.sign_redundancy = 2,
		.spectral_lookahead = 3,
		.constellation_count = count,
		.request_silence = true,
		.acknowledges_peer = true,
		.codec_constellations_differ =
			codec_constellations_differ,
	};

	for (size_t index = 0; index < count; ++index) {
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(index * 19u));
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(127u - index));
		if (codec_constellations_differ) {
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(index * 7u));
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(126u - index));
		}
	}
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return parameters;
}

static uint8_t packed_bit(
	const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_bit(
	uint8_t *frame, size_t bit_index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (bit != 0)
		frame[bit_index / 8u] |= mask;
	else
		frame[bit_index / 8u] &= (uint8_t)~mask;
}

static void replace_lsb_first(
	uint8_t *frame, size_t first_bit, size_t bit_count,
	uint8_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		replace_bit(
			frame, first_bit + index,
			(uint8_t)(value >> index) & 1u);
}

static bool encode_frame(
	const struct v90_cp_parameters *parameters,
	uint8_t *frame, size_t *frame_bits)
{
	memset(frame, 0, V90_CP_MAX_FRAME_BYTES);
	CHECK(v90_cp_encode(
		      parameters, frame, V90_CP_MAX_FRAME_BYTES,
		      frame_bits) == V90_CP_OK,
	      "CP receiver fixture did not encode");
	return true;
}

static bool feed_without_event(
	struct v90_cp_receiver *receiver,
	const uint8_t *frame, size_t first_bit, size_t end_bit)
{
	for (size_t index = first_bit; index < end_bit; ++index) {
		struct v90_cp_receive_event event;

		memset(&event, 0xa5, sizeof(event));
		CHECK(v90_cp_receiver_receive_bit(
			      receiver, packed_bit(frame, index), &event) ==
			      V90_CP_OK &&
		      event.type == V90_CP_RECEIVE_NO_EVENT &&
		      event.frame_result == V90_CP_OK &&
		      event.observed_bits == 0,
		      "CP receiver emitted at input bit %zu", index);
	}
	return true;
}

static bool feed_frame(
	struct v90_cp_receiver *receiver,
	const uint8_t *frame, size_t first_bit, size_t frame_bits,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_receive_event emitted;

	CHECK(frame_bits > first_bit &&
	      feed_without_event(
		      receiver, frame, first_bit, frame_bits - 1u),
	      "CP receiver frame prefix failed");
	memset(&emitted, 0xa5, sizeof(emitted));
	CHECK(v90_cp_receiver_receive_bit(
		      receiver, packed_bit(frame, frame_bits - 1u),
		      &emitted) == V90_CP_OK &&
	      emitted.type != V90_CP_RECEIVE_NO_EVENT,
	      "CP receiver did not finish at bit %zu", frame_bits - 1u);
	*event = emitted;
	return true;
}

static bool event_reencodes_frame(
	const struct v90_cp_receive_event *event,
	const uint8_t *expected, size_t expected_bits)
{
	uint8_t encoded[V90_CP_MAX_FRAME_BYTES];
	size_t encoded_bits = 0;

	CHECK(event->type == V90_CP_RECEIVE_FRAME &&
	      event->frame_result == V90_CP_OK &&
	      event->observed_bits == expected_bits &&
	      v90_cp_encode(
		      &event->parameters, encoded, sizeof(encoded),
		      &encoded_bits) == V90_CP_OK &&
	      encoded_bits == expected_bits &&
	      memcmp(
		      encoded, expected,
		      (expected_bits + 7u) / 8u) == 0,
	      "received CP parameters changed their wire frame");
	return true;
}

static bool test_variable_frames_and_noise(void)
{
	struct v90_cp_receiver receiver;
	static const uint8_t noise[] = { 0x00, 0xff, 0xff, 0x00 };

	CHECK(v90_cp_receiver_init(&receiver) == V90_CP_OK,
	      "CP receiver did not initialize");
	/* Include isolated zeroes, only sixteen ones, and nine extra ones before
	 * the first real prefix. The saturated prefix search must remain exact. */
	CHECK(feed_without_event(&receiver, noise, 0, 32),
	      "CP receiver rejected leading noise");
	for (size_t index = 0; index < 9; ++index) {
		struct v90_cp_receive_event event;

		CHECK(v90_cp_receiver_receive_bit(
			      &receiver, 1, &event) == V90_CP_OK &&
		      event.type == V90_CP_RECEIVE_NO_EVENT,
		      "CP receiver emitted during a long prefix");
	}

	for (uint8_t count = 1;
	     count <= V90_CP_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int codec_copy = 0; codec_copy < 2;
		     ++codec_copy) {
			struct v90_cp_parameters parameters =
				parameters_for(count, codec_copy != 0);
			struct v90_cp_receive_event event;
			uint8_t frame[V90_CP_MAX_FRAME_BYTES];
			size_t frame_bits;

			CHECK(encode_frame(&parameters, frame, &frame_bits) &&
			      feed_frame(
				      &receiver, frame, 0, frame_bits,
				      &event) &&
			      event_reencodes_frame(
				      &event, frame, frame_bits),
			      "CP receiver changed count %u codec-copy %u",
			      count, codec_copy);
		}
	}
	CHECK(v90_cp_receiver_is_valid(&receiver) &&
	      receiver.stage == V90_CP_RECEIVER_SEARCH_SYNC,
	      "CP receiver did not return to search");
	return true;
}

static bool test_crc_rejection_and_recovery(void)
{
	struct v90_cp_parameters parameters = parameters_for(1, false);
	struct v90_cp_receiver receiver;
	struct v90_cp_receive_event event;
	uint8_t valid[V90_CP_MAX_FRAME_BYTES];
	uint8_t corrupt[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits;

	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v90_cp_receiver_init(&receiver) == V90_CP_OK,
	      "CP rejection fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 20, packed_bit(corrupt, 20) ^ 1u);
	CHECK(feed_frame(
		      &receiver, corrupt, 0, frame_bits, &event) &&
	      event.type == V90_CP_RECEIVE_REJECTED &&
	      event.frame_result == V90_CP_CRC_MISMATCH &&
	      event.observed_bits == frame_bits &&
	      feed_frame(&receiver, valid, 0, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CP receiver did not reject and reacquire after CRC failure");
	return true;
}

static bool test_overlapping_prefix_recovery(void)
{
	struct v90_cp_parameters parameters = parameters_for(1, false);
	struct v90_cp_receiver receiver;
	struct v90_cp_receive_event event;
	uint8_t valid[V90_CP_MAX_FRAME_BYTES];
	uint8_t corrupt[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits;

	for (uint8_t ucode = 0; ucode < 16; ++ucode)
		add_ucode(&parameters.transmit_constellations[0], ucode);
	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v90_cp_receiver_init(&receiver) == V90_CP_OK,
	      "CP overlap fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	/* Mask bits 137:152 are ones. Turning their following start bit into
	 * one makes an overlapping 17-one prefix ending at bit 153. */
	replace_bit(corrupt, 153, 1);
	CHECK(feed_without_event(&receiver, corrupt, 0, 153),
	      "CP overlap candidate emitted too early");
	CHECK(v90_cp_receiver_receive_bit(
		      &receiver, 1, &event) == V90_CP_OK &&
	      event.type == V90_CP_RECEIVE_REJECTED &&
	      event.frame_result == V90_CP_INVALID_FRAME &&
	      event.observed_bits == 154 &&
	      receiver.stage == V90_CP_RECEIVER_SEARCH_SYNC &&
	      receiver.sync_one_count == 17,
	      "CP receiver lost an overlapping prefix");
	/* The retained ones replace bits 0:16 of the next valid frame. */
	CHECK(feed_frame(
		      &receiver, valid, 17, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CP receiver did not recover through an overlapping prefix");
	return true;
}

static bool test_header_rejection(void)
{
	struct v90_cp_parameters parameters = parameters_for(1, false);
	struct v90_cp_receiver receiver;
	struct v90_cp_receive_event event;
	uint8_t valid[V90_CP_MAX_FRAME_BYTES];
	uint8_t corrupt[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits;

	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v90_cp_receiver_init(&receiver) == V90_CP_OK,
	      "CP header-rejection fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_lsb_first(corrupt, 103, 4, V90_CP_MAX_CONSTELLATIONS);
	CHECK(feed_without_event(&receiver, corrupt, 0, 128),
	      "CP invalid map emitted before its header completed");
	CHECK(v90_cp_receiver_receive_bit(
		      &receiver, packed_bit(corrupt, 128), &event) ==
		      V90_CP_OK &&
	      event.type == V90_CP_RECEIVE_REJECTED &&
	      event.frame_result == V90_CP_INVALID_FRAME &&
	      event.observed_bits == 129 &&
	      feed_frame(&receiver, valid, 0, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CP receiver did not reject an impossible variable length");
	return true;
}

static bool test_api_transactionality(void)
{
	struct v90_cp_receiver receiver;
	struct v90_cp_receiver saved_receiver;
	struct v90_cp_receiver changed;
	struct v90_cp_receive_event event;
	struct v90_cp_receive_event saved_event;

	CHECK(v90_cp_receiver_init(NULL) == V90_CP_INVALID_ARGUMENT &&
	      v90_cp_receiver_init(&receiver) == V90_CP_OK &&
	      v90_cp_receiver_is_valid(&receiver) &&
	      !v90_cp_receiver_is_valid(NULL),
	      "CP receiver API did not initialize transactionally");
	saved_receiver = receiver;
	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	CHECK(v90_cp_receiver_receive_bit(NULL, 0, &event) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_receiver_receive_bit(&receiver, 2, &event) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_receiver_receive_bit(&receiver, 0, NULL) ==
		      V90_CP_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "invalid CP receiver call changed state or event");

	changed = receiver;
	changed.stage = (enum v90_cp_receive_stage)99;
	CHECK(!v90_cp_receiver_is_valid(&changed),
	      "CP receiver accepted an unknown stage");
	changed = receiver;
	changed.sync_one_count = 18;
	CHECK(!v90_cp_receiver_is_valid(&changed),
	      "CP receiver accepted an excessive prefix");
	changed = receiver;
	changed.bit_index = 1;
	CHECK(!v90_cp_receiver_is_valid(&changed),
	      "CP receiver accepted a search cursor");
	changed = receiver;
	changed.frame[0] = 1;
	CHECK(!v90_cp_receiver_is_valid(&changed),
	      "CP receiver accepted stale frame storage");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "variable frames and noise", test_variable_frames_and_noise },
		{ "CRC rejection and recovery", test_crc_rejection_and_recovery },
		{ "overlapping prefix recovery", test_overlapping_prefix_recovery },
		{ "header rejection", test_header_rejection },
		{ "API transactionality", test_api_transactionality },
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
