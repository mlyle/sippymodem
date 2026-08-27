// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_probe.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { TEST_NOMINAL_AMPLITUDE = 12000 };

static const double TEST_PI = 3.1415926535897932384626433832795;

struct test_projection {
	double cosine;
	double sine;
};

static bool render_period(
	enum v34_phase2_probe_level level,
	int16_t samples[V34_PHASE2_PROBE_PERIOD_SAMPLES])
{
	struct v34_phase2_probe_transmitter transmitter;

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "probe transmitter did not initialize");
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index)
		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, level, &samples[index]) ==
			      V34_PHASE2_PROBE_OK,
		      "probe generation failed at sample %zu", index);
	CHECK(transmitter.period_index == 0,
	      "one probe period did not return to its phase origin");
	return true;
}

static struct test_projection project_harmonic(
	const int16_t samples[V34_PHASE2_PROBE_PERIOD_SAMPLES],
	unsigned int harmonic)
{
	struct test_projection projection = { 0 };

	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		double angle =
			2.0 * TEST_PI *
			(double)V34_PHASE2_PROBE_TONE_SPACING_HZ *
			(double)harmonic * (double)index /
			(double)V34_PHASE2_SAMPLE_RATE;

		projection.cosine += (double)samples[index] * cos(angle);
		projection.sine += (double)samples[index] * sin(angle);
	}
	projection.cosine *=
		2.0 / V34_PHASE2_PROBE_PERIOD_SAMPLES;
	projection.sine *=
		2.0 / V34_PHASE2_PROBE_PERIOD_SAMPLES;
	return projection;
}

static bool test_spectrum_and_initial_phases(void)
{
	static const struct {
		unsigned int harmonic;
		int phase_sign;
	} expected_tones[] = {
		{ 1, 1 },   { 2, -1 },  { 3, 1 },   { 4, 1 },
		{ 5, 1 },   { 7, 1 },   { 9, 1 },   { 10, 1 },
		{ 11, -1 }, { 13, 1 },  { 14, 1 },  { 15, -1 },
		{ 17, 1 },  { 18, -1 }, { 19, 1 },  { 20, -1 },
		{ 21, -1 }, { 22, -1 }, { 23, -1 }, { 24, 1 },
		{ 25, 1 },
	};
	static const unsigned int omitted_harmonics[] = { 6, 8, 12, 16 };
	int16_t samples[V34_PHASE2_PROBE_PERIOD_SAMPLES];
	double expected_tone_peak =
		(double)TEST_NOMINAL_AMPLITUDE /
		sqrt((double)V34_PHASE2_PROBE_TONE_COUNT);

	CHECK(ARRAY_LENGTH(expected_tones) ==
		      V34_PHASE2_PROBE_TONE_COUNT &&
		      render_period(V34_PHASE2_PROBE_L2, samples),
	      "probe spectrum fixture failed");
	for (size_t index = 0; index < ARRAY_LENGTH(expected_tones); ++index) {
		struct test_projection projection = project_harmonic(
			samples, expected_tones[index].harmonic);
		double expected =
			expected_tones[index].phase_sign * expected_tone_peak;

		CHECK(fabs(projection.cosine - expected) < 2.0 &&
			      fabs(projection.sine) < 2.0,
		      "harmonic %u changed amplitude or initial phase "
		      "(cosine %.3f, sine %.3f)",
		      expected_tones[index].harmonic, projection.cosine,
		      projection.sine);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(omitted_harmonics);
	     ++index) {
		struct test_projection projection = project_harmonic(
			samples, omitted_harmonics[index]);

		CHECK(hypot(projection.cosine, projection.sine) < 2.0,
		      "omitted harmonic %u leaked into the probe",
		      omitted_harmonics[index]);
	}
	return true;
}

static double mean_square(
	const int16_t samples[V34_PHASE2_PROBE_PERIOD_SAMPLES])
{
	double power = 0.0;

	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		double sample = samples[index];

		power += sample * sample;
	}
	return power / V34_PHASE2_PROBE_PERIOD_SAMPLES;
}

static bool test_power_levels(void)
{
	int16_t l1[V34_PHASE2_PROBE_PERIOD_SAMPLES];
	int16_t l2[V34_PHASE2_PROBE_PERIOD_SAMPLES];
	double l1_power;
	double l2_power;
	double nominal_power =
		(double)TEST_NOMINAL_AMPLITUDE * TEST_NOMINAL_AMPLITUDE / 2.0;
	double expected_ratio = pow(10.0, 6.0 / 10.0);

	CHECK(render_period(V34_PHASE2_PROBE_L1, l1) &&
		      render_period(V34_PHASE2_PROBE_L2, l2),
	      "probe power fixture failed");
	l1_power = mean_square(l1);
	l2_power = mean_square(l2);
	CHECK(fabs(l2_power - nominal_power) / nominal_power < 0.0001,
	      "L2 power %.3f no longer matches nominal power %.3f",
	      l2_power, nominal_power);
	CHECK(fabs(l1_power / l2_power - expected_ratio) < 0.0005,
	      "L1/L2 power ratio %.6f is not 6 dB",
	      l1_power / l2_power);
	return true;
}

static bool test_period_and_l1_boundary(void)
{
	struct v34_phase2_probe_transmitter transmitter;
	int16_t two_periods[2 * V34_PHASE2_PROBE_PERIOD_SAMPLES];
	int16_t first_l2_sample;

	CHECK(V34_PHASE2_PROBE_PERIOD_SAMPLES == 160 &&
		      V34_PHASE2_PROBE_L1_SAMPLES == 1280 &&
		      V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES == 4000 &&
		      V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES == 4400 &&
		      V34_PHASE2_PROBE_L1_SAMPLES *
				      V34_PHASE2_PROBE_TONE_SPACING_HZ /
			      V34_PHASE2_SAMPLE_RATE ==
			      24,
	      "probe timing constants changed");
	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "period fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(two_periods); ++index)
		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L2,
			      &two_periods[index]) ==
			      V34_PHASE2_PROBE_OK,
		      "period fixture failed at sample %zu", index);
	CHECK(memcmp(
		      two_periods,
		      &two_periods[V34_PHASE2_PROBE_PERIOD_SAMPLES],
		      sizeof(two_periods) / 2) == 0,
	      "generated probe periods are not sample-exact");

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "L1 boundary fixture did not initialize");
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_L1_SAMPLES; ++index) {
		int16_t discarded;

		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L1,
			      &discarded) == V34_PHASE2_PROBE_OK,
		      "L1 boundary fixture failed at sample %zu", index);
	}
	CHECK(transmitter.period_index == 0 &&
		      v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L2,
			      &first_l2_sample) ==
			      V34_PHASE2_PROBE_OK &&
		      first_l2_sample == two_periods[0],
	      "L1 did not hand L2 an exact phase boundary");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase2_probe_transmitter transmitter;
	struct v34_phase2_probe_transmitter before;
	int maximum_amplitude = 0;
	int16_t sample = 1234;
	int16_t maximum_sample = 0;

	for (int amplitude = 1; amplitude <= INT16_MAX; ++amplitude) {
		if (v34_phase2_probe_transmitter_init(
			    &transmitter, (int16_t)amplitude) !=
		    V34_PHASE2_PROBE_OK)
			break;
		maximum_amplitude = amplitude;
	}
	CHECK(maximum_amplitude > TEST_NOMINAL_AMPLITUDE &&
		      maximum_amplitude < INT16_MAX,
	      "probe amplitude bound is missing or unexpectedly restrictive");
	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, (int16_t)(maximum_amplitude + 1)) ==
		      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      v34_phase2_probe_transmitter_init(
			      &transmitter, 0) ==
			      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      v34_phase2_probe_transmitter_init(
			      &transmitter, -1) ==
			      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      v34_phase2_probe_transmitter_init(NULL, 1) ==
			      V34_PHASE2_PROBE_INVALID_ARGUMENT,
	      "invalid probe initialization changed caller-owned state");

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, (int16_t)maximum_amplitude) ==
		      V34_PHASE2_PROBE_OK,
	      "maximum safe probe amplitude was rejected");
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L1,
			      &sample) == V34_PHASE2_PROBE_OK,
		      "maximum L1 level failed at sample %zu", index);
		if (sample > maximum_sample)
			maximum_sample = sample;
	}
	CHECK(maximum_sample >= INT16_MAX - 2,
	      "probe amplitude bound leaves unexplained headroom");

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "validation fixture did not initialize");
	transmitter.period_index =
		V34_PHASE2_PROBE_PERIOD_SAMPLES;
	before = transmitter;
	sample = 1234;
	CHECK(!v34_phase2_probe_transmitter_is_valid(&transmitter) &&
		      v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L2,
			      &sample) ==
			      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      sample == 1234,
	      "corrupt probe state was not rejected transactionally");

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "argument validation fixture did not initialize");
	before = transmitter;
	CHECK(v34_phase2_probe_transmitter_sample(
		      &transmitter,
		      (enum v34_phase2_probe_level)99, &sample) ==
		      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L2,
			      NULL) ==
			      V34_PHASE2_PROBE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
		      !v34_phase2_probe_transmitter_is_valid(NULL),
	      "invalid probe sampling changed transmitter state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "spectrum and initial phases", test_spectrum_and_initial_phases },
		{ "power levels", test_power_levels },
		{ "period and L1 boundary", test_period_and_l1_boundary },
		{ "validation", test_validation },
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
