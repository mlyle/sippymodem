// SPDX-License-Identifier: BSD-3-Clause

#include <v34_equalizer.h>

#include "test.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int16_t patterned_word(size_t index, uint32_t seed)
{
	uint32_t value = seed * UINT32_C(0x45d9f3b) +
			 (uint32_t)index * UINT32_C(0x9e3779b9);

	value ^= value >> 16;
	return (int16_t)(value + ((uint32_t)index << 7));
}

static void seed_equalizer(
	struct v34_equalizer *equalizer, uint32_t seed, uint32_t index)
{
	memset(equalizer, 0, sizeof(*equalizer));
	for (size_t tap = 0; tap < V34_EQUALIZER_TAPS; ++tap) {
		int16_t base = patterned_word(tap, seed + 1u);

		equalizer->in_phase_delay[tap] = base;
		equalizer->quadrature_delay[tap] =
			(int16_t)(base ^ (int16_t)0x55aa);
		equalizer->in_phase_coefficient[tap] =
			patterned_word(tap + 80u, seed + 2u);
		equalizer->quadrature_coefficient[tap] =
			patterned_word(tap + 160u, seed + 3u);
		equalizer->in_phase_residue[tap] =
			(uint16_t)patterned_word(tap + 240u, seed + 4u);
		equalizer->quadrature_residue[tap] =
			(uint16_t)patterned_word(tap + 320u, seed + 5u);
	}
	equalizer->index = index;
	equalizer->tap_count = V34_EQUALIZER_TAPS;
	equalizer->identity_tap = V34_EQUALIZER_IDENTITY_TAP;
	equalizer->initialized = true;
}

static uint32_t coefficient_word(int16_t coefficient, uint16_t residue)
{
	return ((uint32_t)(uint16_t)coefficient << 16) | residue;
}

static int16_t high_word(uint32_t value)
{
	uint16_t word = (uint16_t)(value >> 16);

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

static uint32_t product_low(int16_t left, int16_t right)
{
	return (uint32_t)((int32_t)left * right);
}

static void reference_filter(
	const struct v34_equalizer *equalizer,
	struct v34_equalizer_accumulator *output)
{
	uint32_t in_phase = 0;
	uint32_t quadrature = 0;

	for (size_t coefficient = 0;
	     coefficient < equalizer->tap_count; ++coefficient) {
		size_t sample = equalizer->index + coefficient;

		if (sample >= equalizer->tap_count)
			sample -= equalizer->tap_count;
		in_phase += product_low(equalizer->in_phase_delay[sample],
					equalizer->in_phase_coefficient[coefficient]);
		in_phase -= product_low(equalizer->quadrature_delay[sample],
					equalizer->quadrature_coefficient[coefficient]);
		quadrature += product_low(equalizer->quadrature_delay[sample],
					  equalizer->in_phase_coefficient[coefficient]);
		quadrature += product_low(equalizer->in_phase_delay[sample],
					  equalizer->quadrature_coefficient[coefficient]);
	}
	output->in_phase = (int32_t)in_phase;
	output->quadrature = (int32_t)quadrature;
}

static void reference_adapt_tap(
	struct v34_equalizer *equalizer,
	size_t sample, size_t coefficient,
	struct v34_analytic_sample error, bool preserve_residue)
{
	uint32_t in_phase = coefficient_word(
		equalizer->in_phase_coefficient[coefficient],
		preserve_residue ? equalizer->in_phase_residue[coefficient] : 0);
	uint32_t quadrature = coefficient_word(
		equalizer->quadrature_coefficient[coefficient],
		preserve_residue ? equalizer->quadrature_residue[coefficient] : 0);

	in_phase -= product_low(equalizer->in_phase_delay[sample], error.in_phase);
	in_phase -= product_low(equalizer->quadrature_delay[sample],
				error.quadrature);
	quadrature -= product_low(equalizer->in_phase_delay[sample],
				  error.quadrature);
	quadrature += product_low(equalizer->quadrature_delay[sample],
				  error.in_phase);
	if (!preserve_residue) {
		in_phase += UINT32_C(0x8000);
		quadrature += UINT32_C(0x8000);
	}
	equalizer->in_phase_coefficient[coefficient] = high_word(in_phase);
	equalizer->quadrature_coefficient[coefficient] = high_word(quadrature);
	if (preserve_residue) {
		equalizer->in_phase_residue[coefficient] = (uint16_t)in_phase;
		equalizer->quadrature_residue[coefficient] = (uint16_t)quadrature;
	}
}

static void reference_adapt(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error, bool center_only)
{
	if (center_only) {
		for (size_t offset = 0;
		     offset < V34_EQUALIZER_CENTER_TAPS; ++offset) {
			size_t coefficient = equalizer->identity_tap -
				V34_EQUALIZER_CENTER_TAPS / 2u + offset;
			size_t sample = equalizer->index + coefficient;

			if (sample >= equalizer->tap_count)
				sample -= equalizer->tap_count;
			reference_adapt_tap(
				equalizer, sample, coefficient, error, false);
		}
		return;
	}
	for (size_t coefficient = 0;
	     coefficient < equalizer->tap_count; ++coefficient) {
		size_t sample = equalizer->index + coefficient;

		if (sample >= equalizer->tap_count)
			sample -= equalizer->tap_count;
		reference_adapt_tap(
			equalizer, sample, coefficient, error, true);
	}
}

static bool test_lifecycle(void)
{
	struct v34_equalizer equalizer;
	struct v34_equalizer expected;

	memset(&equalizer, 0xa5, sizeof(equalizer));
	CHECK(v34_equalizer_init(&equalizer) == V34_EQUALIZER_OK &&
	      v34_equalizer_is_valid(&equalizer),
	      "equalizer did not initialize");
	memset(&expected, 0, sizeof(expected));
	expected.in_phase_coefficient[V34_EQUALIZER_IDENTITY_TAP] = 0x1000;
	expected.tap_count = V34_EQUALIZER_TAPS;
	expected.identity_tap = V34_EQUALIZER_IDENTITY_TAP;
	expected.initialized = true;
	CHECK(memcmp(&equalizer, &expected, sizeof(equalizer)) == 0,
	      "equalizer identity state changed");

	seed_equalizer(&equalizer, 0x43, 17);
	expected = equalizer;
	for (size_t tap = V34_EQUALIZER_CENTER_FIRST;
	     tap < V34_EQUALIZER_CENTER_FIRST + V34_EQUALIZER_CENTER_TAPS;
	     ++tap) {
		expected.in_phase_coefficient[tap] = 0;
		expected.quadrature_coefficient[tap] = 0;
	}
	CHECK(v34_equalizer_clear_center(&equalizer) == V34_EQUALIZER_OK &&
	      memcmp(&equalizer, &expected, sizeof(equalizer)) == 0,
	      "equalizer center clearing changed unrelated state");
	return true;
}

static bool test_filter_and_adaptation(void)
{
	static const uint32_t indices[] = { 0, 1, 17, 78, 79 };

	for (size_t iteration = 0;
	     iteration < ARRAY_LENGTH(indices); ++iteration) {
		struct v34_equalizer actual;
		struct v34_equalizer expected;
		struct v34_equalizer before;
		struct v34_equalizer_accumulator actual_output;
		struct v34_equalizer_accumulator expected_output;
		struct v34_analytic_sample sample = {
			.in_phase = (int16_t)(123 + iteration),
			.quadrature = (int16_t)(-456 - (int)iteration),
		};
		struct v34_analytic_sample error = {
			.in_phase = (int16_t)(0x421 + iteration * 37u),
			.quadrature = (int16_t)(-0x173 - (int)iteration * 19),
		};

		seed_equalizer(&actual, (uint32_t)(0x30 + iteration),
			       indices[iteration]);
		before = actual;
		reference_filter(&actual, &expected_output);
		CHECK(v34_equalizer_filter(&actual, &actual_output) ==
			      V34_EQUALIZER_OK &&
		      memcmp(&actual_output, &expected_output,
			     sizeof(actual_output)) == 0 &&
		      memcmp(&actual, &before, sizeof(actual)) == 0,
		      "equalizer filter changed at index %u",
		      indices[iteration]);

		expected = actual;
		expected.in_phase_delay[expected.index] = sample.in_phase;
		expected.quadrature_delay[expected.index] = sample.quadrature;
		expected.index = expected.index + 1u == V34_EQUALIZER_TAPS ?
					 0 : expected.index + 1u;
		CHECK(v34_equalizer_push(&actual, sample) == V34_EQUALIZER_OK &&
		      memcmp(&actual, &expected, sizeof(actual)) == 0,
		      "equalizer push changed at index %u", indices[iteration]);

		expected = actual;
		reference_adapt(&expected, error, false);
		CHECK(v34_equalizer_adapt(&actual, error) == V34_EQUALIZER_OK &&
		      memcmp(&actual, &expected, sizeof(actual)) == 0,
		      "full equalizer adaptation changed at index %u",
		      indices[iteration]);

		expected = actual;
		reference_adapt(&expected, error, true);
		CHECK(v34_equalizer_adapt_center(&actual, error) ==
			      V34_EQUALIZER_OK &&
		      memcmp(&actual, &expected, sizeof(actual)) == 0,
		      "center equalizer adaptation changed at index %u",
		      indices[iteration]);
	}
	return true;
}

static bool test_bounded_geometry(void)
{
	static const struct {
		uint16_t taps;
		uint16_t delay;
	} geometries[] = { { 80, 20 }, { 80, 16 }, { 128, 16 } };

	for (size_t geometry = 0; geometry < ARRAY_LENGTH(geometries);
	     ++geometry) {
		struct v34_equalizer actual;
		struct v34_equalizer expected;
		struct v34_equalizer saved;
		struct v34_equalizer_accumulator filtered;
		struct v34_equalizer_accumulator reference;
		uint16_t taps = geometries[geometry].taps;
		uint16_t delay = geometries[geometry].delay;

		CHECK(v34_equalizer_init_geometry(&actual, taps, delay) ==
			      V34_EQUALIZER_OK &&
		      v34_equalizer_is_valid(&actual) &&
		      actual.tap_count == taps &&
		      actual.identity_tap == taps - 2u * delay,
		      "geometry %u/%u did not initialize", taps, delay);
		for (size_t input = 0; input < taps * 2u; ++input) {
			struct v34_analytic_sample sample = {
				.in_phase = input == 0 ? 7000 : 0,
				.quadrature = input == 0 ? -2300 : 0,
			};
			bool impulse = input + 1u == 2u * delay;

			CHECK(v34_equalizer_push(&actual, sample) ==
				      V34_EQUALIZER_OK &&
			      v34_equalizer_filter(&actual, &filtered) ==
				      V34_EQUALIZER_OK &&
			      filtered.in_phase == (impulse ? 7000 * 4096 : 0) &&
			      filtered.quadrature == (impulse ? -2300 * 4096 : 0),
			      "geometry %u/%u changed output delay at %zu",
			      taps, delay, input);
		}
		for (size_t input = 0; input < taps + 1u; ++input) {
			struct v34_analytic_sample sample = {
				.in_phase = patterned_word(input, 0x15),
				.quadrature = patterned_word(input, 0x27),
			};
			struct v34_analytic_sample error = { 61, -43 };

			CHECK(v34_equalizer_push(&actual, sample) ==
				      V34_EQUALIZER_OK,
			      "geometry sample rejected");
			for (unsigned int center = 0; center < 2u; ++center) {
				expected = actual;
				reference_adapt(&expected, error, center != 0);
				CHECK((center ?
					       v34_equalizer_adapt_center(&actual, error) :
					       v34_equalizer_adapt(&actual, error)) ==
					      V34_EQUALIZER_OK &&
				      memcmp(&actual, &expected, sizeof(actual)) == 0,
				      "geometry %u/%u adaptation changed at %zu",
				      taps, delay, input);
			}
			reference_filter(&actual, &reference);
			CHECK(v34_equalizer_filter(&actual, &filtered) ==
				      V34_EQUALIZER_OK &&
			      memcmp(&filtered, &reference, sizeof(filtered)) == 0,
			      "geometry %u/%u filtering changed at %zu",
			      taps, delay, input);
		}
		expected = actual;
		for (size_t tap = actual.identity_tap - 4u;
		     tap < actual.identity_tap + 4u; ++tap) {
			expected.in_phase_coefficient[tap] = 0;
			expected.quadrature_coefficient[tap] = 0;
		}
		CHECK(v34_equalizer_clear_center(&actual) == V34_EQUALIZER_OK &&
		      memcmp(&actual, &expected, sizeof(actual)) == 0,
		      "geometry %u/%u cleared outside the acquisition span",
		      taps, delay);
		saved = actual;
		CHECK(v34_equalizer_init_geometry(&actual, 129, 16) ==
			      V34_EQUALIZER_INVALID_ARGUMENT &&
		      v34_equalizer_init_geometry(&actual, 80, 0) ==
			      V34_EQUALIZER_INVALID_ARGUMENT &&
		      v34_equalizer_init_geometry(&actual, 80, 39) ==
			      V34_EQUALIZER_INVALID_ARGUMENT &&
		      memcmp(&actual, &saved, sizeof(actual)) == 0,
		      "invalid geometry changed the trained owner");
	}
	return true;
}

static bool test_retained_history(void)
{
	static const uint16_t spans[] = { 8, 12, 80, 128 };
	static const struct v34_analytic_sample errors[] = {
		{ INT16_MIN, INT16_MIN }, { INT16_MAX, INT16_MIN },
		{ 0, -1 }, { 1234, -5678 },
	};

	for (size_t span = 0; span < ARRAY_LENGTH(spans); ++span) {
		uint16_t taps = spans[span];

		for (uint32_t index = 0; index < taps; ++index) {
			struct v34_equalizer live, retained, saved;
			struct v34_analytic_sample error = errors[index % ARRAY_LENGTH(errors)];

			seed_equalizer(&live, index + 1u, (index + 13u) % taps);
			live.tap_count = taps;
			live.identity_tap = taps - (taps > 32u ? 32u : 4u);
			retained = live;
			retained.index = index;
			for (size_t tap = 0; tap < V34_EQUALIZER_MAX_TAPS; ++tap) {
				retained.in_phase_delay[tap] = patterned_word(tap, index + 7u);
				retained.quadrature_delay[tap] = patterned_word(tap, index + 11u);
			}
			saved = retained;
			for (unsigned center = 0; center < 2; ++center) {
				struct v34_equalizer actual = live;
				struct v34_equalizer expected = retained;

				reference_adapt(&expected, error, center != 0);
				/* The reference computes against an old window; restore
				 * the live window before comparing the complete owner. */
				memcpy(expected.in_phase_delay, live.in_phase_delay,
				       sizeof(expected.in_phase_delay));
				memcpy(expected.quadrature_delay, live.quadrature_delay,
				       sizeof(expected.quadrature_delay));
				expected.index = live.index;
				CHECK(v34_equalizer_adapt_history(&actual,
					      retained.in_phase_delay, retained.quadrature_delay,
					      index, error, center != 0) == V34_EQUALIZER_OK &&
				      memcmp(&actual, &expected, sizeof(actual)) == 0 &&
				      memcmp(&retained, &saved, sizeof(retained)) == 0,
				      "retained history changed at span %u index %u center %u",
				      taps, index, center);
			}
		}
	}
	return true;
}

static bool test_transactionality(void)
{
	struct v34_equalizer equalizer;
	struct v34_equalizer saved;
	struct v34_equalizer_accumulator output = { 17, -23 };
	struct v34_equalizer_accumulator saved_output = output;
	struct v34_analytic_sample sample = { 1, 2 };
	int16_t history[V34_EQUALIZER_MAX_TAPS] = { 0 };

	seed_equalizer(&equalizer, 0x6b, 5);
	saved = equalizer;
	CHECK(v34_equalizer_init(NULL) == V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_filter(&equalizer, NULL) ==
		      V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_filter(
		      &equalizer,
		      (struct v34_equalizer_accumulator *)&equalizer) ==
		      V34_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "equalizer alias rejection was not transactional");
	CHECK(v34_equalizer_adapt_history(&equalizer, NULL, history, 0, sample, false) ==
		      V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_adapt_history(&equalizer, history, NULL, 0, sample, true) ==
		      V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_adapt_history(&equalizer, history, history,
		      equalizer.tap_count, sample, false) == V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_adapt_history(&equalizer, equalizer.in_phase_coefficient,
		      history, 0, sample, false) == V34_EQUALIZER_INVALID_ARGUMENT &&
	      v34_equalizer_adapt_history(&equalizer, history,
		      equalizer.quadrature_coefficient + 1, 0, sample, true) ==
		      V34_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(history, (const int16_t[V34_EQUALIZER_MAX_TAPS]){ 0 }, sizeof(history)) == 0,
	      "invalid retained history changed the owner or inputs");

	equalizer.index = V34_EQUALIZER_TAPS;
	saved = equalizer;
	CHECK(!v34_equalizer_is_valid(&equalizer) &&
	      v34_equalizer_clear_center(&equalizer) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      v34_equalizer_push(&equalizer, sample) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      v34_equalizer_filter(&equalizer, &output) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      v34_equalizer_adapt(&equalizer, sample) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      v34_equalizer_adapt_center(&equalizer, sample) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      v34_equalizer_adapt_history(&equalizer, history, history, 0, sample, false) ==
		      V34_EQUALIZER_INVALID_STATE &&
	      memcmp(&equalizer, &saved, sizeof(equalizer)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "invalid equalizer rejection changed state or output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "lifecycle", test_lifecycle },
		{ "filter and adaptation", test_filter_and_adaptation },
		{ "bounded geometry", test_bounded_geometry },
		{ "retained history", test_retained_history },
		{ "transactionality", test_transactionality },
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
