// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_qpsk.h>

#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_SYMBOL_RATE = 600,
	TEST_AMPLITUDE = 12000,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static struct sippymodem_qpsk_channel v22_channel(uint32_t carrier_hz)
{
	return (struct sippymodem_qpsk_channel){
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = TEST_SYMBOL_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_hz = carrier_hz,
		.rolloff = 0.75,
		.pulse_span_symbols = 6,
	};
}

static bool test_generated_pulse(void)
{
	double at_zero = 0.0;
	double at_singularity = 0.0;
	double reflected = 0.0;
	double ordinary = 0.0;
	double expected_zero = 1.0 + 0.75 * (4.0 / TEST_PI - 1.0);

	CHECK(sippymodem_root_raised_cosine(0.0, 0.75, &at_zero) ==
		      SIPPYMODEM_QPSK_OK &&
		      fabs(at_zero - expected_zero) < 1e-12,
	      "QPSK pulse changed its zero-time limit");
	CHECK(sippymodem_root_raised_cosine(
		      1.0 / 3.0, 0.75, &at_singularity) ==
		      SIPPYMODEM_QPSK_OK &&
		      isfinite(at_singularity),
	      "QPSK pulse did not handle its removable singularity");
	CHECK(sippymodem_root_raised_cosine(0.625, 0.75, &ordinary) ==
		      SIPPYMODEM_QPSK_OK &&
		      sippymodem_root_raised_cosine(-0.625, 0.75, &reflected) ==
			      SIPPYMODEM_QPSK_OK &&
		      fabs(ordinary - reflected) < 1e-12,
	      "QPSK pulse lost even symmetry");
	return true;
}

static bool test_exact_symbol_clock(void)
{
	struct sippymodem_qpsk_channel channel = v22_channel(1200);
	struct sippymodem_qpsk_modulator modulator;
	unsigned int accepted_count = 0;
	int16_t peak = 0;

	CHECK(sippymodem_qpsk_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QPSK_OK &&
		      sippymodem_qpsk_modulator_can_accept_symbol(&modulator),
	      "QPSK symbol-clock fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE; ++index) {
		int16_t sample;
		bool accepted;

		CHECK(sippymodem_qpsk_modulator_sample(
			      &modulator, (uint8_t)(accepted_count & 3u),
			      &sample, &accepted) == SIPPYMODEM_QPSK_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE,
		      "QPSK modulator exceeded its waveform bound");
		if (accepted)
			accepted_count++;
		if (abs(sample) > peak)
			peak = (int16_t)abs(sample);
	}
	CHECK(accepted_count == TEST_SYMBOL_RATE &&
		      modulator.symbol_phase_units == 0 &&
		      sippymodem_qpsk_modulator_can_accept_symbol(&modulator) &&
		      peak > TEST_AMPLITUDE / 8,
	      "QPSK rational symbol clock drifted over one second");
	return true;
}

static double tone_projection(
	const int16_t *samples, size_t count, uint32_t frequency_hz)
{
	double in_phase = 0.0;
	double quadrature = 0.0;

	for (size_t index = 0; index < count; ++index) {
		double angle = 2.0 * TEST_PI * (double)frequency_hz *
			       (double)index / TEST_SAMPLE_RATE;

		in_phase += samples[index] * cos(angle);
		quadrature += samples[index] * sin(angle);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool check_carrier(uint32_t carrier_hz, uint32_t reject_hz)
{
	struct sippymodem_qpsk_channel channel = v22_channel(carrier_hz);
	struct sippymodem_qpsk_modulator modulator;
	int16_t samples[TEST_SAMPLE_RATE];

	CHECK(sippymodem_qpsk_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QPSK_OK,
	      "QPSK carrier fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
		bool accepted;

		CHECK(sippymodem_qpsk_modulator_sample(
			      &modulator, 0, &samples[index], &accepted) ==
			      SIPPYMODEM_QPSK_OK,
		      "QPSK carrier fixture failed at sample %zu", index);
	}
	CHECK(tone_projection(samples + TEST_SAMPLE_RATE / 4,
			      TEST_SAMPLE_RATE * 3 / 4, carrier_hz) >
		      tone_projection(samples + TEST_SAMPLE_RATE / 4,
			      TEST_SAMPLE_RATE * 3 / 4, reject_hz) * 1000.0,
	      "QPSK modulator did not retain its %u Hz carrier", carrier_hz);
	return true;
}

static bool test_channel_carriers(void)
{
	return check_carrier(1200, 2400) && check_carrier(2400, 1200);
}

static bool test_validation(void)
{
	struct sippymodem_qpsk_channel channel = v22_channel(1200);
	struct sippymodem_qpsk_modulator modulator;
	int16_t sample = 0;
	bool accepted = false;
	double pulse = 0.0;

	CHECK(sippymodem_root_raised_cosine(0.0, 0.0, &pulse) ==
		      SIPPYMODEM_QPSK_INVALID_ARGUMENT &&
		      sippymodem_root_raised_cosine(0.0, 0.75, NULL) ==
			      SIPPYMODEM_QPSK_INVALID_ARGUMENT &&
		      sippymodem_qpsk_modulator_init(NULL, &channel, 1) ==
			      SIPPYMODEM_QPSK_INVALID_ARGUMENT,
	      "QPSK accepted an invalid initialization argument");
	channel.carrier_frequency_hz = TEST_SAMPLE_RATE / 2;
	CHECK(sippymodem_qpsk_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QPSK_INVALID_ARGUMENT,
	      "QPSK accepted a carrier at Nyquist");
	channel = v22_channel(1200);
	CHECK(sippymodem_qpsk_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QPSK_OK,
	      "QPSK corruption fixture did not initialize");
	modulator.active_count = SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS + 1;
	CHECK(!sippymodem_qpsk_modulator_can_accept_symbol(&modulator) &&
		      sippymodem_qpsk_modulator_sample(
			      &modulator, 0, &sample, &accepted) ==
			      SIPPYMODEM_QPSK_INVALID_ARGUMENT &&
		      strcmp(sippymodem_qpsk_result_name(
			     SIPPYMODEM_QPSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "QPSK accepted corrupt active-symbol state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated pulse", test_generated_pulse },
		{ "exact symbol clock", test_exact_symbol_clock },
		{ "channel carriers", test_channel_carriers },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
