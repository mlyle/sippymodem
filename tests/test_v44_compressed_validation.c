// SPDX-License-Identifier: BSD-3-Clause

#include <v44_compressed_decoder.h>

#include "v44_test_wire.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	WIRE_CAPACITY = 64,
};

static const struct v44_parameters parameters = {
	.codeword_limit = V44_MIN_CODEWORDS,
	.max_string_length = V44_MIN_STRING_LENGTH,
	.history_size = V44_MIN_HISTORY_SIZE,
};

static bool finish_wire(
	struct v44_test_wire *builder,
	uint8_t wire[WIRE_CAPACITY],
	size_t *wire_length)
{
	bool finished = v44_test_wire_finish(
		builder, wire, WIRE_CAPACITY, wire_length);

	v44_test_wire_destroy(builder);
	return finished;
}

static bool process_to_error(
	struct v44_compressed_decoder *decoder,
	const uint8_t *wire,
	size_t wire_length,
	size_t accepted_codes,
	const uint8_t **remaining_input,
	size_t *remaining_length)
{
	const uint8_t *input = wire;
	size_t input_length = wire_length;

	for (size_t code = 0; code < accepted_codes; ++code)
		if (v44_compressed_decoder_process(
			    decoder, &input, &input_length) !=
		    V44_COMPRESSED_OK)
			return false;
	if (v44_compressed_decoder_process(
		    decoder, &input, &input_length) !=
	    V44_COMPRESSED_PROCEDURAL_ERROR)
		return false;
	*remaining_input = input;
	*remaining_length = input_length;
	return true;
}

static bool test_stepup_limits(void)
{
	struct v44_test_wire *builder;
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	uint8_t output = 0;
	const uint8_t *remaining;
	size_t wire_length = 0;
	size_t remaining_length;
	bool built;

	builder = v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	built = builder != NULL &&
		v44_test_wire_stepup_ordinal(builder) &&
		v44_test_wire_ordinal(builder, 0xff) &&
		v44_test_wire_control(
			builder, V44_TEST_STEPUP) &&
		v44_test_wire_put(builder, 0, 1);
	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive ordinal STEPUP wire");
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length) &&
	      v44_compressed_decoder_read(decoder, &output, 1) ==
		      1 &&
	      output == 0xff,
	      "V.44 decoder accepted excessive ordinal width");
	{
		const uint8_t *unchanged_input = remaining;
		size_t unchanged_length = remaining_length;

		CHECK(v44_compressed_decoder_process(
			      decoder, &remaining, &remaining_length) ==
			      V44_COMPRESSED_PROCEDURAL_ERROR &&
		      v44_compressed_decoder_finish(decoder) ==
			      V44_COMPRESSED_PROCEDURAL_ERROR &&
		      remaining == unchanged_input &&
		      remaining_length == unchanged_length,
		      "V.44 procedural error did not remain latched");
	}
	v44_compressed_decoder_destroy(decoder);

	builder = v44_test_wire_create(
		parameters.max_string_length, WIRE_CAPACITY);
	built = builder != NULL &&
		v44_test_wire_stepup_codeword(builder) &&
		v44_test_wire_stepup_codeword(builder) &&
		v44_test_wire_control(
			builder, V44_TEST_STEPUP) &&
		v44_test_wire_put(builder, 1, 1);
	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive codeword STEPUP wire");
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length) &&
	      v44_compressed_decoder_pending(decoder) == 0,
	      "V.44 decoder accepted excessive codeword width");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_extension_limit(void)
{
	struct v44_test_wire *builder =
		v44_test_wire_create(
			parameters.max_string_length, WIRE_CAPACITY);
	struct v44_compressed_decoder *decoder;
	uint8_t wire[WIRE_CAPACITY];
	const uint8_t *remaining;
	size_t wire_length = 0;
	size_t remaining_length;
	bool built = builder != NULL &&
		v44_test_wire_ordinal(builder, 'A') &&
		v44_test_wire_ordinal(builder, 'B') &&
		v44_test_wire_codeword(builder, 4) &&
		v44_test_wire_extension(builder, 31);

	built = finish_wire(builder, wire, &wire_length) && built;
	CHECK(built, "could not build excessive string-extension wire");
	decoder = v44_compressed_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL &&
	      process_to_error(
		      decoder, wire, wire_length, 3, &remaining,
		      &remaining_length),
	      "V.44 decoder accepted extension beyond negotiated N7");
	v44_compressed_decoder_destroy(decoder);
	return true;
}

static bool test_api_contract(void)
{
	static const struct {
		enum v44_compressed_result result;
		const char *name;
	} names[] = {
		{ V44_COMPRESSED_OK, "ok" },
		{ V44_COMPRESSED_NEED_INPUT, "need input" },
		{ V44_COMPRESSED_NEED_OUTPUT, "need output" },
		{ V44_COMPRESSED_FLUSHED, "flushed" },
		{ V44_COMPRESSED_ENTER_TRANSPARENT,
		  "enter transparent" },
		{ V44_COMPRESSED_INVALID_ARGUMENT,
		  "invalid argument" },
		{ V44_COMPRESSED_PROCEDURAL_ERROR,
		  "procedural error" },
	};
	struct v44_compressed_decoder *decoder =
		v44_compressed_decoder_create(
			&parameters, parameters.max_string_length);
	const uint8_t *input = NULL;
	size_t input_length = 0;
	uint8_t unchanged = 0xa5;

	CHECK(decoder != NULL, "could not create V.44 validation decoder");
	CHECK(v44_compressed_decoder_create(NULL, 32) == NULL &&
	      v44_compressed_decoder_create(&parameters, 31) == NULL &&
	      v44_compressed_decoder_finish(decoder) ==
		      V44_COMPRESSED_OK,
	      "V.44 decoder accepted invalid creation arguments");
	CHECK(v44_compressed_decoder_process(
		      NULL, &input, &input_length) ==
		      V44_COMPRESSED_INVALID_ARGUMENT &&
	      v44_compressed_decoder_process(
		      decoder, NULL, &input_length) ==
		      V44_COMPRESSED_INVALID_ARGUMENT &&
	      v44_compressed_decoder_process(
		      decoder, &input, NULL) ==
		      V44_COMPRESSED_INVALID_ARGUMENT &&
	      v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_NEED_INPUT,
	      "V.44 decoder argument validation changed");
	input_length = 1;
	CHECK(v44_compressed_decoder_process(
		      decoder, &input, &input_length) ==
		      V44_COMPRESSED_INVALID_ARGUMENT &&
	      v44_compressed_decoder_pending(NULL) == 0 &&
	      v44_compressed_decoder_read(NULL, &unchanged, 1) == 0 &&
	      v44_compressed_decoder_read(
		      decoder, &unchanged, 0) == 0 &&
	      unchanged == 0xa5,
	      "V.44 decoder accepted invalid input or output arguments");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v44_compressed_result_name(
				      names[index].result),
			      names[index].name) == 0,
		      "V.44 result %zu has the wrong name", index);
	CHECK(strcmp(
		      v44_compressed_result_name(
			      (enum v44_compressed_result)99),
		      "unknown") == 0,
	      "unknown V.44 result has the wrong name");
	v44_compressed_decoder_destroy(decoder);
	v44_compressed_decoder_destroy(NULL);
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
