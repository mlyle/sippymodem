// SPDX-License-Identifier: BSD-3-Clause

#include <v90_mp.h>

#include "test.h"

#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x1021,
	MP_START0_BIT = 17,
	MP_TYPE_BIT = 18,
	MP_TYPE0_CRC_START_BIT = 68,
	MP_TYPE1_CRC_START_BIT = 170,
	MP_FIELD_STRIDE_BITS = 17,
};

static struct v90_mp_record type0_record(bool acknowledged)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 14,
			.upstream_trellis = V34_TRELLIS_64_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.acknowledges_cp = acknowledged,
			.upstream_rate_capability_mask = 0x1555,
		},
	};
}

static struct v90_mp_record type1_record(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = 10,
				.upstream_trellis =
					V34_TRELLIS_32_STATE,
				.upstream_expanded_shaping = true,
				.acknowledges_cp = true,
				.upstream_rate_capability_mask = 0x0a5b,
			},
			.precoder_coefficients = {
				{ .real = 0x1234, .imaginary = -1 },
				{ .real = -32767, .imaginary = INT16_MAX },
				{ .real = 0, .imaginary = 16384 },
			},
		},
	};
}

static bool records_are_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	return v90_mp_record_equal(left, right);
}

static uint8_t packed_bit(const uint8_t *sequence, size_t bit_index)
{
	return (uint8_t)((sequence[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_bit(
	uint8_t *sequence, size_t bit_index, uint8_t value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (value != 0)
		sequence[bit_index / 8u] |= mask;
	else
		sequence[bit_index / 8u] &= (uint8_t)~mask;
}

static uint16_t reference_crc(
	const uint8_t *sequence, size_t crc_start_bit)
{
	uint16_t remainder = UINT16_MAX;

	for (size_t bit = MP_TYPE_BIT; bit < crc_start_bit; ++bit) {
		uint16_t next;

		if ((bit - MP_START0_BIT) % MP_FIELD_STRIDE_BITS == 0)
			continue;
		next = (uint16_t)(remainder << 1);
		if ((((remainder >> 15) ^ packed_bit(sequence, bit)) & 1u) != 0)
			next ^= REFERENCE_CRC_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static void replace_crc(uint8_t *sequence, size_t crc_start_bit)
{
	uint16_t crc = reference_crc(sequence, crc_start_bit);

	for (size_t bit = 0; bit < V90_MP_CRC_BITS; ++bit)
		replace_bit(
			sequence, crc_start_bit + 1u + bit,
			(uint8_t)(crc >>
				  (V90_MP_CRC_BITS - bit - 1u)) &
				1u);
}

static bool test_sequence_sizes(void)
{
	size_t size = 0xa5a5;
	size_t maximum = 0;

	for (uint8_t frame_bits = V90_MP_TRAINING_FRAME_MINIMUM_BITS;
	     frame_bits <= V90_MP_TRAINING_FRAME_MAXIMUM_BITS;
	     ++frame_bits) {
		for (enum v90_mp_record_type type = V90_MP_RECORD_TYPE0;
		     type <= V90_MP_RECORD_TYPE1; ++type) {
			size_t minimum = type == V90_MP_RECORD_TYPE0 ?
					 V90_MP_TYPE0_MINIMUM_BITS :
					 V90_MP_TYPE1_MINIMUM_BITS;
			size_t expected =
				((minimum + frame_bits - 1u) / frame_bits) *
				frame_bits;

			CHECK(v90_mp_sequence_size(
				      type, frame_bits, &size) == V90_MP_OK &&
			      size == expected && size % frame_bits == 0,
			      "MP size changed for type %d and D=%u",
			      type, frame_bits);
			if (size > maximum)
				maximum = size;
		}
	}
	CHECK(maximum == V90_MP_MAXIMUM_SEQUENCE_BITS,
	      "maximum MP sequence size is %zu", maximum);
	size = 0xa5a5;
	CHECK(v90_mp_sequence_size(
		      (enum v90_mp_record_type)2, 9, &size) ==
		      V90_MP_INVALID_PARAMETERS &&
	      v90_mp_sequence_size(V90_MP_RECORD_TYPE0, 8, &size) ==
		      V90_MP_INVALID_PARAMETERS &&
	      v90_mp_sequence_size(V90_MP_RECORD_TYPE0, 31, &size) ==
		      V90_MP_INVALID_PARAMETERS &&
	      v90_mp_sequence_size(V90_MP_RECORD_TYPE0, 9, NULL) ==
		      V90_MP_INVALID_ARGUMENT &&
	      size == 0xa5a5,
	      "invalid MP size request changed its destination");
	return true;
}

static bool check_wire_image(
	const struct v90_mp_record *source, uint8_t frame_bits,
	const uint8_t *expected, size_t expected_size,
	size_t expected_bits, uint16_t expected_crc)
{
	struct v90_mp_record decoded = { 0 };
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES + 2u];
	size_t bit_count = 0;
	size_t crc_start = source->type == V90_MP_RECORD_TYPE0 ?
			   MP_TYPE0_CRC_START_BIT : MP_TYPE1_CRC_START_BIT;

	memset(sequence, 0xa5, sizeof(sequence));
	CHECK(v90_mp_encode(
		      source, frame_bits, sequence, sizeof(sequence),
		      &bit_count) == V90_MP_OK &&
	      bit_count == expected_bits &&
	      memcmp(sequence, expected, expected_size) == 0 &&
	      sequence[expected_size] == 0xa5 &&
	      sequence[expected_size + 1u] == 0xa5,
	      "MP type %d wire image changed", source->type);
	CHECK(reference_crc(sequence, crc_start) == expected_crc &&
	      v90_mp_decode(
		      sequence, bit_count, frame_bits, &decoded) ==
		      V90_MP_OK &&
	      records_are_equal(&decoded, source),
	      "MP type %d wire image did not decode", source->type);
	return true;
}

static bool test_known_wire_images(void)
{
	static const uint8_t type0_expected[] = {
		0xff, 0xff, 0x80, 0x73, 0x8a, 0xaa, 0x80,
		0x00, 0x07, 0xe6, 0x80, 0x00, 0x00,
	};
	static const uint8_t type1_expected[] = {
		0xff, 0xff, 0xa0, 0x54, 0xcd, 0xa5, 0x02,
		0xc4, 0x87, 0xff, 0xfa, 0x00, 0x05, 0xff,
		0xfc, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
		0x1f, 0x5c, 0x40, 0x00, 0x00, 0x00,
	};
	struct v90_mp_record type0 = type0_record(false);
	struct v90_mp_record type1 = type1_record();

	return check_wire_image(
		       &type0, 17, type0_expected, sizeof(type0_expected),
		       102, UINT16_C(0xfcd0)) &&
	       check_wire_image(
		       &type1, 30, type1_expected, sizeof(type1_expected),
		       210, UINT16_C(0xfae2));
}

static bool test_all_training_frame_widths(void)
{
	struct v90_mp_record records[] = {
		type0_record(true),
		type1_record(),
	};
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];

	for (size_t record_index = 0;
	     record_index < ARRAY_LENGTH(records); ++record_index) {
		for (uint8_t frame_bits = V90_MP_TRAINING_FRAME_MINIMUM_BITS;
		     frame_bits <= V90_MP_TRAINING_FRAME_MAXIMUM_BITS;
		     ++frame_bits) {
			struct v90_mp_record decoded = { 0 };
			size_t bit_count = 0;

			CHECK(v90_mp_encode(
				      &records[record_index], frame_bits,
				      sequence, sizeof(sequence), &bit_count) ==
				      V90_MP_OK &&
			      v90_mp_decode(
				      sequence, bit_count, frame_bits,
				      &decoded) == V90_MP_OK &&
			      records_are_equal(
				      &decoded, &records[record_index]),
			      "MP type %zu failed round trip with D=%u",
			      record_index, frame_bits);
		}
	}
	return true;
}

static bool test_frame_rejection(void)
{
	struct v90_mp_record source = type0_record(false);
	struct v90_mp_record decoded = type1_record();
	struct v90_mp_record saved = decoded;
	uint8_t valid[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t corrupt[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t bit_count = 0;

	CHECK(v90_mp_encode(
		      &source, 17, valid, sizeof(valid), &bit_count) ==
		      V90_MP_OK,
	      "MP rejection fixture did not encode");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 24, packed_bit(corrupt, 24) ^ 1u);
	CHECK(v90_mp_decode(corrupt, bit_count, 17, &decoded) ==
		      V90_MP_CRC_MISMATCH &&
	      records_are_equal(&decoded, &saved),
	      "MP information corruption was not transactional");
	for (size_t bit = 0; bit < V90_MP_FRAME_SYNC_BITS; ++bit) {
		memcpy(corrupt, valid, sizeof(corrupt));
		replace_bit(corrupt, bit, 0);
		CHECK(v90_mp_decode(corrupt, bit_count, 17, &decoded) ==
			      V90_MP_INVALID_FRAME &&
		      records_are_equal(&decoded, &saved),
		      "MP accepted a broken frame sync bit %zu", bit);
	}
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 34, 1);
	CHECK(v90_mp_decode(corrupt, bit_count, 17, &decoded) ==
		      V90_MP_INVALID_FRAME,
	      "MP accepted a nonzero start bit");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 85, 1);
	CHECK(v90_mp_decode(corrupt, bit_count, 17, &decoded) ==
		      V90_MP_INVALID_FRAME,
	      "MP accepted a nonzero fill bit");
	CHECK(v90_mp_decode(valid, bit_count - 1u, 17, &decoded) ==
		      V90_MP_INVALID_FRAME &&
	      records_are_equal(&decoded, &saved),
	      "MP accepted a truncated sequence");

	/* Reserved information is intentionally uninterpreted, but still CRC
	 * protected. */
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_bit(corrupt, 19, 1);
	replace_bit(corrupt, 49, 1);
	replace_bit(corrupt, 67, 1);
	replace_crc(corrupt, MP_TYPE0_CRC_START_BIT);
	CHECK(v90_mp_decode(corrupt, bit_count, 17, &decoded) == V90_MP_OK &&
	      records_are_equal(&decoded, &source),
	      "MP interpreted reserved information bits");
	return true;
}

static bool test_record_comparison(void)
{
	struct v90_mp_record type0 = type0_record(false);
	struct v90_mp_record type0_prime = type0_record(true);
	struct v90_mp_record type1 = type1_record();
	struct v90_mp_record type1_base = type1;
	struct v90_mp_record changed = type1;
	struct v90_mp_record invalid = type0_prime;

	type1_base.parameters.type1.common.acknowledges_cp = false;
	changed.parameters.type1.precoder_coefficients[1].imaginary--;
	invalid.parameters.type0.maximum_upstream_rate_2400_bps_units = 1;
	CHECK(v90_mp_record_common(&type0) == &type0.parameters.type0 &&
	      v90_mp_record_common(&type1) ==
		      &type1.parameters.type1.common &&
	      v90_mp_record_mutable_common(&type0) ==
		      &type0.parameters.type0 &&
	      v90_mp_record_mutable_common(&type1) ==
		      &type1.parameters.type1.common,
	      "MP common view selected the wrong record layout");
	CHECK(v90_mp_record_is_initial(&type0) &&
	      v90_mp_record_is_initial(&type1_base) &&
	      !v90_mp_record_is_initial(&type0_prime) &&
	      !v90_mp_record_is_initial(&type1) &&
	      !v90_mp_record_is_initial(&invalid) &&
	      !v90_mp_record_is_initial(NULL),
	      "initial MP accepted an acknowledgement or invalid record");
	CHECK(!v90_mp_record_acknowledges_cp(&type0) &&
	      v90_mp_record_acknowledges_cp(&type0_prime) &&
	      !v90_mp_record_equal(&type0, &type0_prime) &&
	      v90_mp_record_parameters_equal(&type0, &type0_prime) &&
	      !v90_mp_record_acknowledges_cp(&type1_base) &&
	      v90_mp_record_acknowledges_cp(&type1) &&
	      !v90_mp_record_equal(&type1_base, &type1) &&
	      v90_mp_record_parameters_equal(&type1_base, &type1) &&
	      !v90_mp_record_equal(&type1, &changed) &&
	      !v90_mp_record_parameters_equal(&type1, &changed) &&
	      !v90_mp_record_parameters_equal(&type0, &type1) &&
	      !v90_mp_record_acknowledges_cp(&invalid) &&
	      !v90_mp_record_equal(&invalid, &invalid) &&
	      !v90_mp_record_parameters_equal(&invalid, &invalid) &&
	      !v90_mp_record_acknowledges_cp(NULL) &&
	      !v90_mp_record_equal(NULL, &type0) &&
	      !v90_mp_record_parameters_equal(&type0, NULL),
	      "MP record comparison changed field or validation semantics");
	return true;
}

static bool test_maximum_rate_limit(void)
{
	struct v90_mp_record type0 = type0_record(false);
	struct v90_mp_record type1 = type1_record();
	struct v90_mp_record saved;
	struct v90_mp_record invalid;

	CHECK(v90_mp_record_limit_maximum_upstream_rate(
		      &type0, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      type0.parameters.type0.maximum_upstream_rate_2400_bps_units ==
		      V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS &&
	      type0.parameters.type0.upstream_rate_capability_mask == 0x1555,
	      "MP type 0 did not limit to its highest enabled rate");
	saved = type0;
	CHECK(v90_mp_record_limit_maximum_upstream_rate(&type0, 14) &&
	      memcmp(&type0, &saved, sizeof(type0)) == 0,
	      "MP rate limit raised an existing maximum");

	type1.parameters.type1.common.maximum_upstream_rate_2400_bps_units =
		14;
	type1.parameters.type1.common.upstream_rate_capability_mask =
		(uint16_t)((1u << (11u - 2u)) | (1u << (9u - 2u)));
	CHECK(v90_mp_record_limit_maximum_upstream_rate(
		      &type1, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      type1.parameters.type1.common
			      .maximum_upstream_rate_2400_bps_units == 11 &&
	      type1.parameters.type1.precoder_coefficients[1].real == -32767,
	      "MP type 1 did not preserve its profile while limiting rate");

	type0 = type0_record(false);
	type0.parameters.type0.upstream_rate_capability_mask =
		(uint16_t)(1u << (14u - 2u));
	saved = type0;
	CHECK(v90_mp_record_limit_maximum_upstream_rate(
		      &type0, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      memcmp(&type0, &saved, sizeof(type0)) == 0,
	      "MP limit invented an unavailable lower rate");
	type0.parameters.type0.maximum_upstream_rate_2400_bps_units = 0;
	saved = type0;
	CHECK(v90_mp_record_limit_maximum_upstream_rate(
		      &type0, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      memcmp(&type0, &saved, sizeof(type0)) == 0,
	      "MP limit changed a cleardown request");

	invalid = type0_record(false);
	invalid.parameters.type0.maximum_upstream_rate_2400_bps_units = 1;
	saved = invalid;
	CHECK(!v90_mp_record_limit_maximum_upstream_rate(
		      NULL, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      !v90_mp_record_limit_maximum_upstream_rate(
		      &invalid, V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS) &&
	      !v90_mp_record_limit_maximum_upstream_rate(&type0, 1) &&
	      !v90_mp_record_limit_maximum_upstream_rate(&type0, 15) &&
	      memcmp(&invalid, &saved, sizeof(invalid)) == 0,
	      "invalid MP rate limits changed their owner");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v90_mp_record valid = type0_record(false);
	struct v90_mp_record invalid = valid;
	struct v90_mp_record decoded = type1_record();
	struct v90_mp_record saved_decoded = decoded;
	uint8_t output[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t saved_output[sizeof(output)];
	size_t bit_count = 0xa5a5;

	memset(output, 0xa5, sizeof(output));
	memcpy(saved_output, output, sizeof(output));
	invalid.parameters.type0
		.maximum_upstream_rate_2400_bps_units = 1;
	CHECK(!v90_mp_record_is_valid(&invalid) &&
	      v90_mp_encode(
		      &invalid, 17, output, sizeof(output), &bit_count) ==
		      V90_MP_INVALID_PARAMETERS,
	      "MP accepted rate index one");
	invalid = valid;
	invalid.parameters.type0
		.maximum_upstream_rate_2400_bps_units = 15;
	CHECK(!v90_mp_record_is_valid(&invalid),
	      "MP accepted a rate above 33 600 bit/s");
	invalid = valid;
	invalid.parameters.type0.upstream_trellis =
		(enum v34_trellis_selection)3;
	CHECK(!v90_mp_record_is_valid(&invalid),
	      "MP accepted a reserved trellis selection");
	invalid = valid;
	invalid.parameters.type0.upstream_rate_capability_mask =
		0x2000;
	CHECK(!v90_mp_record_is_valid(&invalid),
	      "MP accepted a reserved capability bit");
	invalid = type1_record();
	invalid.parameters.type1.precoder_coefficients[1].real = INT16_MIN;
	CHECK(!v90_mp_record_is_valid(&invalid),
	      "MP accepted an out-of-range Q14 coefficient");
	valid.parameters.type0
		.maximum_upstream_rate_2400_bps_units = 0;
	CHECK(v90_mp_record_is_valid(&valid),
	      "MP rejected the cleardown rate");

	CHECK(v90_mp_encode(
		      NULL, 17, output, sizeof(output), &bit_count) ==
		      V90_MP_INVALID_ARGUMENT &&
	      v90_mp_encode(
		      &valid, 17, NULL, sizeof(output), &bit_count) ==
		      V90_MP_INVALID_ARGUMENT &&
	      v90_mp_encode(
		      &valid, 17, output, 1, &bit_count) ==
		      V90_MP_NEED_OUTPUT &&
	      memcmp(output, saved_output, sizeof(output)) == 0 &&
	      bit_count == 0xa5a5,
	      "rejected MP encoding changed its destinations");
	CHECK(v90_mp_decode(NULL, 102, 17, &decoded) ==
		      V90_MP_INVALID_ARGUMENT &&
	      v90_mp_decode(output, 102, 17, NULL) ==
		      V90_MP_INVALID_ARGUMENT &&
	      v90_mp_decode(output, 102, 8, &decoded) ==
		      V90_MP_INVALID_PARAMETERS &&
	      records_are_equal(&decoded, &saved_decoded),
	      "rejected MP decoding changed its destination");
	CHECK(strcmp(v90_mp_result_name(V90_MP_NEED_OUTPUT),
		     "need output") == 0 &&
	      strcmp(v90_mp_result_name(V90_MP_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(v90_mp_result_name((enum v90_mp_result)6),
		     "unknown") == 0,
	      "MP result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "sequence sizes", test_sequence_sizes },
		{ "known wire images", test_known_wire_images },
		{ "all training frame widths", test_all_training_frame_widths },
		{ "frame rejection", test_frame_rejection },
		{ "record comparison", test_record_comparison },
		{ "maximum rate limit", test_maximum_rate_limit },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
