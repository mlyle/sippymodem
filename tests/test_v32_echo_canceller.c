// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>
#include <echo_canceller.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	CONVERGENCE_WARMUP_SYMBOLS = 768,
	CONVERGENCE_MEASURE_SYMBOLS = 256,
	CONVERGENCE_TRAIN_SYMBOLS = 12000,
	FREEZE_SYMBOLS = 1024,
	RESUME_SYMBOLS = 4096,
	EQUIVALENCE_SYMBOLS = V32_ECHO_REFERENCE_CAPACITY + 400,
};

struct echo_source {
	struct qam_point reference[V32_ECHO_REFERENCE_CAPACITY];
	size_t write;
	size_t fill;
	uint16_t near_delay_symbols;
	uint16_t round_trip_symbols;
	double extra_echo;
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool generated_reference(
	uint32_t *random, struct qam_point *point)
{
	uint8_t quadrant = (uint8_t)(fixture_random(random) >> 30);

	return v32_4800_constellation_point(quadrant, point) ==
	       V32_CONSTELLATION_OK;
}

static void source_push(
	struct echo_source *source, const struct qam_point *point)
{
	source->reference[source->write] = *point;
	source->write++;
	if (source->write == V32_ECHO_REFERENCE_CAPACITY)
		source->write = 0;
	if (source->fill < V32_ECHO_REFERENCE_CAPACITY)
		source->fill++;
}

static struct qam_point source_at_age(
	const struct echo_source *source, size_t age)
{
	size_t index;

	if (age >= source->fill)
		return (struct qam_point){ 0 };
	index = source->write + V32_ECHO_REFERENCE_CAPACITY - 1u - age;
	if (index >= V32_ECHO_REFERENCE_CAPACITY)
		index -= V32_ECHO_REFERENCE_CAPACITY;
	return source->reference[index];
}

static double source_echo(
	const struct echo_source *source, size_t phase)
{
	size_t near = source->near_delay_symbols;
	size_t far = near + source->round_trip_symbols;
	struct qam_point near_a = source_at_age(source, near + 3u);
	struct qam_point near_b = source_at_age(source, near + 17u);
	struct qam_point far_a = source_at_age(source, far + 5u);
	struct qam_point far_b = source_at_age(source, far + 23u);
	struct qam_point extra = source_at_age(source, far + 31u);
	double echo;

	if (phase == 0) {
		echo = 2200.0 * near_a.in_phase -
		       1300.0 * near_b.quadrature +
		       900.0 * far_a.in_phase +
		       500.0 * far_b.quadrature;
	} else if (phase == 1) {
		echo = -1700.0 * near_a.quadrature +
		       750.0 * near_b.in_phase +
		       1400.0 * far_a.quadrature -
		       350.0 * far_b.in_phase;
	} else {
		echo = 1050.0 * near_a.in_phase +
		       1250.0 * near_b.quadrature -
		       800.0 * far_a.in_phase +
		       1100.0 * far_b.quadrature;
	}
	return echo + source->extra_echo * extra.in_phase;
}

static bool drive_symbol(
	struct v32_echo_canceller *canceller,
	struct echo_source *source, const struct qam_point *reference,
	const double far_signal[V32_ECHO_PHASE_COUNT],
	double cleaned[V32_ECHO_PHASE_COUNT], unsigned int fragmentation)
{
	double received[V32_ECHO_PHASE_COUNT];

	source_push(source, reference);
	CHECK(v32_echo_canceller_push_reference(canceller, reference) ==
		      V32_ECHO_OK,
	      "echo reference was not accepted");
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase)
		received[phase] = source_echo(source, phase) +
				  (far_signal == NULL ? 0.0 : far_signal[phase]);
	if (fragmentation == 0) {
		for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
			CHECK(v32_echo_canceller_process(
				      canceller, &received[phase], &cleaned[phase], 1) ==
				      V32_ECHO_OK,
			      "per-sample cancellation failed at phase %zu", phase);
		}
	} else if ((fragmentation & 1u) != 0) {
		CHECK(v32_echo_canceller_process(
			      canceller, received, cleaned, 1) == V32_ECHO_OK &&
		      v32_echo_canceller_process(
			      canceller, received + 1, cleaned + 1, 2) ==
			      V32_ECHO_OK,
		      "one-plus-two cancellation failed");
	} else {
		CHECK(v32_echo_canceller_process(
			      canceller, received, cleaned, 2) == V32_ECHO_OK &&
		      v32_echo_canceller_process(
			      canceller, received + 2, cleaned + 2, 1) ==
			      V32_ECHO_OK,
		      "two-plus-one cancellation failed");
	}
	return true;
}

static bool active_corruption_is_transactional(
	bool adapting, bool corrupt_reference)
{
	struct v32_echo_canceller canceller;
	struct v32_echo_canceller saved;
	struct qam_point reference = { 0.5, -0.25 };
	double received = 123.0;
	double cleaned = -456.0;
	double saved_cleaned = cleaned;
	size_t latest;

	CHECK(v32_echo_canceller_init(
		      &canceller, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK,
	      "corruption fixture did not initialize");
	if (adapting) {
		CHECK(v32_echo_canceller_start_adaptation(&canceller, 0.2) ==
			      V32_ECHO_OK,
		      "corruption fixture did not start adaptation");
	} else {
		CHECK(v32_echo_canceller_freeze(&canceller) == V32_ECHO_OK,
		      "corruption fixture did not freeze");
	}
	CHECK(v32_echo_canceller_push_reference(&canceller, &reference) ==
		      V32_ECHO_OK,
	      "corruption fixture rejected its reference");
	if (corrupt_reference) {
		latest = canceller.reference_write == 0 ?
			 V32_ECHO_REFERENCE_CAPACITY - 1u :
			 canceller.reference_write - 1u;
		canceller.reference[latest].in_phase = INFINITY;
	} else {
		canceller.coefficients[0][V32_ECHO_NEAR_WINDOW][0]
			.in_phase = INFINITY;
	}
	saved = canceller;
	CHECK(v32_echo_canceller_process(
		      &canceller, &received, &cleaned, 1) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      memcmp(&cleaned, &saved_cleaned, sizeof(cleaned)) == 0,
	      "%s active %s corruption was not transactional",
	      adapting ? "adaptive" : "frozen",
	      corrupt_reference ? "reference" : "coefficient");
	return true;
}

static bool test_initialization_and_reset(void)
{
	const uint16_t delays[] = { 0, V32_ECHO_MAX_NEAR_DELAY_SYMBOLS };
	const uint16_t trips[] = {
		V32_ECHO_MIN_ROUND_TRIP_SYMBOLS, V32_ECHO_MAX_ROUND_TRIP_SYMBOLS,
	};
	struct v32_echo_canceller canceller;
	struct v32_echo_canceller expected;
	struct qam_point reference = { 0.5, -0.25 };

	CHECK(v32_echo_canceller_init(NULL, 0, trips[0]) ==
		      V32_ECHO_INVALID_ARGUMENT, "null owner initialized");
	for (size_t delay = 0; delay < ARRAY_LENGTH(delays); ++delay) {
		for (size_t trip = 0; trip < ARRAY_LENGTH(trips); ++trip) {
			memset(&canceller, 0xa5, sizeof(canceller));
			memset(&expected, 0, sizeof(expected));
			expected.near_delay_symbols = delays[delay];
			expected.round_trip_symbols = trips[trip];
			CHECK(v32_echo_canceller_init(
				      &canceller, delays[delay], trips[trip]) ==
				      V32_ECHO_OK &&
			      v32_echo_canceller_is_valid(&canceller) &&
			      memcmp(&canceller, &expected, sizeof(expected)) == 0,
			      "boundary geometry did not initialize the complete owner");
			CHECK(v32_echo_canceller_start_adaptation(&canceller, 0.5) ==
				      V32_ECHO_OK &&
			      v32_echo_canceller_push_reference(&canceller, &reference) ==
				      V32_ECHO_OK, "reset fixture did not advance");
			CHECK(v32_echo_canceller_init(
				      &canceller, delays[delay], trips[trip]) ==
				      V32_ECHO_OK &&
			      v32_echo_canceller_is_valid(&canceller) &&
			      memcmp(&canceller, &expected, sizeof(expected)) == 0,
			      "reset retained pending history or adaptation state");
		}
	}
	return true;
}

static bool test_validation_and_aliases(void)
{
	struct v32_echo_canceller canceller;
	struct v32_echo_canceller saved;
	struct qam_point point = { 0.5, -0.25 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point overflowing = { DBL_MAX, DBL_MAX };
	double samples[] = { 100.0, 200.0, 300.0, 400.0 };
	double saved_samples[ARRAY_LENGTH(samples)];
	double separate[] = { -1.0, -2.0, -3.0 };
	double saved_separate[ARRAY_LENGTH(separate)];

	memset(&canceller, 0xa5, sizeof(canceller));
	saved = canceller;
	CHECK(v32_echo_canceller_init(
		      &canceller, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS - 1u) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      v32_echo_canceller_init(
		      &canceller, 0,
		      V32_ECHO_MAX_ROUND_TRIP_SYMBOLS + 1u) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      v32_echo_canceller_init(
		      &canceller, V32_ECHO_MAX_NEAR_DELAY_SYMBOLS + 1u,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0,
	      "invalid echo geometry changed the destination");
	CHECK(v32_echo_canceller_init(
		      &canceller, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK &&
	      v32_echo_canceller_is_valid(&canceller),
	      "minimum echo geometry was rejected");
	saved = canceller;
	CHECK(v32_echo_canceller_start_adaptation(&canceller, 0.0) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_canceller_start_adaptation(&canceller, 1.01) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_canceller_start_adaptation(&canceller, NAN) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_canceller_push_reference(&canceller, &invalid) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_canceller_push_reference(
		      &canceller, &overflowing) == V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_canceller_push_reference(
		      &canceller, &canceller.reference[0]) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0,
	      "invalid adaptation or aliased reference changed state");
	memcpy(saved_samples, samples, sizeof(samples));
	CHECK(v32_echo_canceller_process(
		      &canceller, samples, samples + 2, 1) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      memcmp(samples, saved_samples, sizeof(samples)) == 0,
	      "processing without a reference changed state or output");
	memcpy(saved_samples, samples, sizeof(samples));
	CHECK(v32_echo_canceller_process(
		      &canceller, samples, samples + 1, 2) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      memcmp(samples, saved_samples, sizeof(samples)) == 0,
	      "partially aliased processing was not transactional");
	CHECK(v32_echo_canceller_push_reference(&canceller, &point) ==
		      V32_ECHO_OK,
	      "valid reference was rejected");
	saved = canceller;
	CHECK(v32_echo_canceller_push_reference(&canceller, &point) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0,
	      "a second reference changed the pending symbol");
	samples[1] = NAN;
	memcpy(saved_samples, samples, sizeof(samples));
	CHECK(v32_echo_canceller_process(
		      &canceller, samples, samples + 2, 2) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      memcmp(samples, saved_samples, sizeof(samples)) == 0,
	      "non-finite processing was not transactional");
	samples[0] = 100.0;
	samples[1] = 200.0;
	samples[2] = 300.0;
	CHECK(v32_echo_canceller_process(
		      &canceller, samples, samples, 1) == V32_ECHO_OK &&
	      samples[0] == 100.0,
	      "exact in-place processing changed a zero predictor");
	saved = canceller;
	CHECK(v32_echo_canceller_freeze(&canceller) ==
		      V32_ECHO_INVALID_STATE &&
	      v32_echo_canceller_start_adaptation(&canceller, 0.2) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0,
	      "a mid-symbol adaptation transition changed state");
	memcpy(saved_samples, samples, sizeof(samples));
	memcpy(saved_separate, separate, sizeof(separate));
	CHECK(v32_echo_canceller_process(
		      &canceller, samples, separate, 3) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0 &&
	      memcmp(samples, saved_samples, sizeof(samples)) == 0 &&
	      memcmp(separate, saved_separate, sizeof(separate)) == 0,
	      "processing beyond the remaining phases changed state or output");
	CHECK(v32_echo_canceller_process(
		      &canceller, samples + 1, samples + 1, 2) == V32_ECHO_OK &&
	      samples[1] == 200.0 && samples[2] == 300.0,
	      "completing the in-place phase group changed a zero predictor");
	CHECK(v32_echo_canceller_push_reference(&canceller, &point) ==
		      V32_ECHO_OK,
	      "second valid reference was rejected");
	saved = canceller;
	CHECK(v32_echo_canceller_process(
		      &canceller, samples,
		      &canceller.coefficients[0][0][0].in_phase, 1) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(canceller)) == 0,
	      "owner-aliased output changed echo state");
	CHECK(v32_echo_canceller_init(
		      &canceller, V32_ECHO_MAX_NEAR_DELAY_SYMBOLS,
		      V32_ECHO_MAX_ROUND_TRIP_SYMBOLS) == V32_ECHO_OK,
	      "maximum echo geometry was rejected");
	canceller.reference_write = 1;
	CHECK(!v32_echo_canceller_is_valid(&canceller),
	      "inconsistent pre-wrap reference positions were accepted");
	canceller.reference_write = 0;
	canceller.coefficients[0][0][0].in_phase = INFINITY;
	CHECK(!v32_echo_canceller_is_valid(&canceller) &&
	      !v32_echo_canceller_is_valid(NULL),
	      "echo validation changed");
	CHECK(active_corruption_is_transactional(false, true) &&
	      active_corruption_is_transactional(false, false) &&
	      active_corruption_is_transactional(true, true) &&
	      active_corruption_is_transactional(true, false),
	      "active corruption contract failed");
	return true;
}

static bool run_convergence_freeze_and_resume(uint16_t round_trip)
{
	struct v32_echo_canceller canceller;
	struct echo_source source = {
		.near_delay_symbols = 4,
		.round_trip_symbols = round_trip,
	};
	struct qam_point frozen_coefficients[V32_ECHO_PHASE_COUNT]
					     [V32_ECHO_WINDOW_COUNT]
					     [V32_ECHO_WINDOW_SYMBOLS];
	uint32_t reference_random = UINT32_C(0x13579bdf);
	uint32_t far_random = UINT32_C(0x2468ace1);
	double initial_power = 0.0;
	double trained_power = 0.0;
	double frozen_error_power = 0.0;
	double frozen_echo_power = 0.0;
	double resume_start_power = 0.0;
	double resume_end_power = 0.0;
	size_t total_symbols = 0;
	size_t warmup = round_trip + CONVERGENCE_WARMUP_SYMBOLS;

	CHECK(v32_echo_canceller_init(
		      &canceller, source.near_delay_symbols,
		      source.round_trip_symbols) == V32_ECHO_OK,
	      "convergence fixture did not initialize");
	for (size_t symbol = 0;
	     symbol < warmup + CONVERGENCE_MEASURE_SYMBOLS;
	     ++symbol, ++total_symbols) {
		struct qam_point reference;
		double cleaned[V32_ECHO_PHASE_COUNT];

		CHECK(generated_reference(&reference_random, &reference) &&
		      drive_symbol(
			      &canceller, &source, &reference, NULL, cleaned,
			      (unsigned int)symbol + 1u),
		      "initial echo fixture stopped at symbol %zu", symbol);
		if (symbol >= warmup) {
			for (size_t phase = 0;
			     phase < V32_ECHO_PHASE_COUNT; ++phase)
				initial_power += cleaned[phase] * cleaned[phase];
		}
	}
	CHECK(v32_echo_canceller_start_adaptation(&canceller, 0.65) ==
		      V32_ECHO_OK,
	      "echo adaptation did not start");
	for (size_t symbol = 0; symbol < CONVERGENCE_TRAIN_SYMBOLS;
	     ++symbol, ++total_symbols) {
		struct qam_point reference;
		double cleaned[V32_ECHO_PHASE_COUNT];

		CHECK(generated_reference(&reference_random, &reference) &&
		      drive_symbol(
			      &canceller, &source, &reference, NULL, cleaned,
			      (unsigned int)symbol),
		      "adaptive echo fixture stopped at symbol %zu", symbol);
		if (symbol >= CONVERGENCE_TRAIN_SYMBOLS -
			      CONVERGENCE_MEASURE_SYMBOLS) {
			for (size_t phase = 0;
			     phase < V32_ECHO_PHASE_COUNT; ++phase)
				trained_power += cleaned[phase] * cleaned[phase];
		}
	}
	CHECK(initial_power > 0.0 &&
	      trained_power < initial_power * 1e-5,
	      "echo residual did not converge: initial=%.9g trained=%.9g",
	      initial_power, trained_power);
	CHECK(v32_echo_canceller_freeze(&canceller) == V32_ECHO_OK,
	      "trained echo canceller did not freeze");
	memcpy(frozen_coefficients, canceller.coefficients,
	       sizeof(frozen_coefficients));
	for (size_t symbol = 0; symbol < FREEZE_SYMBOLS;
	     ++symbol, ++total_symbols) {
		struct qam_point reference;
		double cleaned[V32_ECHO_PHASE_COUNT];
		double far[V32_ECHO_PHASE_COUNT];

		for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
			int32_t centered =
				(int32_t)(fixture_random(&far_random) >> 16) -
				32768;

			far[phase] = (double)centered / 32.0;
		}
		CHECK(generated_reference(&reference_random, &reference) &&
		      drive_symbol(
			      &canceller, &source, &reference, far, cleaned,
			      (unsigned int)symbol + 1u),
		      "frozen echo fixture stopped at symbol %zu", symbol);
		for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
			double error = cleaned[phase] - far[phase];
			double echo = source_echo(&source, phase);

			frozen_error_power += error * error;
			frozen_echo_power += echo * echo;
		}
	}
	CHECK(memcmp(
		      frozen_coefficients, canceller.coefficients,
		      sizeof(frozen_coefficients)) == 0 &&
	      frozen_echo_power > 0.0 &&
	      frozen_error_power < frozen_echo_power * 1e-5,
	      "frozen coefficients changed or stopped cancelling: %.9g/%.9g",
	      frozen_error_power, frozen_echo_power);
	source.extra_echo = 700.0;
	CHECK(v32_echo_canceller_start_adaptation(&canceller, 0.08) ==
		      V32_ECHO_OK &&
	      memcmp(
		      frozen_coefficients, canceller.coefficients,
		      sizeof(frozen_coefficients)) == 0,
	      "resuming adaptation reset learned coefficients");
	for (size_t symbol = 0; symbol < RESUME_SYMBOLS;
	     ++symbol, ++total_symbols) {
		struct qam_point reference;
		double cleaned[V32_ECHO_PHASE_COUNT];

		CHECK(generated_reference(&reference_random, &reference) &&
		      drive_symbol(
			      &canceller, &source, &reference, NULL, cleaned,
			      (unsigned int)symbol),
		      "resumed echo fixture stopped at symbol %zu", symbol);
		if (symbol < CONVERGENCE_MEASURE_SYMBOLS ||
		    symbol >= RESUME_SYMBOLS - CONVERGENCE_MEASURE_SYMBOLS) {
			double *power = symbol < CONVERGENCE_MEASURE_SYMBOLS ?
					&resume_start_power : &resume_end_power;

			for (size_t phase = 0;
			     phase < V32_ECHO_PHASE_COUNT; ++phase)
				*power += cleaned[phase] * cleaned[phase];
		}
	}
	CHECK(resume_start_power > 0.0 &&
	      resume_end_power < resume_start_power * 0.05 &&
	      canceller.reference_fill == V32_ECHO_REFERENCE_CAPACITY &&
	      canceller.reference_write ==
		      total_symbols % V32_ECHO_REFERENCE_CAPACITY,
	      "resumed echo path did not converge or wrap: %.9g/%.9g",
	      resume_end_power, resume_start_power);
	return true;
}

static bool test_convergence_freeze_and_resume(void)
{
	static const uint16_t delays[] = {
		73, 1212, V32_ECHO_MAX_ROUND_TRIP_SYMBOLS,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index)
		CHECK(run_convergence_freeze_and_resume(delays[index]),
		      "echo convergence failed at delay %u", delays[index]);
	return true;
}

static uint64_t hash_samples(
	uint64_t hash, const double samples[V32_ECHO_PHASE_COUNT])
{
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
		uint64_t bits;

		memcpy(&bits, &samples[phase], sizeof(bits));
		hash ^= bits;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

struct generated_run {
	struct v32_echo_canceller canceller;
	struct echo_source source;
	uint32_t random;
	uint64_t hash;
};

static bool run_one_generated_symbol(
	struct generated_run *run, unsigned int fragmentation)
{
	struct qam_point reference;
	double cleaned[V32_ECHO_PHASE_COUNT];

	CHECK(generated_reference(&run->random, &reference) &&
	      drive_symbol(
		      &run->canceller, &run->source, &reference, NULL,
		      cleaned, fragmentation),
	      "generated equivalence symbol failed");
	run->hash = hash_samples(run->hash, cleaned);
	return true;
}

static bool init_generated_run(
	struct generated_run *run, uint16_t near_delay,
	uint16_t round_trip, uint32_t seed)
{
	*run = (struct generated_run){
		.source = {
			.near_delay_symbols = near_delay,
			.round_trip_symbols = round_trip,
		},
		.random = seed,
		.hash = UINT64_C(1469598103934665603),
	};
	return v32_echo_canceller_init(
		       &run->canceller, near_delay, round_trip) ==
		       V32_ECHO_OK &&
	       v32_echo_canceller_start_adaptation(
		       &run->canceller, 0.45) == V32_ECHO_OK;
}

static bool test_reentrant_fragment_equivalence(void)
{
	struct generated_run sequential_min;
	struct generated_run sequential_max;
	struct generated_run interleaved_min;
	struct generated_run interleaved_max;

	CHECK(init_generated_run(
		      &sequential_min, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS,
		      UINT32_C(0x10203040)) &&
	      init_generated_run(
		      &sequential_max, V32_ECHO_MAX_NEAR_DELAY_SYMBOLS,
		      V32_ECHO_MAX_ROUND_TRIP_SYMBOLS,
		      UINT32_C(0x55667788)) &&
	      init_generated_run(
		      &interleaved_min, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS,
		      UINT32_C(0x10203040)) &&
	      init_generated_run(
		      &interleaved_max, V32_ECHO_MAX_NEAR_DELAY_SYMBOLS,
		      V32_ECHO_MAX_ROUND_TRIP_SYMBOLS,
		      UINT32_C(0x55667788)),
	      "equivalence fixtures did not initialize");
	for (size_t symbol = 0; symbol < EQUIVALENCE_SYMBOLS; ++symbol) {
		CHECK(run_one_generated_symbol(&sequential_min, 0),
		      "sequential minimum-delay run stopped at %zu", symbol);
	}
	for (size_t symbol = 0; symbol < EQUIVALENCE_SYMBOLS; ++symbol) {
		CHECK(run_one_generated_symbol(&sequential_max, 0),
		      "sequential maximum-delay run stopped at %zu", symbol);
	}
	for (size_t symbol = 0; symbol < EQUIVALENCE_SYMBOLS; ++symbol) {
		CHECK(run_one_generated_symbol(
			      &interleaved_min, (unsigned int)symbol + 1u) &&
		      run_one_generated_symbol(
			      &interleaved_max, (unsigned int)symbol + 2u),
		      "interleaved run stopped at %zu", symbol);
	}
	CHECK(sequential_min.hash == interleaved_min.hash &&
	      sequential_max.hash == interleaved_max.hash &&
	      memcmp(
		      &sequential_min.canceller,
		      &interleaved_min.canceller,
		      sizeof(sequential_min.canceller)) == 0 &&
	      memcmp(
		      &sequential_max.canceller,
		      &interleaved_max.canceller,
		      sizeof(sequential_max.canceller)) == 0,
	      "fragmentation or interleaving changed owned echo state");
	return true;
}

static bool check_point_validation(
	struct v32_echo_canceller *canceller, struct qam_point *point)
{
	static const double values[] = {
		0.0, -0.0, DBL_TRUE_MIN, -DBL_TRUE_MIN, DBL_MIN, -DBL_MIN,
		DBL_MAX, -DBL_MAX, INFINITY, -INFINITY, NAN, -NAN,
	};
	double *components[] = { &point->in_phase, &point->quadrature };

	for (size_t axis = 0; axis < ARRAY_LENGTH(components); ++axis) {
		for (size_t value = 0; value < ARRAY_LENGTH(values); ++value) {
			*components[axis] = values[value];
			CHECK(v32_echo_canceller_is_valid(canceller) ==
				      (isfinite(values[value]) != 0),
			      "echo point axis %zu value %zu validation changed",
			      axis, value);
		}
		*components[axis] = 0.0;
	}
	return true;
}

static bool test_complete_finite_state_validation(void)
{
	struct v32_echo_canceller canceller;

	CHECK(v32_echo_canceller_init(
		      &canceller, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK, "finite-state fixture did not initialize");
	/* Even unused history and frozen coefficients belong to the full-state
	 * validator; every component must participate in its reduction. */
	for (size_t index = 0; index < V32_ECHO_REFERENCE_CAPACITY; ++index)
		CHECK(check_point_validation(&canceller, &canceller.reference[index]),
		      "reference %zu escaped validation", index);
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase)
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window)
			for (size_t tap = 0; tap < V32_ECHO_WINDOW_SYMBOLS; ++tap)
				CHECK(check_point_validation(
					      &canceller,
					      &canceller.coefficients[phase][window][tap]),
				      "coefficient %zu/%zu/%zu escaped validation",
				      phase, window, tap);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initialization and reset", test_initialization_and_reset },
		{ "validation and aliases", test_validation_and_aliases },
		{ "complete finite-state validation",
		  test_complete_finite_state_validation },
		{ "convergence, freeze, and resume",
		  test_convergence_freeze_and_resume },
		{ "reentrant fragment equivalence",
		  test_reentrant_fragment_equivalence },
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
