// SPDX-License-Identifier: BSD-3-Clause

#include <v44_compressed_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_appendix_wire_image(void)
{
	static const uint8_t wire[] = {
		0x82, 0x84, 0x86, 0x88, 0x8a,
		0xb0, 0x09, 0x29, 0x5b, 0x29,
		0xf8, 0x17, 0x64, 0x68, 0x00,
	};
	static const uint8_t expected[] = {
		'A', 'B', 'C', 'D', 'E', 'X',
		'A', 'B', 'C', 'D', 'E', 'Y',
		'A', 'B', 'C', 'D', 'E', 0xff, 'A', 'C',
	};
	const struct sippymodem_v44_parameters parameters =
		sippymodem_v44_default_parameters();
	struct sippymodem_v44_compressed_decoder *decoder =
		sippymodem_v44_compressed_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = NULL;
	size_t input_length = 0;
	size_t wire_offset = 0;
	size_t item = 0;
	uint8_t actual[ARRAY_LENGTH(expected)] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 wire-image decoder");
	while (item < 15) {
		enum sippymodem_v44_compressed_result result;

		if (input_length == 0 && wire_offset < sizeof(wire)) {
			input = wire + wire_offset++;
			input_length = 1;
		}
		result = sippymodem_v44_compressed_decoder_process(
			decoder, &input, &input_length);
		if (result == SIPPYMODEM_V44_COMPRESSED_NEED_INPUT) {
			CHECK(input_length == 0 && wire_offset < sizeof(wire),
			      "V.44 wire image ended in a partial field");
			continue;
		}
		CHECK(result == (item == 14 ?
					 SIPPYMODEM_V44_COMPRESSED_FLUSHED :
					 SIPPYMODEM_V44_COMPRESSED_OK),
		      "V.44 wire item %zu returned %s", item,
		      sippymodem_v44_compressed_result_name(result));
		item++;
	}
	CHECK(wire_offset == sizeof(wire) && input_length == 0 &&
	      sippymodem_v44_compressed_decoder_pending(decoder) ==
		      sizeof(expected) &&
	      sippymodem_v44_compressed_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(expected)) == 0,
	      "V.44 Appendix II.1 wire image decoded incorrectly");
	sippymodem_v44_compressed_decoder_destroy(decoder);
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] Appendix II.1 wire image\n");
	if (!test_appendix_wire_image()) {
		fprintf(stderr, "[  FAILED  ] Appendix II.1 wire image\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] Appendix II.1 wire image\n");
	return 0;
}
