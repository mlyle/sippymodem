// SPDX-License-Identifier: BSD-3-Clause

#include <v44_compressed_decoder.h>

#include "v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	WIRE_CAPACITY = 256,
	ITEM_COUNT = 20,
	ORDINAL_COUNT = 15,
	EXTENSION_LENGTH = 13,
};

static bool build_incremental_wire(
	uint8_t wire[WIRE_CAPACITY],
	size_t *wire_length,
	uint8_t expected[31])
{
	struct v44_test_wire *builder =
		v44_test_wire_create(
			V44_MIN_STRING_LENGTH, WIRE_CAPACITY);
	bool built = builder != NULL;

	for (size_t index = 0; built && index < ORDINAL_COUNT; ++index) {
		expected[index] = (uint8_t)('A' + index);
		built = v44_test_wire_ordinal(
			builder, expected[index]);
	}
	expected[15] = 'A';
	expected[16] = 'B';
	memcpy(expected + 17, expected + 2, EXTENSION_LENGTH);
	expected[30] = 0xff;
	built = built && v44_test_wire_codeword(builder, 4) &&
		v44_test_wire_extension(builder, EXTENSION_LENGTH) &&
		v44_test_wire_stepup_ordinal(builder) &&
		v44_test_wire_ordinal(builder, 0xff) &&
		v44_test_wire_control(
			builder, V44_TEST_FLUSH) &&
		v44_test_wire_finish(
			builder, wire, WIRE_CAPACITY, wire_length);
	v44_test_wire_destroy(builder);
	return built;
}

static bool decode_at_split(
	const uint8_t *wire,
	size_t wire_length,
	size_t split,
	const uint8_t *expected,
	size_t expected_length)
{
	const struct v44_parameters parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_compressed_decoder *decoder =
		v44_compressed_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = wire;
	size_t input_length = split;
	size_t item = 0;
	bool tail_supplied = split == wire_length;
	uint8_t actual[31];
	bool passed = false;

	if (decoder == NULL)
		return false;
	while (item < ITEM_COUNT) {
		enum v44_compressed_result result =
			v44_compressed_decoder_process(
				decoder, &input, &input_length);
		enum v44_compressed_result expected_result =
			item + 1 == ITEM_COUNT ?
				V44_COMPRESSED_FLUSHED :
				V44_COMPRESSED_OK;

		if (result == V44_COMPRESSED_NEED_INPUT) {
			if (input_length != 0 || tail_supplied)
				goto out;
			input = wire + split;
			input_length = wire_length - split;
			tail_supplied = true;
			continue;
		}
		if (result != expected_result)
			goto out;
		item++;
	}
	if (input_length != 0 ||
	    v44_compressed_decoder_finish(decoder) !=
		    V44_COMPRESSED_OK ||
	    v44_compressed_decoder_read(
		    decoder, actual, sizeof(actual)) != expected_length ||
	    memcmp(actual, expected, expected_length) != 0)
		goto out;
	passed = true;

out:
	v44_compressed_decoder_destroy(decoder);
	return passed;
}

static bool test_every_byte_split(void)
{
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[31];
	size_t wire_length = 0;

	CHECK(build_incremental_wire(wire, &wire_length, expected),
	      "could not build incremental V.44 wire image");
	for (size_t split = 0; split <= wire_length; ++split)
		CHECK(decode_at_split(
			      wire, wire_length, split, expected, sizeof(expected)),
		      "V.44 decoder changed at byte split %zu/%zu", split,
		      wire_length);
	return true;
}

static bool test_truncated_fields_fail_closed(void)
{
	const struct v44_parameters parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;

	CHECK(v44_compressed_decoder_finish(NULL) ==
		      V44_COMPRESSED_INVALID_ARGUMENT,
	      "V.44 stream finish accepted a null decoder");
	CHECK(builder != NULL &&
	      v44_test_wire_stepup_codeword(builder) &&
	      v44_test_wire_codeword(builder, 4) &&
	      v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length) &&
	      wire_length == 2,
	      "could not build partial V.44 codeword fixture");
	v44_test_wire_destroy(builder);
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create partial V.44 decoder");
	input = wire;
	input_length = 1;
	CHECK(v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_OK &&
	      v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_NEED_INPUT &&
	      v44_compressed_decoder_finish(decoder) ==
		      V44_COMPRESSED_PROCEDURAL_ERROR &&
	      v44_compressed_decoder_finish(decoder) ==
		      V44_COMPRESSED_PROCEDURAL_ERROR,
	      "partial V.44 codeword did not latch a stream error");
	v44_compressed_decoder_destroy(decoder);

	builder = v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	CHECK(builder != NULL &&
	      v44_test_wire_control(
		      builder, V44_TEST_STEPUP) &&
	      v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length),
	      "could not build unresolved V.44 STEPUP fixture");
	v44_test_wire_destroy(builder);
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create STEPUP V.44 decoder");
	input = wire;
	input_length = wire_length;
	CHECK(v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_OK &&
	      v44_compressed_decoder_finish(decoder) ==
		      V44_COMPRESSED_PROCEDURAL_ERROR,
	      "unresolved V.44 STEPUP was accepted at stream end");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_nonzero_control_padding(void)
{
	const struct v44_parameters parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;

	CHECK(builder != NULL &&
	      v44_test_wire_ordinal(builder, 'A') &&
	      v44_test_wire_control(
		      builder, V44_TEST_FLUSH) &&
	      v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length) &&
	      wire_length == 2,
	      "could not build V.44 padding fixture");
	v44_test_wire_destroy(builder);
	wire[wire_length - 1] |= 0x80;
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 padding decoder");
	input = wire;
	input_length = wire_length;
	CHECK(v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_OK &&
	      v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_PROCEDURAL_ERROR &&
	      v44_compressed_decoder_finish(decoder) ==
		      V44_COMPRESSED_PROCEDURAL_ERROR,
	      "V.44 decoder accepted nonzero FLUSH padding");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "every byte split", test_every_byte_split },
		{ "truncated fields", test_truncated_fields_fail_closed },
		{ "control padding", test_nonzero_control_padding },
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
