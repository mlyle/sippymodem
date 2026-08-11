// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct sippymodem_v44_parameters parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
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
	struct sippymodem_v44_decoder *decoder =
		sippymodem_v44_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[ARRAY_LENGTH(expected)];
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	size_t actual_length = 0;
	size_t output_length = 99;
	unsigned int flush_count = 0;

	CHECK(decoder != NULL, "could not create V.44 mode decoder");
	CHECK(sippymodem_v44_decoder_process(
		      decoder, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DECODE_NEED_OUTPUT &&
	      output_length == 0 && input == wire + 1,
	      "V.44 compressed byte did not queue transactionally");
	{
		const uint8_t *unchanged_input = input;
		size_t unchanged_length = input_length;

		CHECK(sippymodem_v44_decoder_process(
			      decoder, &input, &input_length, NULL, 0,
			      &output_length) ==
			      SIPPYMODEM_V44_DECODE_NEED_OUTPUT &&
		      input == unchanged_input && input_length == unchanged_length,
		      "V.44 output backpressure consumed ETM");
	}
	CHECK(sippymodem_v44_decoder_process(
		      decoder, &input, &input_length, actual, 1,
		      &output_length) == SIPPYMODEM_V44_DECODE_OK &&
	      output_length == 1 && actual[actual_length++] == 'A' &&
	      input == wire + 1,
	      "V.44 did not deliver compressed output before ETM");

	while (input_length != 0) {
		size_t output_capacity =
			actual_length < sizeof(actual) ? 1 : 0;
		enum sippymodem_v44_decode_result result =
			sippymodem_v44_decoder_process(
				decoder, &input, &input_length,
				output_capacity != 0 ? actual + actual_length : NULL,
				output_capacity, &output_length);

		CHECK(result == SIPPYMODEM_V44_DECODE_OK ||
		      result == SIPPYMODEM_V44_DECODE_FLUSHED,
		      "V.44 mode round trip returned %s",
		      sippymodem_v44_decode_result_name(result));
		if (result == SIPPYMODEM_V44_DECODE_FLUSHED)
			flush_count++;
		CHECK(output_length <= 1 &&
		      actual_length + output_length <= sizeof(actual),
		      "V.44 mode decoder exceeded its output bound");
		actual_length += output_length;
	}
	CHECK(flush_count == 1 && actual_length == sizeof(expected) &&
	      memcmp(actual, expected, sizeof(expected)) == 0 &&
	      sippymodem_v44_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_DECODE_OK,
	      "V.44 ETM/ECM round trip changed decoded data");
	sippymodem_v44_decoder_destroy(decoder);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "mode round trip and ordering", test_mode_round_trip_and_ordering },
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
