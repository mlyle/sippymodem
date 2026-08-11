// SPDX-License-Identifier: BSD-3-Clause

#include <v44_transparent_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_escape_rotation_and_backpressure(void)
{
	static const uint8_t wire[] = {
		'A', 0, 1, 'B', 51, 1, 'C', 102, 1, 'D', 153, 1,
		'E', 204, 1, 'F', 255, 1, 'G', 50, 1, 'H',
	};
	static const uint8_t expected[] = {
		'A', 0, 'B', 51, 'C', 102, 'D', 153, 'E', 204,
		'F', 255, 'G', 50, 'H',
	};
	struct v44_transparent_decoder *decoder =
		v44_transparent_decoder_create();
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	uint8_t actual[sizeof(expected)];
	size_t actual_length = 0;

	CHECK(decoder != NULL &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "could not enter V.44 transparent mode");
	while (input_length != 0) {
		const uint8_t *pending_input;
		size_t pending_length;
		size_t produced = 99;

		CHECK(v44_transparent_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &produced) ==
			      V44_TRANSPARENT_NEED_OUTPUT &&
		      produced == 0,
		      "V.44 transparent byte ignored output backpressure");
		pending_input = input;
		pending_length = input_length;
		produced = 99;
		CHECK(v44_transparent_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &produced) ==
			      V44_TRANSPARENT_NEED_OUTPUT &&
		      produced == 0 && input == pending_input &&
		      input_length == pending_length,
		      "V.44 output retry consumed a pending octet");
		CHECK(actual_length < sizeof(actual),
		      "V.44 decoder produced too many transparent bytes");
		CHECK(v44_transparent_decoder_process(
			      decoder, &input, &input_length,
			      actual + actual_length, 1, &produced) ==
			      V44_TRANSPARENT_OK &&
		      produced == 1,
		      "V.44 transparent byte did not resume with output space");
		actual_length += produced;
	}
	CHECK(v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_OK &&
	      actual_length == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 escape rotation changed transparent output");
	v44_transparent_decoder_destroy(decoder);
	return true;
}

static bool test_mode_handoffs(void)
{
	static const uint8_t first_segment[] = { 0, 1, 51 };
	static const uint8_t compressed_command[] = { 0, 0xaa, 0xbb };
	static const uint8_t compressed_tail[] = { 0xaa, 0xbb };
	static const uint8_t second_segment[] = { 51, 1, 'Z' };
	static const uint8_t parameter_segment[] = { 102, 2, 0x11, 0x22 };
	static const uint8_t parameter_tail[] = { 0x11, 0x22 };
	static const uint8_t third_segment[] = { 102, 1 };
	struct v44_transparent_decoder *decoder =
		v44_transparent_decoder_create();
	const uint8_t *input = first_segment;
	size_t input_length = sizeof(first_segment);
	uint8_t output = 0xff;
	size_t produced = 99;

	CHECK(decoder != NULL &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "could not create V.44 handoff decoder");
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_OK &&
	      produced == 1 && output == 0 &&
	      v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_NEED_INPUT &&
	      produced == 0 && input_length == 0,
	      "split V.44 ECM did not retain command state");
	input = compressed_command;
	input_length = sizeof(compressed_command);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_ENTER_COMPRESSED &&
	      produced == 0 && input_length == sizeof(compressed_tail) &&
	      memcmp(input, compressed_tail, sizeof(compressed_tail)) == 0,
	      "V.44 ECM did not order output or preserve compressed input");
	{
		const uint8_t *unchanged_input = input;
		size_t unchanged_length = input_length;

		produced = 99;
		CHECK(v44_transparent_decoder_process(
			      decoder, &input, &input_length, &output, 1,
			      &produced) ==
			      V44_TRANSPARENT_ENTER_COMPRESSED &&
		      produced == 0 && input == unchanged_input &&
		      input_length == unchanged_length &&
		      v44_transparent_decoder_finish(decoder) ==
			      V44_TRANSPARENT_ENTER_COMPRESSED,
		      "V.44 compressed transition was not latched");
	}
	CHECK(v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "V.44 decoder did not re-enter transparent mode");
	input = second_segment;
	input_length = sizeof(second_segment);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_OK &&
	      produced == 1 && output == 51 &&
	      v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_OK &&
	      produced == 1 && output == 'Z' && input_length == 0,
	      "V.44 ESCAPE did not persist across compressed mode");

	input = parameter_segment;
	input_length = sizeof(parameter_segment);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_ENTER_PARAMETER &&
	      produced == 0 && input_length == sizeof(parameter_tail) &&
	      memcmp(input, parameter_tail, sizeof(parameter_tail)) == 0 &&
	      v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_ENTER_PARAMETER,
	      "V.44 EPM did not preserve parameter input");
	CHECK(v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "V.44 decoder did not return from parameter mode");
	input = third_segment;
	input_length = sizeof(third_segment);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_OK &&
	      produced == 1 && output == 102 && input_length == 0,
	      "V.44 EPM changed the current ESCAPE");
	v44_transparent_decoder_destroy(decoder);
	return true;
}

static bool test_incremental_and_malformed_commands(void)
{
	static const uint8_t escape_only[] = { 0 };
	static const uint8_t eid[] = { 1 };
	static const uint8_t invalid_command[] = { 0, 3, 'X' };
	struct v44_transparent_decoder *decoder =
		v44_transparent_decoder_create();
	const uint8_t *input = escape_only;
	size_t input_length = sizeof(escape_only);
	uint8_t output = 0xff;
	size_t produced = 99;

	CHECK(decoder != NULL &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "could not create V.44 incremental decoder");
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_NEED_INPUT &&
	      produced == 0 && input_length == 0,
	      "split V.44 ESCAPE did not retain command state");
	input = eid;
	input_length = sizeof(eid);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_OK &&
	      produced == 1 && output == 0 && input_length == 0 &&
	      v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_OK,
	      "split V.44 EID did not resume exactly");
	v44_transparent_decoder_destroy(decoder);

	decoder = v44_transparent_decoder_create();
	CHECK(decoder != NULL &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "could not recreate V.44 truncation decoder");
	input = escape_only;
	input_length = sizeof(escape_only);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_NEED_INPUT &&
	      v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_PROCEDURAL_ERROR &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_PROCEDURAL_ERROR,
	      "dangling V.44 ESCAPE did not fail closed");
	v44_transparent_decoder_destroy(decoder);

	decoder = v44_transparent_decoder_create();
	CHECK(decoder != NULL &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK,
	      "could not recreate V.44 command decoder");
	input = invalid_command;
	input_length = sizeof(invalid_command);
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_PROCEDURAL_ERROR &&
	      produced == 0 && input_length == 1 && *input == 'X' &&
	      v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_PROCEDURAL_ERROR &&
	      input_length == 1,
	      "undefined V.44 command did not latch a local error");
	v44_transparent_decoder_destroy(decoder);
	return true;
}

static bool test_api_contract(void)
{
	static const struct {
		enum v44_transparent_result result;
		const char *name;
	} names[] = {
		{ V44_TRANSPARENT_OK, "ok" },
		{ V44_TRANSPARENT_NEED_INPUT, "need input" },
		{ V44_TRANSPARENT_NEED_OUTPUT, "need output" },
		{ V44_TRANSPARENT_ENTER_COMPRESSED,
		  "enter compressed" },
		{ V44_TRANSPARENT_ENTER_PARAMETER,
		  "enter parameter" },
		{ V44_TRANSPARENT_INVALID_ARGUMENT,
		  "invalid argument" },
		{ V44_TRANSPARENT_PROCEDURAL_ERROR,
		  "procedural error" },
	};
	struct v44_transparent_decoder *decoder =
		v44_transparent_decoder_create();
	const uint8_t *input = NULL;
	size_t input_length = 0;
	uint8_t output = 0xa5;
	size_t produced = 99;

	CHECK(decoder != NULL &&
	      v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_ENTER_COMPRESSED &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_OK &&
	      v44_transparent_decoder_enter(decoder) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT,
	      "V.44 transparent lifecycle validation changed");
	CHECK(v44_transparent_decoder_process(
		      NULL, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT &&
	      v44_transparent_decoder_process(
		      decoder, NULL, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT &&
	      v44_transparent_decoder_process(
		      decoder, &input, NULL, &output, 1, &produced) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT &&
	      v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, NULL) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT,
	      "V.44 transparent decoder accepted invalid API pointers");
	input_length = 1;
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT &&
	      v44_transparent_decoder_process(
		      decoder, &input, &input_length, NULL, 1, &produced) ==
		      V44_TRANSPARENT_INVALID_ARGUMENT,
	      "V.44 transparent decoder accepted invalid buffers");
	input_length = 0;
	CHECK(v44_transparent_decoder_process(
		      decoder, &input, &input_length, NULL, 0, &produced) ==
		      V44_TRANSPARENT_NEED_INPUT &&
	      produced == 0 && output == 0xa5 &&
	      v44_transparent_decoder_finish(decoder) ==
		      V44_TRANSPARENT_OK,
	      "empty V.44 transparent input changed state or output");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v44_transparent_result_name(
				      names[index].result),
			      names[index].name) == 0,
		      "V.44 transparent result %zu has the wrong name", index);
	CHECK(strcmp(
		      v44_transparent_result_name(
			      (enum v44_transparent_result)99),
		      "unknown") == 0,
	      "unknown V.44 transparent result has the wrong name");
	v44_transparent_decoder_destroy(decoder);
	v44_transparent_decoder_destroy(NULL);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "escape rotation and backpressure",
		  test_escape_rotation_and_backpressure },
		{ "mode handoffs", test_mode_handoffs },
		{ "incremental and malformed commands",
		  test_incremental_and_malformed_commands },
		{ "API contract", test_api_contract },
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
