// SPDX-License-Identifier: BSD-3-Clause

#include <v90_mp_receiver.h>

#include "test.h"

#include <string.h>

static struct v90_mp_record type0_record(bool acknowledged)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.acknowledges_cp = acknowledged,
			.upstream_rate_capability_mask = 0x1fff,
		},
	};
}

static struct v90_mp_record type1_record(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = 14,
				.upstream_trellis = V34_TRELLIS_64_STATE,
				.acknowledges_cp = true,
				.upstream_rate_capability_mask = 0x1555,
			},
			.precoder_coefficients = {
				{ .real = 1, .imaginary = -1 },
				{ .real = 8192, .imaginary = -8192 },
				{ .real = INT16_MAX, .imaginary = -32767 },
			},
		},
	};
}

static uint8_t packed_bit(const uint8_t *sequence, size_t bit_index)
{
	return (uint8_t)((sequence[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void flip_bit(uint8_t *sequence, size_t bit_index)
{
	sequence[bit_index / 8u] ^=
		(uint8_t)(1u << (7u - bit_index % 8u));
}

static bool records_match(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right, uint8_t frame_bits)
{
	uint8_t left_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t right_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t left_bits;
	size_t right_bits;

	return v90_mp_encode(
		       left, frame_bits, left_sequence, sizeof(left_sequence),
		       &left_bits) == V90_MP_OK &&
	       v90_mp_encode(
		       right, frame_bits, right_sequence,
		       sizeof(right_sequence), &right_bits) == V90_MP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_sequence, right_sequence,
		      (left_bits + 7u) / 8u) == 0;
}

static bool encode_record(
	const struct v90_mp_record *record, uint8_t frame_bits,
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES], size_t *bit_count)
{
	CHECK(v90_mp_encode(
		      record, frame_bits, sequence,
		      V90_MP_MAXIMUM_SEQUENCE_BYTES, bit_count) == V90_MP_OK,
	      "MP receiver fixture did not encode");
	return true;
}

static bool feed_bits(
	struct v90_mp_receiver *receiver, const uint8_t *sequence,
	size_t first_bit, size_t bit_count, struct v90_mp_event *event,
	bool expect_event)
{
	struct v90_mp_event emitted = { 0 };

	for (size_t offset = 0; offset < bit_count; ++offset) {
		CHECK(v90_mp_receive_bit(
			      receiver,
			      packed_bit(sequence, first_bit + offset),
			      &emitted) == V90_MP_RECEIVER_OK,
		      "MP receiver stopped at bit %zu/%zu", offset,
		      bit_count);
		if (offset + 1u < bit_count || !expect_event)
			CHECK(emitted.type == V90_MP_EVENT_NONE,
			      "MP receiver emitted early at bit %zu/%zu", offset,
			      bit_count);
	}
	if (expect_event)
		CHECK(emitted.type != V90_MP_EVENT_NONE,
		      "MP receiver omitted a terminal event");
	*event = emitted;
	return true;
}

static bool test_continuous_records(void)
{
	enum { FRAME_BITS = 17 };
	struct v90_mp_record type0 = type0_record(false);
	struct v90_mp_record type1 = type1_record();
	struct v90_mp_receiver receiver;
	struct v90_mp_event event;
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t bit_count;

	CHECK(v90_mp_receiver_init(&receiver, FRAME_BITS) ==
		      V90_MP_RECEIVER_OK,
	      "MP receiver did not initialize");
	for (size_t bit = 0; bit < 75; ++bit) {
		CHECK(v90_mp_receive_bit(&receiver, 1, &event) ==
			      V90_MP_RECEIVER_OK &&
		      event.type == V90_MP_EVENT_NONE,
		      "TRN2d suffix emitted at bit %zu", bit);
	}
	CHECK(receiver.stage == V90_MP_SEARCH_SYNC &&
	      receiver.sync_one_count == V90_MP_FRAME_SYNC_BITS,
	      "TRN2d suffix did not saturate the sync detector");

	CHECK(encode_record(&type0, FRAME_BITS, sequence, &bit_count) &&
	      feed_bits(
		      &receiver, sequence, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_TYPE0 &&
	      event.frame_result == V90_MP_OK &&
	      records_match(&event.record, &type0, FRAME_BITS),
	      "continuous Type 0 MP changed");
	CHECK(encode_record(&type1, FRAME_BITS, sequence, &bit_count) &&
	      feed_bits(
		      &receiver, sequence, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_TYPE1 &&
	      event.frame_result == V90_MP_OK &&
	      records_match(&event.record, &type1, FRAME_BITS) &&
	      v90_mp_receiver_is_valid(&receiver) &&
	      receiver.stage == V90_MP_SEARCH_SYNC,
	      "continuous Type 1 MP changed");
	return true;
}

static bool test_rejection_and_recovery(void)
{
	enum { FRAME_BITS = 17 };
	struct v90_mp_record source = type0_record(true);
	struct v90_mp_receiver receiver;
	struct v90_mp_event event;
	uint8_t valid[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t corrupt[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t bit_count;

	CHECK(v90_mp_receiver_init(&receiver, FRAME_BITS) ==
		      V90_MP_RECEIVER_OK &&
	      encode_record(&source, FRAME_BITS, valid, &bit_count),
	      "MP recovery fixture did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	flip_bit(corrupt, 24);
	CHECK(feed_bits(
		      &receiver, corrupt, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_REJECTED &&
	      event.frame_result == V90_MP_CRC_MISMATCH &&
	      feed_bits(
		      &receiver, valid, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_TYPE0 &&
	      records_match(&event.record, &source, FRAME_BITS),
	      "MP receiver did not reject and recover");

	memcpy(corrupt, valid, sizeof(corrupt));
	flip_bit(corrupt, 34);
	CHECK(feed_bits(
		      &receiver, corrupt, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_REJECTED &&
	      event.frame_result == V90_MP_INVALID_FRAME,
	      "MP receiver accepted a nonzero start bit");
	memcpy(corrupt, valid, sizeof(corrupt));
	flip_bit(corrupt, 85);
	CHECK(feed_bits(
		      &receiver, corrupt, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_REJECTED &&
	      event.frame_result == V90_MP_INVALID_FRAME,
	      "MP receiver accepted a nonzero fill bit");
	return true;
}

static bool test_overlapping_reacquisition(void)
{
	enum { FRAME_BITS = 30 };
	struct v90_mp_record source = type1_record();
	struct v90_mp_receiver receiver;
	struct v90_mp_event event;
	uint8_t valid[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t bit_count;

	CHECK(v90_mp_receiver_init(&receiver, FRAME_BITS) ==
		      V90_MP_RECEIVER_OK &&
	      encode_record(&source, FRAME_BITS, valid, &bit_count),
	      "overlapping MP fixture did not initialize");
	/* Begin a candidate, then put the complete valid prefix inside the
	 * candidate before its nominal end. The newer 17-one run must win. */
	CHECK(feed_bits(
		      &receiver, valid, 0, 40, &event, false) &&
	      receiver.stage == V90_MP_RECEIVE_SEQUENCE &&
	      feed_bits(
		      &receiver, valid, 0, bit_count, &event, true) &&
	      event.type == V90_MP_EVENT_TYPE1 &&
	      records_match(&event.record, &source, FRAME_BITS) &&
	      receiver.stage == V90_MP_SEARCH_SYNC,
	      "MP receiver lost an overlapping newer prefix");
	return true;
}

static bool test_truncation_recovery(void)
{
	const struct v90_mp_record records[] = {
		type0_record(true), type1_record(),
	};

	for (uint8_t width = V90_MP_TRAINING_FRAME_MINIMUM_BITS;
	     width <= V90_MP_TRAINING_FRAME_MAXIMUM_BITS; ++width)
	for (unsigned old = 0u; old < ARRAY_LENGTH(records); ++old)
	for (unsigned next = 0u; next < ARRAY_LENGTH(records); ++next) {
		uint8_t first[V90_MP_MAXIMUM_SEQUENCE_BYTES];
		uint8_t retry[V90_MP_MAXIMUM_SEQUENCE_BYTES];
		size_t first_bits, retry_bits;

		CHECK(encode_record(&records[old], width, first, &first_bits) &&
		      encode_record(&records[next], width, retry, &retry_bits),
		      "truncated MP fixture did not encode");
		for (size_t prefix = 0u; prefix < first_bits; ++prefix) {
			struct v90_mp_receiver receiver;
			struct v90_mp_event event;
			unsigned accepted = 0u;

			CHECK(v90_mp_receiver_init(&receiver, width) == V90_MP_RECEIVER_OK &&
			      feed_bits(&receiver, first, 0u, prefix, &event, false),
			      "truncated MP prefix did not initialize");
			for (size_t bit = 0u; bit < retry_bits; ++bit) {
				CHECK(v90_mp_receive_bit(&receiver,
						packed_bit(retry, bit), &event) ==
					      V90_MP_RECEIVER_OK &&
				      v90_mp_receiver_is_valid(&receiver),
				      "MP retry stopped at bit %zu", bit);
				if (event.type == V90_MP_EVENT_NONE ||
				    event.type == V90_MP_EVENT_REJECTED)
					continue;
				CHECK(bit + 1u == retry_bits && event.frame_result == V90_MP_OK &&
				      records_match(&event.record, &records[next], width),
				      "MP retry accepted the wrong record");
				accepted++;
			}
			CHECK(accepted == 1u && receiver.stage == V90_MP_SEARCH_SYNC &&
			      receiver.sync_one_count == 0u,
			      "lost retry width=%u old=%u next=%u prefix=%zu",
			      width, old, next, prefix);
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_mp_receiver receiver;
	struct v90_mp_receiver saved;
	struct v90_mp_event event;
	struct v90_mp_event saved_event;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v90_mp_receiver_init(NULL, 17) ==
		      V90_MP_RECEIVER_INVALID_ARGUMENT &&
	      v90_mp_receiver_init(&receiver, 8) ==
		      V90_MP_RECEIVER_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      v90_mp_receiver_init(&receiver, 17) ==
		      V90_MP_RECEIVER_OK &&
	      v90_mp_receiver_is_valid(&receiver) &&
	      !v90_mp_receiver_is_valid(NULL),
	      "MP receiver initialization contract changed");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	CHECK(v90_mp_receive_bit(NULL, 0, &event) ==
		      V90_MP_RECEIVER_INVALID_ARGUMENT &&
	      v90_mp_receive_bit(&receiver, 2, &event) ==
		      V90_MP_RECEIVER_INVALID_ARGUMENT &&
	      v90_mp_receive_bit(&receiver, 0, NULL) ==
		      V90_MP_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "invalid MP receive call changed state or event");
	receiver.stage = (enum v90_mp_receive_stage)3;
	CHECK(!v90_mp_receiver_is_valid(&receiver) &&
	      v90_mp_receive_bit(&receiver, 0, &event) ==
		      V90_MP_RECEIVER_INVALID_STATE,
	      "MP receiver accepted an unknown stage");
	receiver = saved;
	receiver.sequence[0] = 1;
	CHECK(!v90_mp_receiver_is_valid(&receiver),
	      "MP receiver accepted stale candidate storage");
	CHECK(strcmp(v90_mp_receiver_result_name(
		     V90_MP_RECEIVER_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(v90_mp_receiver_result_name(
		     (enum v90_mp_receiver_result)5),
		     "unknown") == 0,
	      "MP receiver result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous records", test_continuous_records },
		{ "rejection and recovery", test_rejection_and_recovery },
		{ "overlapping reacquisition", test_overlapping_reacquisition },
		{ "truncated record recovery", test_truncation_recovery },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
