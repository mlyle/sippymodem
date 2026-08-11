// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_compressed_decoder.h>

#include "sippymodem_v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	WIRE_CAPACITY = 64,
};

static const struct sippymodem_v44_parameters parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
};

static bool finish_wire(
	struct sippymodem_v44_test_wire *builder,
	uint8_t wire[WIRE_CAPACITY],
	size_t *wire_length)
{
	bool finished = sippymodem_v44_test_wire_finish(
		builder, wire, WIRE_CAPACITY, wire_length);

	sippymodem_v44_test_wire_destroy(builder);
	return finished;
}

static bool process_to_error(
	struct sippymodem_v44_compressed_decoder *decoder,
	const uint8_t *wire,
	size_t wire_length,
	size_t accepted_codes,
	const uint8_t **remaining_input,
	size_t *remaining_length)
{
	const uint8_t *input = wire;
	size_t input_length = wire_length;

	for (size_t code = 0; code < accepted_codes; ++code)
		if (sippymodem_v44_compressed_decoder_process(
			    decoder, &input, &input_length) !=
		    SIPPYMODEM_V44_COMPRESSED_OK)
			return false;
	if (sippymodem_v44_compressed_decoder_process(
		    decoder, &input, &input_length) !=
	    SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR)
		return false;
	*remaining_input = input;
	*remaining_length = input_length;
	return true;
}

static bool test_stepup_limits(void)
{
	struct sippymodem_v44_test_wire *builder;
	struct sippymodem_v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	uint8_t output = 0;
	const uint8_t *remaining;
	size_t wire_length = 0;
	size_t remaining_length;
	bool built;

	builder = sippymodem_v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	built = builder != NULL &&
		sippymodem_v44_test_wire_stepup_ordinal(builder) &&
		sippymodem_v44_test_wire_ordinal(builder, 0xff) &&
		sippymodem_v44_test_wire_control(
			builder, SIPPYMODEM_V44_TEST_STEPUP) &&
		sippymodem_v44_test_wire_put(builder, 0, 1);
	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive ordinal STEPUP wire");
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length) &&
	      sippymodem_v44_compressed_decoder_read(decoder, &output, 1) ==
		      1 &&
	      output == 0xff,
	      "V.44 decoder accepted excessive ordinal width");
	{
		const uint8_t *unchanged_input = remaining;
		size_t unchanged_length = remaining_length;

		CHECK(sippymodem_v44_compressed_decoder_process(
			      decoder, &remaining, &remaining_length) ==
			      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR &&
		      sippymodem_v44_compressed_decoder_finish(decoder) ==
			      SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR &&
		      remaining == unchanged_input &&
		      remaining_length == unchanged_length,
		      "V.44 procedural error did not remain latched");
	}
	sippymodem_v44_compressed_decoder_destroy(decoder);

	builder = sippymodem_v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	built = builder != NULL &&
		sippymodem_v44_test_wire_stepup_codeword(builder) &&
		sippymodem_v44_test_wire_stepup_codeword(builder) &&
		sippymodem_v44_test_wire_control(
			builder, SIPPYMODEM_V44_TEST_STEPUP) &&
		sippymodem_v44_test_wire_put(builder, 1, 1);
	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive codeword STEPUP wire");
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length) &&
	      sippymodem_v44_compressed_decoder_pending(decoder) == 0,
	      "V.44 decoder accepted excessive codeword width");
	sippymodem_v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_extension_limit(void)
{
	struct sippymodem_v44_test_wire *builder =
		sippymodem_v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct sippymodem_v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *remaining;
	size_t wire_length = 0;
	size_t remaining_length;
	bool built = builder != NULL &&
		sippymodem_v44_test_wire_ordinal(builder, 'A') &&
		sippymodem_v44_test_wire_ordinal(builder, 'B') &&
		sippymodem_v44_test_wire_codeword(builder, 4) &&
		sippymodem_v44_test_wire_extension(builder, 31);

	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive string-extension wire");
	decoder = sippymodem_v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length),
	      "V.44 decoder accepted extension beyond negotiated N7");
	sippymodem_v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_api_contract(void)
{
	static const struct {
		enum sippymodem_v44_compressed_result result;
		const char *name;
	} names[] = {
		{ SIPPYMODEM_V44_COMPRESSED_OK, "ok" },
		{ SIPPYMODEM_V44_COMPRESSED_NEED_INPUT, "need input" },
		{ SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT, "need output" },
		{ SIPPYMODEM_V44_COMPRESSED_FLUSHED, "flushed" },
		{ SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT,
		  "enter transparent" },
		{ SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT,
		  "invalid argument" },
		{ SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR,
		  "procedural error" },
	};
	struct sippymodem_v44_compressed_decoder *decoder =
		sippymodem_v44_compressed_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = NULL;
	size_t input_length = 0;
	uint8_t unchanged = 0xa5;

	CHECK(decoder != NULL, "could not create V.44 validation decoder");
	CHECK(sippymodem_v44_compressed_decoder_create(NULL, 32) == NULL &&
	      sippymodem_v44_compressed_decoder_create(&parameters, 31) == NULL &&
	      sippymodem_v44_compressed_decoder_finish(decoder) ==
		      SIPPYMODEM_V44_COMPRESSED_OK,
	      "V.44 decoder accepted invalid creation arguments");
	CHECK(sippymodem_v44_compressed_decoder_process(
		      NULL, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT &&
	      sippymodem_v44_compressed_decoder_process(
		      decoder, NULL, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT &&
	      sippymodem_v44_compressed_decoder_process(
		      decoder, &input, NULL) ==
		      SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT &&
	      sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_NEED_INPUT,
	      "V.44 decoder argument validation changed");
	input_length = 1;
	CHECK(sippymodem_v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT &&
	      sippymodem_v44_compressed_decoder_pending(NULL) == 0 &&
	      sippymodem_v44_compressed_decoder_read(NULL, &unchanged, 1) == 0 &&
	      sippymodem_v44_compressed_decoder_read(
		      decoder, &unchanged, 0) == 0 &&
	      unchanged == 0xa5,
	      "V.44 decoder accepted invalid input or output arguments");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      sippymodem_v44_compressed_result_name(
				      names[index].result),
			      names[index].name) == 0,
		      "V.44 result %zu has the wrong name", index);
	CHECK(strcmp(
		      sippymodem_v44_compressed_result_name(
			      (enum sippymodem_v44_compressed_result)99),
		      "unknown") == 0,
	      "unknown V.44 result has the wrong name");
	sippymodem_v44_compressed_decoder_destroy(decoder);
	sippymodem_v44_compressed_decoder_destroy(NULL);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "STEPUP limits", test_stepup_limits },
		{ "extension limit", test_extension_limit },
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
