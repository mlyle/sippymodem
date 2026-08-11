// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_decoder.h>
#include <sippymodem_v44_encoder.h>

#include "sippymodem_v44_test_wire.h"
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
	const struct sippymodem_v44_parameters *selected_parameters,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	struct sippymodem_v44_encoder *encoder =
		sippymodem_v44_encoder_create(selected_parameters, 2);
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
	const struct sippymodem_v44_parameters *selected_parameters,
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length)
{
	struct sippymodem_v44_decoder *decoder =
		sippymodem_v44_decoder_create(
			selected_parameters,
			selected_parameters->max_string_length);
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
		      &parameters, plain, sizeof(plain), wire, sizeof(wire),
		      &wire_length),
	      "could not encode literal V.44 stream");
	CHECK(wire_length == sizeof(expected_wire) &&
	      memcmp(wire, expected_wire, sizeof(expected_wire)) == 0,
	      "V.44 literal encoder changed its exact LSB-first wire image");
	CHECK(decode_wire(
		      &parameters, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "V.44 literal encoder did not round-trip through the decoder");
	return true;
}

static bool test_dictionary_exhaustion_reinitializes(void)
{
	enum {
		V44_FIRST_CODEWORD = 4,
		REINIT_INDEX = SIPPYMODEM_V44_MIN_CODEWORDS -
			       V44_FIRST_CODEWORD + 1,
		INPUT_LENGTH = REINIT_INDEX + 2,
		WIRE_CAPACITY = 1024,
	};
	const struct sippymodem_v44_parameters boundary_parameters = {
		.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MAX_HISTORY_SIZE,
	};
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			boundary_parameters.max_string_length, WIRE_CAPACITY);
	uint8_t plain[INPUT_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[WIRE_CAPACITY];
	uint8_t decoded[INPUT_LENGTH];
	size_t wire_length = 0;
	size_t expected_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL;

	for (size_t index = 0; index < sizeof(plain); ++index)
		plain[index] = (uint8_t)(0x20U + index % 95U);
	plain[REINIT_INDEX] = 0xff;
	for (size_t index = 0; built && index < sizeof(plain); ++index) {
		if (index == REINIT_INDEX)
			built = sippymodem_v44_test_wire_control(
					builder, SIPPYMODEM_V44_TEST_REINIT) &&
				sippymodem_v44_test_wire_stepup_ordinal(builder);
		if (built)
			built = sippymodem_v44_test_wire_ordinal(
				builder, plain[index]);
	}
	built = built && sippymodem_v44_test_wire_control(
				   builder, SIPPYMODEM_V44_TEST_FLUSH) &&
		sippymodem_v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	sippymodem_v44_test_wire_destroy(builder);

	CHECK(built, "could not build dictionary-boundary wire image");
	CHECK(encode_and_flush(
		      &boundary_parameters, plain, sizeof(plain), wire,
		      sizeof(wire), &wire_length),
	      "dictionary-boundary stream did not encode");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 encoder did not reinitialize before a full-node append");
	CHECK(decode_wire(
		      &boundary_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "dictionary-boundary stream did not round-trip");
	return true;
}

static bool test_history_exhaustion_reinitializes(void)
{
	enum {
		INPUT_LENGTH = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
		WIRE_CAPACITY = 1024,
	};
	const struct sippymodem_v44_parameters boundary_parameters = {
		.codeword_limit = SIPPYMODEM_V44_MAX_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			boundary_parameters.max_string_length, WIRE_CAPACITY);
	uint8_t plain[INPUT_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	uint8_t expected[WIRE_CAPACITY];
	uint8_t decoded[INPUT_LENGTH];
	size_t wire_length = 0;
	size_t expected_length = 0;
	size_t decoded_length = 0;
	bool built = builder != NULL;

	for (size_t index = 0; index < sizeof(plain); ++index)
		plain[index] = (uint8_t)(0x20U + index % 95U);
	plain[sizeof(plain) - 1] = 0xfe;
	for (size_t index = 0; built && index < sizeof(plain); ++index) {
		if (index == sizeof(plain) - 1)
			built = sippymodem_v44_test_wire_stepup_ordinal(builder);
		if (built)
			built = sippymodem_v44_test_wire_ordinal(
				builder, plain[index]);
	}
	built = built && sippymodem_v44_test_wire_control(
				   builder, SIPPYMODEM_V44_TEST_REINIT) &&
		sippymodem_v44_test_wire_control(
			builder, SIPPYMODEM_V44_TEST_FLUSH) &&
		sippymodem_v44_test_wire_finish(
			builder, expected, sizeof(expected), &expected_length);
	sippymodem_v44_test_wire_destroy(builder);

	CHECK(built, "could not build history-boundary wire image");
	CHECK(encode_and_flush(
		      &boundary_parameters, plain, sizeof(plain), wire,
		      sizeof(wire), &wire_length),
	      "history-boundary stream did not encode");
	CHECK(wire_length == expected_length &&
	      memcmp(wire, expected, expected_length) == 0,
	      "V.44 encoder did not reinitialize immediately at full history");
	CHECK(decode_wire(
		      &boundary_parameters, wire, wire_length, decoded,
		      sizeof(decoded), &decoded_length) &&
	      decoded_length == sizeof(plain) &&
	      memcmp(decoded, plain, sizeof(plain)) == 0,
	      "history-boundary stream did not round-trip");
	return true;
}

static bool test_api_and_backpressure(void)
{
	struct sippymodem_v44_parameters invalid_parameters = parameters;
	struct sippymodem_v44_encoder *encoder;
	static const uint8_t plain[] = { 0xff, 'A' };
	const uint8_t *input = NULL;
	const uint8_t *saved_input;
	size_t input_length = 0;
	size_t saved_length;
	uint8_t output = 0xa5;

	invalid_parameters.codeword_limit--;
	CHECK(sippymodem_v44_encoder_create(NULL, 2) == NULL &&
	      sippymodem_v44_encoder_create(&invalid_parameters, 2) == NULL &&
	      sippymodem_v44_encoder_create(&parameters, 1) == NULL,
	      "V.44 encoder accepted invalid creation parameters");
	encoder = sippymodem_v44_encoder_create(&parameters, 2);
	CHECK(encoder != NULL, "could not create API-test V.44 encoder");
	CHECK(sippymodem_v44_encoder_pending(encoder) == 0 &&
	      sippymodem_v44_encoder_pending(NULL) == 0 &&
	      sippymodem_v44_encoder_read(encoder, &output, 1) == 0 &&
	      output == 0xa5 &&
	      sippymodem_v44_encoder_read(NULL, &output, 1) == 0 &&
	      sippymodem_v44_encoder_read(encoder, NULL, 1) == 0 &&
	      sippymodem_v44_encoder_read(encoder, &output, 0) == 0,
	      "empty V.44 encoder output changed its destination");
	CHECK(sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_NEED_INPUT &&
	      input == NULL && input_length == 0 &&
	      sippymodem_v44_encoder_process(NULL, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT &&
	      sippymodem_v44_encoder_process(encoder, NULL, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT &&
	      sippymodem_v44_encoder_process(encoder, &input, NULL) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT &&
	      sippymodem_v44_encoder_flush(NULL) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT,
	      "V.44 encoder API validation changed state");
	input_length = 1;
	CHECK(sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT &&
	      input == NULL && input_length == 1,
	      "V.44 encoder accepted null non-empty input");

	input = plain;
	input_length = sizeof(plain);
	CHECK(sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_OK &&
	      input == plain + 1 && input_length == 1 &&
	      sippymodem_v44_encoder_pending(encoder) == 2,
	      "V.44 encoder did not atomically accept its first octet");
	saved_input = input;
	saved_length = input_length;
	CHECK(sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
	      sippymodem_v44_encoder_flush(encoder) ==
		      SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
	      input == saved_input && input_length == saved_length,
	      "V.44 output backpressure consumed more input");
	CHECK(sippymodem_v44_encoder_read(encoder, &output, 1) == 1 &&
	      sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
	      input == saved_input && input_length == saved_length &&
	      sippymodem_v44_encoder_read(encoder, &output, 1) == 1,
	      "partial V.44 output drain changed the pending transaction");
	CHECK(sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_OK &&
	      input == plain + sizeof(plain) && input_length == 0 &&
	      sippymodem_v44_encoder_pending(encoder) == 1,
	      "V.44 encoder did not resume after output drain");
	CHECK(sippymodem_v44_encoder_flush(encoder) ==
		      SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
	      sippymodem_v44_encoder_read(encoder, &output, 1) == 1 &&
	      sippymodem_v44_encoder_flush(encoder) ==
		      SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
	      sippymodem_v44_encoder_pending(encoder) == 1,
	      "V.44 FLUSH did not preserve queued output ordering");
	saved_input = input;
	saved_length = input_length;
	CHECK(sippymodem_v44_encoder_read(encoder, &output, 1) == 1 &&
	      sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT &&
	      input == saved_input && input_length == saved_length &&
	      sippymodem_v44_encoder_flush(encoder) ==
		      SIPPYMODEM_V44_ENCODE_FLUSHED &&
	      sippymodem_v44_encoder_process(
		      encoder, &input, &input_length) ==
		      SIPPYMODEM_V44_ENCODE_NEED_INPUT,
	      "V.44 FLUSH completion allowed an early input transition");
	CHECK(strcmp(sippymodem_v44_encode_result_name(
			     SIPPYMODEM_V44_ENCODE_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v44_encode_result_name(
			     SIPPYMODEM_V44_ENCODE_NEED_OUTPUT),
		     "need output") == 0 &&
	      strcmp(sippymodem_v44_encode_result_name(
			     SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR),
		     "procedural error") == 0 &&
	      strcmp(sippymodem_v44_encode_result_name(
			     (enum sippymodem_v44_encode_result)99),
		     "unknown") == 0,
	      "V.44 encoder result names changed");
	sippymodem_v44_encoder_destroy(encoder);
	sippymodem_v44_encoder_destroy(NULL);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "literal wire and round trip",
		  test_literal_wire_and_round_trip },
		{ "dictionary exhaustion",
		  test_dictionary_exhaustion_reinitializes },
		{ "history exhaustion",
		  test_history_exhaustion_reinitializes },
		{ "API and backpressure", test_api_and_backpressure },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
