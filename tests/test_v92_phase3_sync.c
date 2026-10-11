// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase3_sync.h>
#include <v92_pcm_phase3_trn1u.h>

#include "test.h"
#include "pcm_sign_state_fixture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TRN1U_SYMBOLS = V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS,
};

static bool sync_states_equal(
	const struct v92_phase3_sync *left, const struct v92_phase3_sync *right)
{
	return pcm_binary_sign_states_equal(&left->training, &right->training) &&
	       pcm_binary_sign_states_equal(
		       &left->window_start, &right->window_start) &&
	       left->trained_symbols == right->trained_symbols &&
	       memcmp(left->marker_signs, right->marker_signs,
		      sizeof(left->marker_signs)) == 0 &&
	       left->stage == right->stage &&
	       left->marker_index == right->marker_index &&
	       left->marker_count == right->marker_count;
}

struct sign_source {
	struct v34_scrambler scrambler;
	uint8_t previous_sign;
};

static bool source_init(struct sign_source *source)
{
	struct sign_source initialized = { 0 };

	if (source == NULL ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return false;
	*source = initialized;
	return true;
}

static bool source_trn1u(struct sign_source *source, uint8_t *sign)
{
	if (v34_scramble_group(
		    &source->scrambler, 1u, 1u, sign) != V34_FRAMING_OK)
		return false;
	source->previous_sign = *sign;
	return true;
}

static bool source_marker(
	struct sign_source *source, uint8_t bit, uint8_t *sign)
{
	uint8_t scrambled;

	if (v34_scramble_group(
		    &source->scrambler, bit, 1u, &scrambled) !=
	    V34_FRAMING_OK)
		return false;
	source->previous_sign ^= scrambled;
	*sign = source->previous_sign;
	return true;
}

static bool prepare_search(
	struct v92_phase3_sync *sync, struct sign_source *source,
	size_t search_start)
{
	if (search_start < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS ||
	    search_start >= TRN1U_SYMBOLS || !source_init(source) ||
	    v92_phase3_sync_init(sync) != V92_PHASE3_SYNC_OK)
		return false;
	for (size_t index = 0; index < TRN1U_SYMBOLS; ++index) {
		uint8_t sign;
		enum v92_phase3_sync_result result;

		if (!source_trn1u(source, &sign))
			return false;
		if (index < search_start) {
			result = v92_phase3_sync_train(sync, sign);
		} else {
			if (index == search_start &&
			    v92_phase3_sync_begin(sync) != V92_PHASE3_SYNC_OK)
				return false;
			result = v92_phase3_sync_receive(sync, sign);
		}
		if (result != V92_PHASE3_SYNC_OK)
			return false;
	}
	return sync->stage == V92_PHASE3_SYNC_SEARCHING;
}

static bool has_error(
	size_t index, const size_t *errors, size_t error_count)
{
	for (size_t error = 0; error < error_count; ++error) {
		if (index == errors[error])
			return true;
	}
	return false;
}

static bool run_marker(
	struct v92_phase3_sync *sync, struct sign_source *source,
	const size_t *errors, size_t error_count, bool expect_sync)
{
	struct pcm_binary_sign handoff;
	struct pcm_binary_sign saved = {
		.scrambler = { .history = UINT32_C(0x123456),
			       .polynomial = V34_POLYNOMIAL_GPC },
		.previous_sign = 1u,
	};

	handoff = saved;
	for (size_t index = 0; index < V92_PHASE3_MARKER_BITS; ++index) {
		uint8_t bit = index + 1u < V92_PHASE3_MARKER_BITS;
		uint8_t sign;
		enum v92_phase3_sync_result expected =
			expect_sync && index + 1u == V92_PHASE3_MARKER_BITS ?
				V92_PHASE3_SYNC_SYNCHRONIZED : V92_PHASE3_SYNC_OK;

		CHECK(source_marker(source, bit, &sign),
		      "sign source stopped at marker bit %zu", index);
		if (has_error(index, errors, error_count))
			sign ^= 1u;
		CHECK(v92_phase3_sync_receive(sync, sign) == expected,
		      "Phase 3 synchronization differed at marker bit %zu",
		      index);
	}
	if (!expect_sync) {
		CHECK(!v92_phase3_sync_is_complete(sync) &&
		      !v92_phase3_sync_handoff(sync, &handoff) &&
		      pcm_binary_sign_states_equal(&handoff, &saved),
		      "rejected Phase 3 marker changed its handoff");
		return true;
	}
	CHECK(v92_phase3_sync_is_complete(sync) &&
	      v92_phase3_sync_handoff(sync, &handoff) &&
	      handoff.scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	      handoff.scrambler.history == source->scrambler.history &&
	      handoff.previous_sign == source->previous_sign,
	      "Phase 3 synchronization did not repair its coding handoff");
	{
		struct v92_phase3_sync before = *sync;

		CHECK(v92_phase3_sync_receive(sync, 0u) ==
			      V92_PHASE3_SYNC_ALREADY_COMPLETE &&
		      sync_states_equal(sync, &before),
		      "complete Phase 3 synchronization consumed another sign");
	}
	return true;
}

static bool test_generated_boundaries(void)
{
	static const size_t one_error[] = { 17u };
	static const size_t three_errors[] = { 2u, 11u, 23u };
	static const struct {
		size_t search_start;
		const size_t *errors;
		size_t error_count;
	} cases[] = {
		{ V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS, NULL, 0u },
		{ 511u, one_error, ARRAY_LENGTH(one_error) },
		{ TRN1U_SYMBOLS - 1u, three_errors,
		  ARRAY_LENGTH(three_errors) },
	};

	CHECK(V92_PHASE3_PREAMBLE_BITS == 24 &&
	      V92_PHASE3_FRAME_SYNC_BITS == 17 && V92_PHASE3_MARKER_BITS == 42 &&
	      V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS == 23 &&
	      V92_PHASE3_SYNC_MAX_PREAMBLE_ERRORS == 3,
	      "Phase 3 synchronization dimensions changed");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v92_phase3_sync sync;
		struct sign_source source;

		CHECK(prepare_search(
			      &sync, &source, cases[index].search_start) &&
		      run_marker(
			      &sync, &source, cases[index].errors,
			      cases[index].error_count, true),
		      "generated Phase 3 boundary case %zu failed", index);
	}
	return true;
}

static bool check_rejected_marker(
	const size_t *errors, size_t error_count)
{
	struct v92_phase3_sync sync;
	struct sign_source source;

	return prepare_search(&sync, &source, 257u) &&
	       run_marker(&sync, &source, errors, error_count, false);
}

static bool test_marker_rejection(void)
{
	static const size_t four_preamble_errors[] = { 0u, 6u, 12u, 18u };
	static const size_t sync_error[] = {
		V92_PHASE3_PREAMBLE_BITS,
	};
	static const size_t start_error[] = { V92_PHASE3_MARKER_BITS - 1u };

	CHECK(check_rejected_marker(
		      four_preamble_errors,
		      ARRAY_LENGTH(four_preamble_errors)),
	      "Phase 3 marker accepted four preamble errors");
	CHECK(check_rejected_marker(sync_error, ARRAY_LENGTH(sync_error)),
	      "Phase 3 marker accepted a damaged frame-sync bit");
	CHECK(check_rejected_marker(start_error, ARRAY_LENGTH(start_error)),
	      "Phase 3 marker accepted a damaged start bit");
	return true;
}

static bool test_contracts(void)
{
	struct v92_phase3_sync sync;
	struct v92_phase3_sync before;
	static const char *const names[] = {
		"ok", "synchronized", "already complete", "invalid argument",
		"invalid state",
	};

	memset(&sync, 0xa5, sizeof(sync));
	before = sync;
	CHECK(v92_phase3_sync_init(NULL) == V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      memcmp(&sync, &before, sizeof(sync)) == 0 &&
	      v92_phase3_sync_init(&sync) == V92_PHASE3_SYNC_OK &&
	      v92_phase3_sync_is_valid(&sync) &&
	      !v92_phase3_sync_is_complete(&sync),
	      "Phase 3 synchronization initialization was not transactional");
	before = sync;
	CHECK(v92_phase3_sync_begin(&sync) == V92_PHASE3_SYNC_INVALID_STATE &&
	      v92_phase3_sync_train(NULL, 0u) == V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      v92_phase3_sync_train(&sync, 2u) ==
		      V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      v92_phase3_sync_receive(NULL, 0u) ==
		      V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      v92_phase3_sync_receive_descriptor(NULL, 0u) ==
		      V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      v92_phase3_sync_receive_descriptor(&sync, 2u) ==
		      V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      v92_phase3_sync_receive(&sync, 2u) ==
		      V92_PHASE3_SYNC_INVALID_ARGUMENT &&
	      sync_states_equal(&sync, &before),
	      "invalid Phase 3 synchronization call changed state");
	for (size_t index = 0;
	     index < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS; ++index)
		CHECK(v92_phase3_sync_train(&sync, (uint8_t)(index & 1u)) ==
			      V92_PHASE3_SYNC_OK,
		      "Phase 3 synchronization training stopped at %zu", index);
	CHECK(v92_phase3_sync_begin(&sync) == V92_PHASE3_SYNC_OK,
	      "Phase 3 synchronization did not begin searching");
	before = sync;
	CHECK(v92_phase3_sync_train(&sync, 0u) == V92_PHASE3_SYNC_INVALID_STATE &&
	      sync_states_equal(&sync, &before),
	      "Phase 3 synchronization trained after search began");
	sync.marker_signs[0] = 2u;
	before = sync;
	CHECK(!v92_phase3_sync_is_valid(&sync) &&
	      v92_phase3_sync_receive(&sync, 0u) == V92_PHASE3_SYNC_INVALID_STATE &&
	      v92_phase3_sync_receive_descriptor(&sync, 0u) ==
		      V92_PHASE3_SYNC_INVALID_STATE &&
	      sync_states_equal(&sync, &before),
	      "corrupt Phase 3 synchronization state was not rejected");
	CHECK(v92_phase3_sync_init(&sync) == V92_PHASE3_SYNC_OK,
	      "Phase 3 synchronization overflow fixture did not reset");
	sync.trained_symbols = UINT64_MAX;
	before = sync;
	CHECK(v92_phase3_sync_train(&sync, 0u) == V92_PHASE3_SYNC_INVALID_STATE &&
	      sync_states_equal(&sync, &before) &&
	      !v92_phase3_sync_is_complete(NULL) &&
	      !v92_phase3_sync_handoff(NULL, NULL),
	      "Phase 3 synchronization overflow/null contract differed");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase3_sync_result_name(
				      (enum v92_phase3_sync_result)index),
			      names[index]) == 0,
		      "Phase 3 synchronization result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase3_sync_result_name((enum v92_phase3_sync_result)-1),
		      "unknown") == 0,
	      "unknown Phase 3 synchronization result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated boundaries", test_generated_boundaries },
		{ "marker rejection", test_marker_rejection },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
