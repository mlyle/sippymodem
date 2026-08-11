// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_compressed_decoder.h>

#include "sippymodem_v44_test_wire.h"
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
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			SIPPYMODEM_V44_MIN_STRING_LENGTH, WIRE_CAPACITY);
	bool built = builder != NULL;

	for (size_t index = 0; built && index < ORDINAL_COUNT; ++index) {
		expected[index] = (uint8_t)('A' + index);
		built = sippymodem_v44_test_wire_ordinal(
			builder, expected[index]);
	}
	expected[15] = 'A';
	expected[16] = 'B';
	memcpy(expected + 17, expected + 2, EXTENSION_LENGTH);
	expected[30] = 0xff;
	built = built && sippymodem_v44_test_wire_codeword(builder, 4) &&
		sippymodem_v44_test_wire_extension(builder, EXTENSION_LENGTH) &&
		sippymodem_v44_test_wire_stepup_ordinal(builder) &&
		sippymodem_v44_test_wire_ordinal(builder, 0xff) &&
		sippymodem_v44_test_wire_control(
			builder, SIPPYMODEM_V44_TEST_FLUSH) &&
		sippymodem_v44_test_wire_finish(
			builder, wire, WIRE_CAPACITY, wire_length);
	sippymodem_v44_test_wire_destroy(builder);
	return built;
}

static bool decode_at_split(
	const uint8_t *wire,
	size_t wire_length,
	size_t split,
	const uint8_t *expected,
	size_t expected_length)
{
	const struct sippymodem_v44_parameters parameters = {
		.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
	struct sippymodem_v44_compressed_decoder *decoder =
		sippymodem_v44_compressed_decoder_create(
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
		enum sippymodem_v44_compressed_result result =
			sippymodem_v44_compressed_decoder_process(
				decoder, &input, &input_length);
		enum sippymodem_v44_compressed_result expected_result =
			item + 1 == ITEM_COUNT ?
				SIPPYMODEM_V44_COMPRESSED_FLUSHED :
				SIPPYMODEM_V44_COMPRESSED_OK;

		if (result == SIPPYMODEM_V44_COMPRESSED_NEED_INPUT) {
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
	    sippymodem_v44_compressed_decoder_finish(decoder) !=
		    SIPPYMODEM_V44_COMPRESSED_OK ||
	    sippymodem_v44_compressed_decoder_read(
		    decoder, actual, sizeof(actual)) != expected_length ||
	    memcmp(actual, expected, expected_length) != 0)
		goto out;
	passed = true;

out:
	sippymodem_v44_compressed_decoder_destroy(decoder);
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
	const struct sippymodem_v44_parameters parameters = {
		.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct sippymodem_v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;

	CHECK(sippymodem_v44_compressed_decoder_finish(NULL) ==
		      SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT,
	      "V.44 stream finish accepted a null decoder");
	CHECK(builder != NULL &&
	      sippymodem_v44_test_wire_stepup_codeword(builder) &&
	      sippymodem_v44_test_wire_codeword(builder, 4) &&
	      sippymodem_v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length) &&
	      wire_length == 2,
	      "could not build partial V.44 codeword fixture");
	sippymodem_v44_test_wire_destroy(builder);
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create partial V.44 decoder");
	input = wire;
	input_length = 1;
	CHECK(sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_OK &&
	      sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_NEED_INPUT &&
	      sippymodem_v44_compressed_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR &&
	      sippymodem_v44_compressed_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR,
	      "partial V.44 codeword did not latch a stream error");
	sippymodem_v44_compressed_decoder_destroy(decoder);

	builder = sippymodem_v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	CHECK(builder != NULL &&
	      sippymodem_v44_test_wire_control(
		      builder, SIPPYMODEM_V44_TEST_STEPUP) &&
	      sippymodem_v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length),
	      "could not build unresolved V.44 STEPUP fixture");
	sippymodem_v44_test_wire_destroy(builder);
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create STEPUP V.44 decoder");
	input = wire;
	input_length = wire_length;
	CHECK(sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_OK &&
	      sippymodem_v44_compressed_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR,
	      "unresolved V.44 STEPUP was accepted at stream end");
	sippymodem_v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_nonzero_control_padding(void)
{
	const struct sippymodem_v44_parameters parameters = {
		.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct sippymodem_v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;

	CHECK(builder != NULL &&
	      sippymodem_v44_test_wire_ordinal(builder, 'A') &&
	      sippymodem_v44_test_wire_control(
		      builder, SIPPYMODEM_V44_TEST_FLUSH) &&
	      sippymodem_v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length) &&
	      wire_length == 2,
	      "could not build V.44 padding fixture");
	sippymodem_v44_test_wire_destroy(builder);
	wire[wire_length - 1] |= 0x80;
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 padding decoder");
	input = wire;
	input_length = wire_length;
	CHECK(sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_OK &&
	      sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR &&
	      sippymodem_v44_compressed_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR,
	      "V.44 decoder accepted nonzero FLUSH padding");
	sippymodem_v44_compressed_decoder_destroy(decoder);
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
