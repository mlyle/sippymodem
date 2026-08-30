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
	CHECK(v90_host_echo_is_ready(&echo),
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

int main(void)
{
	CHECK(test_contracts(), "contracts failed");
	CHECK(test_long_delay_cancellation(),
	      "long-delay cancellation failed");
	CHECK(test_unrelated_input_does_not_train(),
	      "uncorrelated-input gate failed");
	puts("V.90 host echo tests passed");
	return 0;
}
