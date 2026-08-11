// SPDX-License-Identifier: BSD-3-Clause

#include <v44_decoder.h>
#include <v44_encoder.h>

#include "v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct v44_parameters parameters = {
	.codeword_limit = V44_MIN_CODEWORDS,
	.max_string_length = V44_MIN_STRING_LENGTH,
	.history_size = V44_MIN_HISTORY_SIZE,
};

static bool drain_one(
	struct v44_encoder *encoder,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	if (*wire_length >= wire_capacity)
		return false;
	if (v44_encoder_read(
		    encoder, wire + *wire_length, 1) != 1)
		return false;
	(*wire_length)++;
	return true;
}

static bool encode_segment(
	struct v44_encoder *encoder,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	const uint8_t *input = plain;
	size_t input_length = plain_length;

	while (input_length != 0) {
		enum v44_encode_result result =
			v44_encoder_process(
				encoder, &input, &input_length);

		if (result == V44_ENCODE_NEED_OUTPUT) {
			if (!drain_one(
				    encoder, wire, wire_capacity, wire_length))
				return false;
		} else if (result != V44_ENCODE_OK) {
			return false;
		}
	}
	for (;;) {
		enum v44_encode_result result =
			v44_encoder_flush(encoder);

		if (result == V44_ENCODE_FLUSHED)
			break;
		if (result != V44_ENCODE_NEED_OUTPUT ||
		    !drain_one(encoder, wire, wire_capacity, wire_length))
			return false;
	}
	return true;
}

static bool encode_and_flush(
	const struct v44_parameters *selected_parameters,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	struct v44_encoder *encoder =
		v44_encoder_create(selected_parameters, 2);
	bool encoded;

	if (encoder == NULL)
		return false;
	*wire_length = 0;
	encoded = encode_segment(
		encoder, plain, plain_length, wire, wire_capacity, wire_length);
	v44_encoder_destroy(encoder);
	return encoded;
}

static bool decode_wire_flushes(
	const struct v44_parameters *selected_parameters,
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length,
	unsigned int expected_flushes)
{
	struct v44_decoder *decoder =
		v44_decoder_create(
			selected_parameters,
			selected_parameters->max_string_length);
	const uint8_t *input = wire;
	size_t input_length = wire_length;
	unsigned int flush_count = 0;

	if (decoder == NULL)
		return false;
	*plain_length = 0;
	while (flush_count < expected_flushes) {
		size_t available = plain_capacity - *plain_length;
		size_t produced = 0;
		enum v44_decode_result result;

		result = v44_decoder_process(
			decoder, &input, &input_length,
			available != 0 ? plain + *plain_length : NULL,
			available, &produced);
		if (result == V44_DECODE_FLUSHED)
			flush_count++;
		else if (result != V44_DECODE_OK)
			goto fail;
		*plain_length += produced;
	}
	if (input_length != 0 || flush_count != expected_flushes ||
	    v44_decoder_finish(decoder) !=
		    V44_DECODE_OK)
		goto fail;
	v44_decoder_destroy(decoder);
	return true;

fail:
	v44_decoder_destroy(decoder);
	return false;
}

static bool decode_wire(
	const struct v44_parameters *selected_parameters,
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length)
{
	return decode_wire_flushes(
		selected_parameters, wire, wire_length, plain, plain_capacity,
		plain_length, 1);
}

static bool test_literal_wire_and_round_trip(void)
{
	static const uint8_t plain[] = { 'A', 0xff, 'B' };
	static const uint8_t expected_wire[] = { 0x82, 0x05, 0xff, 0x84, 0x06 };
	uint8_t wire[16];
	uint8_t decoded[sizeof(plain)];
	size_t wire_length = 0;
	size_t decoded_length = 0;

	CHECK(encode_and_flush(
		      &parameters, plain, sizeof(plain), wire, sizeof(wire),
		      &wire_length),
	      "could not encode literal V.44 stream");
	CHECK(wire_length == sizeof(expected_wire) &&
	      memcmp(wire, expected_wire, sizeof(expected_wire)) == 0,
	      "V.44 literal encoder changed its exact LSB-first wire image");
	CHECK(decode_wire(
		      &parameters, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "V.44 literal encoder did not round-trip through the decoder");
	return true;
}

static bool test_appendix_wire_image(void)
{
	static const uint8_t plain[] = {
		'A', 'B', 'C', 'D', 'E', 'X',
		'A', 'B', 'C', 'D', 'E', 'Y',
		'A', 'B', 'C', 'D', 'E', 0xff, 'A', 'C',
	};
	static const uint8_t expected_wire[] = {
		0x82, 0x84, 0x86, 0x88, 0x8a,
		0xb0, 0x09, 0x29, 0x5b, 0x29,
		0xf8, 0x17, 0x64, 0x68, 0x00,
	};
	const struct v44_parameters default_parameters =
		v44_default_parameters();
	uint8_t wire[32];
	uint8_t decoded[sizeof(plain)];
	size_t wire_length = 0;
	size_t decoded_length = 0;

	CHECK(encode_and_flush(
		      &default_parameters, plain, sizeof(plain), wire,
		      sizeof(wire), &wire_length),
	      "could not encode V.44 Appendix II.1 example");
	CHECK(wire_length == sizeof(expected_wire) &&
	      memcmp(wire, expected_wire, sizeof(expected_wire)) == 0,
	      "V.44 encoder changed the Appendix II.1 wire image");
	CHECK(decode_wire(
		      &default_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "V.44 Appendix II.1 encoding did not round-trip");
	return true;
}

static bool test_repeated_string_wire_image(void)
{
	static const uint8_t plain[] = "CCCCCCCCCCX";
	const struct v44_parameters default_parameters =
		v44_default_parameters();
	struct v44_test_wire *builder =
		v44_test_wire_create(
			default_parameters.max_string_length, 32);
	uint8_t expected[32];
	uint8_t wire[32];
	uint8_t decoded[sizeof(plain) - 1U];
	size_t expected_length = 0;
	size_t wire_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL &&
		v44_test_wire_ordinal(builder, 'C') &&
		v44_test_wire_codeword(builder, 4) &&
		v44_test_wire_extension(builder, 7) &&
		v44_test_wire_ordinal(builder, 'X') &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);

	v44_test_wire_destroy(builder);
	CHECK(built && encode_and_flush(
				 &default_parameters, plain, sizeof(plain) - 1U,
				 wire, sizeof(wire), &wire_length),
	      "could not encode repeated V.44 string");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 repeated-string encoding changed its wire image");
	CHECK(decode_wire(
		      &default_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(decoded) &&
	      memcmp(decoded, plain, sizeof(decoded)) == 0,
	      "V.44 repeated-string encoding did not round-trip");
	return true;
}

static bool check_codeword_stepup(size_t target_codeword)
{
	const struct v44_parameters default_parameters =
		v44_default_parameters();
	struct v44_test_wire *builder =
		v44_test_wire_create(
			default_parameters.max_string_length, 512);
	uint8_t plain[256];
	uint8_t expected[512];
	uint8_t wire[512];
	uint8_t decoded[sizeof(plain)];
	size_t prefix_length = target_codeword - 2U;
	size_t expected_length = 0;
	size_t wire_length = 0;
	size_t decoded_length = 0;
	size_t threshold = 64;
	bool built = builder != NULL;

	if (target_codeword < 4 || target_codeword > sizeof(plain))
		return false;
	for (size_t index = 0; index < prefix_length; ++index)
		plain[index] = index % 2U == 0 ?
				       0 : (uint8_t)((index + 1U) / 2U);
	plain[target_codeword - 2U] = plain[target_codeword - 4U];
	plain[target_codeword - 1U] = plain[target_codeword - 3U];
	for (size_t index = 0; built && index < prefix_length; ++index)
		built = v44_test_wire_ordinal(builder, plain[index]);
	while (built && target_codeword >= threshold) {
		built = v44_test_wire_stepup_codeword(builder);
		threshold *= 2U;
	}
	built = built && v44_test_wire_codeword(
				   builder, (uint32_t)target_codeword) &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	v44_test_wire_destroy(builder);
	CHECK(built && encode_and_flush(
				 &default_parameters, plain, target_codeword, wire,
				 sizeof(wire), &wire_length),
	      "could not encode V.44 codeword %zu boundary", target_codeword);
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 codeword %zu STEPUP wire image changed", target_codeword);
	CHECK(decode_wire(
		      &default_parameters, wire, wire_length, decoded,
		      target_codeword, &decoded_length) &&
	      decoded_length == target_codeword &&
	      memcmp(decoded, plain, target_codeword) == 0,
	      "V.44 codeword %zu STEPUP did not round-trip", target_codeword);
	return true;
}

static bool test_codeword_stepup_boundaries(void)
{
	return check_codeword_stepup(64) && check_codeword_stepup(256);
}

static bool test_extension_field_boundaries(void)
{
	static const struct {
		uint32_t max_string_length;
		uint32_t extension_length;
	} cases[] = {
		{ 32, 1 }, { 32, 4 }, { 32, 5 }, { 32, 12 }, { 32, 13 },
		{ 32, 30 }, { 46, 44 }, { 47, 45 }, { 78, 76 },
		{ 79, 77 }, { 142, 140 }, { 143, 141 }, { 255, 253 },
	};
	uint8_t plain[257];
	uint8_t expected[512];
	uint8_t wire[512];
	uint8_t decoded[sizeof(plain)];

	for (size_t item = 0; item < ARRAY_LENGTH(cases); ++item) {
		const struct v44_parameters selected = {
			.codeword_limit = V44_DEFAULT_CODEWORDS,
			.max_string_length = cases[item].max_string_length,
			.history_size = V44_DEFAULT_HISTORY_SIZE,
		};
		struct v44_test_wire *builder =
			v44_test_wire_create(
				selected.max_string_length, sizeof(expected));
		size_t plain_length = cases[item].extension_length + 4U;
		size_t expected_length = 0;
		size_t wire_length = 0;
		size_t decoded_length = 0;
		bool built;

		memset(plain, 'C', plain_length - 1U);
		plain[plain_length - 1U] = 'X';
		built = builder != NULL &&
			v44_test_wire_ordinal(builder, 'C') &&
			v44_test_wire_codeword(builder, 4) &&
			v44_test_wire_extension(
				builder, cases[item].extension_length) &&
			v44_test_wire_ordinal(builder, 'X') &&
			v44_test_wire_control(
				builder, V44_TEST_FLUSH) &&
			v44_test_wire_finish(
				builder, expected, sizeof(expected),
				&expected_length);
		v44_test_wire_destroy(builder);
		CHECK(built && encode_and_flush(
					 &selected, plain, plain_length, wire,
					 sizeof(wire), &wire_length),
		      "could not encode V.44 extension boundary %u/%u",
		      cases[item].max_string_length,
		      cases[item].extension_length);
		CHECK(wire_length == expected_length &&
		      memcmp(wire, expected, expected_length) == 0,
		      "V.44 extension boundary %u/%u wire image changed",
		      cases[item].max_string_length,
		      cases[item].extension_length);
		CHECK(decode_wire(
			      &selected, wire, wire_length, decoded,
			      plain_length, &decoded_length) &&
		      decoded_length == plain_length &&
		      memcmp(decoded, plain, plain_length) == 0,
		      "V.44 extension boundary %u/%u did not round-trip",
		      cases[item].max_string_length,
		      cases[item].extension_length);
	}
	return true;
}

static bool test_flush_dictionary_continuity(void)
{
	static const uint8_t first[] = { 'A', 'B' };
	static const uint8_t second[] = { 'A', 'B', 'X' };
	static const uint8_t plain[] = { 'A', 'B', 'A', 'B', 'X' };
	struct v44_encoder *encoder =
		v44_encoder_create(&parameters, 2);
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, 32);
	uint8_t expected[32];
	uint8_t wire[32];
	uint8_t decoded[sizeof(plain)];
	size_t expected_length = 0;
	size_t wire_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL &&
		v44_test_wire_ordinal(builder, 'A') &&
		v44_test_wire_ordinal(builder, 'B') &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_codeword(builder, 4) &&
		v44_test_wire_ordinal(builder, 'X') &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	bool encoded = encoder != NULL && built;

	v44_test_wire_destroy(builder);
	if (encoded)
		encoded = encode_segment(
			encoder, first, sizeof(first), wire, sizeof(wire),
			&wire_length);
	if (encoded)
		encoded = encode_segment(
			encoder, second, sizeof(second), wire, sizeof(wire),
			&wire_length);
	v44_encoder_destroy(encoder);
	CHECK(encoded,
	      "could not encode V.44 stream across FLUSH");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 FLUSH did not preserve dictionary wire context");
	CHECK(decode_wire_flushes(
		      &parameters, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length, 2) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "V.44 FLUSH dictionary continuity did not round-trip");
	return true;
}

static bool test_flush_terminates_extension(void)
{
	static const uint8_t plain[] = { 'C', 'C', 'C', 'C' };
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, 16);
	uint8_t expected[16];
	uint8_t wire[16];
	uint8_t decoded[sizeof(plain)];
	size_t expected_length = 0;
	size_t wire_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL &&
		v44_test_wire_ordinal(builder, 'C') &&
		v44_test_wire_codeword(builder, 4) &&
		v44_test_wire_extension(builder, 1) &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);

	v44_test_wire_destroy(builder);
	CHECK(built && encode_and_flush(
				 &parameters, plain, sizeof(plain), wire,
				 sizeof(wire), &wire_length),
	      "could not flush a pending V.44 string extension");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 FLUSH did not terminate its pending extension exactly");
	CHECK(decode_wire(
		      &parameters, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "flushed V.44 string extension did not round-trip");
	return true;
}

static bool test_dictionary_exhaustion_reinitializes(void)
{
	enum {
		V44_FIRST_CODEWORD = 4,
		REINIT_INDEX = V44_MIN_CODEWORDS -
			       V44_FIRST_CODEWORD + 1,
		INPUT_LENGTH = REINIT_INDEX + 2,
		WIRE_CAPACITY = 1024,
	};
	const struct v44_parameters boundary_parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MAX_HISTORY_SIZE,
	};
	struct v44_test_wire *builder =
		v44_test_wire_create(
			boundary_parameters.max_string_length, WIRE_CAPACITY);
	uint8_t plain[INPUT_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[WIRE_CAPACITY];
	uint8_t decoded[INPUT_LENGTH];
	size_t wire_length = 0;
	size_t expected_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL;

	for (size_t index = 0; index < sizeof(plain); ++index)
		plain[index] = index % 2U == 0 ?
				       0 : (uint8_t)((index + 1U) / 2U);
	plain[REINIT_INDEX] = 0xff;
	for (size_t index = 0; built && index < sizeof(plain); ++index) {
		if (index == REINIT_INDEX)
			built = v44_test_wire_control(
					builder, V44_TEST_REINIT) &&
				v44_test_wire_stepup_ordinal(builder);
		if (built)
			built = v44_test_wire_ordinal(
				builder, plain[index]);
	}
	built = built && v44_test_wire_control(
				   builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	v44_test_wire_destroy(builder);

	CHECK(built, "could not build dictionary-boundary wire image");
	CHECK(encode_and_flush(
		      &boundary_parameters, plain, sizeof(plain), wire,
		      sizeof(wire), &wire_length),
	      "dictionary-boundary stream did not encode");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 encoder did not reinitialize before a full-node append");
	CHECK(decode_wire(
		      &boundary_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "dictionary-boundary stream did not round-trip");
	return true;
}

static bool test_history_exhaustion_reinitializes(void)
{
	enum {
		INPUT_LENGTH = V44_MIN_HISTORY_SIZE,
		WIRE_CAPACITY = 1024,
	};
	const struct v44_parameters boundary_parameters = {
		.codeword_limit = V44_MAX_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_test_wire *builder =
		v44_test_wire_create(
			boundary_parameters.max_string_length, WIRE_CAPACITY);
	uint8_t plain[INPUT_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[WIRE_CAPACITY];
	uint8_t decoded[INPUT_LENGTH];
	size_t wire_length = 0;
	size_t expected_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL;
	bool ordinal_stepped = false;

	for (size_t index = 0; index < sizeof(plain); ++index)
		plain[index] = index % 2U == 0 ?
				       0 : (uint8_t)((index + 1U) / 2U);
	for (size_t index = 0; built && index < sizeof(plain); ++index) {
		if (!ordinal_stepped && plain[index] > 127) {
			built = v44_test_wire_stepup_ordinal(builder);
			ordinal_stepped = true;
		}
		if (built)
			built = v44_test_wire_ordinal(
				builder, plain[index]);
	}
	built = built && v44_test_wire_control(
				   builder, V44_TEST_REINIT) &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	v44_test_wire_destroy(builder);

	CHECK(built, "could not build history-boundary wire image");
	CHECK(encode_and_flush(
		      &boundary_parameters, plain, sizeof(plain), wire,
		      sizeof(wire), &wire_length),
	      "history-boundary stream did not encode");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 encoder did not reinitialize immediately at full history");
	CHECK(decode_wire(
		      &boundary_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "history-boundary stream did not round-trip");
	return true;
}

static bool test_generated_compression_round_trips(void)
{
	enum {
		PLAIN_LENGTH = 1536,
		WIRE_CAPACITY = 4096,
	};
	const struct v44_parameters selected[] = {
		parameters,
		{
			.codeword_limit = V44_DEFAULT_CODEWORDS,
			.max_string_length =
				V44_DEFAULT_STRING_LENGTH,
			.history_size = V44_DEFAULT_HISTORY_SIZE,
		},
	};
	uint8_t plain[PLAIN_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	uint8_t decoded[PLAIN_LENGTH];
	uint32_t random = UINT32_C(0x6d2b79f5);

	for (size_t profile = 0; profile < ARRAY_LENGTH(selected); ++profile) {
		for (unsigned int pattern = 0; pattern < 3; ++pattern) {
			size_t wire_length = 0;
			size_t decoded_length = 0;

			for (size_t index = 0; index < sizeof(plain); ++index) {
				if (pattern == 0) {
					plain[index] = 'C';
				} else if (pattern == 1) {
					static const uint8_t phrase[] =
						"ABCDEXABCDEY";

					plain[index] = phrase[
						index % (sizeof(phrase) - 1U)];
				} else {
					random = random * UINT32_C(1664525) +
						 UINT32_C(1013904223);
					plain[index] = (uint8_t)(random >> 24);
				}
			}
			CHECK(encode_and_flush(
				      &selected[profile], plain, sizeof(plain),
				      wire, sizeof(wire), &wire_length),
			      "generated V.44 profile %zu pattern %u did not encode",
			      profile, pattern);
			CHECK(decode_wire(
				      &selected[profile], wire, wire_length, decoded,
				      sizeof(decoded), &decoded_length) &&
			      decoded_length == sizeof(plain) &&
			      memcmp(decoded, plain, sizeof(plain)) == 0,
			      "generated V.44 profile %zu pattern %u did not round-trip",
			      profile, pattern);
			if (pattern != 2)
				CHECK(wire_length < sizeof(plain),
				      "compressible V.44 profile %zu pattern %u expanded",
				      profile, pattern);
		}
	}
	return true;
}

static bool test_api_and_backpressure(void)
{
	struct v44_parameters invalid_parameters = parameters;
	struct v44_encoder *encoder;
	static const uint8_t plain[] = { 0xff, 'A' };
	const uint8_t *input = NULL;
	const uint8_t *saved_input;
	size_t input_length = 0;
	size_t saved_length;
	uint8_t output = 0xa5;

	invalid_parameters.codeword_limit--;
	CHECK(v44_encoder_create(NULL, 2) == NULL &&
	      v44_encoder_create(&invalid_parameters, 2) == NULL &&
	      v44_encoder_create(&parameters, 1) == NULL,
	      "V.44 encoder accepted invalid creation parameters");
	encoder = v44_encoder_create(&parameters, 2);
	CHECK(encoder != NULL, "could not create API-test V.44 encoder");
	CHECK(v44_encoder_pending(encoder) == 0 &&
	      v44_encoder_pending(NULL) == 0 &&
	      v44_encoder_read(encoder, &output, 1) == 0 &&
	      output == 0xa5 &&
	      v44_encoder_read(NULL, &output, 1) == 0 &&
	      v44_encoder_read(encoder, NULL, 1) == 0 &&
	      v44_encoder_read(encoder, &output, 0) == 0,
	      "empty V.44 encoder output changed its destination");
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_NEED_INPUT &&
	      input == NULL && input_length == 0 &&
	      v44_encoder_process(NULL, &input, &input_length) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      v44_encoder_process(encoder, NULL, &input_length) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      v44_encoder_process(encoder, &input, NULL) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      v44_encoder_flush(NULL) ==
		      V44_ENCODE_INVALID_ARGUMENT,
	      "V.44 encoder API validation changed state");
	input_length = 1;
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      input == NULL && input_length == 1,
	      "V.44 encoder accepted null non-empty input");

	input = plain;
	input_length = sizeof(plain);
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      input == plain + 1 && input_length == 1 &&
	      v44_encoder_pending(encoder) == 2,
	      "V.44 encoder did not atomically accept its first octet");
	saved_input = input;
	saved_length = input_length;
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      input == saved_input && input_length == saved_length,
	      "V.44 output backpressure consumed more input");
	CHECK(v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      input == saved_input && input_length == saved_length &&
	      v44_encoder_read(encoder, &output, 1) == 1,
	      "partial V.44 output drain changed the pending transaction");
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      input == plain + sizeof(plain) && input_length == 0 &&
	      v44_encoder_pending(encoder) == 1,
	      "V.44 encoder did not resume after output drain");
	saved_input = input;
	saved_length = input_length;
	CHECK(v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_INVALID_STATE &&
	      input == saved_input && input_length == saved_length &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      v44_encoder_pending(encoder) == 1,
	      "V.44 FLUSH did not preserve queued output ordering");
	CHECK(v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_INVALID_STATE &&
	      input == saved_input && input_length == saved_length &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_FLUSHED &&
	      v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_NEED_INPUT,
	      "V.44 FLUSH completion allowed an early input transition");
	CHECK(strcmp(v44_encode_result_name(
			     V44_ENCODE_OK),
		     "ok") == 0 &&
	      strcmp(v44_encode_result_name(
			     V44_ENCODE_NEED_OUTPUT),
		     "need output") == 0 &&
	      strcmp(v44_encode_result_name(
			     V44_ENCODE_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(v44_encode_result_name(
			     V44_ENCODE_PROCEDURAL_ERROR),
		     "procedural error") == 0 &&
	      strcmp(v44_encode_result_name(
			     (enum v44_encode_result)99),
		     "unknown") == 0,
	      "V.44 encoder result names changed");
	v44_encoder_destroy(encoder);
	v44_encoder_destroy(NULL);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "literal wire and round trip",
		  test_literal_wire_and_round_trip },
		{ "Appendix II.1 wire image", test_appendix_wire_image },
		{ "repeated string wire image",
		  test_repeated_string_wire_image },
		{ "codeword STEPUP boundaries",
		  test_codeword_stepup_boundaries },
		{ "extension field boundaries",
		  test_extension_field_boundaries },
		{ "FLUSH dictionary continuity",
		  test_flush_dictionary_continuity },
		{ "FLUSH terminates extension",
		  test_flush_terminates_extension },
		{ "dictionary exhaustion",
		  test_dictionary_exhaustion_reinitializes },
		{ "history exhaustion",
		  test_history_exhaustion_reinitializes },
		{ "generated compression round trips",
		  test_generated_compression_round_trips },
		{ "API and backpressure", test_api_and_backpressure },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
