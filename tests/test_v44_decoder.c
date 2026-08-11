// SPDX-License-Identifier: BSD-3-Clause

#include <v44_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct v44_parameters parameters = {
	.codeword_limit = V44_MIN_CODEWORDS,
	.max_string_length = V44_MIN_STRING_LENGTH,
	.history_size = V44_MIN_HISTORY_SIZE,
};

static bool test_mode_round_trip_and_ordering(void)
{
	/* A, ETM, T, escaped NUL, U, ECM, C/D/CD, FLUSH, ETM, escaped 33. */
	static const uint8_t wire[] = {
		0x82, 0x01, 0x54, 0x00, 0x01, 0x55,
		0x33, 0x00, 0x86, 0x88, 0x89, 0x01,
		0x01, 0x33, 0x01,
	};
	static const uint8_t expected[] = {
		'A', 'T', 0x00, 'U', 'C', 'D', 'C', 'D', 0x33,
	};
	struct v44_decoder *decoder =
		v44_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[ARRAY_LENGTH(expected)];
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	size_t actual_length = 0;
	size_t output_length = 99;
	unsigned int flush_count = 0;

	CHECK(decoder != NULL, "could not create V.44 mode decoder");
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, NULL, 0,
		      &output_length) == V44_DECODE_NEED_OUTPUT &&
	      output_length == 0 && input == wire + 1,
	      "V.44 compressed byte did not queue transactionally");
	{
		const uint8_t *unchanged_input = input;
		size_t unchanged_length = input_length;

		CHECK(v44_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &output_length) ==
			      V44_DECODE_NEED_OUTPUT &&
		      input == unchanged_input && input_length == unchanged_length,
		      "V.44 output backpressure consumed ETM");
	}
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, actual, 1,
		      &output_length) == V44_DECODE_OK &&
	      output_length == 1 && actual[actual_length++] == 'A' &&
	      input == wire + 1,
	      "V.44 did not deliver compressed output before ETM");

	while (input_length != 0) {
		size_t output_capacity =
			actual_length < sizeof(actual) ? 1 : 0;
		enum v44_decode_result result =
			v44_decoder_process(
				decoder, &input, &input_length,
				output_capacity != 0 ? actual + actual_length : NULL,
				output_capacity, &output_length);

		CHECK(result == V44_DECODE_OK ||
		      result == V44_DECODE_FLUSHED,
		      "V.44 mode round trip returned %s",
		      v44_decode_result_name(result));
		if (result == V44_DECODE_FLUSHED)
			flush_count++;
		CHECK(output_length <= 1 &&
		      actual_length + output_length <= sizeof(actual),
		      "V.44 mode decoder exceeded its output bound");
		actual_length += output_length;
	}
	CHECK(flush_count == 1 && actual_length == sizeof(expected) &&
	      memcmp(actual, expected, sizeof(expected)) == 0 &&
	      v44_decoder_finish(decoder) ==
		      V44_DECODE_OK,
	      "V.44 ETM/ECM round trip changed decoded data");
	v44_decoder_destroy(decoder);
	return true;
}

static bool test_parameter_handoff_and_failure(void)
{
	/* ETM, EPM, caller-owned parameter bytes, then transparent P. */
	static const uint8_t wire[] = {
		0x01, 0x00, 0x02, 0xaa, 0xbb, 'P',
	};
	static const uint8_t dangling_escape[] = { 0x00 };
	struct v44_decoder *decoder =
		v44_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	size_t output_length;
	uint8_t output = 0;

	CHECK(decoder != NULL, "could not create V.44 parameter decoder");
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_OK &&
	      output_length == 0 &&
	      v44_decoder_process(
		      decoder, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_ENTER_PARAMETER &&
	      input == wire + 3 && input_length == 3,
	      "V.44 EPM did not preserve parameter input");
	{
		const uint8_t *unchanged_input = input;

		CHECK(v44_decoder_process(
			      decoder, &input, &input_length, &output, 1,
			      &output_length) ==
			      V44_DECODE_ENTER_PARAMETER &&
		      v44_decoder_finish(decoder) ==
			      V44_DECODE_ENTER_PARAMETER &&
		      input == unchanged_input && input_length == 3,
		      "V.44 parameter handoff was not latched");
	}
	/* Stand in for an external parameter parser consuming through End. */
	input += 2;
	input_length -= 2;
	CHECK(v44_decoder_resume_transparent(decoder) ==
		      V44_DECODE_OK &&
	      v44_decoder_process(
		      decoder, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_OK &&
	      output_length == 1 && output == 'P' && input_length == 0 &&
	      v44_decoder_resume_transparent(decoder) ==
		      V44_DECODE_INVALID_ARGUMENT,
	      "V.44 did not resume transparent mode after parameter End");
	input = dangling_escape;
	input_length = sizeof(dangling_escape);
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_NEED_INPUT &&
	      input_length == 0 &&
	      v44_decoder_finish(decoder) ==
		      V44_DECODE_PROCEDURAL_ERROR &&
	      v44_decoder_process(
		      decoder, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_PROCEDURAL_ERROR,
	      "V.44 truncated transparent input did not fail closed");
	v44_decoder_destroy(decoder);
	return true;
}

static bool test_api_contract(void)
{
	static const struct {
		enum v44_decode_result result;
		const char *name;
	} names[] = {
		{ V44_DECODE_OK, "ok" },
		{ V44_DECODE_NEED_INPUT, "need input" },
		{ V44_DECODE_NEED_OUTPUT, "need output" },
		{ V44_DECODE_FLUSHED, "flushed" },
		{ V44_DECODE_ENTER_PARAMETER, "enter parameter" },
		{ V44_DECODE_INVALID_ARGUMENT, "invalid argument" },
		{ V44_DECODE_PROCEDURAL_ERROR,
		  "procedural error" },
	};
	struct v44_decoder *decoder =
		v44_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = NULL;
	size_t input_length = 0;
	size_t output_length = 0;
	uint8_t output = 0xa5;

	CHECK(decoder != NULL &&
	      v44_decoder_create(NULL, 32) == NULL &&
	      v44_decoder_create(&parameters, 31) == NULL,
	      "V.44 mode decoder accepted invalid creation arguments");
	CHECK(v44_decoder_process(
		      NULL, &input, &input_length, &output, 1,
		      &output_length) == V44_DECODE_INVALID_ARGUMENT &&
	      v44_decoder_process(
		      decoder, NULL, &input_length, &output, 1,
		      &output_length) == V44_DECODE_INVALID_ARGUMENT &&
	      v44_decoder_process(
		      decoder, &input, NULL, &output, 1,
		      &output_length) == V44_DECODE_INVALID_ARGUMENT &&
	      v44_decoder_process(
		      decoder, &input, &input_length, NULL, 1,
		      &output_length) == V44_DECODE_INVALID_ARGUMENT &&
	      v44_decoder_process(
		      decoder, &input, &input_length, &output, 1, NULL) ==
		      V44_DECODE_INVALID_ARGUMENT,
	      "V.44 mode decoder accepted invalid process arguments");
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, NULL, 0,
		      &output_length) == V44_DECODE_NEED_INPUT &&
	      output_length == 0 && output == 0xa5 &&
	      v44_decoder_finish(decoder) ==
		      V44_DECODE_OK &&
	      v44_decoder_resume_transparent(NULL) ==
		      V44_DECODE_INVALID_ARGUMENT &&
	      v44_decoder_finish(NULL) ==
		      V44_DECODE_INVALID_ARGUMENT,
	      "V.44 empty stream or lifecycle validation changed");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v44_decode_result_name(names[index].result),
			      names[index].name) == 0,
		      "V.44 decode result %zu has the wrong name", index);
	CHECK(strcmp(
		      v44_decode_result_name(
			      (enum v44_decode_result)99),
		      "unknown") == 0,
	      "unknown V.44 decode result has the wrong name");
	v44_decoder_destroy(decoder);
	v44_decoder_destroy(NULL);
	return true;
}

static bool test_buffered_final_control(void)
{
	/* STEPUP, ordinal ff, ordinal B, and FLUSH packed across octets. */
	static const uint8_t wire[] = { 0x05, 0xff, 0x84, 0x06 };
	struct v44_decoder *decoder =
		v44_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	size_t produced = 0;
	uint8_t output[2];

	CHECK(decoder != NULL, "could not create buffered V.44 decoder");
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, output, sizeof(output),
		      &produced) == V44_DECODE_OK &&
	      produced == 0 &&
	      v44_decoder_process(
		      decoder, &input, &input_length, output, sizeof(output),
		      &produced) == V44_DECODE_OK &&
	      produced == 1 && output[0] == 0xff &&
	      v44_decoder_process(
		      decoder, &input, &input_length, output + 1, 1,
		      &produced) == V44_DECODE_OK &&
	      produced == 1 && output[1] == 'B' && input_length == 0,
	      "V.44 decoder did not retain the final buffered control");
	CHECK(v44_decoder_process(
		      decoder, &input, &input_length, NULL, 0, &produced) ==
		      V44_DECODE_FLUSHED &&
	      produced == 0 &&
	      v44_decoder_finish(decoder) ==
		      V44_DECODE_OK,
	      "V.44 decoder ignored a complete buffered FLUSH");
	v44_decoder_destroy(decoder);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "mode round trip and ordering", test_mode_round_trip_and_ordering },
		{ "parameter handoff and failure",
		  test_parameter_handoff_and_failure },
		{ "API contract", test_api_contract },
		{ "buffered final control", test_buffered_final_control },
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
