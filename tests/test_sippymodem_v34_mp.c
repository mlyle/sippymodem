// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mp.h>

#include "test.h"

#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x1021,
	REFERENCE_CRC_INITIAL = 0xffff,
	MP_TYPE_BIT = 18,
	MP_START1_BIT = 34,
	MP_START2_BIT = 51,
	MP_START3_BIT = 68,
	MP_CRC_FIRST_BIT = 69,
};

static struct sippymodem_v34_mp_parameters base_parameters(bool acknowledged)
{
	return (struct sippymodem_v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 10,
		.remote_transmitter_trellis =
			SIPPYMODEM_V34_MP_TRELLIS_32_STATE,
		.acknowledges_peer_mp = acknowledged,
		.data_rate_capability_mask = 0x0fff,
	};
}

static bool parameters_are_equal(
	const struct sippymodem_v34_mp_parameters *left,
	const struct sippymodem_v34_mp_parameters *right)
{
	return left->maximum_call_to_answer_rate_2400_bps_units ==
		       right->maximum_call_to_answer_rate_2400_bps_units &&
	       left->maximum_answer_to_call_rate_2400_bps_units ==
		       right->maximum_answer_to_call_rate_2400_bps_units &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled &&
	       left->remote_transmitter_trellis ==
		       right->remote_transmitter_trellis &&
	       left->remote_transmitter_nonlinear_encoding ==
		       right->remote_transmitter_nonlinear_encoding &&
	       left->remote_transmitter_expanded_shaping ==
		       right->remote_transmitter_expanded_shaping &&
	       left->acknowledges_peer_mp == right->acknowledges_peer_mp &&
	       left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->asymmetric_rates_enabled ==
		       right->asymmetric_rates_enabled;
}

static uint8_t reference_frame_bit(
	const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_frame_bit(
	uint8_t *frame, size_t bit_index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (bit != 0)
		frame[bit_index / 8u] |= mask;
	else
		frame[bit_index / 8u] &= (uint8_t)~mask;
}

static uint16_t reference_type0_crc(const uint8_t *frame)
{
	uint16_t remainder = REFERENCE_CRC_INITIAL;

	for (size_t index = MP_TYPE_BIT; index < MP_CRC_FIRST_BIT; ++index) {
		uint16_t next;

		if (index == MP_START1_BIT || index == MP_START2_BIT ||
		    index == MP_START3_BIT)
			continue;
		next = (uint16_t)(remainder << 1);
		if ((((remainder >> 15) ^ reference_frame_bit(frame, index)) &
		     1u) != 0)
			next ^= REFERENCE_CRC_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static void replace_type0_crc(uint8_t *frame)
{
	uint16_t remainder = reference_type0_crc(frame);

	for (size_t index = 0; index < SIPPYMODEM_V34_MP_CRC_BITS; ++index)
		replace_frame_bit(
			frame, MP_CRC_FIRST_BIT + index,
			(uint8_t)(remainder >>
				  (SIPPYMODEM_V34_MP_CRC_BITS - index - 1u)) &
				1u);
}

static uint16_t chronological_crc_word(const uint8_t *frame)
{
	uint16_t word = 0;

	for (size_t index = 0; index < SIPPYMODEM_V34_MP_CRC_BITS; ++index)
		word |= (uint16_t)reference_frame_bit(
				frame, MP_CRC_FIRST_BIT + index) <<
			index;
	return word;
}

static bool test_known_wire_images(void)
{
	static const uint8_t expected_mp[] = {
		0xff, 0xff, 0x85, 0x54, 0x1f, 0xfe,
		0x00, 0x00, 0x07, 0x52, 0xb0,
	};
	static const uint8_t expected_mp_prime[] = {
		0xff, 0xff, 0x85, 0x54, 0x5f, 0xfe,
		0x00, 0x00, 0x02, 0x00, 0x38,
	};
	struct sippymodem_v34_mp_parameters parameters =
		base_parameters(false);
	struct sippymodem_v34_mp_parameters decoded = { 0 };
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES + 2u];

	memset(frame, 0xa5, sizeof(frame));
	CHECK(SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS == 17 &&
	      SIPPYMODEM_V34_MP_TYPE0_INFORMATION_BITS == 48 &&
	      SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS == 88 &&
	      SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES == 11 &&
	      sippymodem_v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_OK &&
	      memcmp(frame, expected_mp, sizeof(expected_mp)) == 0 &&
	      frame[11] == 0xa5 && frame[12] == 0xa5 &&
	      chronological_crc_word(frame) == UINT16_C(0x6a57) &&
	      sippymodem_v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      SIPPYMODEM_V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "known Type 0 MP wire image changed");

	parameters.acknowledges_peer_mp = true;
	CHECK(sippymodem_v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_OK &&
	      memcmp(frame, expected_mp_prime, sizeof(expected_mp_prime)) == 0 &&
	      chronological_crc_word(frame) == UINT16_C(0xe002) &&
	      sippymodem_v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      SIPPYMODEM_V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "known Type 0 MP-prime wire image changed");
	return true;
}

static bool test_selected_fields_round_trip(void)
{
	struct sippymodem_v34_mp_parameters parameters = {
		.maximum_call_to_answer_rate_2400_bps_units = 1,
		.maximum_answer_to_call_rate_2400_bps_units = 14,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis =
			SIPPYMODEM_V34_MP_TRELLIS_64_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = true,
		.data_rate_capability_mask = 0x2555,
		.asymmetric_rates_enabled = true,
	};
	struct sippymodem_v34_mp_parameters decoded = { 0 };
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];

	CHECK(sippymodem_v34_mp_parameters_are_valid(&parameters) &&
	      sippymodem_v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_OK &&
	      sippymodem_v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      SIPPYMODEM_V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "selected Type 0 MP fields did not round trip");
	return true;
}

static bool test_reserved_bits_are_ignored(void)
{
	static const size_t reserved_bits[] = { 19, 49, 52, 67 };
	struct sippymodem_v34_mp_parameters parameters =
		base_parameters(false);
	struct sippymodem_v34_mp_parameters decoded = { 0 };
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];

	CHECK(sippymodem_v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_OK,
	      "reserved-bit fixture did not encode");
	for (size_t index = 0; index < ARRAY_LENGTH(reserved_bits); ++index)
		replace_frame_bit(frame, reserved_bits[index], 1);
	replace_type0_crc(frame);
	CHECK(sippymodem_v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      SIPPYMODEM_V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "Type 0 MP decoder interpreted reserved information bits");
	return true;
}

static bool test_frame_rejection(void)
{
	static const size_t framing_bits[] = { 0, 17, 18, 34, 51, 68, 85, 87 };
	struct sippymodem_v34_mp_parameters parameters =
		base_parameters(false);
	struct sippymodem_v34_mp_parameters sentinel = {
		.maximum_call_to_answer_rate_2400_bps_units = 3,
		.maximum_answer_to_call_rate_2400_bps_units = 4,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis =
			SIPPYMODEM_V34_MP_TRELLIS_16_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.data_rate_capability_mask = 0x0005,
		.asymmetric_rates_enabled = true,
	};
	uint8_t valid[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];

	CHECK(sippymodem_v34_mp_type0_encode(
		      &parameters, valid, sizeof(valid)) ==
		      SIPPYMODEM_V34_MP_OK,
	      "frame-rejection fixture did not encode");
	for (size_t index = 0; index < ARRAY_LENGTH(framing_bits); ++index) {
		struct sippymodem_v34_mp_parameters decoded = sentinel;
		uint8_t malformed[sizeof(valid)];

		memcpy(malformed, valid, sizeof(malformed));
		replace_frame_bit(
			malformed, framing_bits[index],
			reference_frame_bit(malformed, framing_bits[index]) ^ 1u);
		CHECK(sippymodem_v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      SIPPYMODEM_V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted malformed framing bit %zu",
		      framing_bits[index]);
	}
	{
		struct sippymodem_v34_mp_parameters decoded = sentinel;
		uint8_t corrupt[sizeof(valid)];

		memcpy(corrupt, valid, sizeof(corrupt));
		replace_frame_bit(corrupt, 20, reference_frame_bit(corrupt, 20) ^ 1u);
		CHECK(sippymodem_v34_mp_type0_decode(
			      corrupt, sizeof(corrupt), &decoded) ==
			      SIPPYMODEM_V34_MP_CRC_MISMATCH &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a corrupt information bit");
		memcpy(corrupt, valid, sizeof(corrupt));
		replace_frame_bit(corrupt, MP_CRC_FIRST_BIT,
				  reference_frame_bit(corrupt, MP_CRC_FIRST_BIT) ^ 1u);
		CHECK(sippymodem_v34_mp_type0_decode(
			      corrupt, sizeof(corrupt), &decoded) ==
			      SIPPYMODEM_V34_MP_CRC_MISMATCH &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a corrupt CRC bit");
	}
	{
		static const size_t invalid_field_bits[] = { 29, 30 };
		struct sippymodem_v34_mp_parameters decoded = sentinel;
		uint8_t malformed[sizeof(valid)];

		memcpy(malformed, valid, sizeof(malformed));
		for (size_t index = 0; index < ARRAY_LENGTH(invalid_field_bits);
		     ++index)
			replace_frame_bit(malformed, invalid_field_bits[index], 1);
		replace_type0_crc(malformed);
		CHECK(sippymodem_v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      SIPPYMODEM_V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted reserved trellis selection");

		memcpy(malformed, valid, sizeof(malformed));
		for (size_t index = 20; index < 24; ++index)
			replace_frame_bit(malformed, index, 0);
		replace_type0_crc(malformed);
		decoded = sentinel;
		CHECK(sippymodem_v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      SIPPYMODEM_V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a zero maximum rate");
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_mp_parameters valid = base_parameters(false);
	struct sippymodem_v34_mp_parameters invalid = valid;
	struct sippymodem_v34_mp_parameters decoded = base_parameters(true);
	struct sippymodem_v34_mp_parameters saved_decoded = decoded;
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	invalid.maximum_call_to_answer_rate_2400_bps_units = 0;
	CHECK(!sippymodem_v34_mp_parameters_are_valid(&invalid) &&
	      sippymodem_v34_mp_type0_encode(
		      &invalid, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_INVALID_PARAMETERS &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "Type 0 MP accepted a zero maximum rate");
	invalid = valid;
	invalid.maximum_answer_to_call_rate_2400_bps_units = 15;
	CHECK(!sippymodem_v34_mp_parameters_are_valid(&invalid),
	      "Type 0 MP accepted an out-of-range maximum rate");
	invalid = valid;
	invalid.remote_transmitter_trellis =
		(enum sippymodem_v34_mp_trellis)3;
	CHECK(!sippymodem_v34_mp_parameters_are_valid(&invalid),
	      "Type 0 MP accepted a reserved trellis selection");
	invalid = valid;
	invalid.data_rate_capability_mask = 0x4000;
	CHECK(!sippymodem_v34_mp_parameters_are_valid(&invalid) &&
	      !sippymodem_v34_mp_parameters_are_valid(NULL),
	      "Type 0 MP accepted capability bits outside its mask");

	CHECK(sippymodem_v34_mp_type0_encode(
		      NULL, frame, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_mp_type0_encode(
		      &valid, NULL, sizeof(frame)) ==
		      SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_mp_type0_encode(
		      &valid, frame,
		      SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES - 1u) ==
		      SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid Type 0 MP encoding mutated output");
	CHECK(sippymodem_v34_mp_type0_decode(
		      NULL, sizeof(frame), &decoded) ==
		      SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_mp_type0_decode(
		      frame, sizeof(frame), NULL) ==
		      SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_mp_type0_decode(
		      frame, SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES - 1u,
		      &decoded) == SIPPYMODEM_V34_MP_INVALID_ARGUMENT &&
	      parameters_are_equal(&decoded, &saved_decoded),
	      "invalid Type 0 MP decoding mutated output");
	CHECK(strcmp(sippymodem_v34_mp_result_name(
		     SIPPYMODEM_V34_MP_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(sippymodem_v34_mp_result_name(
		     SIPPYMODEM_V34_MP_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(sippymodem_v34_mp_result_name(
		     (enum sippymodem_v34_mp_result)5),
		     "unknown") == 0,
	      "Type 0 MP result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "known wire images", test_known_wire_images },
		{ "selected fields round trip", test_selected_fields_round_trip },
		{ "reserved bits ignored", test_reserved_bits_are_ignored },
		{ "frame rejection", test_frame_rejection },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
