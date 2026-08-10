// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_dpsk.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
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
			       SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE;

		in_phase += samples[index] * cos(angle);
		quadrature += samples[index] * sin(angle);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool render_station(
	enum sippymodem_v34_phase2_station_role role,
	int16_t samples[SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE],
	unsigned int *accepted_count)
{
	struct sippymodem_v34_phase2_dpsk_transmitter transmitter;

	CHECK(sippymodem_v34_phase2_dpsk_transmitter_init(
		      &transmitter, role, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V34_PHASE2_DPSK_OK,
	      "Phase 2 transmitter did not initialize");
	*accepted_count = 0;
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE; ++index) {
		bool accepted;

		CHECK(sippymodem_v34_phase2_dpsk_transmitter_sample(
			      &transmitter, 0, &samples[index], &accepted) ==
			      SIPPYMODEM_V34_PHASE2_DPSK_OK,
		      "Phase 2 transmitter failed at sample %zu", index);
		if (accepted)
			(*accepted_count)++;
	}
	CHECK(!transmitter.initial_point_pending,
	      "Phase 2 transmitter omitted its reference point");
	if (role == SIPPYMODEM_V34_PHASE2_CALLING_STATION) {
		CHECK(transmitter.carrier.amplitude == TEST_AMPLITUDE &&
			      transmitter.guard_amplitude == 0,
		      "calling transmitter changed its nominal level");
	} else {
		CHECK(transmitter.carrier.amplitude >= 10690 &&
			      transmitter.carrier.amplitude <= 10700 &&
			      transmitter.guard_amplitude >= 5355 &&
			      transmitter.guard_amplitude <= 5365,
		      "answering transmitter changed its relative levels");
	}
	return true;
}

static bool test_carriers_and_cadence(void)
{
	int16_t calling[SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE];
	int16_t answering[SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE];
	const size_t skip = SIPPYMODEM_V34_PHASE2_DPSK_SAMPLE_RATE / 4;
	const size_t count = ARRAY_LENGTH(calling) - skip;
	unsigned int calling_accepted;
	unsigned int answering_accepted;

	CHECK(render_station(
		      SIPPYMODEM_V34_PHASE2_CALLING_STATION, calling,
		      &calling_accepted) &&
		      render_station(
			      SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
			      answering, &answering_accepted),
	      "Phase 2 waveform fixture failed");
	CHECK(calling_accepted ==
		      SIPPYMODEM_V34_PHASE2_DPSK_BIT_RATE - 1 &&
		      answering_accepted ==
			      SIPPYMODEM_V34_PHASE2_DPSK_BIT_RATE - 1,
	      "Phase 2 symbol clock drifted over one second");
	CHECK(tone_projection(
		      calling + skip, count,
		      SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ) >
		      tone_projection(
			      calling + skip, count,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0,
	      "calling transmitter did not retain its carrier");
	CHECK(tone_projection(
		      answering + skip, count,
		      SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ) >
		      tone_projection(
			      answering + skip, count,
			      SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ) *
			      1000.0 &&
		      tone_projection(
			      answering + skip, count,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ) >
			      tone_projection(
				      answering + skip, count,
				      SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ) *
				      1000.0,
	      "answering transmitter omitted its carrier or guard tone");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase2_dpsk_transmitter transmitter;
	unsigned char snapshot[sizeof(transmitter)];
	int16_t sample = 123;
	bool ready = true;

	CHECK(sippymodem_v34_phase2_dpsk_transmitter_init(
		      NULL, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE) ==
		      SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_dpsk_transmitter_init(
			      &transmitter,
			      (enum sippymodem_v34_phase2_station_role)99,
			      TEST_AMPLITUDE) ==
			      SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_dpsk_transmitter_init(
			      &transmitter,
			      SIPPYMODEM_V34_PHASE2_ANSWERING_STATION,
			      30000) ==
			      SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT,
	      "Phase 2 transmitter accepted an invalid configuration");
	CHECK(sippymodem_v34_phase2_dpsk_transmitter_init(
		      &transmitter, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE) == SIPPYMODEM_V34_PHASE2_DPSK_OK,
	      "Phase 2 validation fixture did not initialize");
	memcpy(snapshot, &transmitter, sizeof(snapshot));
	CHECK(sippymodem_v34_phase2_dpsk_transmitter_sample(
		      &transmitter, 2, &sample, &ready) ==
		      SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
		      sample == 123 && ready,
	      "Phase 2 transmitter mutated state after invalid input");
	CHECK(strcmp(sippymodem_v34_phase2_dpsk_result_name(
			     SIPPYMODEM_V34_PHASE2_DPSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "Phase 2 transmitter returned an unstable result name");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "carriers and cadence", test_carriers_and_cadence },
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
