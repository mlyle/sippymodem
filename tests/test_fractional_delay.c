// SPDX-License-Identifier: BSD-3-Clause

#include <fractional_delay.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static bool test_integer_endpoints_are_exact(void)
{
	static const int32_t phases[] = {
		-FRACTIONAL_DELAY_ONE_SAMPLE_Q16,
		0,
		FRACTIONAL_DELAY_ONE_SAMPLE_Q16,
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(phases); ++fixture) {
		struct fractional_delay delay;
		size_t expected = (size_t)(FRACTIONAL_DELAY_CENTER +
			phases[fixture] / FRACTIONAL_DELAY_ONE_SAMPLE_Q16);

		fractional_delay_init(&delay);
		CHECK(fractional_delay_set_phase(&delay, phases[fixture]) ==
			      FRACTIONAL_DELAY_OK,
		      "integer delay phase %d was rejected", phases[fixture]);
		for (size_t index = 0; index < FRACTIONAL_DELAY_TAPS; ++index) {
			int16_t output = INT16_MIN;
			int16_t input = index == 0 ? 4096 : 0;

			CHECK(fractional_delay_process(&delay, input, &output) ==
				      FRACTIONAL_DELAY_OK &&
			      output == (index == expected ? 4096 : 0),
			      "phase %d emitted %d at %zu instead of tap %zu",
			      phases[fixture], output, index, expected);
		}
	}
	return true;
}

static bool test_fractional_phases_preserve_dc(void)
{
	static const int32_t phases[] = {
		-FRACTIONAL_DELAY_HALF_SAMPLE_Q16,
		-0x2345,
		0x2345,
		FRACTIONAL_DELAY_HALF_SAMPLE_Q16,
		0x7f10,
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(phases); ++fixture) {
		struct fractional_delay delay;
		int16_t output = 0;

		fractional_delay_init(&delay);
		CHECK(fractional_delay_set_phase(&delay, phases[fixture]) ==
			      FRACTIONAL_DELAY_OK &&
			      fractional_delay_is_valid(&delay),
		      "fractional delay phase %d did not initialize",
		      phases[fixture]);
		for (size_t index = 0; index < 2u * FRACTIONAL_DELAY_TAPS;
		     ++index)
			CHECK(fractional_delay_process(&delay, 3000, &output) ==
				      FRACTIONAL_DELAY_OK,
			      "DC phase %d failed at sample %zu",
			      phases[fixture], index);
		CHECK(output == 3000,
		      "phase %d changed steady DC to %d",
		      phases[fixture], output);
	}
	return true;
}

static bool test_all_phases_preserve_valid_stream(void)
{
	struct fractional_delay delay;
	int16_t output = 0;

	fractional_delay_init(&delay);
	for (size_t index = 0; index < FRACTIONAL_DELAY_TAPS; ++index)
		CHECK(fractional_delay_process(&delay, 3000, &output) ==
			      FRACTIONAL_DELAY_OK,
		      "phase sweep history setup failed");
	for (int32_t phase = -FRACTIONAL_DELAY_ONE_SAMPLE_Q16;
	     phase <= FRACTIONAL_DELAY_ONE_SAMPLE_Q16; ++phase) {
		struct fractional_delay before = delay;

		CHECK(fractional_delay_set_phase(&delay, phase) ==
			      FRACTIONAL_DELAY_OK &&
		      fractional_delay_is_valid(&delay),
		      "accepted phase %d left an invalid filter", phase);
		CHECK(delay.phase_q16 == phase &&
		      delay.history_position == before.history_position &&
		      memcmp(delay.history, before.history,
			     sizeof(delay.history)) == 0,
		      "phase %d changed stream history", phase);
		CHECK(fractional_delay_process(&delay, 3000, &output) ==
			      FRACTIONAL_DELAY_OK && output == 3000,
		      "phase %d could not preserve steady DC: %d", phase, output);
	}
	return true;
}

static bool test_half_sample_preserves_voice_band_energy(void)
{
	static const double pi = 3.14159265358979323846264338327950288;
	enum { WARMUP_SAMPLES = 64, MEASURED_SAMPLES = 4096 };
	struct fractional_delay delay;
	uint64_t input_energy = 0;
	uint64_t output_energy = 0;

	fractional_delay_init(&delay);
	CHECK(fractional_delay_set_phase(
		      &delay, FRACTIONAL_DELAY_HALF_SAMPLE_Q16) ==
		      FRACTIONAL_DELAY_OK,
	      "half-sample delay did not initialize");
	for (size_t index = 0;
	     index < WARMUP_SAMPLES + MEASURED_SAMPLES; ++index) {
		int16_t input = (int16_t)llround(
			10000.0 * sin(2.0 * pi * 3500.0 * (double)index / 8000.0));
		int16_t output = 0;

		CHECK(fractional_delay_process(&delay, input, &output) ==
			      FRACTIONAL_DELAY_OK,
		      "half-sample delay failed at sample %zu", index);
		if (index >= WARMUP_SAMPLES) {
			input_energy += (uint64_t)((int32_t)input * input);
			output_energy += (uint64_t)((int32_t)output * output);
		}
	}
	CHECK(output_energy * 100u >= input_energy * 95u &&
		      output_energy * 100u <= input_energy * 105u,
	      "half-sample delay changed 3.5 kHz energy from %llu to %llu",
	      (unsigned long long)input_energy,
	      (unsigned long long)output_energy);
	return true;
}

static bool test_reconfiguration_is_continuous_and_transactional(void)
{
	struct fractional_delay delay;
	struct fractional_delay snapshot;
	struct fractional_delay invalid;
	int16_t output = 1234;

	fractional_delay_init(&delay);
	for (size_t index = 0; index < 11; ++index)
		CHECK(fractional_delay_process(
			      &delay, (int16_t)(index * 73 - 200), &output) ==
			      FRACTIONAL_DELAY_OK,
		      "fractional delay history setup failed");
	snapshot = delay;
	CHECK(fractional_delay_set_phase(&delay, 0x4000) ==
		      FRACTIONAL_DELAY_OK &&
		      delay.phase_q16 == 0x4000 &&
		      delay.history_position == snapshot.history_position &&
		      memcmp(delay.history, snapshot.history,
			     sizeof(delay.history)) == 0 &&
		      fractional_delay_is_valid(&delay),
	      "phase reconfiguration reset or invalidated stream history");
	snapshot = delay;
	CHECK(fractional_delay_set_phase(
		      &delay, FRACTIONAL_DELAY_ONE_SAMPLE_Q16 + 1) ==
		      FRACTIONAL_DELAY_OUT_OF_RANGE &&
		      memcmp(&delay, &snapshot, sizeof(delay)) == 0,
	      "out-of-range phase changed fractional delay state");
	CHECK(fractional_delay_set_phase(&delay, delay.phase_q16) ==
		      FRACTIONAL_DELAY_OK &&
		      memcmp(&delay, &snapshot, sizeof(delay)) == 0,
	      "idempotent phase selection changed state");
	invalid = delay;
	invalid.coefficients_q15[0]++;
	snapshot = invalid;
	output = 1234;
	CHECK(!fractional_delay_is_valid(&invalid) &&
		      fractional_delay_process(&invalid, 42, &output) ==
		      FRACTIONAL_DELAY_INVALID_STATE &&
		      output == 1234 &&
		      memcmp(&invalid, &snapshot, sizeof(invalid)) == 0,
	      "invalid delay state consumed input or changed output");
	invalid = delay;
	invalid.coefficients_q15[0] += INT32_MAX / 2;
	invalid.coefficients_q15[1] -= INT32_MAX / 2;
	snapshot = invalid;
	CHECK(!fractional_delay_is_valid(&invalid) &&
	      fractional_delay_set_phase(&invalid, 0) ==
		      FRACTIONAL_DELAY_INVALID_STATE &&
	      fractional_delay_process(&invalid, 42, &output) ==
		      FRACTIONAL_DELAY_INVALID_STATE && output == 1234 &&
	      memcmp(&invalid, &snapshot, sizeof(invalid)) == 0,
	      "unity-sum invalid coefficients escaped validation");
	CHECK(fractional_delay_set_phase(NULL, 0) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
		      fractional_delay_process(NULL, 0, &output) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
		      fractional_delay_process(&delay, 0, NULL) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
		      strcmp(fractional_delay_result_name(
			     FRACTIONAL_DELAY_NUMERIC_FAILURE),
			     "fractional delay coefficient generation failed") == 0 &&
		      strcmp(fractional_delay_result_name(
			     (enum fractional_delay_result)99),
			     "unknown fractional delay result") == 0,
	      "fractional delay validation diagnostics were incomplete");
	return true;
}

static bool test_cubic_interpolation(void)
{
	static const double polynomial_samples[] = { 4.0, 3.0, 2.0, 7.0 };
	static const double constant_samples[] = { 6.25, 6.25, 6.25, 6.25 };
	double invalid_samples[] = { 1.0, 2.0, NAN, 4.0 };
	double overflowing_samples[] = {
		-DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX,
	};
	double output = 123.0;

	CHECK(fractional_delay_interpolate_cubic(
		      polynomial_samples, 0.0, &output) ==
		      FRACTIONAL_DELAY_OK &&
	      output == polynomial_samples[1] &&
	      fractional_delay_interpolate_cubic(
		      polynomial_samples, 1.0, &output) ==
		      FRACTIONAL_DELAY_OK &&
	      output == polynomial_samples[2],
	      "cubic interpolation changed an endpoint");
	CHECK(fractional_delay_interpolate_cubic(
		      polynomial_samples, 0.25, &output) ==
		      FRACTIONAL_DELAY_OK &&
	      output == 2.515625,
	      "cubic interpolation did not reproduce a cubic polynomial");
	CHECK(fractional_delay_interpolate_cubic(
		      constant_samples, 0.375, &output) ==
		      FRACTIONAL_DELAY_OK &&
	      output == constant_samples[0],
	      "cubic interpolation changed a constant signal");
	output = 123.0;
	CHECK(fractional_delay_interpolate_cubic(
		      NULL, 0.5, &output) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
	      fractional_delay_interpolate_cubic(
		      polynomial_samples, 0.5, NULL) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
	      fractional_delay_interpolate_cubic(
		      invalid_samples, 0.5, &output) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
	      fractional_delay_interpolate_cubic(
		      polynomial_samples, NAN, &output) ==
		      FRACTIONAL_DELAY_INVALID_ARGUMENT &&
	      fractional_delay_interpolate_cubic(
		      polynomial_samples, -0.01, &output) ==
		      FRACTIONAL_DELAY_OUT_OF_RANGE &&
	      fractional_delay_interpolate_cubic(
		      polynomial_samples, 1.01, &output) ==
		      FRACTIONAL_DELAY_OUT_OF_RANGE &&
	      fractional_delay_interpolate_cubic(
		      overflowing_samples, 0.5, &output) ==
		      FRACTIONAL_DELAY_NUMERIC_FAILURE &&
	      output == 123.0,
	      "cubic interpolation failure changed its output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact integer endpoints", test_integer_endpoints_are_exact },
		{ "fractional DC gain", test_fractional_phases_preserve_dc },
		{ "all phases preserve a valid stream",
		  test_all_phases_preserve_valid_stream },
		{ "voice-band energy", test_half_sample_preserves_voice_band_energy },
		{ "continuous reconfiguration",
		  test_reconfiguration_is_continuous_and_transactional },
		{ "cubic interpolation", test_cubic_interpolation },
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
