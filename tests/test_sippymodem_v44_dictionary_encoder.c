// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_dictionary_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct sippymodem_v44_parameters parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
};

static bool append_octets(
	struct sippymodem_v44_dictionary_encoder *encoder,
	const uint8_t *values,
	size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		uint32_t history_index = UINT32_MAX;

		if (sippymodem_v44_dictionary_encoder_append(
			    encoder, values[index], &history_index) !=
			    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK ||
		    history_index != index)
			return false;
	}
	return true;
}

static bool test_history_and_node_insertion(void)
{
	struct sippymodem_v44_dictionary_encoder *encoder =
		sippymodem_v44_dictionary_encoder_create(&parameters);
	uint8_t history[40];
	struct sippymodem_v44_dictionary_extension extension = { 0 };
	uint32_t codeword = UINT32_MAX;

	for (size_t index = 0; index < sizeof(history); ++index)
		history[index] = (uint8_t)('A' + index % 26U);
	CHECK(encoder != NULL && append_octets(
				  encoder, history, sizeof(history)),
	      "could not initialize V.44 encoder dictionary history");
	CHECK(sippymodem_v44_dictionary_encoder_history_length(encoder) ==
		      sizeof(history) &&
	      sippymodem_v44_dictionary_encoder_next_codeword(encoder) == 4,
	      "V.44 encoder dictionary initial counters changed");
	CHECK(sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 4 &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, codeword, 2, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 5 &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 3, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 6 &&
	      sippymodem_v44_dictionary_encoder_next_codeword(encoder) == 7,
	      "V.44 encoder dictionary did not assign ordered codewords");
	CHECK(sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 5, 4, 29, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 7 &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, codeword, 34, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 0 && !extension.needs_more_input,
	      "V.44 encoder dictionary rejected a maximum-length string");
	codeword = UINT32_MAX;
	CHECK(sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 7, 33, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      codeword == UINT32_MAX &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 0, 0, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', sizeof(history), 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 3, 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 8, 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 1, 1, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_next_codeword(encoder) == 8,
	      "invalid V.44 node insertion changed dictionary state");
	sippymodem_v44_dictionary_encoder_destroy(encoder);
	return true;
}

static bool test_bounded_exhaustion_and_reset(void)
{
	enum { V44_FIRST_CODEWORD = 4 };
	struct sippymodem_v44_dictionary_encoder *encoder =
		sippymodem_v44_dictionary_encoder_create(&parameters);
	uint32_t history_index = UINT32_MAX;
	uint32_t codeword = UINT32_MAX;

	CHECK(encoder != NULL, "could not create bounded V.44 dictionary");
	for (uint32_t index = 0; index < parameters.history_size; ++index)
		CHECK(sippymodem_v44_dictionary_encoder_append(
			      encoder, (uint8_t)index, &history_index) ==
			      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
		      history_index == index,
		      "V.44 history filled early at %u", index);
	history_index = UINT32_MAX;
	CHECK(sippymodem_v44_dictionary_encoder_append(
		      encoder, 0, &history_index) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL &&
	      history_index == UINT32_MAX,
	      "full V.44 history accepted another octet");
	for (uint32_t index = V44_FIRST_CODEWORD;
	     index < parameters.codeword_limit; ++index)
		CHECK(sippymodem_v44_dictionary_encoder_add_root_segment(
			      encoder, 'A', 1, 1, &codeword) ==
			      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
		      codeword == index,
		      "V.44 node tree filled early at %u", index);
	codeword = UINT32_MAX;
	CHECK(sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL &&
	      codeword == UINT32_MAX,
	      "full V.44 node tree accepted another segment");
	CHECK(sippymodem_v44_dictionary_encoder_reinitialize(encoder) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      sippymodem_v44_dictionary_encoder_history_length(encoder) == 0 &&
	      sippymodem_v44_dictionary_encoder_next_codeword(encoder) ==
		      V44_FIRST_CODEWORD,
	      "V.44 encoder dictionary did not reset atomically");
	sippymodem_v44_dictionary_encoder_destroy(encoder);
	return true;
}

static bool expect_match(
	const struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t history_start,
	uint32_t available,
	uint32_t expected_codeword,
	uint32_t expected_length,
	bool expected_more)
{
	struct sippymodem_v44_dictionary_match match = {
		.codeword = UINT32_MAX,
		.length = UINT32_MAX,
		.needs_more_input = !expected_more,
	};

	CHECK(sippymodem_v44_dictionary_encoder_find_longest(
		      encoder, history_start, available, &match) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      match.codeword == expected_codeword &&
	      match.length == expected_length &&
	      match.needs_more_input == expected_more,
	      "unexpected V.44 match at history %u length %u: %u/%u/%u",
	      history_start, available, match.codeword, match.length,
	      match.needs_more_input ? 1U : 0U);
	return true;
}

static bool test_longest_match_lookup(void)
{
	static const uint8_t history[] = {
		'B', 'C', 'X', 'Y', 'Z', 'C', 'D', 'E',
		'A',
		'A', 'B',
		'A', 'B', 'C',
		'A', 'B', 'Q',
		'A', 'B', 'D',
		'Q',
		'B', 'C',
		'A', 'X',
		'A', 'X', 'Y',
		'A', 'X', 'Y', 'Z',
		'A', 'B', 'C', 'Q',
	};
	struct sippymodem_v44_dictionary_encoder *encoder =
		sippymodem_v44_dictionary_encoder_create(&parameters);
	struct sippymodem_v44_dictionary_match unchanged = {
		.codeword = UINT32_MAX,
		.length = UINT32_MAX,
		.needs_more_input = true,
	};
	uint32_t codeword = UINT32_MAX;

	CHECK(encoder != NULL && append_octets(encoder, history, sizeof(history)),
	      "could not initialize V.44 match history");
	CHECK(sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 0, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 4 &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 4, 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 5 &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'A', 2, 3, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 6 &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      encoder, 4, 6, 2, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 7 &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'B', 5, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 8,
	      "could not build V.44 match tree");
	CHECK(expect_match(encoder, 8, 1, 0, 0, true) &&
	      expect_match(encoder, 9, 2, 4, 2, true) &&
	      expect_match(encoder, 11, 3, 5, 3, false) &&
	      expect_match(encoder, 14, 3, 4, 2, false) &&
	      expect_match(encoder, 17, 3, 4, 2, true) &&
	      expect_match(encoder, 20, 1, 0, 0, false) &&
	      expect_match(encoder, 21, 2, 8, 2, false) &&
	      expect_match(encoder, 23, 2, 0, 0, true) &&
	      expect_match(encoder, 25, 3, 0, 0, true) &&
	      expect_match(encoder, 28, 4, 6, 4, false) &&
	      expect_match(encoder, 32, 4, 5, 3, false),
	      "V.44 longest-match traversal failed");
	CHECK(sippymodem_v44_dictionary_encoder_find_longest(
		      NULL, 0, 1, &unchanged) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_longest(
		      encoder, 0, 0, &unchanged) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_longest(
		      encoder, sizeof(history), 1, &unchanged) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_longest(
		      encoder, sizeof(history) - 1U, 2, &unchanged) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_longest(
		      encoder, 8, 1, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      unchanged.codeword == UINT32_MAX &&
	      unchanged.length == UINT32_MAX && unchanged.needs_more_input,
	      "invalid V.44 match lookup changed output");
	CHECK(sippymodem_v44_dictionary_encoder_reinitialize(encoder) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      append_octets(encoder, (const uint8_t *)"AB", 2) &&
	      expect_match(encoder, 0, 2, 0, 0, false),
	      "V.44 reset left an old match reachable");
	sippymodem_v44_dictionary_encoder_destroy(encoder);
	return true;
}

static bool test_extension_lookup(void)
{
	static const uint8_t repeated[] = "CCCCCCCCCCX";
	struct sippymodem_v44_dictionary_encoder *encoder =
		sippymodem_v44_dictionary_encoder_create(&parameters);
	struct sippymodem_v44_dictionary_extension extension = {
		.length = UINT32_MAX,
		.needs_more_input = false,
	};
	uint32_t codeword = UINT32_MAX;

	CHECK(encoder != NULL && append_octets(
				  encoder, repeated, sizeof(repeated) - 1U) &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      encoder, 'C', 1, 1, &codeword) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      codeword == 4,
	      "could not initialize repeated V.44 extension history");
	CHECK(sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 3, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 1 && extension.needs_more_input &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 3, 7, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 7 && extension.needs_more_input &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 3, 8, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 7 && !extension.needs_more_input &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 10, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 0 && !extension.needs_more_input &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 11, 0, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_OK &&
	      extension.length == 0 && extension.needs_more_input,
	      "V.44 extension lookup did not preserve overlapping history");
	extension = (struct sippymodem_v44_dictionary_extension){
		.length = UINT32_MAX,
		.needs_more_input = true,
	};
	CHECK(sippymodem_v44_dictionary_encoder_find_extension(
		      NULL, 4, 3, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 3, 3, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 5, 3, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 2, 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, sizeof(repeated), 1, &extension) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_find_extension(
		      encoder, 4, 3, 1, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      extension.length == UINT32_MAX && extension.needs_more_input,
	      "invalid V.44 extension lookup changed output");
	sippymodem_v44_dictionary_encoder_destroy(encoder);
	return true;
}

static bool test_creation_and_result_names(void)
{
	struct sippymodem_v44_parameters invalid = parameters;

	invalid.history_size--;
	CHECK(sippymodem_v44_dictionary_encoder_create(NULL) == NULL &&
	      sippymodem_v44_dictionary_encoder_create(&invalid) == NULL &&
	      sippymodem_v44_dictionary_encoder_reinitialize(NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_append(NULL, 0, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_root_segment(
		      NULL, 0, 0, 0, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_add_child_segment(
		      NULL, 0, 0, 0, NULL) ==
		      SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT &&
	      sippymodem_v44_dictionary_encoder_history_length(NULL) == 0 &&
	      sippymodem_v44_dictionary_encoder_next_codeword(NULL) == 0,
	      "V.44 encoder dictionary accepted an invalid owner");
	CHECK(strcmp(sippymodem_v44_dictionary_encoder_result_name(
			     SIPPYMODEM_V44_DICTIONARY_ENCODER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v44_dictionary_encoder_result_name(
			     SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL),
		     "full") == 0 &&
	      strcmp(sippymodem_v44_dictionary_encoder_result_name(
			     SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v44_dictionary_encoder_result_name(
			     (enum sippymodem_v44_dictionary_encoder_result)99),
		     "unknown") == 0,
	      "V.44 encoder dictionary result names changed");
	sippymodem_v44_dictionary_encoder_destroy(NULL);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "history and node insertion", test_history_and_node_insertion },
		{ "bounded exhaustion and reset",
		  test_bounded_exhaustion_and_reset },
		{ "longest match lookup", test_longest_match_lookup },
		{ "extension lookup", test_extension_lookup },
		{ "creation and result names", test_creation_and_result_names },
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
