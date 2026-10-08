// SPDX-License-Identifier: BSD-3-Clause

#include <v44_dictionary_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static struct v44_parameters minimum_parameters(void)
{
	return (struct v44_parameters){
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
}

static bool test_repeated_string_example(void)
{
	static const uint8_t expected[] = "CCCCCCCCCCX";
	const struct v44_parameters parameters =
		minimum_parameters();
	struct v44_dictionary_decoder *decoder =
		v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 dictionary decoder");
	/* The standard's repeated-character example exercises the special case
	 * where codeword 4 is also the decoder's next uncreated codeword, then
	 * extends its two-character string through overlapping history copies. */
	CHECK(v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_extension(decoder, 7) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'X') ==
		      V44_DICTIONARY_OK,
	      "V.44 repeated-string codes were rejected");
	CHECK(v44_dictionary_decoder_pending(decoder) ==
		      sizeof(actual) &&
	      v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 repeated-string example decoded incorrectly");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_string_creation_and_reuse(void)
{
	static const uint8_t expected[] = "ABABCABCABCA";
	const struct v44_parameters parameters =
		minimum_parameters();
	struct v44_dictionary_decoder *decoder =
		v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 string fixture");
	/* A,B creates codeword 4 = AB. Reusing it and then receiving C creates
	 * codeword 6 = ABC; codeword 5 is the intervening BA append. Codeword 8
	 * is the next uncreated codeword, so it expands to ABC followed by A. */
	CHECK(v44_dictionary_decoder_decode_ordinal(decoder, 'A') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'B') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 6) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 8) ==
		      V44_DICTIONARY_OK,
	      "V.44 string creation sequence was rejected");
	CHECK(v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 reused strings decoded incorrectly");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_mixed_string_example(void)
{
	static const uint8_t expected[] = {
		'A', 'B', 'C', 'D', 'E', 'X',
		'A', 'B', 'C', 'D', 'E', 'Y',
		'A', 'B', 'C', 'D', 'E', 0xff, 'A', 'C',
	};
	static const uint8_t initial_ordinals[] = {
		'A', 'B', 'C', 'D', 'E', 'X',
	};
	const struct v44_parameters parameters =
		minimum_parameters();
	struct v44_dictionary_decoder *decoder =
		v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[ARRAY_LENGTH(expected)] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 mixed-string fixture");
	for (size_t index = 0; index < ARRAY_LENGTH(initial_ordinals); ++index)
		CHECK(v44_dictionary_decoder_decode_ordinal(
			      decoder, initial_ordinals[index]) ==
			      V44_DICTIONARY_OK,
		      "V.44 initial ordinal %zu was rejected", index);
	/* This is the standard's mixed-string decoding example: codeword 4 is
	 * AB, its three-character extension creates codeword 10 = ABCDE, and
	 * later ordinals exercise the append rules around that longer string. */
	CHECK(v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_extension(decoder, 3) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'Y') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 10) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 0xff) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'A') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      V44_DICTIONARY_OK,
	      "V.44 mixed-string codes were rejected");
	CHECK(v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 mixed-string example decoded incorrectly");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_atomic_backpressure_and_wrap(void)
{
	enum {
		INITIAL_ORDINALS = 32,
		TRAILING_ORDINALS = 5,
		OUTPUT_COUNT = INITIAL_ORDINALS + 2 + TRAILING_ORDINALS + 2,
	};
	const struct v44_parameters parameters =
		minimum_parameters();
	struct v44_dictionary_decoder *decoder =
		v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t expected[OUTPUT_COUNT];
	uint8_t actual[OUTPUT_COUNT];
	size_t actual_length = 0;

	CHECK(decoder != NULL, "could not create V.44 backpressure fixture");
	for (size_t index = 0; index < INITIAL_ORDINALS; ++index) {
		expected[index] = (uint8_t)(index + 1);
		CHECK(v44_dictionary_decoder_decode_ordinal(
			      decoder, expected[index]) ==
			      V44_DICTIONARY_OK,
		      "V.44 output queue filled early at %zu", index);
	}
	/* The first two ordinals created codeword 4. */
	CHECK(v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_NEED_OUTPUT &&
	      v44_dictionary_decoder_pending(decoder) ==
		      parameters.max_string_length,
	      "V.44 full output queue did not reject a complete codeword");
	actual_length += v44_dictionary_decoder_read(
		decoder, actual, 7);
	CHECK(v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_OK,
	      "V.44 codeword did not resume after output was drained");
	expected[INITIAL_ORDINALS] = 1;
	expected[INITIAL_ORDINALS + 1] = 2;
	for (size_t index = 0; index < TRAILING_ORDINALS; ++index) {
		expected[INITIAL_ORDINALS + 2 + index] =
			(uint8_t)(INITIAL_ORDINALS + index + 1);
		CHECK(v44_dictionary_decoder_decode_ordinal(
			      decoder,
			      expected[INITIAL_ORDINALS + 2 + index]) ==
			      V44_DICTIONARY_OK,
		      "V.44 wrapped output queue filled early at %zu", index);
	}
	actual_length += v44_dictionary_decoder_read(
		decoder, actual + actual_length,
		ARRAY_LENGTH(actual) - actual_length);
	CHECK(actual_length == OUTPUT_COUNT - 2 &&
		      memcmp(actual, expected, OUTPUT_COUNT - 2) == 0,
	      "V.44 wrapped output queue changed byte ordering");
	/* The retried codeword created codeword 35 from byte 32 and the first
	 * character of codeword 4. */
	CHECK(v44_dictionary_decoder_decode_codeword(decoder, 35) ==
		      V44_DICTIONARY_OK,
	      "V.44 backpressure changed dictionary assignment");
	expected[OUTPUT_COUNT - 2] = 32;
	expected[OUTPUT_COUNT - 1] = 1;
	actual_length += v44_dictionary_decoder_read(
		decoder, actual + actual_length,
		ARRAY_LENGTH(actual) - actual_length);
	CHECK(actual_length == OUTPUT_COUNT &&
		      memcmp(actual, expected, OUTPUT_COUNT) == 0,
	      "V.44 backpressure changed dictionary output");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_reinitialization_and_errors(void)
{
	static const uint8_t expected[] = "QQQQR";
	const struct v44_parameters parameters =
		minimum_parameters();
	struct v44_dictionary_decoder *decoder =
		v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };
	uint8_t unchanged = 0xa5;

	CHECK(decoder != NULL, "could not create V.44 error fixture");
	CHECK(v44_dictionary_decoder_decode_ordinal(decoder, 'Q') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_codeword(decoder, 5) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_decode_extension(decoder, 1) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_extension(decoder, 31) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_decode_extension(decoder, 1) ==
		      V44_DICTIONARY_OK,
	      "V.44 procedural error handling changed valid state");
	CHECK(v44_dictionary_decoder_reinitialize(decoder) ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_decode_ordinal(decoder, 'R') ==
		      V44_DICTIONARY_OK &&
	      v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 reinitialization discarded pending output");
	CHECK(v44_dictionary_decoder_pending(NULL) == 0 &&
	      v44_dictionary_decoder_read(
		      decoder, &unchanged, 0) == 0 && unchanged == 0xa5 &&
	      v44_dictionary_decoder_read(
		      decoder, NULL, 1) == 0 &&
	      v44_dictionary_decoder_reinitialize(NULL) ==
		      V44_DICTIONARY_INVALID_ARGUMENT &&
	      v44_dictionary_decoder_decode_ordinal(NULL, 0) ==
		      V44_DICTIONARY_INVALID_ARGUMENT &&
	      v44_dictionary_decoder_decode_codeword(decoder, 3) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_decode_extension(decoder, 0) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR,
	      "V.44 decoder accepted an invalid operation");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool consume_ordinal_and_drain(
	struct v44_dictionary_decoder *decoder,
	uint8_t ordinal)
{
	uint8_t actual = 0;

	return v44_dictionary_decoder_decode_ordinal(
		       decoder, ordinal) == V44_DICTIONARY_OK &&
	       v44_dictionary_decoder_read(decoder, &actual, 1) == 1 &&
	       actual == ordinal;
}

static bool test_dictionary_and_history_bounds(void)
{
	struct v44_parameters parameters = minimum_parameters();
	struct v44_dictionary_decoder *decoder;

	decoder = v44_dictionary_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 dictionary-bound fixture");
	for (uint32_t index = 0;
	     index < parameters.codeword_limit - 3; ++index)
		CHECK(consume_ordinal_and_drain(decoder, (uint8_t)index),
		      "V.44 dictionary filled early at ordinal %u", index);
	CHECK(v44_dictionary_decoder_decode_ordinal(decoder, 0) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_pending(decoder) == 0,
	      "V.44 full dictionary did not fail transactionally");
	v44_dictionary_decoder_destroy(decoder);

	parameters.codeword_limit = V44_MAX_CODEWORDS;
	decoder = v44_dictionary_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 history-bound fixture");
	for (uint32_t index = 0; index < parameters.history_size; ++index)
		CHECK(consume_ordinal_and_drain(decoder, (uint8_t)index),
		      "V.44 history filled early at ordinal %u", index);
	CHECK(v44_dictionary_decoder_decode_ordinal(decoder, 0) ==
		      V44_DICTIONARY_PROCEDURAL_ERROR &&
	      v44_dictionary_decoder_pending(decoder) == 0,
	      "V.44 full history did not fail transactionally");
	v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_creation_validation(void)
{
	const struct v44_parameters parameters =
		minimum_parameters();

	CHECK(v44_dictionary_decoder_create(NULL, 32) == NULL &&
	      v44_dictionary_decoder_create(&parameters, 31) == NULL,
	      "V.44 decoder accepted invalid creation arguments");
	v44_dictionary_decoder_destroy(NULL);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "repeated-string example", test_repeated_string_example },
		{ "string creation and reuse", test_string_creation_and_reuse },
		{ "mixed-string example", test_mixed_string_example },
		{ "atomic backpressure and wrap", test_atomic_backpressure_and_wrap },
		{ "reinitialization and errors", test_reinitialization_and_errors },
		{ "dictionary and history bounds", test_dictionary_and_history_bounds },
		{ "creation validation",
		  test_creation_validation },
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
