// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_dictionary_decoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static struct sippymodem_v44_parameters minimum_parameters(void)
{
	return (struct sippymodem_v44_parameters){
		.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
}

static bool test_repeated_string_example(void)
{
	static const uint8_t expected[] = "CCCCCCCCCCX";
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder =
		sippymodem_v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 dictionary decoder");
	/* The standard's repeated-character example exercises the special case
	 * where codeword 4 is also the decoder's next uncreated codeword, then
	 * extends its two-character string through overlapping history copies. */
	CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 7) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'X') ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 repeated-string codes were rejected");
	CHECK(sippymodem_v44_dictionary_decoder_pending(decoder) ==
		      sizeof(actual) &&
	      sippymodem_v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 repeated-string example decoded incorrectly");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_string_creation_and_reuse(void)
{
	static const uint8_t expected[] = "ABABCABCABCA";
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder =
		sippymodem_v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 string fixture");
	/* A,B creates codeword 4 = AB. Reusing it and then receiving C creates
	 * codeword 6 = ABC; codeword 5 is the intervening BA append. Codeword 8
	 * is the next uncreated codeword, so it expands to ABC followed by A. */
	CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'A') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'B') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 6) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 8) ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 string creation sequence was rejected");
	CHECK(sippymodem_v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 reused strings decoded incorrectly");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
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
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder =
		sippymodem_v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[ARRAY_LENGTH(expected)] = { 0 };

	CHECK(decoder != NULL, "could not create V.44 mixed-string fixture");
	for (size_t index = 0; index < ARRAY_LENGTH(initial_ordinals); ++index)
		CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(
			      decoder, initial_ordinals[index]) ==
			      SIPPYMODEM_V44_DICTIONARY_OK,
		      "V.44 initial ordinal %zu was rejected", index);
	/* This is the standard's mixed-string decoding example: codeword 4 is
	 * AB, its three-character extension creates codeword 10 = ABCDE, and
	 * later ordinals exercise the append rules around that longer string. */
	CHECK(sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 3) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'Y') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 10) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 0xff) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'A') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'C') ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 mixed-string codes were rejected");
	CHECK(sippymodem_v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 mixed-string example decoded incorrectly");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_atomic_backpressure_and_wrap(void)
{
	enum {
		INITIAL_ORDINALS = 32,
		TRAILING_ORDINALS = 5,
		OUTPUT_COUNT = INITIAL_ORDINALS + 2 + TRAILING_ORDINALS + 2,
	};
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder =
		sippymodem_v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t expected[OUTPUT_COUNT];
	uint8_t actual[OUTPUT_COUNT];
	size_t actual_length = 0;

	CHECK(decoder != NULL, "could not create V.44 backpressure fixture");
	for (size_t index = 0; index < INITIAL_ORDINALS; ++index) {
		expected[index] = (uint8_t)(index + 1);
		CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(
			      decoder, expected[index]) ==
			      SIPPYMODEM_V44_DICTIONARY_OK,
		      "V.44 output queue filled early at %zu", index);
	}
	/* The first two ordinals created codeword 4. */
	CHECK(sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT &&
	      sippymodem_v44_dictionary_decoder_pending(decoder) ==
		      parameters.max_string_length,
	      "V.44 full output queue did not reject a complete codeword");
	actual_length += sippymodem_v44_dictionary_decoder_read(
		decoder, actual, 7);
	CHECK(sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 codeword did not resume after output was drained");
	expected[INITIAL_ORDINALS] = 1;
	expected[INITIAL_ORDINALS + 1] = 2;
	for (size_t index = 0; index < TRAILING_ORDINALS; ++index) {
		expected[INITIAL_ORDINALS + 2 + index] =
			(uint8_t)(INITIAL_ORDINALS + index + 1);
		CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(
			      decoder,
			      expected[INITIAL_ORDINALS + 2 + index]) ==
			      SIPPYMODEM_V44_DICTIONARY_OK,
		      "V.44 wrapped output queue filled early at %zu", index);
	}
	actual_length += sippymodem_v44_dictionary_decoder_read(
		decoder, actual + actual_length,
		ARRAY_LENGTH(actual) - actual_length);
	CHECK(actual_length == OUTPUT_COUNT - 2 &&
		      memcmp(actual, expected, OUTPUT_COUNT - 2) == 0,
	      "V.44 wrapped output queue changed byte ordering");
	/* The retried codeword created codeword 35 from byte 32 and the first
	 * character of codeword 4. */
	CHECK(sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 35) ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 backpressure changed dictionary assignment");
	expected[OUTPUT_COUNT - 2] = 32;
	expected[OUTPUT_COUNT - 1] = 1;
	actual_length += sippymodem_v44_dictionary_decoder_read(
		decoder, actual + actual_length,
		ARRAY_LENGTH(actual) - actual_length);
	CHECK(actual_length == OUTPUT_COUNT &&
		      memcmp(actual, expected, OUTPUT_COUNT) == 0,
	      "V.44 backpressure changed dictionary output");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_reinitialization_and_errors(void)
{
	static const uint8_t expected[] = "QQQQR";
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder =
		sippymodem_v44_dictionary_decoder_create(
			&parameters, parameters.max_string_length);
	uint8_t actual[sizeof(expected) - 1] = { 0 };
	uint8_t unchanged = 0xa5;

	CHECK(decoder != NULL, "could not create V.44 error fixture");
	CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'Q') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 5) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 1) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 4) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 31) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 1) ==
		      SIPPYMODEM_V44_DICTIONARY_OK,
	      "V.44 procedural error handling changed valid state");
	CHECK(sippymodem_v44_dictionary_decoder_reinitialize(decoder) ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 'R') ==
		      SIPPYMODEM_V44_DICTIONARY_OK &&
	      sippymodem_v44_dictionary_decoder_read(
		      decoder, actual, sizeof(actual)) == sizeof(actual) &&
	      memcmp(actual, expected, sizeof(actual)) == 0,
	      "V.44 reinitialization discarded pending output");
	CHECK(sippymodem_v44_dictionary_decoder_pending(NULL) == 0 &&
	      sippymodem_v44_dictionary_decoder_read(
		      decoder, &unchanged, 0) == 0 && unchanged == 0xa5 &&
	      sippymodem_v44_dictionary_decoder_read(
		      decoder, NULL, 1) == 0 &&
	      sippymodem_v44_dictionary_decoder_reinitialize(NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_decoder_decode_ordinal(NULL, 0) ==
		      SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_decoder_decode_codeword(decoder, 3) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_decode_extension(decoder, 0) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR,
	      "V.44 decoder accepted an invalid operation");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool consume_ordinal_and_drain(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t ordinal)
{
	uint8_t actual = 0;

	return sippymodem_v44_dictionary_decoder_decode_ordinal(
		       decoder, ordinal) == SIPPYMODEM_V44_DICTIONARY_OK &&
	       sippymodem_v44_dictionary_decoder_read(decoder, &actual, 1) == 1 &&
	       actual == ordinal;
}

static bool test_dictionary_and_history_bounds(void)
{
	struct sippymodem_v44_parameters parameters = minimum_parameters();
	struct sippymodem_v44_dictionary_decoder *decoder;

	decoder = sippymodem_v44_dictionary_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 dictionary-bound fixture");
	for (uint32_t index = 0;
	     index < parameters.codeword_limit - 3; ++index)
		CHECK(consume_ordinal_and_drain(decoder, (uint8_t)index),
		      "V.44 dictionary filled early at ordinal %u", index);
	CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 0) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_pending(decoder) == 0,
	      "V.44 full dictionary did not fail transactionally");
	sippymodem_v44_dictionary_decoder_destroy(decoder);

	parameters.codeword_limit = SIPPYMODEM_V44_MAX_CODEWORDS;
	decoder = sippymodem_v44_dictionary_decoder_create(
		&parameters, parameters.max_string_length);
	CHECK(decoder != NULL, "could not create V.44 history-bound fixture");
	for (uint32_t index = 0; index < parameters.history_size; ++index)
		CHECK(consume_ordinal_and_drain(decoder, (uint8_t)index),
		      "V.44 history filled early at ordinal %u", index);
	CHECK(sippymodem_v44_dictionary_decoder_decode_ordinal(decoder, 0) ==
		      SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR &&
	      sippymodem_v44_dictionary_decoder_pending(decoder) == 0,
	      "V.44 full history did not fail transactionally");
	sippymodem_v44_dictionary_decoder_destroy(decoder);
	return true;
}

static bool test_creation_validation_and_result_names(void)
{
	const struct sippymodem_v44_parameters parameters =
		minimum_parameters();

	CHECK(sippymodem_v44_dictionary_decoder_create(NULL, 32) == NULL &&
	      sippymodem_v44_dictionary_decoder_create(&parameters, 31) == NULL,
	      "V.44 decoder accepted invalid creation arguments");
	CHECK(strcmp(sippymodem_v44_dictionary_result_name(
			     SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT),
		     "need output") == 0 &&
	      strcmp(sippymodem_v44_dictionary_result_name(
			     SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR),
		     "procedural error") == 0 &&
	      strcmp(sippymodem_v44_dictionary_result_name(
			     (enum sippymodem_v44_dictionary_result)99),
		     "unknown") == 0,
	      "V.44 dictionary result names changed");
	sippymodem_v44_dictionary_decoder_destroy(NULL);
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
		{ "creation validation and result names",
		  test_creation_validation_and_result_names },
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
