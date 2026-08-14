// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpt_receiver.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters parameters_for(
	uint8_t count, bool codec_constellations_differ)
{
	struct v92_cpt_parameters parameters = {
		.law = count % 2u != 0u ? G711_LAW_PCMU : G711_LAW_PCMA,
		.trn1d_ratio_q3_13 = UINT16_C(0x2345),
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.selected_rate_index = 17,
		.sign_redundancy = 2,
		.spectral_lookahead = 3,
		.constellation_count = count,
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
	     interval < V92_CPT_FRAME_INTERVALS; ++interval)
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return parameters;
}

static uint8_t packed_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_bit(
	uint8_t *frame, size_t bit_index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (bit != 0u)
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
	const struct v92_cpt_parameters *parameters,
	uint8_t *frame, size_t *frame_bits)
{
	memset(frame, 0, V92_CPT_MAX_FRAME_BYTES);
	CHECK(v92_cpt_encode(
		      parameters, frame, V92_CPT_MAX_FRAME_BYTES,
		      frame_bits) == V92_CPT_OK,
	      "CPt receiver fixture did not encode");
	return true;
}

static bool feed_without_event(
	struct v92_cpt_receiver *receiver,
	const uint8_t *frame, size_t first_bit, size_t end_bit)
{
	for (size_t index = first_bit; index < end_bit; ++index) {
		struct v92_cpt_receive_event event;

		memset(&event, 0xa5, sizeof(event));
		CHECK(v92_cpt_receiver_receive_bit(
			      receiver, packed_bit(frame, index), &event) ==
			      V92_CPT_OK &&
		      event.type == V92_CPT_RECEIVE_NO_EVENT &&
		      event.frame_result == V92_CPT_OK &&
		      event.observed_bits == 0u,
		      "CPt receiver emitted at input bit %zu", index);
	}
	return true;
}

static bool feed_frame(
	struct v92_cpt_receiver *receiver,
	const uint8_t *frame, size_t first_bit, size_t frame_bits,
	struct v92_cpt_receive_event *event)
{
	struct v92_cpt_receive_event emitted;

	CHECK(frame_bits > first_bit &&
	      feed_without_event(
		      receiver, frame, first_bit, frame_bits - 1u),
	      "CPt receiver frame prefix failed");
	memset(&emitted, 0xa5, sizeof(emitted));
	CHECK(v92_cpt_receiver_receive_bit(
		      receiver, packed_bit(frame, frame_bits - 1u),
		      &emitted) == V92_CPT_OK &&
	      emitted.type != V92_CPT_RECEIVE_NO_EVENT,
	      "CPt receiver did not finish at bit %zu", frame_bits - 1u);
	*event = emitted;
	return true;
}

static bool event_reencodes_frame(
	const struct v92_cpt_receive_event *event,
	const uint8_t *expected, size_t expected_bits)
{
	uint8_t encoded[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t encoded_bits = 0u;

	CHECK(event->type == V92_CPT_RECEIVE_FRAME &&
	      event->frame_result == V92_CPT_OK &&
	      event->observed_bits == expected_bits &&
	      v92_cpt_encode(
		      &event->parameters, encoded, sizeof(encoded),
		      &encoded_bits) == V92_CPT_OK &&
	      encoded_bits == expected_bits &&
	      memcmp(
		      encoded, expected,
		      (expected_bits + 7u) / 8u) == 0,
	      "received CPt parameters changed their wire frame");
	return true;
}

static bool test_variable_frames_and_handoff(void)
{
	struct v92_cpt_receiver receiver;
	static const uint8_t noise[] = { 0x00, 0xff, 0xfe, 0x00 };

	CHECK(v92_cpt_receiver_init(&receiver) == V92_CPT_OK &&
	      feed_without_event(&receiver, noise, 0u, 32u),
	      "CPt receiver rejected leading noise");
	for (uint8_t count = 1u;
	     count <= V92_CPT_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int codec_copy = 0u;
		     codec_copy < 2u; ++codec_copy) {
			struct v92_cpt_parameters parameters = parameters_for(
				count, codec_copy != 0u);
			struct v92_cpt_receive_event event;
			uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
			size_t frame_bits;
			size_t first_bit = 0u;

			CHECK(encode_frame(&parameters, frame, &frame_bits),
			      "CPt frame generation failed");
			if (count == 1u && codec_copy == 0u) {
				CHECK(v92_cpt_receiver_begin_after_sync(
					      &receiver) == V92_CPT_OK,
				      "CPt handoff did not seed frame sync");
				first_bit = 18u;
			}
			CHECK(feed_frame(
				      &receiver, frame, first_bit,
				      frame_bits, &event) &&
			      event_reencodes_frame(
				      &event, frame, frame_bits),
			      "CPt receiver changed count %u codec-copy %u",
			      count, codec_copy);
		}
	}
	CHECK(v92_cpt_receiver_is_valid(&receiver) &&
	      receiver.stage == V92_CPT_RECEIVER_SEARCH_SYNC,
	      "CPt receiver did not return to search");
	return true;
}

static bool test_crc_rejection_and_recovery(void)
{
	struct v92_cpt_parameters parameters = parameters_for(1u, false);
	struct v92_cpt_receiver receiver;
	struct v92_cpt_receive_event event;
	uint8_t valid[V92_CPT_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bits;

	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v92_cpt_receiver_init(&receiver) == V92_CPT_OK,
	      "CPt rejection fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 21u, packed_bit(corrupt, 21u) ^ 1u);
	CHECK(feed_frame(
		      &receiver, corrupt, 0u, frame_bits, &event) &&
	      event.type == V92_CPT_RECEIVE_REJECTED &&
	      event.frame_result == V92_CPT_CRC_MISMATCH &&
	      event.observed_bits == frame_bits &&
	      feed_frame(&receiver, valid, 0u, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CPt receiver did not reject and recover after CRC failure");
	return true;
}

static bool test_header_and_overlap_recovery(void)
{
	struct v92_cpt_parameters parameters = parameters_for(1u, false);
	struct v92_cpt_receiver receiver;
	struct v92_cpt_receive_event event;
	uint8_t valid[V92_CPT_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bits;

	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v92_cpt_receiver_init(&receiver) == V92_CPT_OK,
	      "CPt header fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_lsb_first(corrupt, 103u, 4u, 6u);
	CHECK(feed_without_event(&receiver, corrupt, 0u, 128u) &&
	      v92_cpt_receiver_receive_bit(
		      &receiver, packed_bit(corrupt, 128u), &event) ==
		      V92_CPT_OK &&
	      event.type == V92_CPT_RECEIVE_REJECTED &&
	      event.frame_result == V92_CPT_INVALID_FRAME &&
	      event.observed_bits == 129u &&
	      feed_frame(&receiver, valid, 0u, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CPt receiver did not recover after an invalid map");

	for (uint8_t ucode = 0u; ucode < 16u; ++ucode)
		add_ucode(&parameters.transmit_constellations[0], ucode);
	CHECK(encode_frame(&parameters, valid, &frame_bits) &&
	      v92_cpt_receiver_init(&receiver) == V92_CPT_OK,
	      "CPt overlap fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 153u, 1u);
	CHECK(feed_without_event(&receiver, corrupt, 0u, 153u) &&
	      v92_cpt_receiver_receive_bit(&receiver, 1u, &event) ==
		      V92_CPT_OK &&
	      event.type == V92_CPT_RECEIVE_REJECTED &&
	      event.observed_bits == 154u &&
	      receiver.sync_one_count == 17u &&
	      feed_frame(&receiver, valid, 17u, frame_bits, &event) &&
	      event_reencodes_frame(&event, valid, frame_bits),
	      "CPt receiver lost an overlapping frame sync");
	return true;
}

static bool test_contracts(void)
{
	struct v92_cpt_receiver receiver;
	struct v92_cpt_receiver before;
	struct v92_cpt_receive_event event;
	struct v92_cpt_receive_event event_before;

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_cpt_receiver_init(NULL) == V92_CPT_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      v92_cpt_receiver_init(&receiver) == V92_CPT_OK &&
	      v92_cpt_receiver_is_valid(&receiver) &&
	      !v92_cpt_receiver_is_valid(NULL),
	      "CPt receiver initialization contract differed");
	memset(&event, 0xa5, sizeof(event));
	event_before = event;
	before = receiver;
	CHECK(v92_cpt_receiver_receive_bit(NULL, 0u, &event) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      v92_cpt_receiver_receive_bit(&receiver, 2u, &event) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      v92_cpt_receiver_receive_bit(&receiver, 0u, NULL) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid CPt receive changed state or event");
	CHECK(v92_cpt_receiver_begin_after_sync(&receiver) == V92_CPT_OK,
	      "CPt sync handoff contract did not begin");
	before = receiver;
	CHECK(v92_cpt_receiver_begin_after_sync(&receiver) ==
		      V92_CPT_INVALID_FRAME &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "duplicate CPt sync handoff changed state");
	receiver.stage = (enum v92_cpt_receive_stage)-1;
	before = receiver;
	CHECK(!v92_cpt_receiver_is_valid(&receiver) &&
	      v92_cpt_receiver_receive_bit(&receiver, 0u, &event) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      v92_cpt_receiver_begin_after_sync(&receiver) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "corrupt CPt receiver state was not transactional");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "variable frames and handoff",
		  test_variable_frames_and_handoff },
		{ "CRC rejection and recovery",
		  test_crc_rejection_and_recovery },
		{ "header and overlap recovery",
		  test_header_and_overlap_recovery },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
