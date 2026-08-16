// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v34_phase3_trn.h"
#include "v90_ja.h"
#include "v90_ja_search.h"

#include <stdint.h>
#include <string.h>

enum {
	SEARCH_START_TRN_SYMBOL = 480,
	JA_START_TRN_SYMBOL = 617,
	JA_SYMBOLS = V90_DIL_ZERO_DESCRIPTOR_BITS / 2,
};

static bool feed_rotation(
	struct v90_ja_search *search, uint8_t rotation,
	uint8_t phase_offset, enum v90_ja_search_result *event)
{
	enum v90_ja_search_result result = v90_ja_search_receive(
		search, (uint8_t)((rotation + phase_offset) & 3u));

	if (result == V90_JA_SEARCH_REJECTED ||
	    result == V90_JA_SEARCH_DETECTED)
		*event = result;
	return result != V90_JA_SEARCH_INVALID_ARGUMENT &&
	       result != V90_JA_SEARCH_COMPLETE;
}

static bool feed_trn(
	struct v34_phase3_trn *trn, struct v90_ja_search *search,
	unsigned int first, unsigned int count, uint8_t phase_offset,
	enum v90_ja_search_result *event)
{
	for (unsigned int symbol = first; symbol < count; ++symbol) {
		struct qam_point ignored;
		uint8_t rotation;

		if (v34_phase3_trn_next(trn, &ignored, &rotation) !=
			    V34_PHASE3_TRN_OK ||
		    (symbol >= SEARCH_START_TRN_SYMBOL &&
		     !feed_rotation(search, rotation, phase_offset, event)))
			return false;
	}
	return true;
}

static bool feed_ja(
	struct v90_ja_tx *tx, struct v90_ja_search *search,
	size_t symbols, uint8_t phase_offset, size_t corrupt_symbol,
	enum v90_ja_search_result *event)
{
	for (size_t symbol = 0; symbol < symbols; ++symbol) {
		struct qam_point ignored;
		uint8_t rotation;

		if (v90_ja_tx_next(tx, &ignored, &rotation) != V90_JA_OK)
			return false;
		if (symbol == corrupt_symbol)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		if (!feed_rotation(search, rotation, phase_offset, event))
			return false;
	}
	return true;
}

static bool run_first_descriptor(uint8_t phase_offset)
{
	struct v34_phase3_trn trn;
	struct v90_ja_tx tx;
	struct v90_ja_search search;
	enum v90_ja_search_result event = V90_JA_SEARCH_OK;

	CHECK(v34_phase3_trn_init(
		      &trn, V34_ANSWERING_STATION) == V34_PHASE3_TRN_OK &&
	      v90_ja_search_init(&search) == V90_JA_SEARCH_OK &&
	      feed_trn(
		      &trn, &search, 0, JA_START_TRN_SYMBOL,
		      phase_offset, &event) &&
	      event == V90_JA_SEARCH_OK &&
	      search.warmup_bits == V90_JA_SEARCH_WARMUP_BITS &&
	      v90_ja_tx_init_from_trn(&tx, &trn) == V90_JA_OK &&
	      feed_ja(
		      &tx, &search, JA_SYMBOLS * 2u, phase_offset, SIZE_MAX,
		      &event) &&
	      event == V90_JA_SEARCH_DETECTED &&
	      v90_ja_search_is_complete(&search) &&
	      v90_dil_decode_zero(
		      search.descriptor, sizeof(search.descriptor)) == V90_DIL_OK,
	      "Ja search phase offset %u did not detect a repeated descriptor",
	      phase_offset);
	return true;
}

static bool test_boundary_free_acquisition(void)
{
	for (uint8_t offset = 0; offset < 4u; ++offset)
		CHECK(run_first_descriptor(offset),
		      "Ja acquisition failed at phase offset %u", offset);
	return true;
}

static bool test_damaged_repetition_recovers(void)
{
	struct v34_phase3_trn trn;
	struct v90_ja_tx tx;
	struct v90_ja_search search;
	enum v90_ja_search_result event = V90_JA_SEARCH_OK;
	bool rejection_seen = false;

	CHECK(v34_phase3_trn_init(
		      &trn, V34_ANSWERING_STATION) == V34_PHASE3_TRN_OK &&
	      v90_ja_search_init(&search) == V90_JA_SEARCH_OK &&
	      feed_trn(
		      &trn, &search, 0, JA_START_TRN_SYMBOL,
		      3, &event) &&
	      v90_ja_tx_init_from_trn(&tx, &trn) == V90_JA_OK,
	      "damaged Ja fixture did not initialize");
	CHECK(feed_ja(
		      &tx, &search, JA_SYMBOLS, 3, SIZE_MAX, &event) &&
	      !v90_ja_search_is_complete(&search),
	      "Ja search did not consume its synchronization repetition");
	event = V90_JA_SEARCH_OK;
	for (size_t symbol = 0; symbol < JA_SYMBOLS; ++symbol) {
		enum v90_ja_search_result before = event;

		CHECK(feed_ja(
			      &tx, &search, 1, 3,
			      symbol == 61 ? 0 : SIZE_MAX, &event),
		      "damaged Ja stopped at symbol %zu", symbol);
		if (event == V90_JA_SEARCH_REJECTED && event != before)
			rejection_seen = true;
	}
	CHECK(rejection_seen && !v90_ja_search_is_complete(&search),
	      "damaged Ja repetition was not rejected");
	event = V90_JA_SEARCH_OK;
	CHECK(feed_ja(
		      &tx, &search, JA_SYMBOLS, 3, SIZE_MAX, &event) &&
	      event == V90_JA_SEARCH_DETECTED &&
	      v90_ja_search_is_complete(&search),
	      "Ja search did not recover on the next clean repetition");
	return true;
}

static uint8_t descriptor_dibit(
	const uint8_t *descriptor, size_t symbol)
{
	size_t bit = symbol * 2u;
	unsigned int shift = 6u - bit % 8u;

	return (uint8_t)((descriptor[bit / 8u] >> shift) & 3u);
}

static bool feed_descriptor(
	struct v34_scrambler *scrambler, uint8_t *previous_rotation,
	const uint8_t *descriptor, size_t descriptor_bits,
	struct v90_ja_search *search,
	enum v90_ja_search_result *event)
{
	if (descriptor_bits == 0u || descriptor_bits % 2u != 0u)
		return false;
	for (size_t symbol = 0; symbol < descriptor_bits / 2u; ++symbol) {
		uint8_t rotation;

		if (v34_scramble_differential_dibit(
			    scrambler, descriptor_dibit(descriptor, symbol),
			    *previous_rotation, &rotation) != V34_FRAMING_OK ||
		    !feed_rotation(search, rotation, 0, event))
			return false;
		*previous_rotation = rotation;
	}
	return true;
}

static bool test_variable_descriptor(void)
{
	struct v34_phase3_trn trn;
	struct v90_ja_search search;
	const struct v90_dil_parameters expected = {
		.segment_count = 144,
		.sign_pattern_bits = 120,
		.training_pattern_bits = 120,
	};
	struct v90_dil_parameters decoded;
	uint8_t descriptor[V90_DIL_MAX_DESCRIPTOR_BYTES];
	size_t descriptor_bits;
	uint8_t previous_rotation;
	enum v90_ja_search_result event = V90_JA_SEARCH_OK;

	CHECK(v34_phase3_trn_init(
		      &trn, V34_ANSWERING_STATION) == V34_PHASE3_TRN_OK &&
	      v90_ja_search_init(&search) == V90_JA_SEARCH_OK &&
		      feed_trn(
		      &trn, &search, 0, JA_START_TRN_SYMBOL,
		      0, &event) &&
	      v90_dil_encode(
		      &expected, descriptor, sizeof(descriptor),
		      &descriptor_bits) == V90_DIL_OK &&
	      descriptor_bits == 1702u,
	      "variable Ja fixture did not initialize");
	previous_rotation = trn.last_clockwise_quarter_turns;
	CHECK(feed_descriptor(
		      &trn.scrambler, &previous_rotation, descriptor,
		      descriptor_bits,
		      &search, &event) &&
	      !v90_ja_search_is_complete(&search),
	      "Ja search did not consume its synchronization repetition");
	event = V90_JA_SEARCH_OK;
	CHECK(feed_descriptor(
		      &trn.scrambler, &previous_rotation, descriptor,
		      descriptor_bits,
		      &search, &event) &&
	      event == V90_JA_SEARCH_DETECTED &&
	      v90_ja_search_is_complete(&search) &&
	      v90_ja_search_parameters(&search, &decoded) &&
	      memcmp(&decoded, &expected, sizeof(decoded)) == 0 &&
	      v90_ja_search_receive(&search, 0) == V90_JA_SEARCH_COMPLETE,
	      "Ja search did not retain a variable descriptor");
	return true;
}

static bool test_api_and_validation(void)
{
	struct v90_ja_search search;
	struct v90_ja_search saved;
	struct v90_dil_parameters parameters;
	struct v90_dil_parameters saved_parameters;

	memset(&search, 0xa5, sizeof(search));
	saved = search;
	CHECK(v90_ja_search_init(NULL) == V90_JA_SEARCH_INVALID_ARGUMENT &&
	      memcmp(&search, &saved, sizeof(search)) == 0 &&
	      v90_ja_search_init(&search) == V90_JA_SEARCH_OK &&
	      v90_ja_search_is_valid(&search) &&
	      !v90_ja_search_is_complete(&search),
	      "Ja search initialization changed");
	saved = search;
	memset(&parameters, 0xa5, sizeof(parameters));
	saved_parameters = parameters;
	CHECK(v90_ja_search_receive(NULL, 0) ==
		      V90_JA_SEARCH_INVALID_ARGUMENT &&
	      v90_ja_search_receive(&search, 4) ==
		      V90_JA_SEARCH_INVALID_ARGUMENT &&
	      memcmp(&search, &saved, sizeof(search)) == 0 &&
	      !v90_ja_search_parameters(NULL, &parameters) &&
	      !v90_ja_search_parameters(&search, NULL) &&
	      !v90_ja_search_parameters(&search, &parameters) &&
	      memcmp(&parameters, &saved_parameters, sizeof(parameters)) == 0,
	      "Ja search invalid call changed state");
	CHECK(v90_ja_search_receive(&search, 3) == V90_JA_SEARCH_OK &&
	      search.has_previous_rotation && search.warmup_bits == 0,
	      "Ja search did not seed differential phase");
	search.warmup_bits = V90_JA_SEARCH_WARMUP_BITS + 1u;
	CHECK(!v90_ja_search_is_valid(&search) &&
	      !v90_ja_search_is_complete(NULL) &&
	      strcmp(v90_ja_search_result_name(V90_JA_SEARCH_REJECTED),
		      "rejected") == 0 &&
	      strcmp(v90_ja_search_result_name(
		      (enum v90_ja_search_result)99),
		      "unknown") == 0,
	      "Ja search validation or names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "boundary-free acquisition", test_boundary_free_acquisition },
		{ "damaged repetition recovery", test_damaged_repetition_recovers },
		{ "variable descriptor retention", test_variable_descriptor },
		{ "API and validation", test_api_and_validation },
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
