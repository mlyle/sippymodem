// SPDX-License-Identifier: BSD-3-Clause

#include <v44_compressed_decoder.h>

#include "v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	WIRE_CAPACITY = 1024,
	OUTPUT_CAPACITY = V44_MIN_STRING_LENGTH,
};

static bool test_control_boundaries_and_backpressure(void)
{
	static const uint8_t transparent_bytes[] = { 0x5a, 0xa5 };
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
	uint8_t expected[40];
	uint8_t actual[ARRAY_LENGTH(expected)];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;
	size_t actual_length = 0;
	size_t item = 0;
	unsigned int backpressure_count = 0;

	CHECK(builder != NULL, "could not create V.44 control wire");
	for (size_t index = 0; index < 32; ++index) {
		expected[index] = (uint8_t)(index + 1);
		CHECK(v44_test_wire_ordinal(builder, expected[index]),
		      "could not encode V.44 control ordinal %zu", index);
	}
	expected[32] = 1;
	expected[33] = 2;
	expected[34] = 32;
	expected[35] = 1;
	expected[36] = 0xff;
	expected[37] = 'X';
	expected[38] = 'X';
	expected[39] = 'X';
	CHECK(v44_test_wire_control(
		      builder, V44_TEST_FLUSH) &&
	      v44_test_wire_codeword(builder, 4) &&
	      v44_test_wire_codeword(builder, 35) &&
	      v44_test_wire_stepup_ordinal(builder) &&
	      v44_test_wire_ordinal(builder, 0xff) &&
	      v44_test_wire_control(
		      builder, V44_TEST_REINIT) &&
	      v44_test_wire_ordinal(builder, 'X') &&
	      v44_test_wire_codeword(builder, 4) &&
	      v44_test_wire_control(
		      builder, V44_TEST_ETM) &&
	      v44_test_wire_finish(
		      builder, wire, sizeof(wire), &wire_length) &&
	      wire_length + sizeof(transparent_bytes) <= sizeof(wire),
	      "could not finish V.44 control wire");
	v44_test_wire_destroy(builder);
	memcpy(wire + wire_length, transparent_bytes, sizeof(transparent_bytes));
	input = wire;
	input_length = wire_length + sizeof(transparent_bytes);
	decoder = v44_compressed_decoder_create(
		&parameters, OUTPUT_CAPACITY);
	CHECK(decoder != NULL, "could not create V.44 control decoder");

	while (item < 41) {
		enum v44_compressed_result expected_result =
			item == 32 ? V44_COMPRESSED_FLUSHED :
			item == 40 ?
				V44_COMPRESSED_ENTER_TRANSPARENT :
				V44_COMPRESSED_OK;
		enum v44_compressed_result result =
			v44_compressed_decoder_process(
				decoder, &input, &input_length);

		if (result == V44_COMPRESSED_NEED_OUTPUT) {
			const uint8_t *unchanged_input = input;
			size_t unchanged_length = input_length;
			size_t count;

			CHECK(v44_compressed_decoder_finish(decoder) ==
				      V44_COMPRESSED_NEED_OUTPUT &&
			      v44_compressed_decoder_process(
				      decoder, &input, &input_length) ==
				      V44_COMPRESSED_NEED_OUTPUT &&
			      input == unchanged_input &&
			      input_length == unchanged_length,
			      "V.44 output retry consumed input at item %zu", item);
			count = v44_compressed_decoder_read(
				decoder, actual + actual_length, 7);
			CHECK(count != 0, "V.44 backpressure exposed no output");
			actual_length += count;
			backpressure_count++;
			continue;
		}
		CHECK(result == expected_result,
		      "V.44 control item %zu returned %s", item,
		      v44_compressed_result_name(result));
		if (result == V44_COMPRESSED_FLUSHED)
			CHECK(v44_compressed_decoder_pending(decoder) ==
				      OUTPUT_CAPACITY,
			      "V.44 FLUSH did not preserve queued output");
		item++;
	}
	CHECK(backpressure_count >= 2 &&
	      input_length == sizeof(transparent_bytes) &&
	      memcmp(input, transparent_bytes, sizeof(transparent_bytes)) == 0,
	      "V.44 ETM consumed transparent data or missed backpressure");
	{
		const uint8_t *unchanged_input = input;
		size_t unchanged_length = input_length;

		CHECK(v44_compressed_decoder_process(
			      decoder, &input, &input_length) ==
			      V44_COMPRESSED_ENTER_TRANSPARENT &&
		      v44_compressed_decoder_finish(decoder) ==
			      V44_COMPRESSED_ENTER_TRANSPARENT &&
		      input == unchanged_input && input_length == unchanged_length,
		      "V.44 transparent transition was not latched");
	}
	actual_length += v44_compressed_decoder_read(
		decoder, actual + actual_length, sizeof(actual) - actual_length);
	CHECK(actual_length == sizeof(expected) &&
	      v44_compressed_decoder_pending(decoder) == 0 &&
	      memcmp(actual, expected, sizeof(expected)) == 0,
	      "V.44 controls or backpressure changed decoded output");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool run_codeword_width_case(
	uint32_t codeword_limit, unsigned int stepup_count)
{
	static const uint8_t expected[] = "ABABCDCD";
	const struct v44_parameters parameters = {
		.codeword_limit = codeword_limit,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	uint8_t actual[sizeof(expected) - 1];
	const uint8_t *input;
	size_t input_length;
	size_t wire_length = 0;
	size_t item_count = (size_t)stepup_count + 8;

	if (builder == NULL ||
	    !v44_test_wire_ordinal(builder, 'A') ||
	    !v44_test_wire_ordinal(builder, 'B'))
		goto fail_builder;
	for (unsigned int index = 0; index < stepup_count; ++index)
		if (!v44_test_wire_stepup_codeword(builder))
			goto fail_builder;
	if (!v44_test_wire_codeword(builder, 4) ||
	    !v44_test_wire_control(
		    builder, V44_TEST_REINIT) ||
	    !v44_test_wire_ordinal(builder, 'C') ||
	    !v44_test_wire_ordinal(builder, 'D') ||
	    !v44_test_wire_codeword(builder, 4) ||
	    !v44_test_wire_control(
		    builder, V44_TEST_FLUSH) ||
	    !v44_test_wire_finish(
		    builder, wire, sizeof(wire), &wire_length))
		goto fail_builder;
	v44_test_wire_destroy(builder);
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	if (decoder == NULL)
		return false;
	input = wire;
	input_length = wire_length;
	for (size_t item = 0; item < item_count; ++item) {
		enum v44_compressed_result result =
			v44_compressed_decoder_process(
				decoder, &input, &input_length);
		enum v44_compressed_result expected_result =
			item + 1 == item_count ?
				V44_COMPRESSED_FLUSHED :
				V44_COMPRESSED_OK;

		if (result != expected_result) {
			v44_compressed_decoder_destroy(decoder);
			return false;
		}
	}
	if (input_length != 0 ||
	    v44_compressed_decoder_read(
		    decoder, actual, sizeof(actual)) != sizeof(actual) ||
	    memcmp(actual, expected, sizeof(actual)) != 0) {
		v44_compressed_decoder_destroy(decoder);
		return false;
	}
	v44_compressed_decoder_destroy(decoder);
	return true;

fail_builder:
	v44_test_wire_destroy(builder);
	return false;
}

static bool test_codeword_widths_and_reinitialization(void)
{
	CHECK(run_codeword_width_case(257, 3),
	      "V.44 non-power-of-two N2 width or REINIT failed");
	CHECK(run_codeword_width_case(V44_MAX_CODEWORDS, 10),
	      "V.44 maximum N2 width or REINIT failed");
	return true;
}

static bool test_ecm_reentry(void)
{
	static const uint8_t expected[] = { 'A', 'B', 0xff, 'C', 'D', 'C', 'D' };
	/* Initial-width ordinals C/D, codeword 4, then FLUSH; LSB first. */
	static const uint8_t after_ecm[] = { 0x86, 0x88, 0x89, 0x01 };
	const struct v44_parameters parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct v44_test_wire *before_etm =
		v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct v44_compressed_decoder *decoder;
	uint8_t first_wire[WIRE_CAPACITY];
	uint8_t actual[ARRAY_LENGTH(expected)];
	const uint8_t *input;
	size_t first_length = 0;
	size_t input_length;

	CHECK(before_etm != NULL &&
	      v44_test_wire_ordinal(before_etm, 'A') &&
	      v44_test_wire_ordinal(before_etm, 'B') &&
	      v44_test_wire_stepup_ordinal(before_etm) &&
	      v44_test_wire_ordinal(before_etm, 0xff) &&
	      v44_test_wire_control(
		      before_etm, V44_TEST_ETM) &&
	      v44_test_wire_finish(
		      before_etm, first_wire, sizeof(first_wire), &first_length),
	      "could not build V.44 ECM re-entry wire");
	v44_test_wire_destroy(before_etm);
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 ECM decoder");
	input = first_wire;
	input_length = first_length;
	for (size_t item = 0; item < 5; ++item) {
		enum v44_compressed_result expected_result =
			item == 4 ?
				V44_COMPRESSED_ENTER_TRANSPARENT :
				V44_COMPRESSED_OK;

		CHECK(v44_compressed_decoder_process(
			      decoder, &input, &input_length) == expected_result,
		      "V.44 pre-ECM item %zu failed", item);
	}
	CHECK(input_length == 0 &&
	      v44_compressed_decoder_pending(decoder) == 3 &&
	      v44_compressed_decoder_enter(decoder) ==
		      V44_COMPRESSED_OK,
	      "V.44 ECM did not preserve output and resume compression");
	input = after_ecm;
	input_length = sizeof(after_ecm);
	for (size_t item = 0; item < 4; ++item) {
		enum v44_compressed_result expected_result =
			item == 3 ? V44_COMPRESSED_FLUSHED :
				    V44_COMPRESSED_OK;

		CHECK(v44_compressed_decoder_process(
			      decoder, &input, &input_length) == expected_result,
		      "V.44 post-ECM item %zu failed", item);
	}
	CHECK(input_length == 0 &&
	      v44_compressed_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(expected)) == 0,
	      "V.44 ECM did not reset widths and dictionary state");
	CHECK(v44_compressed_decoder_enter(decoder) ==
		      V44_COMPRESSED_INVALID_ARGUMENT &&
	      v44_compressed_decoder_enter(NULL) ==
		      V44_COMPRESSED_INVALID_ARGUMENT,
	      "V.44 compressed re-entry accepted an invalid lifecycle");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "control boundaries and backpressure",
		  test_control_boundaries_and_backpressure },
		{ "codeword widths and reinitialization",
		  test_codeword_widths_and_reinitialization },
		{ "ECM re-entry", test_ecm_reentry },
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
