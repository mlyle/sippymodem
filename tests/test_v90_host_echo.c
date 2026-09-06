// SPDX-License-Identifier: BSD-3-Clause

#include <v90_host_echo.h>

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, ...)                                                \
	do {                                                                   \
		if (!(condition)) {                                               \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);            \
			fprintf(stderr, __VA_ARGS__);                              \
			fputc('\n', stderr);                                       \
			return false;                                              \
		}                                                              \
	} while (0)

enum {
	RETURN_DELAY = 5600,
	FILTER_DELAY = RETURN_DELAY - V90_HOST_ECHO_FILTER_PRE_SAMPLES,
	TRAIN_SAMPLES = 16000,
	VERIFY_SAMPLES = 4000,
	FALSE_TRAIN_SAMPLES = 8000,
	SHORT_RETURN_DELAY = 724,
	SHORT_PHASE2_DELAY = 324,
	SHORT_FILTER_DELAY =
		SHORT_RETURN_DELAY - V90_HOST_ECHO_FILTER_PRE_SAMPLES,
	SHORT_TRAIN_SAMPLES = 5000,
};

static int16_t generated_sample(size_t index, uint32_t salt, int16_t span)
{
	uint32_t value = (uint32_t)index + salt;

	value ^= value >> 15;
	value *= UINT32_C(0x2c1b3c6d);
	value ^= value >> 12;
	value *= UINT32_C(0x297a2d39);
	value ^= value >> 15;
	return (int16_t)((int32_t)(value % (uint32_t)(2 * span + 1)) -
			 span);
}

static int16_t transmit_sample(size_t index)
{
	return generated_sample(index, UINT32_C(0x6d2b79f5), 8000);
}

static int16_t scale_q15(int16_t sample, int16_t gain)
{
	return (int16_t)((int32_t)sample * gain / 32768);
}

static int16_t add_s16(int32_t value)
{
	return value > INT16_MAX ? INT16_MAX :
	       value < INT16_MIN ? INT16_MIN : (int16_t)value;
}

static int16_t echo_sample(size_t index)
{
	int32_t echo = 0;

	if (index >= RETURN_DELAY)
		echo += scale_q15(
			transmit_sample(index - RETURN_DELAY), 11000);
	if (index >= RETURN_DELAY + 7u)
		echo += scale_q15(
			transmit_sample(index - RETURN_DELAY - 7u), 4500);
	if (index >= RETURN_DELAY + 23u)
		echo -= scale_q15(
			transmit_sample(index - RETURN_DELAY - 23u), 2200);
	return add_s16(echo);
}

static int16_t short_echo_sample(size_t index)
{
	return index >= SHORT_RETURN_DELAY ?
		scale_q15(transmit_sample(index - SHORT_RETURN_DELAY), 11000) : 0;
}

static int16_t remote_sample(size_t index)
{
	return generated_sample(index, UINT32_C(0x9e3779b9), 500);
}

static uint64_t energy_add(uint64_t accumulated, int32_t sample)
{
	uint64_t energy = (uint64_t)((int64_t)sample * sample);

	return UINT64_MAX - accumulated < energy ?
		UINT64_MAX : accumulated + energy;
}

static bool echo_step(
	struct v90_host_echo *echo, enum v90_host_echo_mode mode,
	size_t index, int16_t received, int16_t *cleaned)
{
	return v90_host_echo_predict(echo, mode, received, cleaned) ==
		       V90_HOST_ECHO_OK &&
	       v90_host_echo_advance(
		       echo, mode, received, *cleaned,
		       transmit_sample(index)) == V90_HOST_ECHO_OK;
}

static bool test_contracts(void)
{
	struct v90_host_echo echo;
	struct v90_host_echo saved;
	int16_t cleaned = 1234;

	CHECK(v90_host_echo_init(NULL) == V90_HOST_ECHO_INVALID_ARGUMENT,
	      "null owner initialized");
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
	      v90_host_echo_is_valid(&echo) &&
	      !v90_host_echo_is_ready(&echo) &&
	      !v90_host_echo_has_weak_margin(&echo) &&
	      v90_host_echo_window_delay_samples(&echo) ==
		      V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES,
	      "ordinary owner did not initialize");
	CHECK(!v90_host_echo_is_ready(NULL) &&
	      !v90_host_echo_has_weak_margin(NULL) &&
	      v90_host_echo_window_delay_samples(NULL) == 0,
	      "null owner answered a lifecycle query");
	CHECK(v90_host_echo_predict(NULL, V90_HOST_ECHO_BYPASS, 17,
				    &cleaned) ==
		      V90_HOST_ECHO_INVALID_STATE && cleaned == 1234,
	      "null prediction changed output");
	CHECK(v90_host_echo_predict(
		      &echo, (enum v90_host_echo_mode)99, 17, &cleaned) ==
		      V90_HOST_ECHO_INVALID_ARGUMENT && cleaned == 1234,
	      "invalid mode changed prediction output");
	CHECK(v90_host_echo_predict(
		      &echo, V90_HOST_ECHO_BYPASS, 17, NULL) ==
		      V90_HOST_ECHO_INVALID_ARGUMENT,
	      "null prediction output was accepted");
	CHECK(v90_host_echo_predict(
		      &echo, V90_HOST_ECHO_BYPASS, 17, (int16_t *)&echo) ==
		      V90_HOST_ECHO_INVALID_ARGUMENT,
	      "owner/output overlap was accepted");
	CHECK(v90_host_echo_predict(
		      &echo, V90_HOST_ECHO_FILTER, 17, &cleaned) ==
		      V90_HOST_ECHO_OK && cleaned == 17,
	      "untrained filter changed input");
	saved = echo;
	CHECK(v90_host_echo_advance(
		      &echo, V90_HOST_ECHO_FILTER, 17, 18, 91) ==
		      V90_HOST_ECHO_SEQUENCE_MISMATCH &&
	      memcmp(&echo, &saved, sizeof(saved)) == 0,
	      "prediction mismatch changed owner");
	CHECK(v90_host_echo_advance(
		      &echo, (enum v90_host_echo_mode)99, 17, 17, 91) ==
		      V90_HOST_ECHO_INVALID_ARGUMENT &&
	      memcmp(&echo, &saved, sizeof(saved)) == 0,
	      "invalid advance mode changed owner");
	CHECK(v90_host_echo_advance(
		      &echo, V90_HOST_ECHO_FILTER, 17, 17, 91) ==
		      V90_HOST_ECHO_OK && echo.reference.fill == 1,
	      "matching prediction did not advance reference");
	echo.window_delay_samples = 0;
	CHECK(!v90_host_echo_is_valid(&echo) &&
	      v90_host_echo_predict(
		      &echo, V90_HOST_ECHO_BYPASS, 0, &cleaned) ==
		      V90_HOST_ECHO_INVALID_STATE,
	      "corrupt owner was accepted");
	return true;
}

static bool test_long_delay_cancellation(void)
{
	struct v90_host_echo echo;
	uint64_t echo_energy = 0;
	uint64_t residual_energy = 0;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "training owner did not initialize");
	for (size_t index = 0; index < TRAIN_SAMPLES; ++index) {
		int16_t received = echo_sample(index);
		int16_t cleaned;

		CHECK(echo_step(
			      &echo, V90_HOST_ECHO_TRAIN, index,
			      received, &cleaned),
		      "training failed at sample %zu", index);
		CHECK(cleaned == received,
		      "training filtered sample %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&echo) &&
	      !v90_host_echo_has_weak_margin(&echo),
	      "trained filter did not become ready: delay=%u train=%u peak=%u",
	      v90_host_echo_window_delay_samples(&echo),
	      echo.train_samples, echo.train_peak);
	CHECK(echo.delay_acquired &&
	      v90_host_echo_window_delay_samples(&echo) == FILTER_DELAY,
	      "long delay was not acquired: %u",
	      v90_host_echo_window_delay_samples(&echo));

	for (size_t index = TRAIN_SAMPLES;
	     index < TRAIN_SAMPLES + VERIFY_SAMPLES; ++index) {
		int16_t remote = remote_sample(index);
		int16_t returned = echo_sample(index);
		int16_t received = add_s16((int32_t)remote + returned);
		int16_t cleaned;
		int32_t error;

		CHECK(echo_step(
			      &echo, V90_HOST_ECHO_FILTER, index,
			      received, &cleaned),
		      "filtering failed at sample %zu", index);
		error = (int32_t)cleaned - remote;
		echo_energy = energy_add(echo_energy, returned);
		residual_energy = energy_add(residual_energy, error);
	}
	CHECK(residual_energy * 100u < echo_energy,
	      "echo suppression too small: residual=%" PRIu64
	      " source=%" PRIu64,
	      residual_energy, echo_energy);

	{
		struct v90_host_echo corrupt = echo;
		int16_t unchanged = 321;

		corrupt.coefficients[0] = NAN;
		CHECK(!v90_host_echo_is_valid(&corrupt) &&
		      v90_host_echo_predict(
			      &corrupt, V90_HOST_ECHO_FILTER, 0,
			      &unchanged) == V90_HOST_ECHO_NUMERIC_FAILURE &&
		      unchanged == 321,
		      "non-finite active model changed output");
	}
	{
		struct v90_host_echo fresh;
		int16_t cleaned = 321;

		CHECK(v90_host_echo_init(&fresh) == V90_HOST_ECHO_OK &&
		      v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
		      !v90_host_echo_is_ready(&echo) &&
		      memcmp(&echo, &fresh, sizeof(echo)) == 0,
		      "new training epoch retained old model or readiness");
		CHECK(v90_host_echo_predict(
			      &echo, V90_HOST_ECHO_FILTER, 1234, &cleaned) ==
			      V90_HOST_ECHO_OK && cleaned == 1234,
		      "reset model filtered input before new training");
		CHECK(v90_host_echo_seed_phase2_delay(
			      &echo, SHORT_PHASE2_DELAY) == V90_HOST_ECHO_OK &&
		      v90_host_echo_window_delay_samples(&echo) ==
			      SHORT_FILTER_DELAY,
		      "new training epoch rejected a fresh ranging delay");
	}
	return true;
}

static bool test_phase2_delay_seed(void)
{
	struct v90_host_echo seeded;
	struct v90_host_echo unseeded;
	struct v90_host_echo saved;

	CHECK(v90_host_echo_init(&seeded) == V90_HOST_ECHO_OK &&
	      v90_host_echo_init(&unseeded) == V90_HOST_ECHO_OK,
	      "delay-seed owners did not initialize");
	saved = seeded;
	CHECK(v90_host_echo_seed_phase2_delay(NULL, SHORT_PHASE2_DELAY) ==
		      V90_HOST_ECHO_INVALID_STATE &&
	      v90_host_echo_seed_phase2_delay(&seeded, 0) ==
		      V90_HOST_ECHO_INVALID_ARGUMENT &&
	      memcmp(&seeded, &saved, sizeof(saved)) == 0,
	      "invalid Phase-2 delay changed owner");
	CHECK(v90_host_echo_seed_phase2_delay(
		      &seeded, SHORT_PHASE2_DELAY) == V90_HOST_ECHO_OK &&
	      v90_host_echo_window_delay_samples(&seeded) ==
		      SHORT_FILTER_DELAY,
	      "Phase-2 delay did not seed host echo: %u",
	      v90_host_echo_window_delay_samples(&seeded));
	CHECK(seeded.reference.scan_required_fill >=
		      PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES,
	      "short seed narrowed long-delay coverage: %u",
	      seeded.reference.scan_required_fill);

	for (size_t index = 0; index < SHORT_TRAIN_SAMPLES; ++index) {
		int16_t received = short_echo_sample(index);
		int16_t seeded_cleaned;
		int16_t unseeded_cleaned;

		CHECK(echo_step(
			      &seeded, V90_HOST_ECHO_TRAIN, index,
			      received, &seeded_cleaned) &&
		      echo_step(
			      &unseeded, V90_HOST_ECHO_TRAIN, index,
			      received, &unseeded_cleaned),
		      "delay-seed training failed at sample %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&seeded) &&
	      !v90_host_echo_is_ready(&unseeded),
	      "Phase-2 seed did not accelerate acquisition: seeded=%d "
	      "unseeded=%d",
	      v90_host_echo_is_ready(&seeded),
	      v90_host_echo_is_ready(&unseeded));
	saved = seeded;
	CHECK(v90_host_echo_seed_phase2_delay(
		      &seeded, SHORT_PHASE2_DELAY) ==
		      V90_HOST_ECHO_INVALID_STATE &&
	      memcmp(&seeded, &saved, sizeof(saved)) == 0,
	      "trained owner accepted a new Phase-2 delay");
	CHECK(v90_host_echo_init(&seeded) == V90_HOST_ECHO_OK &&
	      v90_host_echo_seed_phase2_delay(
		      &seeded, PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES +
			       V90_HOST_ECHO_FILTER_PRE_SAMPLES) ==
		      V90_HOST_ECHO_OK && v90_host_echo_is_valid(&seeded) &&
	      seeded.window_delay_samples ==
		      PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES,
	      "last representable measured delay was not retained");
	CHECK(v90_host_echo_init(&seeded) == V90_HOST_ECHO_OK &&
	      v90_host_echo_seed_phase2_delay(&seeded, UINT32_MAX) ==
		      V90_HOST_ECHO_OK && v90_host_echo_is_valid(&seeded) &&
	      seeded.window_delay_samples ==
		      V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES,
	      "out-of-history ranging broke the default search");
	return true;
}

static bool test_long_phase2_delay_seed(void)
{
	static const uint32_t delays[] = { 2640, 3128, 5600 };
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	const size_t training = 4000;

	for (size_t trial = 0; trial < sizeof(delays) / sizeof(delays[0]);
	     ++trial) {
		struct v90_host_echo echo;
		uint32_t delay = delays[trial];
		uint64_t echo_energy = 0;
		uint64_t residual_energy = 0;

		CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
		      v90_host_echo_seed_phase2_delay(&echo, delay) ==
			      V90_HOST_ECHO_OK,
		      "long ranging seed failed: %u", delay);
		/* Phase 2 has already supplied TX history. A measured return inside
		 * that history should train without first searching from the default
		 * short delay; readiness must still require actual correlated echo. */
		for (size_t index = 0; index < history + training; ++index) {
			bool train = index >= history;
			int16_t received = train ? scale_q15(
				transmit_sample(index - delay), 11000) : 0;
			int16_t cleaned;

			CHECK(echo_step(&echo, train ? V90_HOST_ECHO_TRAIN :
						     V90_HOST_ECHO_BYPASS,
					index, received, &cleaned) &&
			      cleaned == received,
			      "long seed changed training input: %u/%zu", delay, index);
			CHECK(train || !v90_host_echo_is_ready(&echo),
			      "ranging alone qualified an echo filter: %u", delay);
		}
		CHECK(v90_host_echo_is_ready(&echo) && !echo.delay_acquired &&
		      echo.window_delay_samples ==
			      delay - V90_HOST_ECHO_FILTER_PRE_SAMPLES,
		      "long measured delay needed a fresh search: %u ready=%d window=%u",
		      delay, v90_host_echo_is_ready(&echo), echo.window_delay_samples);
		for (size_t index = history + training;
		     index < history + training + VERIFY_SAMPLES; ++index) {
			int16_t remote = remote_sample(index);
			int16_t returned = scale_q15(
				transmit_sample(index - delay), 11000);
			int16_t cleaned;

			CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, index,
					add_s16((int32_t)remote + returned), &cleaned),
			      "long seeded filtering failed: %u/%zu", delay, index);
			echo_energy = energy_add(echo_energy, returned);
			residual_energy = energy_add(
				residual_energy, (int32_t)cleaned - remote);
		}
		CHECK(residual_energy * 100u < echo_energy,
		      "long seeded suppression too small: %u residual=%" PRIu64
		      " source=%" PRIu64, delay, residual_energy, echo_energy);
	}
	return true;
}

static bool test_weak_joint_margin(void)
{
	struct v90_host_echo echo;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "weak-margin owner did not initialize");
	for (size_t index = 0; index < TRAIN_SAMPLES; ++index) {
		int16_t unrelated = generated_sample(
			index, UINT32_C(0x4f1bbcdc), 1400);
		int16_t received = add_s16(
			(int32_t)echo_sample(index) + unrelated);
		int16_t cleaned;

		CHECK(echo_step(
			      &echo, V90_HOST_ECHO_TRAIN, index,
			      received, &cleaned),
		      "weak-margin training failed at sample %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&echo) &&
	      v90_host_echo_has_weak_margin(&echo),
	      "joint echo model did not retain weak margin: error=%" PRIu64
	      " input=%" PRIu64,
	      echo.error_energy, echo.input_energy);
	return true;
}

static bool test_decimated_adjacent_correlation(void)
{
	struct v90_host_echo echo;
	struct pcm_echo_reference *reference = &echo.reference;
	uint32_t adjacent;
	int16_t cleaned;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "decimated-correlation owner did not initialize");
	for (size_t index = 0;
	     index < reference->scan_required_fill;
	     ++index)
		CHECK(pcm_echo_reference_push(reference, 100) ==
			      PCM_ECHO_REFERENCE_OK,
		      "decimated-correlation reference fill failed");

	adjacent = reference->scan_current_index + 1u;
	CHECK(adjacent < reference->scan_count,
	      "decimated-correlation scan has no adjacent delay");
	echo.coefficients[0] = 1.0f;
	echo.reference_energy = UINT64_C(1000000000);
	echo.input_energy = UINT64_C(1000000000);
	echo.error_energy = 0;
	echo.cross_energy = 0;
	echo.quiet_samples = V90_HOST_ECHO_QUIET_SAMPLES - 1u;
	echo.train_samples = V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES - 1u;
	echo.train_peak = 100;
	echo.delay_acquired = true;
	reference->scan_reference_energy[adjacent] = UINT64_C(1000000000);
	reference->scan_input_energy = UINT64_C(1000000000);
	reference->scan_cross_energy[adjacent] = INT64_C(1000000000);
	reference->scan_samples =
		V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES /
			V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES -
		1u;

	CHECK(echo_step(
		      &echo, V90_HOST_ECHO_TRAIN,
		      reference->scan_required_fill,
		      100, &cleaned),
	      "decimated adjacent-correlation step failed");
	CHECK(cleaned == 100 &&
	      reference->scan_samples < echo.train_samples &&
	      (uint64_t)reference->scan_samples *
			      V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES >=
		      echo.train_samples &&
	      v90_host_echo_is_ready(&echo),
	      "full-rate training rejected covered decimated correlation: "
	      "scan=%u train=%u ready=%d",
	      reference->scan_samples, echo.train_samples,
	      v90_host_echo_is_ready(&echo));
	return true;
}

static bool test_unrelated_input_does_not_train(void)
{
	struct v90_host_echo echo;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "noise owner did not initialize");
	for (size_t index = 0; index < FALSE_TRAIN_SAMPLES; ++index) {
		int16_t input = generated_sample(
			index, UINT32_C(0x31415927), 1000);
		int16_t cleaned;

		CHECK(echo_step(
			      &echo, V90_HOST_ECHO_TRAIN, index,
			      input, &cleaned),
		      "noise training failed at sample %zu", index);
	}
	CHECK(!v90_host_echo_is_ready(&echo) && !echo.delay_acquired,
	      "uncorrelated input produced a ready model: delay=%u corr=%" PRIi64,
	      echo.window_delay_samples, echo.cross_energy);
	CHECK(echo.reference.scan_samples != 0 &&
	      echo.reference.scan_samples <=
		      FALSE_TRAIN_SAMPLES /
		      V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES,
	      "delay scan was not decimated: %u observations",
	      echo.reference.scan_samples);
	return true;
}

static bool test_acquisition_after_unrelated_prefix(void)
{
	struct v90_host_echo echo;
	const size_t prefix = V90_HOST_ECHO_TRAIN_WINDOW_SAMPLES +
		V90_HOST_ECHO_QUIET_SAMPLES;
	const size_t training = 3u * prefix;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	uint64_t echo_energy = 0;
	uint64_t residual_energy = 0;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "prefix owner did not initialize");
	/* The peer can still be transmitting strongly when local TRN1d starts.
	 * Keep ordinary TX history, then let that unrelated input subside after
	 * one rejected training window. The later echo has a different delay
	 * from our initial hypothesis and must be acquired from fresh evidence. */
	for (size_t index = 0; index < history + training; ++index) {
		int16_t received = echo_sample(index);
		int16_t cleaned;
		enum v90_host_echo_mode mode = index < history ?
			V90_HOST_ECHO_BYPASS : V90_HOST_ECHO_TRAIN;

		if (index >= history && index < history + prefix)
			received = add_s16((int32_t)received + generated_sample(
				index, UINT32_C(0x31415927), 30000));
		CHECK(echo_step(&echo, mode, index, received, &cleaned),
		      "prefix training failed at sample %zu", index);
		CHECK(cleaned == received,
		      "unqualified/training sample was filtered at %zu", index);
		if (index < history + prefix)
			CHECK(!v90_host_echo_is_ready(&echo),
			      "unrelated prefix qualified a model at %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&echo) && echo.delay_acquired &&
	      echo.window_delay_samples == FILTER_DELAY,
	      "clean training retained rejected correlation: ready=%d delay=%u",
	      v90_host_echo_is_ready(&echo), echo.window_delay_samples);
	for (size_t index = history + training;
	     index < history + training + VERIFY_SAMPLES; ++index) {
		int16_t remote = remote_sample(index);
		int16_t returned = echo_sample(index);
		int16_t cleaned;

		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, index,
			       add_s16((int32_t)returned + remote), &cleaned),
		      "post-prefix filtering failed at sample %zu", index);
		echo_energy = energy_add(echo_energy, returned);
		residual_energy = energy_add(
			residual_energy, (int32_t)cleaned - remote);
	}
	CHECK(residual_energy * 100u < echo_energy,
	      "post-prefix suppression too small: residual=%" PRIu64
	      " source=%" PRIu64, residual_energy, echo_energy);
	return true;
}

int main(void)
{
	if (!test_contracts() || !test_long_delay_cancellation() ||
	    !test_phase2_delay_seed() || !test_long_phase2_delay_seed() ||
	    !test_weak_joint_margin() ||
	    !test_decimated_adjacent_correlation() ||
	    !test_unrelated_input_does_not_train() ||
	    !test_acquisition_after_unrelated_prefix())
		return 1;
	puts("V.90 host echo tests passed");
	return 0;
}
