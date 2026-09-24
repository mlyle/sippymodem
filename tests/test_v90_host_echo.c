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
	       v90_host_echo_advance_predicted(
		       echo, mode, received, *cleaned,
		       transmit_sample(index)) == V90_HOST_ECHO_OK;
}

static bool test_contracts(void)
{
	struct v90_host_echo echo;
	struct v90_host_echo saved;
	struct v90_host_echo predicted;
	int16_t cleaned = 1234;

	CHECK(v90_host_echo_init(NULL) == V90_HOST_ECHO_INVALID_ARGUMENT,
	      "null owner initialized");
	memset(&echo, 0xa5, sizeof(echo));
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
	      v90_host_echo_is_valid(&echo) &&
	      !v90_host_echo_is_ready(&echo) &&
	      !v90_host_echo_has_weak_margin(&echo) &&
	      v90_host_echo_window_delay_samples(&echo) ==
		      V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES,
	      "ordinary owner did not initialize");
	memset(&saved, 0, sizeof(saved));
	saved.window_delay_samples = V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES;
	CHECK(pcm_echo_reference_start_scan(&saved.reference,
		      V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES,
		      V90_HOST_ECHO_FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK &&
	      memcmp(&echo, &saved, sizeof(echo)) == 0,
	      "owner differs from zero state with configured scan");
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
	predicted = echo;
	CHECK(v90_host_echo_advance(
		      &echo, V90_HOST_ECHO_FILTER, 17, 17, 91) ==
		      V90_HOST_ECHO_OK && echo.reference.fill == 1,
	      "matching prediction did not advance reference");
	CHECK(v90_host_echo_advance_predicted(
		      &predicted, V90_HOST_ECHO_FILTER, 17, 17, 91) ==
	      V90_HOST_ECHO_OK && memcmp(&echo, &predicted, sizeof(echo)) == 0,
	      "predicted advance diverged from checked advance");
	echo.window_delay_samples = 0;
	CHECK(!v90_host_echo_is_valid(&echo) &&
	      v90_host_echo_predict(
		      &echo, V90_HOST_ECHO_BYPASS, 0, &cleaned) ==
		      V90_HOST_ECHO_INVALID_STATE,
	      "corrupt owner was accepted");
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
	      v90_host_echo_is_valid(&echo) &&
	      memcmp(&echo, &saved, sizeof(echo)) == 0,
	      "reset retained history or scan state");
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
	static const struct {
		uint32_t delay;
		int seed_offset;
	} paths[] = {
		{ 2640, 0 }, { 3128, 0 }, { 5600, 0 },
		{ 2967, 3 }, { 2967, -5 }, { 2967, 31 }, { 2967, -31 },
		{ 2967, 63 }, { 2967, -317 },
	};
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	const size_t training = 4000;

	for (size_t trial = 0; trial < sizeof(paths) / sizeof(paths[0]);
	     ++trial) {
		struct v90_host_echo echo;
		uint32_t delay = paths[trial].delay;
		uint32_t seed = (uint32_t)((int)delay + paths[trial].seed_offset);
		uint64_t echo_energy = 0;
		uint64_t residual_energy = 0;

		CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
		      v90_host_echo_seed_phase2_delay(&echo, seed) ==
			      V90_HOST_ECHO_OK,
		      "long ranging seed failed: %u", delay);
		/* Phase 2 has already supplied TX history. A measured return inside
		 * that history should train without first searching from the default
		 * short delay. Small ranging errors still fit inside the filter;
		 * do not discard that model merely to recenter it. Readiness must
		 * still require actual correlated echo. */
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
			      seed - V90_HOST_ECHO_FILTER_PRE_SAMPLES,
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

static int16_t distributed_echo_sample(size_t index, size_t returns, bool late_peak)
{
	int32_t sample = 0;

	/* A delay bin captures only a fraction of the return energy. Readiness
	 * must instead predict the sum on later, untrained samples. */
	for (size_t tap = 0; tap < returns; ++tap) {
		int16_t gain = late_peak && tap + 1u == returns ? 1600 : 1024;
		sample += scale_q15(transmit_sample(index - 2967 - 17 * tap),
			tap % 3 == 0 ? -gain : gain);
	}
	return add_s16(sample);
}

static bool test_distributed_echo(bool acquire, bool late_peak)
{
	struct v90_host_echo echo;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	const size_t training = acquire ? 8000 : 4000;
	const size_t returns = acquire && !late_peak ? 8 : 16;
	uint64_t echo_energy = 0, residual_energy = 0;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
	      v90_host_echo_seed_phase2_delay(&echo, acquire ? 2138 : 2967) == V90_HOST_ECHO_OK,
	      "distributed echo owner did not initialize");
	for (size_t index = 0; index < history + training; ++index) {
		bool train = index >= history;
		int16_t received = train ? distributed_echo_sample(index, returns, late_peak) : 0;
		int16_t cleaned;

		CHECK(echo_step(&echo, train ? V90_HOST_ECHO_TRAIN :
			V90_HOST_ECHO_BYPASS, index, received, &cleaned),
		      "distributed training failed at %zu", index);
		CHECK(cleaned == received, "training changed input");
		if (echo.delay_acquired &&
		    echo.train_samples < V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES)
			CHECK(!v90_host_echo_is_ready(&echo),
			      "delay acquisition alone qualified a filter");
	}
	CHECK(v90_host_echo_is_ready(&echo) && echo.delay_acquired == acquire &&
	      (acquire || echo.window_delay_samples == 2967 - V90_HOST_ECHO_FILTER_PRE_SAMPLES) &&
	      echo.window_delay_samples <= 2967 &&
	      echo.window_delay_samples + V90_HOST_ECHO_FILTER_TAPS > 2967 + 17 * (returns - 1),
	      "distributed model did not qualify: ready=%d delay=%u",
	      echo.filter_ready, echo.window_delay_samples);
	for (size_t index = history + training;
	     index < history + training + VERIFY_SAMPLES; ++index) {
		int16_t remote = remote_sample(index);
		int16_t returned = distributed_echo_sample(index, returns, late_peak);
		int16_t cleaned;
		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, index,
			add_s16((int32_t)remote + returned), &cleaned),
		      "distributed verification failed at %zu", index);
		echo_energy = energy_add(echo_energy, returned);
		residual_energy = energy_add(residual_energy, (int32_t)cleaned - remote);
	}
	CHECK(residual_energy * 100 < echo_energy,
	      "distributed suppression too small: residual=%" PRIu64 " source=%" PRIu64,
	      residual_energy, echo_energy);
	return true;
}

static bool test_retained_cancellation(size_t training, bool unrelated)
{
	struct v90_host_echo echo;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	uint64_t source_energy = 0, residual_energy = 0;
	bool ready = false, retained = false;
	float saved[V90_HOST_ECHO_FILTER_TAPS];

	/* The seeded window covers enough later returns to qualify weakly,
	 * but leaves the first returns outside. A new window needs more time
	 * to qualify; short training must keep the original cancellation. */
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK &&
	      v90_host_echo_seed_phase2_delay(&echo, 3067) == V90_HOST_ECHO_OK,
	      "retained echo owner did not initialize");
	for (size_t i = 0; i < history + training; ++i) {
		int16_t clean, received = i >= history ? distributed_echo_sample(i, 8, false) : 0;

		if (retained && unrelated)
			received = generated_sample(i, UINT32_C(0xf4a8b223), 30000);
		CHECK(echo_step(&echo, i >= history ? V90_HOST_ECHO_TRAIN : V90_HOST_ECHO_BYPASS,
			      i, received, &clean), "retained training failed at %zu", i);
		CHECK(clean == received && (!ready || v90_host_echo_is_ready(&echo)),
		      "training changed input or lost qualified cancellation at %zu", i);
		ready |= v90_host_echo_is_ready(&echo);
		if (echo.retained_delay_samples) {
			if (!retained)
				memcpy(saved, echo.retained_coefficients, sizeof(saved));
			CHECK(!memcmp(saved, echo.retained_coefficients, sizeof(saved)),
			      "retained coefficients were refitted");
			CHECK(!echo.filter_ready && v90_host_echo_has_weak_margin(&echo),
			      "unqualified replacement lost retained quality");
			retained = true;
		}
	}
	CHECK(retained && v90_host_echo_is_ready(&echo),
	      "fixture did not retain its partial model");
	bool replaced = training >= 6500 && !unrelated;

	CHECK((echo.retained_delay_samples == 0) == replaced && echo.filter_ready == replaced,
	      "replacement did not wait for stronger qualification");
	for (size_t i = history + training; i < history + training + VERIFY_SAMPLES; ++i) {
		int16_t returned = distributed_echo_sample(i, 8, false);
		int16_t peer = remote_sample(i), clean;

		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, i,
			      add_s16((int32_t)returned + peer), &clean),
		      "retained filtering failed at %zu", i);
		source_energy = energy_add(source_energy, returned);
		residual_energy = energy_add(residual_energy, (int32_t)clean - peer);
	}
	CHECK(residual_energy * (replaced ? 20u : 2u) < source_energy,
	      "retained cancellation failed: training=%zu unrelated=%u residual=%" PRIu64
	      " source=%" PRIu64, training, unrelated, residual_energy, source_energy);
	if (echo.retained_delay_samples) {
		struct v90_host_echo invalid = echo;

		invalid.retained_delay_samples = PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES + 1u;
		CHECK(!v90_host_echo_is_valid(&invalid), "out-of-history retained model accepted");
		invalid = echo;
		invalid.retained_coefficients[0] = NAN;
		CHECK(!v90_host_echo_is_valid(&invalid), "nonfinite retained model accepted");
		invalid = echo;
		invalid.filter_ready = true;
		CHECK(!v90_host_echo_is_valid(&invalid), "two active models accepted");
		invalid = echo;
		invalid.retained_delay_samples = 0;
		CHECK(!v90_host_echo_is_valid(&invalid), "unowned retained coefficients accepted");
	}
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK, "retained owner reset failed");
	echo.retained_coefficients[0] = 1.0f;
	CHECK(v90_host_echo_seed_phase2_delay(&echo, 3067) == V90_HOST_ECHO_INVALID_STATE,
	      "unowned retained coefficients accepted as pristine");
	return true;
}

static bool test_training_quiet_boundary(void)
{
	for (unsigned scenario = 0; scenario < 3; ++scenario) {
		struct v90_host_echo echo;

		CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
		      "quiet-boundary owner did not initialize");
		if (scenario != 0) {
			for (unsigned index = 0;
			     index < echo.window_delay_samples +
				     V90_HOST_ECHO_FILTER_TAPS - 1u; ++index)
				CHECK(pcm_echo_reference_push(
					      &echo.reference, scenario == 2 ? 100 : 0) ==
					      PCM_ECHO_REFERENCE_OK,
				      "quiet-boundary reference did not fill");
		}
		echo.quiet_samples = V90_HOST_ECHO_QUIET_SAMPLES - 2u;
		for (unsigned step = 0; step < 2; ++step) {
			unsigned trained = scenario == 2 && step == 1;

			CHECK(v90_host_echo_advance(
				      &echo, V90_HOST_ECHO_TRAIN, 100, 100, 100) ==
				      V90_HOST_ECHO_OK &&
			      echo.quiet_samples ==
				      V90_HOST_ECHO_QUIET_SAMPLES - 1u + step &&
			      echo.train_samples == trained &&
			      (trained ? echo.coefficients[0] > 0.0f :
					 echo.coefficients[0] == 0.0f) &&
			      v90_host_echo_is_valid(&echo),
			      "quiet-boundary scenario %u step %u changed training",
			      scenario, step);
		}
	}
	return true;
}

static bool test_prediction_energy_saturation(void)
{
	for (int sign = -1; sign <= 1; sign += 2) {
		struct v90_host_echo echo;
		int16_t received = sign * 100;

		CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
		      "saturation owner did not initialize");
		for (size_t index = 0; index < PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
		     ++index)
			CHECK(pcm_echo_reference_push(&echo.reference, 100) ==
			      PCM_ECHO_REFERENCE_OK, "saturation history fill failed");
		echo.coefficients[0] = 1e20f;
		echo.quiet_samples = V90_HOST_ECHO_QUIET_SAMPLES;
		CHECK(v90_host_echo_advance(&echo, V90_HOST_ECHO_TRAIN,
			received, received, 100) == V90_HOST_ECHO_OK &&
		      v90_host_echo_is_valid(&echo) && !v90_host_echo_is_ready(&echo),
		      "finite oversized prediction broke training");
		CHECK(echo.prediction_energy == UINT64_MAX &&
		      echo.error_energy == UINT64_MAX &&
		      echo.prediction_cross_energy == (sign < 0 ? INT64_MIN : INT64_MAX),
		      "prediction evidence did not saturate safely: sign=%d", sign);
	}
	return true;
}

static bool test_unrelated_input_does_not_train(uint32_t salt, int16_t span)
{
	struct v90_host_echo echo;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;

	/* Adaptation improves the current sample's fit even for independent
	 * noise. Readiness must use predictions made before that update. */
	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "noise owner did not initialize");
	for (size_t index = 0; index < history + FALSE_TRAIN_SAMPLES; ++index) {
		int16_t input = generated_sample(index, salt, span);
		int16_t cleaned;

		CHECK(echo_step(
			      &echo, index < history ? V90_HOST_ECHO_BYPASS : V90_HOST_ECHO_TRAIN, index,
			      input, &cleaned),
		      "noise training failed at sample %zu", index);
		CHECK(cleaned == input && !v90_host_echo_is_ready(&echo) && !echo.delay_acquired,
		      "uncorrelated input acquired or enabled a filter at %zu", index);
	}
	CHECK(!v90_host_echo_is_ready(&echo) && !echo.delay_acquired,
	      "uncorrelated input produced a ready model: delay=%u corr=%" PRIi64,
	      echo.window_delay_samples, echo.prediction_cross_energy);
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

static bool test_frozen_delay_tracking(int changed_delay, bool transient)
{
	struct v90_host_echo echo;
	uint64_t source_energy = 0, residual_energy = 0;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "timing owner did not initialize");
	for (size_t index = 0; index < TRAIN_SAMPLES; ++index) {
		int16_t cleaned;

		CHECK(echo_step(&echo, V90_HOST_ECHO_TRAIN, index,
			       echo_sample(index) / 16, &cleaned),
		      "weak-return training failed at %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&echo), "weak return was not qualified");
	float coefficients[V90_HOST_ECHO_FILTER_TAPS];
	uint32_t original_delay = echo.window_delay_samples;

	memcpy(coefficients, echo.coefficients, sizeof(coefficients));
	/* A weak echo changes delay after training while the remote remains
	 * much louder. Only timing may change; never fit the remote as echo. */
	for (size_t index = TRAIN_SAMPLES; index < TRAIN_SAMPLES + 48000; ++index) {
		int16_t remote = remote_sample(index) * 7;
		int shift = transient && index >= TRAIN_SAMPLES + V90_HOST_ECHO_TIMING_WINDOW ?
			0 : changed_delay;
		int16_t returned = echo_sample((size_t)((int64_t)index - shift)) / 16;
		int16_t cleaned;

		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, index,
			       add_s16((int32_t)remote + returned), &cleaned),
		      "changed-return filtering failed at %zu", index);
		if (index >= TRAIN_SAMPLES + 40000) {
			source_energy = energy_add(source_energy, returned);
			residual_energy = energy_add(residual_energy,
						    (int32_t)cleaned - remote);
		}
	}
	CHECK((int64_t)echo.window_delay_samples ==
		      (int64_t)original_delay + (transient ? 0 : changed_delay),
	      "frozen return did not retime: %u -> %u", original_delay,
	      echo.window_delay_samples);
	CHECK(memcmp(coefficients, echo.coefficients, sizeof(coefficients)) == 0,
	      "timing correction changed filter coefficients");
	CHECK(residual_energy * 50 < source_energy,
	      "retimed residual too large: %" PRIu64 " / %" PRIu64,
	      residual_energy, source_energy);
	{
		struct v90_host_echo saved = echo;
		int16_t cleaned;

		CHECK(v90_host_echo_advance(&echo, V90_HOST_ECHO_FILTER,
			0, INT16_MIN, 0) == V90_HOST_ECHO_SEQUENCE_MISMATCH &&
		      memcmp(&echo, &saved, sizeof(echo)) == 0,
		      "bad prediction advanced timing evidence");
		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER,
			       TRAIN_SAMPLES + 48000, INT16_MAX, &cleaned) &&
		      echo.timing.fill == 0 && echo.timing.count == 0,
		      "clipped input retained timing evidence");
		CHECK(memcmp(coefficients, echo.coefficients, sizeof(coefficients)) == 0,
		      "clipped input changed the frozen response");
	}
	return true;
}

static bool test_unrelated_input_does_not_retime(uint32_t salt)
{
	struct v90_host_echo echo;

	CHECK(v90_host_echo_init(&echo) == V90_HOST_ECHO_OK,
	      "false-timing owner did not initialize");
	for (size_t index = 0; index < TRAIN_SAMPLES; ++index) {
		int16_t cleaned;

		CHECK(echo_step(&echo, V90_HOST_ECHO_TRAIN, index,
			       echo_sample(index) / 16, &cleaned),
		      "false-timing training failed at %zu", index);
	}
	CHECK(v90_host_echo_is_ready(&echo), "false-timing model not qualified");
	uint32_t original_delay = echo.window_delay_samples;

	/* Remove the echo entirely. Coloured remote noise plus a tone must not
	 * authorize moving a previously qualified filter onto unrelated data. */
	for (size_t index = TRAIN_SAMPLES; index < TRAIN_SAMPLES + 96000; ++index) {
		int32_t remote = generated_sample(index, salt, 3000) +
			generated_sample(index - 1, salt, 3000);
		int16_t cleaned;

		remote = remote / 2 + (int16_t)(500 * sin(index * .7));
		CHECK(echo_step(&echo, V90_HOST_ECHO_FILTER, index,
			       (int16_t)remote, &cleaned),
		      "unrelated input failed at %zu", index);
		CHECK(echo.window_delay_samples == original_delay,
		      "unrelated data moved echo at %zu: %u -> %u", index,
		      original_delay, echo.window_delay_samples);
	}
	return true;
}

int main(void)
{
	for (uint32_t trial = 0; trial < 4; ++trial)
		if (!test_unrelated_input_does_not_train(
			UINT32_C(0x31415927) + trial * 7919u, trial % 2 ? 30000 : 1000))
			return 1;
	if (!test_contracts() || !test_training_quiet_boundary() ||
	    !test_prediction_energy_saturation() ||
	    !test_long_delay_cancellation() ||
	    !test_phase2_delay_seed() || !test_long_phase2_delay_seed() ||
	    !test_weak_joint_margin() ||
	    !test_distributed_echo(false, false) || !test_distributed_echo(true, false) ||
	    !test_distributed_echo(true, true) ||
	    !test_retained_cancellation(4100, false) || !test_retained_cancellation(5000, false) ||
	    !test_retained_cancellation(6500, false) || !test_retained_cancellation(8000, false) ||
	    !test_retained_cancellation(12000, true) ||
	    !test_acquisition_after_unrelated_prefix())
		return 1;
	const int shifts[] = {0, 8, -60, -V90_HOST_ECHO_TIMING_RADIUS,
		V90_HOST_ECHO_TIMING_RADIUS};
	for (size_t index = 0; index < sizeof(shifts) / sizeof(shifts[0]); ++index)
		if (!test_frozen_delay_tracking(shifts[index], false))
			return 1;
	if (!test_frozen_delay_tracking(8, true))
		return 1;
	for (uint32_t salt = 1; salt <= 8; ++salt)
		if (!test_unrelated_input_does_not_retime(salt * UINT32_C(7919)))
			return 1;
	puts("V.90 host echo tests passed");
	return 0;
}
