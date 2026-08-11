// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_decoder.h>
#include <sippymodem_v44_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct sippymodem_v44_parameters parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
};

static bool drain_one(
	struct sippymodem_v44_encoder *encoder,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	if (*wire_length >= wire_capacity)
		return false;
	if (sippymodem_v44_encoder_read(
		    encoder, wire + *wire_length, 1) != 1)
		return false;
	(*wire_length)++;
	return true;
}

static bool encode_and_flush(
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	struct sippymodem_v44_encoder *encoder =
		sippymodem_v44_encoder_create(&parameters, 2);
	const uint8_t *input = plain;
	size_t input_length = plain_length;

	if (encoder == NULL)
		return false;
	*wire_length = 0;
	while (input_length != 0) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_process(
				encoder, &input, &input_length);

		if (result == SIPPYMODEM_V44_ENCODE_NEED_OUTPUT) {
			if (!drain_one(
				    encoder, wire, wire_capacity, wire_length))
				goto fail;
		} else if (result != SIPPYMODEM_V44_ENCODE_OK) {
			goto fail;
		}
	}
	for (;;) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_flush(encoder);

		if (result == SIPPYMODEM_V44_ENCODE_FLUSHED)
			break;
		if (result != SIPPYMODEM_V44_ENCODE_NEED_OUTPUT ||
		    !drain_one(encoder, wire, wire_capacity, wire_length))
			goto fail;
	}
	sippymodem_v44_encoder_destroy(encoder);
	return true;

fail:
	sippymodem_v44_encoder_destroy(encoder);
	return false;
}

static bool decode_wire(
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length)
{
	struct sippymodem_v44_decoder *decoder =
		sippymodem_v44_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = wire;
	size_t input_length = wire_length;
	unsigned int flush_count = 0;

	if (decoder == NULL)
		return false;
	*plain_length = 0;
	while (flush_count == 0) {
		size_t available = plain_capacity - *plain_length;
		size_t produced = 0;
		enum sippymodem_v44_decode_result result;

		result = sippymodem_v44_decoder_process(
			decoder, &input, &input_length,
			available != 0 ? plain + *plain_length : NULL,
			available, &produced);
		if (result == SIPPYMODEM_V44_DECODE_FLUSHED)
			flush_count++;
		else if (result != SIPPYMODEM_V44_DECODE_OK)
			goto fail;
		*plain_length += produced;
	}
	if (input_length != 0 || flush_count != 1 ||
	    sippymodem_v44_decoder_finish(decoder) !=
		    SIPPYMODEM_V44_DECODE_OK)
		goto fail;
	sippymodem_v44_decoder_destroy(decoder);
	return true;

fail:
	sippymodem_v44_decoder_destroy(decoder);
	return false;
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
		      plain, sizeof(plain), wire, sizeof(wire), &wire_length),
	      "could not encode literal V.44 stream");
	CHECK(wire_length == sizeof(expected_wire) &&
	      memcmp(wire, expected_wire, sizeof(expected_wire)) == 0,
	      "V.44 literal encoder changed its exact LSB-first wire image");
	CHECK(decode_wire(
		      wire, wire_length, decoded, sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "V.44 literal encoder did not round-trip through the decoder");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] literal wire and round trip\n");
	if (!test_literal_wire_and_round_trip()) {
		fprintf(stderr, "[  FAILED  ] literal wire and round trip\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] literal wire and round trip\n");
	return 0;
}
