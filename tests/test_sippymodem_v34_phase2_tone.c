// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_tone.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLES = SIPPYMODEM_V34_PHASE2_SAMPLE_RATE / 10,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static double tone_projection(
	const int16_t *samples, size_t count, uint32_t frequency_hz)
{
	double in_phase = 0.0;
	double quadrature = 0.0;

	for (size_t index = 0; index < count; ++index) {
		double angle = 2.0 * TEST_PI * (double)frequency_hz *
			       (double)index /
			       SIPPYMODEM_V34_PHASE2_SAMPLE_RATE;

		in_phase += samples[index] * cos(angle);
		quadrature += samples[index] * sin(angle);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool render(
	enum sippymodem_v34_phase2_station_role role,
	int16_t samples[TEST_SAMPLES],
	struct sippymodem_v34_phase2_tone_transmitter *transmitter)
{
	CHECK(sippymodem_v34_phase2_tone_transmitter_init(
		      transmitter, role, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V34_PHASE2_TONE_OK,
	      "Phase 2 tone transmitter did not initialize");
	for (size_t index = 0; index < TEST_SAMPLES; ++index)
		CHECK(sippymodem_v34_phase2_tone_transmitter_sample(
			      transmitter, &samples[index]) ==
			      SIPPYMODEM_V34_PHASE2_TONE_OK,
		      "Phase 2 tone generation failed at sample %zu", index);
	return true;
}

static bool test_tone_levels(void)
{
	struct sippymodem_v34_phase2_tone_transmitter calling;
	struct sippymodem_v34_phase2_tone_transmitter answering;
	int16_t calling_samples[TEST_SAMPLES];
	int16_t answering_samples[TEST_SAMPLES];
	double answering_carrier;
	double answering_guard;

	CHECK(render(
		      SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      calling_samples, &calling) &&
		      render(
			      SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
			      answering_samples, &answering),
	      "Phase 2 tone fixture failed");
	CHECK(calling.carrier_amplitude == TEST_AMPLITUDE &&
		      calling.guard_amplitude == 0,
	      "Tone B changed its nominal level");
	CHECK(answering.carrier_amplitude >= 10690 &&
		      answering.carrier_amplitude <= 10700 &&
		      answering.guard_amplitude == TEST_AMPLITUDE,
	      "Tone A changed its carrier or guard level");
	CHECK(tone_projection(
		      calling_samples, TEST_SAMPLES,
		      SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ) >
		      tone_projection(
			      calling_samples, TEST_SAMPLES,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0,
	      "Tone B omitted its 1200 Hz carrier");
	answering_carrier = tone_projection(
		answering_samples, TEST_SAMPLES,
		SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ);
	answering_guard = tone_projection(
		answering_samples, TEST_SAMPLES,
		SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ);
	CHECK(answering_guard > answering_carrier * 1.24 &&
		      answering_guard < answering_carrier * 1.28,
	      "Tone A did not retain its nominal guard and -1 dB carrier");
	return true;
}

static bool test_phase_reversal(void)
{
	struct sippymodem_v34_phase2_tone_transmitter baseline;
	struct sippymodem_v34_phase2_tone_transmitter reversed;
	int16_t baseline_samples[TEST_SAMPLES];
	int16_t reversed_samples[TEST_SAMPLES];
	int16_t sum[TEST_SAMPLES];
	int16_t difference[TEST_SAMPLES];

	CHECK(sippymodem_v34_phase2_tone_transmitter_init(
		      &baseline, SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
		      TEST_AMPLITUDE) == SIPPYMODEM_V34_PHASE2_TONE_OK,
	      "Tone A reversal fixture did not initialize");
	for (size_t index = 0; index < 37; ++index) {
		int16_t discarded;

		CHECK(sippymodem_v34_phase2_tone_transmitter_sample(
			      &baseline, &discarded) ==
			      SIPPYMODEM_V34_PHASE2_TONE_OK,
		      "Tone A prelude failed");
	}
	reversed = baseline;
	CHECK(sippymodem_v34_phase2_tone_transmitter_reverse(&reversed) ==
		      SIPPYMODEM_V34_PHASE2_TONE_OK,
	      "Tone A did not accept a phase reversal");
	for (size_t index = 0; index < TEST_SAMPLES; ++index) {
		CHECK(sippymodem_v34_phase2_tone_transmitter_sample(
			      &baseline, &baseline_samples[index]) ==
			      SIPPYMODEM_V34_PHASE2_TONE_OK &&
		      sippymodem_v34_phase2_tone_transmitter_sample(
			      &reversed, &reversed_samples[index]) ==
			      SIPPYMODEM_V34_PHASE2_TONE_OK,
		      "Tone A comparison failed at sample %zu", index);
		sum[index] = (int16_t)(
			baseline_samples[index] + reversed_samples[index]);
		difference[index] = (int16_t)(
			baseline_samples[index] - reversed_samples[index]);
	}
	CHECK(baseline.guard_phase == reversed.guard_phase &&
		      baseline.carrier_phase ==
			      reversed.carrier_phase + UINT32_C(0x80000000),
	      "Tone A reversal disturbed guard continuity");
	CHECK(tone_projection(
		      sum, TEST_SAMPLES,
		      SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ) >
		      tone_projection(
			      sum, TEST_SAMPLES,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0 &&
		      tone_projection(
			      difference, TEST_SAMPLES,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ) >
		      tone_projection(
			      difference, TEST_SAMPLES,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ) *
			      1000.0,
	      "Tone A reversal did not invert only its carrier");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase2_tone_transmitter transmitter;
	struct sippymodem_v34_phase2_tone_transmitter before;
	int16_t sample = 1234;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	CHECK(sippymodem_v34_phase2_tone_transmitter_init(
		      &transmitter, SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
		      20000) == SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "invalid Tone A level changed caller-owned state");
	CHECK(sippymodem_v34_phase2_tone_transmitter_init(
		      &transmitter, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE) == SIPPYMODEM_V34_PHASE2_TONE_OK,
	      "validation fixture did not initialize");
	transmitter.carrier_phase_step++;
	before = transmitter;
	CHECK(sippymodem_v34_phase2_tone_transmitter_sample(
		      &transmitter, &sample) ==
		      SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      sample == 1234 &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "corrupt tone state was not rejected transactionally");
	CHECK(strcmp(
		      sippymodem_v34_phase2_tone_result_name(
			      SIPPYMODEM_V34_PHASE2_TONE_OK),
		      "ok") == 0 &&
		      strcmp(
			      sippymodem_v34_phase2_tone_result_name(
				      (enum sippymodem_v34_phase2_tone_result)99),
			      "unknown") == 0,
	      "tone result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "tone levels", test_tone_levels },
		{ "phase reversal", test_phase_reversal },
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
