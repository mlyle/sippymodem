// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_transparent_decoder.h>

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
	struct sippymodem_v44_transparent_decoder *decoder =
		sippymodem_v44_transparent_decoder_create();
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	uint8_t actual[sizeof(expected)];
	size_t actual_length = 0;

	CHECK(decoder != NULL &&
	      sippymodem_v44_transparent_decoder_enter(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK,
	      "could not enter V.44 transparent mode");
	while (input_length != 0) {
		const uint8_t *pending_input;
		size_t pending_length;
		size_t produced = 99;

		CHECK(sippymodem_v44_transparent_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &produced) ==
			      SIPPYMODEM_V44_TRANSPARENT_NEED_OUTPUT &&
		      produced == 0,
		      "V.44 transparent byte ignored output backpressure");
		pending_input = input;
		pending_length = input_length;
		produced = 99;
		CHECK(sippymodem_v44_transparent_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &produced) ==
			      SIPPYMODEM_V44_TRANSPARENT_NEED_OUTPUT &&
		      produced == 0 && input == pending_input &&
		      input_length == pending_length,
		      "V.44 output retry consumed a pending octet");
		CHECK(actual_length < sizeof(actual),
		      "V.44 decoder produced too many transparent bytes");
		CHECK(sippymodem_v44_transparent_decoder_process(
			      decoder, &input, &input_length,
			      actual + actual_length, 1, &produced) ==
			      SIPPYMODEM_V44_TRANSPARENT_OK &&
		      produced == 1,
		      "V.44 transparent byte did not resume with output space");
		actual_length += produced;
	}
	CHECK(sippymodem_v44_transparent_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK &&
	      actual_length == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 escape rotation changed transparent output");
	sippymodem_v44_transparent_decoder_destroy(decoder);
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
	struct sippymodem_v44_transparent_decoder *decoder =
		sippymodem_v44_transparent_decoder_create();
	const uint8_t *input = first_segment;
	size_t input_length = sizeof(first_segment);
	uint8_t output = 0xff;
	size_t produced = 99;

	CHECK(decoder != NULL &&
	      sippymodem_v44_transparent_decoder_enter(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK,
	      "could not create V.44 handoff decoder");
	CHECK(sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK &&
	      produced == 1 && output == 0 &&
	      sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_NEED_INPUT &&
	      produced == 0 && input_length == 0,
	      "split V.44 ECM did not retain command state");
	input = compressed_command;
	input_length = sizeof(compressed_command);
	CHECK(sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_ENTER_COMPRESSED &&
	      produced == 0 && input_length == sizeof(compressed_tail) &&
	      memcmp(input, compressed_tail, sizeof(compressed_tail)) == 0,
	      "V.44 ECM did not order output or preserve compressed input");
	{
		const uint8_t *unchanged_input = input;
		size_t unchanged_length = input_length;

		produced = 99;
		CHECK(sippymodem_v44_transparent_decoder_process(
			      decoder, &input, &input_length, &output, 1,
			      &produced) ==
			      SIPPYMODEM_V44_TRANSPARENT_ENTER_COMPRESSED &&
		      produced == 0 && input == unchanged_input &&
		      input_length == unchanged_length &&
		      sippymodem_v44_transparent_decoder_finish(decoder) ==
			      SIPPYMODEM_V44_TRANSPARENT_ENTER_COMPRESSED,
		      "V.44 compressed transition was not latched");
	}
	CHECK(sippymodem_v44_transparent_decoder_enter(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK,
	      "V.44 decoder did not re-enter transparent mode");
	input = second_segment;
	input_length = sizeof(second_segment);
	CHECK(sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK &&
	      produced == 1 && output == 51 &&
	      sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK &&
	      produced == 1 && output == 'Z' && input_length == 0,
	      "V.44 ESCAPE did not persist across compressed mode");

	input = parameter_segment;
	input_length = sizeof(parameter_segment);
	CHECK(sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_ENTER_PARAMETER &&
	      produced == 0 && input_length == sizeof(parameter_tail) &&
	      memcmp(input, parameter_tail, sizeof(parameter_tail)) == 0 &&
	      sippymodem_v44_transparent_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_ENTER_PARAMETER,
	      "V.44 EPM did not preserve parameter input");
	CHECK(sippymodem_v44_transparent_decoder_enter(decoder) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK,
	      "V.44 decoder did not return from parameter mode");
	input = third_segment;
	input_length = sizeof(third_segment);
	CHECK(sippymodem_v44_transparent_decoder_process(
		      decoder, &input, &input_length, &output, 1, &produced) ==
		      SIPPYMODEM_V44_TRANSPARENT_OK &&
	      produced == 1 && output == 102 && input_length == 0,
	      "V.44 EPM changed the current ESCAPE");
	sippymodem_v44_transparent_decoder_destroy(decoder);
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
