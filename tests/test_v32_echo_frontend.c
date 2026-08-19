// SPDX-License-Identifier: BSD-3-Clause

#include <echo_frontend.h>
#include <qam.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	PROFILE_INPUT_SAMPLES = 4096,
	SPECTRUM_INPUT_SAMPLES = 8800,
	SPECTRUM_WARMUP_SAMPLES = 800,
	COMPOSED_TRAIN_INPUT_SAMPLES = 40000,
	COMPOSED_MEASURE_INPUT_SAMPLES = 8000,
	COMPOSED_FREEZE_INPUT_SAMPLES = 8000,
	COMPOSED_ECHO_HISTORY = 32,
	EQUIVALENCE_INPUT_SAMPLES = 997,
};

static const double TEST_PI = 3.14159265358979323846;
static const struct qam_channel V32_TRANSMIT_CHANNEL = {
	.sample_rate = V32_ECHO_FRONTEND_INPUT_RATE,
	.symbol_rate_numerator = 2400,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = 1800,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = 6,
};

struct frontend_driver {
	struct v32_echo_frontend frontend;
	struct qam_modulator modulator;
	struct qam_point transmit_point;
	uint64_t output_hash;
	uint32_t random;
	size_t accepted_count;
	size_t output_count;
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static struct qam_point fixture_point(size_t symbol)
{
	double in_phase = (symbol & 1u) != 0 ? -0.5 : 0.5;
	double quadrature = (symbol & 2u) != 0 ? -0.25 : 0.25;

	return (struct qam_point){ in_phase, quadrature };
}

static uint64_t hash_output(uint64_t hash, double cleaned)
{
	long rounded = lround(cleaned);
	uint16_t word = (uint16_t)rounded;

	hash ^= word & UINT16_C(0xff);
	hash *= UINT64_C(1099511628211);
	hash ^= word >> 8;
	hash *= UINT64_C(1099511628211);
	return hash;
}

static bool driver_init(struct frontend_driver *driver, uint32_t seed)
{
	memset(driver, 0, sizeof(*driver));
	driver->output_hash = UINT64_C(14695981039346656037);
	driver->random = seed;
	CHECK(v32_echo_frontend_init(
		      &driver->frontend, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) == V32_ECHO_OK,
	      "echo frontend did not initialize");
	CHECK(qam_modulator_init(
		      &driver->modulator, &V32_TRANSMIT_CHANNEL, 12000) ==
		      QAM_OK,
	      "V.32 reference clock did not initialize");
	return true;
}

static bool driver_sample(
	struct frontend_driver *driver, int16_t received,
	double *cleaned, bool *cleaned_ready, bool *symbol_accepted)
{
	bool can_accept =
		qam_modulator_can_accept_symbol(&driver->modulator);
	bool accepted;
	struct v32_echo_reference reference;
	int16_t ignored_transmit;

	if (can_accept) {
		driver->transmit_point =
			fixture_point(driver->accepted_count);
		CHECK(v32_echo_reference_init(
			      &reference, &driver->transmit_point,
			      driver->modulator.carrier_phase) == V32_ECHO_OK,
		      "reference rotation failed at symbol %zu",
		      driver->accepted_count);
	}
	CHECK(qam_modulator_sample(
		      &driver->modulator, &driver->transmit_point,
		      &ignored_transmit, &accepted) == QAM_OK &&
	      accepted == can_accept,
	      "reference clock rejected sample %zu",
	      driver->accepted_count);
	CHECK(v32_echo_frontend_sample(
		      &driver->frontend, received,
		      accepted ? &reference : NULL,
		      cleaned, cleaned_ready) == V32_ECHO_OK,
	      "frontend rejected an aligned reference at symbol %zu",
	      driver->accepted_count);
	if (accepted)
		driver->accepted_count++;
	if (*cleaned_ready) {
		driver->output_hash =
			hash_output(driver->output_hash, *cleaned);
		driver->output_count++;
	}
	if (symbol_accepted != NULL)
		*symbol_accepted = accepted;
	return true;
}

static bool test_cadence_and_profile(void)
{
	static const size_t expected_acceptance[] = {
		0, 4, 7, 10, 14, 17, 20, 24, 27, 30, 34, 37,
	};
	static const int expected_impulse[] = { 39, 95, 156, 67 };
	struct frontend_driver driver;
	size_t acceptance[ARRAY_LENGTH(expected_acceptance)];
	int impulse[ARRAY_LENGTH(expected_impulse)];
	size_t accepted = 0;
	size_t outputs = 0;

	CHECK(driver_init(&driver, UINT32_C(0x12345678)),
	      "cadence fixture failed to initialize");
	for (size_t sample = 0; sample < 40; ++sample) {
		double cleaned = 0.0;
		bool ready;
		bool symbol_accepted;

		CHECK(driver_sample(
			      &driver, sample == 0 ? INT16_MAX : 0,
			      &cleaned, &ready, &symbol_accepted),
		      "cadence sample %zu failed", sample);
		if (symbol_accepted)
			acceptance[accepted++] = sample;
		if (ready && outputs < ARRAY_LENGTH(impulse))
			impulse[outputs] = (int)lround(cleaned);
		if (ready)
			outputs++;
	}
	CHECK(accepted == ARRAY_LENGTH(expected_acceptance) &&
	      memcmp(acceptance, expected_acceptance,
		     sizeof(expected_acceptance)) == 0,
	      "transmit epochs did not follow 0,4,7,10 cadence");
	CHECK(outputs == 36 && driver.output_count == 36,
	      "40 line samples produced %zu echo samples", outputs);
	CHECK(memcmp(impulse, expected_impulse, sizeof(impulse)) == 0,
	      "phase-six impulse response began %d,%d,%d,%d",
	      impulse[0], impulse[1], impulse[2], impulse[3]);

	CHECK(driver_init(&driver, UINT32_C(0x12345678)),
	      "profile fixture failed to reset");
	for (size_t sample = 0; sample < PROFILE_INPUT_SAMPLES; ++sample) {
		uint32_t random = fixture_random(&driver.random);
		int16_t received = (int16_t)(random >> 16);
		double cleaned = 0.0;
		bool ready;

		CHECK(driver_sample(
			      &driver, received, &cleaned, &ready, NULL),
		      "profile sample %zu failed", sample);
	}
	CHECK(driver.output_count == 3686 &&
	      driver.output_hash == UINT64_C(0x41429e73578ce6b1),
	      "line-filter profile changed: count=%zu digest=%016llx",
	      driver.output_count,
	      (unsigned long long)driver.output_hash);
	return true;
}

static bool test_reference_contract(void)
{
	struct v32_echo_frontend frontend;
	struct v32_echo_frontend saved;
	struct qam_point point = { 0.5, -0.25 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point overflowing = { DBL_MAX, DBL_MAX };
	struct v32_echo_reference reference = {
		.point = { -1.0, 1.0 },
	};
	struct v32_echo_reference saved_reference = reference;
	struct v32_echo_reference quarter_turn;
	struct v32_echo_reference invalid_reference = {
		.point = { NAN, 0.0 },
	};
	double cleaned = -123.0;
	double saved_cleaned;
	bool ready = true;
	bool saved_ready;

	CHECK(v32_echo_reference_init(NULL, &point, 0) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_reference_init(&reference, NULL, 0) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_reference_init(&reference, &invalid, 0) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_reference_init(
		      &reference, &overflowing, UINT32_C(0x20000000)) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&reference, &saved_reference, sizeof(reference)) == 0,
	      "invalid reference construction changed its destination");
	CHECK(v32_echo_reference_init(&reference, &point, 0) ==
		      V32_ECHO_OK &&
	      reference.point.in_phase == point.in_phase &&
	      reference.point.quadrature == point.quadrature &&
	      v32_echo_reference_init(
		      &quarter_turn, &point, UINT32_C(0x40000000)) ==
		      V32_ECHO_OK &&
	      fabs(quarter_turn.point.in_phase - 0.25) < 1e-15 &&
	      fabs(quarter_turn.point.quadrature - 0.5) < 1e-15,
	      "reference construction did not preserve its carrier epoch");
	CHECK(v32_echo_frontend_init(
		      &frontend, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK,
	      "reference fixture did not initialize");
	saved = frontend;
	saved_cleaned = cleaned;
	saved_ready = ready;
	CHECK(v32_echo_frontend_sample(
		      NULL, 100, &reference, &cleaned, &ready) ==
		      V32_ECHO_INVALID_STATE &&
	      v32_echo_frontend_sample(
		      &frontend, 100, &reference, NULL, &ready) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_frontend_sample(
		      &frontend, 100, &reference, &cleaned, NULL) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_frontend_sample(
		      &frontend, 100,
		      (const struct v32_echo_reference *)
			      &frontend.echo.reference[0],
		      &cleaned, &ready) == V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_frontend_sample(
		      &frontend, 100, &reference,
		      &frontend.echo.reference[0].in_phase, &ready) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_frontend_sample(
		      &frontend, 100, &reference, &cleaned,
		      (bool *)&frontend.echo.reference[0].in_phase) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      v32_echo_frontend_sample(
		      &frontend, 100, &reference, &cleaned,
		      (bool *)&cleaned) == V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "null or overlapping arguments changed state or output");
	CHECK(v32_echo_frontend_sample(
		      &frontend, 100, NULL, &cleaned, &ready) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "missing first reference changed state or output");
	CHECK(v32_echo_frontend_sample(
		      &frontend, 100, &reference, &cleaned, &ready) ==
		      V32_ECHO_OK && ready,
	      "first aligned reference was rejected");
	saved = frontend;
	saved_cleaned = cleaned;
	saved_ready = ready;
	CHECK(v32_echo_frontend_sample(
		      &frontend, 200, &reference, &cleaned, &ready) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "extra mid-group reference changed state or output");
	CHECK(v32_echo_frontend_sample(
		      &frontend, 200, NULL, &cleaned, &ready) == V32_ECHO_OK &&
	      ready &&
	      v32_echo_frontend_sample(
		      &frontend, 300, NULL, &cleaned, &ready) == V32_ECHO_OK &&
	      ready &&
	      v32_echo_frontend_sample(
		      &frontend, 400, NULL, &cleaned, &ready) == V32_ECHO_OK &&
	      !ready,
	      "three-phase group or 9/10 input gap was not preserved");
	saved = frontend;
	saved_cleaned = cleaned;
	saved_ready = ready;
	CHECK(v32_echo_frontend_sample(
		      &frontend, 500, NULL, &cleaned, &ready) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "missing later reference changed state or output");
	CHECK(v32_echo_frontend_sample(
		      &frontend, 500, &invalid_reference, &cleaned, &ready) ==
		      V32_ECHO_INVALID_ARGUMENT &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "nonfinite reference changed state or output");
	return true;
}

static bool test_push_rollback(void)
{
	struct v32_echo_frontend frontend;
	struct v32_echo_frontend saved;
	struct qam_point point = { 0.5, -0.25 };
	struct v32_echo_reference reference;
	double cleaned = 321.0;
	double saved_cleaned;
	bool ready = true;
	bool saved_ready;
	size_t calls = 0;

	CHECK(v32_echo_frontend_init(
		      &frontend, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK,
	      "rollback fixture did not initialize");
	while (frontend.echo.reference_fill < V32_ECHO_REFERENCE_CAPACITY ||
	       frontend.filter.inputs_needed != 1 || frontend.echo.phase != 0) {
		bool expects = frontend.filter.inputs_needed == 1 &&
			       frontend.echo.phase == 0;

		point = fixture_point(frontend.echo.reference_fill + calls);
		if (expects) {
			CHECK(v32_echo_reference_init(&reference, &point, 0) ==
				      V32_ECHO_OK,
			      "rollback reference construction failed");
		}
		CHECK(v32_echo_frontend_sample(
			      &frontend, (int16_t)calls,
			      expects ? &reference : NULL, &cleaned, &ready) ==
			      V32_ECHO_OK,
		      "rollback history setup failed at sample %zu", calls);
		calls++;
		CHECK(calls < 10000, "rollback history setup did not converge");
	}
	frontend.echo.coefficients[0][V32_ECHO_NEAR_WINDOW][0].in_phase =
		INFINITY;
	saved = frontend;
	saved_cleaned = cleaned;
	saved_ready = ready;
	point = fixture_point(calls);
	CHECK(v32_echo_reference_init(&reference, &point, 0) == V32_ECHO_OK,
	      "failed-path reference construction failed");
	CHECK(v32_echo_frontend_sample(
		      &frontend, 777, &reference, &cleaned, &ready) ==
		      V32_ECHO_INVALID_STATE &&
	      memcmp(&frontend, &saved, sizeof(frontend)) == 0 &&
	      cleaned == saved_cleaned && ready == saved_ready,
	      "failed cancellation did not roll back the pushed reference");
	return true;
}

static bool measure_gain(double frequency, double *gain)
{
	struct frontend_driver driver;
	double input_energy = 0.0;
	double output_energy = 0.0;
	size_t input_count = 0;
	size_t output_count = 0;

	CHECK(driver_init(&driver, UINT32_C(0x13579bdf)),
	      "spectrum fixture did not initialize");
	for (size_t sample = 0; sample < SPECTRUM_INPUT_SAMPLES; ++sample) {
		double waveform = 1000.0 * cos(
			2.0 * TEST_PI * frequency * (double)sample /
			(double)V32_ECHO_FRONTEND_INPUT_RATE);
		int16_t received = (int16_t)lround(waveform);
		double cleaned = 0.0;
		bool ready;

		CHECK(driver_sample(
			      &driver, received, &cleaned, &ready, NULL),
		      "spectrum sample %zu failed", sample);
		if (sample < SPECTRUM_WARMUP_SAMPLES)
			continue;
		input_energy += (double)received * (double)received;
		input_count++;
		if (ready) {
			output_energy += cleaned * cleaned;
			output_count++;
		}
	}
	CHECK(input_count != 0 && output_count != 0,
	      "spectrum fixture produced no measurement");
	*gain = sqrt(
		(output_energy / (double)output_count) /
		(input_energy / (double)input_count));
	return true;
}

static bool test_voice_band(void)
{
	static const double pass_frequency[] = { 600.0, 1800.0, 3000.0 };
	static const double stop_frequency[] = { 0.0, 3600.0 };

	for (size_t index = 0; index < ARRAY_LENGTH(pass_frequency); ++index) {
		double gain;

		CHECK(measure_gain(pass_frequency[index], &gain),
		      "pass-band measurement failed");
		CHECK(gain > 0.85 && gain < 1.2,
		      "%.0f Hz pass-band gain was %.3f",
		      pass_frequency[index], gain);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(stop_frequency); ++index) {
		double gain;

		CHECK(measure_gain(stop_frequency[index], &gain),
		      "stop-band measurement failed");
		CHECK(gain < 0.05, "%.0f Hz stop-band gain was %.3f",
		      stop_frequency[index], gain);
	}
	return true;
}

struct composed_echo_path {
	int16_t samples[COMPOSED_ECHO_HISTORY];
	size_t write;
};

struct composed_fixture {
	struct v32_echo_frontend adaptive;
	struct v32_echo_frontend control;
	struct qam_modulator modulator;
	struct composed_echo_path path;
	struct qam_point point;
	uint32_t random;
	size_t accepted_count;
};

static int16_t echo_at_age(
	const struct composed_echo_path *path, size_t age)
{
	size_t index = path->write + COMPOSED_ECHO_HISTORY - 1u - age;

	if (index >= COMPOSED_ECHO_HISTORY)
		index -= COMPOSED_ECHO_HISTORY;
	return path->samples[index];
}

static int16_t composed_echo_sample(
	struct composed_echo_path *path, int16_t transmit)
{
	double echo;

	path->samples[path->write] = transmit;
	path->write++;
	if (path->write == COMPOSED_ECHO_HISTORY)
		path->write = 0;
	echo = 0.45 * (double)echo_at_age(path, 0) +
	       0.25 * (double)echo_at_age(path, 7) -
	       0.15 * (double)echo_at_age(path, 23);
	return (int16_t)lround(echo);
}

static bool composed_step(
	struct composed_fixture *fixture, size_t sample,
	double *adaptive_cleaned, double *control_cleaned, bool *ready)
{
	bool can_accept =
		qam_modulator_can_accept_symbol(&fixture->modulator);
	bool accepted;
	bool control_ready;
	struct v32_echo_reference reference;
	int16_t transmit;
	int16_t received;

	if (can_accept) {
		uint32_t bits = fixture_random(&fixture->random);

		fixture->point.in_phase =
			(bits & 1u) != 0 ? -0.75 : 0.75;
		fixture->point.quadrature =
			(bits & 2u) != 0 ? -0.75 : 0.75;
		CHECK(v32_echo_reference_init(
			      &reference, &fixture->point,
			      fixture->modulator.carrier_phase) == V32_ECHO_OK,
		      "composed reference rotation failed at sample %zu", sample);
	}
	CHECK(qam_modulator_sample(
		      &fixture->modulator, &fixture->point,
		      &transmit, &accepted) == QAM_OK &&
	      accepted == can_accept,
	      "composed QAM clock failed at sample %zu", sample);
	received = composed_echo_sample(&fixture->path, transmit);
	if (accepted)
		fixture->accepted_count++;
	CHECK(v32_echo_frontend_sample(
		      &fixture->adaptive, received,
		      accepted ? &reference : NULL,
		      adaptive_cleaned, ready) == V32_ECHO_OK &&
	      v32_echo_frontend_sample(
		      &fixture->control, received,
		      accepted ? &reference : NULL,
		      control_cleaned, &control_ready) == V32_ECHO_OK &&
	      *ready == control_ready,
	      "composed frontends diverged at sample %zu", sample);
	return true;
}

static bool test_composed_echo(void)
{
	struct composed_fixture fixture = {
		.random = UINT32_C(0x2468ace1),
	};
	double adaptive_training = 0.0;
	double control_training = 0.0;
	double adaptive_frozen = 0.0;
	double control_frozen = 0.0;

	CHECK(v32_echo_frontend_init(
		      &fixture.adaptive, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK &&
	      v32_echo_frontend_init(
		      &fixture.control, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK &&
	      v32_echo_frontend_start_adaptation(
		      &fixture.adaptive, 0.65) ==
		      V32_ECHO_OK &&
	      qam_modulator_init(
		      &fixture.modulator, &V32_TRANSMIT_CHANNEL, 12000) ==
		      QAM_OK,
	      "composed echo fixture did not initialize");
	for (size_t sample = 0;
	     sample < COMPOSED_TRAIN_INPUT_SAMPLES +
			      COMPOSED_FREEZE_INPUT_SAMPLES;
	     ++sample) {
		bool ready;
		double adaptive_cleaned = 0.0;
		double control_cleaned = 0.0;

		if (sample == COMPOSED_TRAIN_INPUT_SAMPLES / 2u) {
			CHECK(v32_echo_frontend_start_adaptation(
				      &fixture.adaptive, 0.065) == V32_ECHO_OK,
			      "composed frontend did not enter fine adaptation");
		}
		if (sample == COMPOSED_TRAIN_INPUT_SAMPLES) {
			CHECK(v32_echo_frontend_freeze(&fixture.adaptive) ==
				      V32_ECHO_OK,
			      "trained frontend did not freeze at a group boundary");
		}
		CHECK(composed_step(
			      &fixture, sample, &adaptive_cleaned,
			      &control_cleaned, &ready),
		      "composed echo step failed");
		if (ready && sample < COMPOSED_TRAIN_INPUT_SAMPLES &&
		    sample >= COMPOSED_TRAIN_INPUT_SAMPLES -
				      COMPOSED_MEASURE_INPUT_SAMPLES) {
			adaptive_training += adaptive_cleaned * adaptive_cleaned;
			control_training += control_cleaned * control_cleaned;
		} else if (ready &&
			   sample >= COMPOSED_TRAIN_INPUT_SAMPLES) {
			adaptive_frozen += adaptive_cleaned * adaptive_cleaned;
			control_frozen += control_cleaned * control_cleaned;
		}
	}
	CHECK(control_training > 0.0 && control_frozen > 0.0,
	      "composed control had no echo energy");
	CHECK(fixture.accepted_count == 14400,
	      "composed clock accepted %zu references instead of 14400",
	      fixture.accepted_count);
	CHECK(isfinite(adaptive_training / control_training) &&
	      isfinite(adaptive_frozen / control_frozen),
	      "composed ratio was not finite");
	CHECK(adaptive_training < 1e-5 * control_training,
	      "fine composed residual ratio was %.9g",
	      adaptive_training / control_training);
	CHECK(adaptive_frozen < 1e-5 * control_frozen,
	      "frozen composed residual ratio was %.9g",
	      adaptive_frozen / control_frozen);
	return true;
}

static bool advance_driver(struct frontend_driver *driver, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		int16_t received =
			(int16_t)(fixture_random(&driver->random) >> 16);
		double cleaned = 0.0;
		bool ready;

		CHECK(driver_sample(
			      driver, received, &cleaned, &ready, NULL),
		      "fragmented stream failed at local sample %zu", index);
	}
	return true;
}

static bool drivers_match(
	const struct frontend_driver *left,
	const struct frontend_driver *right)
{
	return memcmp(&left->frontend, &right->frontend,
		      sizeof(left->frontend)) == 0 &&
	       memcmp(&left->modulator, &right->modulator,
		      sizeof(left->modulator)) == 0 &&
	       left->output_hash == right->output_hash &&
	       left->random == right->random &&
	       left->accepted_count == right->accepted_count &&
	       left->output_count == right->output_count;
}

static bool test_fragmentation_reset_and_reentrancy(void)
{
	static const size_t chunks[] = { 1, 7, 40, 3, 19 };
	struct frontend_driver reference_a;
	struct frontend_driver reference_b;
	struct frontend_driver interleaved_a;
	struct frontend_driver interleaved_b;
	struct v32_echo_frontend fresh;
	size_t done_a = 0;
	size_t done_b = 0;
	size_t chunk = 0;

	CHECK(driver_init(&reference_a, UINT32_C(0x11111111)) &&
	      driver_init(&reference_b, UINT32_C(0x22222222)) &&
	      driver_init(&interleaved_a, UINT32_C(0x11111111)) &&
	      driver_init(&interleaved_b, UINT32_C(0x22222222)),
	      "equivalence fixtures did not initialize");
	CHECK(advance_driver(&reference_a, EQUIVALENCE_INPUT_SAMPLES) &&
	      advance_driver(&reference_b, EQUIVALENCE_INPUT_SAMPLES),
	      "sequential equivalence runs failed");
	while (done_a < EQUIVALENCE_INPUT_SAMPLES ||
	       done_b < EQUIVALENCE_INPUT_SAMPLES) {
		size_t count = chunks[chunk++ % ARRAY_LENGTH(chunks)];

		if (count > EQUIVALENCE_INPUT_SAMPLES - done_a)
			count = EQUIVALENCE_INPUT_SAMPLES - done_a;
		if (count != 0) {
			CHECK(advance_driver(&interleaved_a, count),
			      "fragmented stream A failed");
			done_a += count;
		}
		count = chunks[chunk++ % ARRAY_LENGTH(chunks)];
		if (count > EQUIVALENCE_INPUT_SAMPLES - done_b)
			count = EQUIVALENCE_INPUT_SAMPLES - done_b;
		if (count != 0) {
			CHECK(advance_driver(&interleaved_b, count),
			      "fragmented stream B failed");
			done_b += count;
		}
	}
	CHECK(drivers_match(&reference_a, &interleaved_a) &&
	      drivers_match(&reference_b, &interleaved_b),
	      "fragmentation or interleaving changed an owner");
	CHECK(v32_echo_frontend_init(
		      &interleaved_a.frontend, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) == V32_ECHO_OK &&
	      v32_echo_frontend_init(
		      &fresh, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_ECHO_OK &&
	      memcmp(&interleaved_a.frontend, &fresh, sizeof(fresh)) == 0,
	      "reset did not restore a fresh frontend");
	return true;
}

int main(void)
{
	if (!test_cadence_and_profile() || !test_reference_contract() ||
	    !test_push_rollback() || !test_voice_band() ||
	    !test_composed_echo() ||
	    !test_fragmentation_reset_and_reentrancy())
		return 1;
	puts("V.32 echo frontend tests passed");
	return 0;
}
