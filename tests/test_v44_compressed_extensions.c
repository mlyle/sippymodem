// SPDX-License-Identifier: BSD-3-Clause

#include <v44_compressed_decoder.h>

#include "v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	WIRE_CAPACITY = 1024,
	OUTPUT_CAPACITY = 1024,
};

static bool decode_wire(
	struct v44_compressed_decoder *decoder,
	const uint8_t *wire,
	size_t wire_length,
	size_t item_count,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	const uint8_t *input = wire;
	size_t input_length = wire_length;

	for (size_t item = 0; item < item_count; ++item) {
		enum v44_compressed_result expected_result =
			item + 1 == item_count ?
				V44_COMPRESSED_FLUSHED :
				V44_COMPRESSED_OK;
		enum v44_compressed_result result =
			v44_compressed_decoder_process(
				decoder, &input, &input_length);

		if (result != expected_result)
			return false;
	}
	if (input_length != 0 ||
	    v44_compressed_decoder_pending(decoder) > output_capacity)
		return false;
	*output_length = v44_compressed_decoder_read(
		decoder, output, output_capacity);
	return v44_compressed_decoder_pending(decoder) == 0;
}

static bool test_repeated_string_extension(void)
{
	static const uint8_t expected[] = "CCCCCCCCCCX";
	struct v44_parameters parameters =
		v44_default_parameters();
	struct v44_test_wire *builder;
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	uint8_t actual[sizeof(expected) - 1];
	size_t wire_length = 0;
	size_t actual_length = 0;

	parameters.max_string_length = V44_MIN_STRING_LENGTH;
	builder = v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	CHECK(builder != NULL &&
	      v44_test_wire_ordinal(builder, 'C') &&
	      v44_test_wire_codeword(builder, 4) &&
	      v44_test_wire_extension(builder, 7) &&
	      v44_test_wire_ordinal(builder, 'X') &&
	      v44_test_wire_control(
		      builder, V44_TEST_FLUSH) &&
	      v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length),
	      "could not build V.44 repeated-string wire image");
	v44_test_wire_destroy(builder);
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      decode_wire(
		      decoder, wire, wire_length, 5, actual, sizeof(actual),
		      &actual_length) &&
	      actual_length == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 repeated-string extension changed output");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool run_extension_case(
	uint32_t max_string_length, uint32_t extension_length)
{
	struct v44_parameters parameters =
		v44_default_parameters();
	struct v44_test_wire *builder;
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[OUTPUT_CAPACITY];
	uint8_t actual[OUTPUT_CAPACITY];
	size_t initial_length = (size_t)extension_length + 2;
	size_t expected_length = (size_t)extension_length * 2 + 4;
	size_t wire_length = 0;
	size_t actual_length = 0;
	bool passed;

	parameters.max_string_length = max_string_length;
	builder = v44_test_wire_create(
		max_string_length, WIRE_CAPACITY);
	if (builder == NULL)
		return false;
	for (size_t index = 0; index < initial_length; ++index) {
		expected[index] = (uint8_t)((index * 17 + 3) & 0x7f);
		if (!v44_test_wire_ordinal(builder, expected[index])) {
			v44_test_wire_destroy(builder);
			return false;
		}
	}
	expected[initial_length] = expected[0];
	expected[initial_length + 1] = expected[1];
	memcpy(expected + initial_length + 2, expected + 2, extension_length);
	if (!v44_test_wire_codeword(builder, 4) ||
	    !v44_test_wire_extension(builder, extension_length) ||
	    !v44_test_wire_control(
		    builder, V44_TEST_FLUSH) ||
	    !v44_test_wire_finish(
		    builder, wire, sizeof(wire), &wire_length)) {
		v44_test_wire_destroy(builder);
		return false;
	}
	v44_test_wire_destroy(builder);
	decoder = v44_compressed_decoder_create(
		&parameters, OUTPUT_CAPACITY);
	if (decoder == NULL)
		return false;
	passed = decode_wire(
		decoder, wire, wire_length, (size_t)extension_length + 5,
		actual, sizeof(actual), &actual_length) &&
		 actual_length == expected_length &&
		 memcmp(actual, expected, expected_length) == 0;
	v44_compressed_decoder_destroy(decoder);
	return passed;
}

static bool test_extension_length_boundaries(void)
{
	static const struct {
		uint32_t max_string_length;
		uint32_t extension_length;
	} cases[] = {
		{ 32, 1 }, { 32, 2 }, { 32, 4 }, { 32, 5 }, { 32, 12 },
		{ 32, 13 }, { 32, 30 }, { 46, 44 }, { 47, 45 },
		{ 78, 76 }, { 79, 77 }, { 142, 140 }, { 143, 141 },
		{ 255, 253 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(run_extension_case(
			      cases[index].max_string_length,
			      cases[index].extension_length),
		      "V.44 extension boundary %zu (%u/%u) failed", index,
		      cases[index].extension_length,
		      cases[index].max_string_length);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "repeated-string extension", test_repeated_string_extension },
		{ "extension length boundaries", test_extension_length_boundaries },
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
