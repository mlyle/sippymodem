// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_probe_spectrum.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_NOMINAL_AMPLITUDE = 12000,
	TEST_PHASE_HARMONIC = 9,
	TEST_PHASE_AMPLITUDE = 10000,
	TEST_SKIP_SAMPLES = 53,
};

static const double TEST_TWO_PI = 6.283185307179586476925286766559;
static const double TEST_PHASE_RADIANS = 0.63;
static const double TEST_BIN_TOLERANCE = 0.25;

static bool harmonic_is_omitted(unsigned int harmonic)
{
	switch (harmonic) {
	case 6:
	case 8:
	case 12:
	case 16:
		return true;
	default:
		return false;
	}
}

static double bin_peak(
	const struct sippymodem_v34_phase2_probe_spectrum_bin *bin)
{
	return hypot(bin->in_phase_peak, bin->quadrature_peak);
}

static bool test_generated_probe_spectrum(void)
{
	struct sippymodem_v34_phase2_probe_transmitter transmitter;
	struct sippymodem_v34_phase2_probe_spectrum spectrum;
	struct sippymodem_v34_phase2_probe_spectrum_measurement measurement;
	double expected_peak =
		(double)TEST_NOMINAL_AMPLITUDE /
		sqrt((double)SIPPYMODEM_V34_PHASE2_PROBE_TONE_COUNT);
	double measured_power = 0.0;

	CHECK(sippymodem_v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_OK &&
		      sippymodem_v34_phase2_probe_spectrum_init(&spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	      "probe spectrum fixture did not initialize");
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES; ++index) {
		int16_t sample;

		CHECK(sippymodem_v34_phase2_probe_transmitter_sample(
			      &transmitter, SIPPYMODEM_V34_PHASE2_PROBE_L2,
			      &sample) == SIPPYMODEM_V34_PHASE2_PROBE_OK &&
		      sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum, sample) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
		      "probe spectrum failed at sample %zu", index);
	}
	CHECK(sippymodem_v34_phase2_probe_spectrum_measure(
		      &spectrum, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK &&
		      measurement.sample_count ==
			      SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES,
	      "probe spectrum did not measure one complete period");
	for (unsigned int harmonic = 1;
	     harmonic <= SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		const struct sippymodem_v34_phase2_probe_spectrum_bin *bin =
			&measurement.harmonics[harmonic - 1u];
		double peak = bin_peak(bin);

		if (harmonic_is_omitted(harmonic)) {
			CHECK(peak < TEST_BIN_TOLERANCE,
			      "omitted harmonic %u measured peak %.9f",
			      harmonic, peak);
		} else {
			CHECK(fabs(peak - expected_peak) < TEST_BIN_TOLERANCE &&
				      fabs(bin->quadrature_peak) <
					      TEST_BIN_TOLERANCE,
			      "harmonic %u measured peak %.9f%+.9fj, want %.9f",
			      harmonic, bin->in_phase_peak,
			      bin->quadrature_peak, expected_peak);
		}
		measured_power += bin->mean_square;
	}
	CHECK(fabs(measured_power -
		   (double)TEST_NOMINAL_AMPLITUDE *
			   TEST_NOMINAL_AMPLITUDE / 2.0) < 5000.0,
	      "probe spectrum changed nominal power: %.9f", measured_power);
	return true;
}

static int16_t phase_test_sample(uint32_t sample_index)
{
	double angle = TEST_TWO_PI * TEST_PHASE_HARMONIC *
		       SIPPYMODEM_V34_PHASE2_PROBE_TONE_SPACING_HZ *
		       (double)sample_index /
		       (double)SIPPYMODEM_V34_PHASE2_SAMPLE_RATE +
		       TEST_PHASE_RADIANS;

	return (int16_t)lround(TEST_PHASE_AMPLITUDE * cos(angle));
}

static bool test_phase_and_window_restart(void)
{
	struct sippymodem_v34_phase2_probe_spectrum spectrum;
	struct sippymodem_v34_phase2_probe_spectrum before;
	struct sippymodem_v34_phase2_probe_spectrum_measurement measurement;
	struct sippymodem_v34_phase2_probe_spectrum_measurement untouched;
	double expected_in_phase =
		TEST_PHASE_AMPLITUDE * cos(TEST_PHASE_RADIANS);
	double expected_quadrature =
		TEST_PHASE_AMPLITUDE * sin(TEST_PHASE_RADIANS);

	CHECK(sippymodem_v34_phase2_probe_spectrum_init(&spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	      "phase spectrum fixture did not initialize");
	for (uint32_t index = 0; index < TEST_SKIP_SAMPLES; ++index)
		CHECK(sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum, 0) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
		      "phase spectrum skip failed at sample %u", index);
	CHECK(sippymodem_v34_phase2_probe_spectrum_restart_window(
		      &spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK &&
		      spectrum.samples_observed == TEST_SKIP_SAMPLES &&
		      spectrum.window_start_sample == TEST_SKIP_SAMPLES,
	      "window restart did not preserve absolute phase");
	for (uint32_t index = 0;
	     index < 2u * SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES;
	     ++index) {
		CHECK(sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum,
			      phase_test_sample(TEST_SKIP_SAMPLES + index)) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
		      "phase spectrum failed at window sample %u", index);
	}
	CHECK(sippymodem_v34_phase2_probe_spectrum_measure(
		      &spectrum, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK &&
		      measurement.sample_count ==
			      2u * SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES,
	      "phase spectrum measured the wrong window");
	{
		const struct sippymodem_v34_phase2_probe_spectrum_bin *bin =
			&measurement.harmonics[TEST_PHASE_HARMONIC - 1u];

		CHECK(fabs(bin->in_phase_peak - expected_in_phase) <
			      TEST_BIN_TOLERANCE &&
			      fabs(bin->quadrature_peak - expected_quadrature) <
				      TEST_BIN_TOLERANCE,
		      "phase spectrum measured %.9f%+.9fj, want %.9f%+.9fj",
		      bin->in_phase_peak, bin->quadrature_peak,
		      expected_in_phase, expected_quadrature);
	}
	before = spectrum;
	CHECK(sippymodem_v34_phase2_probe_spectrum_restart_window(
		      &spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK &&
		      spectrum.samples_observed == before.samples_observed &&
		      spectrum.window_start_sample == before.samples_observed,
	      "second window restart changed the sample timeline");
	memset(&measurement, 0xa5, sizeof(measurement));
	untouched = measurement;
	CHECK(sippymodem_v34_phase2_probe_spectrum_measure(
		      &spectrum, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      memcmp(&measurement, &untouched, sizeof(measurement)) == 0,
	      "empty restarted window produced a measurement");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase2_probe_spectrum spectrum;
	struct sippymodem_v34_phase2_probe_spectrum before;
	struct sippymodem_v34_phase2_probe_spectrum_measurement measurement;
	struct sippymodem_v34_phase2_probe_spectrum_measurement measurement_before;

	CHECK(sippymodem_v34_phase2_probe_spectrum_init(&spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	      "validation fixture did not initialize");
	memset(&measurement, 0xa5, sizeof(measurement));
	measurement_before = measurement;
	before = spectrum;
	CHECK(sippymodem_v34_phase2_probe_spectrum_measure(
		      &spectrum, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      memcmp(&spectrum, &before, sizeof(spectrum)) == 0 &&
		      memcmp(&measurement, &measurement_before,
			     sizeof(measurement)) == 0 &&
		      sippymodem_v34_phase2_probe_spectrum_measure(
			      &spectrum, NULL) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT,
	      "invalid measurement changed caller-owned state");
	spectrum.window_start_sample = 1;
	before = spectrum;
	CHECK(!sippymodem_v34_phase2_probe_spectrum_is_valid(&spectrum) &&
		      sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum, 1) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_probe_spectrum_restart_window(
			      &spectrum) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      memcmp(&spectrum, &before, sizeof(spectrum)) == 0,
	      "inconsistent window timeline was accepted");
	CHECK(sippymodem_v34_phase2_probe_spectrum_init(&spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	      "validation fixture did not reset");
	spectrum.harmonics[3].in_phase_sum = NAN;
	before = spectrum;
	CHECK(!sippymodem_v34_phase2_probe_spectrum_is_valid(&spectrum) &&
		      sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum, 1) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      memcmp(&spectrum, &before, sizeof(spectrum)) == 0,
	      "non-finite accumulator was accepted");
	CHECK(sippymodem_v34_phase2_probe_spectrum_init(&spectrum) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	      "overflow fixture did not initialize");
	spectrum.samples_observed = UINT32_MAX;
	spectrum.window_start_sample = UINT32_MAX;
	before = spectrum;
	CHECK(sippymodem_v34_phase2_probe_spectrum_is_valid(&spectrum) &&
		      sippymodem_v34_phase2_probe_spectrum_sample(
			      &spectrum, 1) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      memcmp(&spectrum, &before, sizeof(spectrum)) == 0,
	      "sample counter overflow was not rejected transactionally");
	CHECK(sippymodem_v34_phase2_probe_spectrum_init(NULL) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_probe_spectrum_sample(NULL, 0) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_probe_spectrum_restart_window(NULL) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT &&
		      !sippymodem_v34_phase2_probe_spectrum_is_valid(NULL),
	      "null spectrum was accepted");
	CHECK(strcmp(
		      sippymodem_v34_phase2_probe_spectrum_result_name(
			      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK),
		      "ok") == 0 &&
		      strcmp(
			      sippymodem_v34_phase2_probe_spectrum_result_name(
				      SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT),
			      "invalid argument") == 0 &&
		      strcmp(
			      sippymodem_v34_phase2_probe_spectrum_result_name(
				      (enum sippymodem_v34_phase2_probe_spectrum_result)99),
			      "unknown") == 0,
	      "spectrum result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated probe spectrum", test_generated_probe_spectrum },
		{ "phase and window restart", test_phase_and_window_restart },
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
