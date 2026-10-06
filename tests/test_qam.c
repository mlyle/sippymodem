// SPDX-License-Identifier: BSD-3-Clause

#include <qam.h>
#include <qam_gain.h>

#include "test.h"
#include "qam_state_fixture.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_SYMBOL_RATE = 600,
	TEST_AMPLITUDE = 12000,
	TEST_SYMBOL_COUNT = 640,
	TEST_SYNC_SYMBOLS = 32,
	TEST_PAYLOAD_SYMBOLS = 128,
	TRACKING_SYMBOL_COUNT = 8192,
	TRACKING_DECODED_CAPACITY = TRACKING_SYMBOL_COUNT + 1024,
	TRACKING_SYNC_SYMBOLS = 64,
	TRACKING_PAYLOAD_SYMBOLS = 2048,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static struct qam_channel v22_channel(uint32_t carrier_hz)
{
	return (struct qam_channel){
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = TEST_SYMBOL_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = carrier_hz,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.75,
		.pulse_span_symbols = 6,
	};
}

static struct qam_point quadrant_point(
	uint8_t quadrant, double magnitude)
{
	switch (quadrant & 3u) {
	case 0:
		return (struct qam_point){ magnitude, 0.0 };
	case 1:
		return (struct qam_point){ 0.0, magnitude };
	case 2:
		return (struct qam_point){ -magnitude, 0.0 };
	default:
		return (struct qam_point){ 0.0, -magnitude };
	}
}

static struct qam_point complex_product(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static bool points_are_close(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return fabs(left->in_phase - right->in_phase) < 1e-12 &&
	       fabs(left->quadrature - right->quadrature) < 1e-12;
}

static bool test_coordinate_quantization(void)
{
	const double invalid[] = {
		NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX,
		INT16_MIN - 0.5, INT16_MAX + 0.5,
	};
	const double zero[] = { 0.0, -0.0, DBL_MIN, -DBL_MIN };

	CHECK(!qam_coordinate_to_s16(0.0, NULL),
	      "coordinate quantizer accepted a null output");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid); ++index) {
		int16_t fixed = 12345;

		CHECK(!qam_coordinate_to_s16(invalid[index], &fixed) &&
		      fixed == 12345,
		      "invalid coordinate %zu changed the output", index);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(zero); ++index) {
		int16_t fixed = 12345;

		CHECK(qam_coordinate_to_s16(zero[index], &fixed) && fixed == 0,
		      "near-zero coordinate %zu did not round to zero", index);
	}
	/* Check every half-integer boundary and its immediate neighbors.
	 * Integer expectations do not reuse the quantizer or its lround call. */
	for (int32_t lower = INT16_MIN - 1; lower <= INT16_MAX; ++lower) {
		double half = lower + 0.5;
		const double coordinates[] = {
			nextafter(half, -INFINITY), half, nextafter(half, INFINITY),
		};
		const int32_t expected[] = {
			lower, lower < 0 ? lower : lower + 1, lower + 1,
		};

		for (size_t index = 0; index < ARRAY_LENGTH(coordinates); ++index) {
			bool fits = expected[index] >= INT16_MIN &&
				    expected[index] <= INT16_MAX;
			int16_t fixed = 12345;

			CHECK(qam_coordinate_to_s16(coordinates[index], &fixed) ==
				      fits &&
			      fixed == (fits ? expected[index] : 12345),
			      "half-integer boundary %d/%zu quantized incorrectly",
			      lower, index);
		}
	}
	for (int32_t word = INT16_MIN; word <= INT16_MAX; ++word) {
		int16_t fixed = 12345;

		CHECK(qam_coordinate_to_s16(word, &fixed) && fixed == word,
		      "integer coordinate %d changed", word);
	}
	return true;
}

static bool test_point_validation(void)
{
	const struct qam_point finite = { 1.0, -2.0 };
	const struct qam_point bad_in_phase = { NAN, 0.0 };
	const struct qam_point bad_quadrature = { 0.0, INFINITY };
	const struct qam_point invalid_power[] = {
		{ 0.0, -0.0 }, bad_in_phase, bad_quadrature, { DBL_MAX, 0.0 },
		{ nextafter(1e-6, 0.0), 0.0 }, { 1e-6, 0.0 },
	};
	const struct qam_point above = { nextafter(1e-6, INFINITY), 0.0 };
	struct qam_point alias_i = finite, alias_q = finite;
	struct qam_point rejected_alias = { 1e-6, 0.0 };
	double power = 7.0;

	CHECK(qam_point_power(&finite) == 5.0 &&
	      qam_point_power(&invalid_power[0]) == 0.0 &&
	      !signbit(qam_point_power(&invalid_power[0])) &&
	      isnan(qam_point_power(&bad_in_phase)) &&
	      isinf(qam_point_power(&bad_quadrature)) &&
	      isinf(qam_point_power(&invalid_power[3])),
	      "raw point power added validation or changed arithmetic");
	CHECK(point_is_finite(&finite) &&
	      !point_is_finite(NULL) &&
	      !point_is_finite(&bad_in_phase) &&
	      !point_is_finite(&bad_quadrature),
	      "QAM point finiteness accepted a non-finite coordinate");
	CHECK(qam_point_fits_power(&finite, 5.0) &&
	      qam_point_fits_power(&finite, 5.0 - 1e-12) &&
	      !qam_point_fits_power(&finite, 4.0) &&
	      !qam_point_fits_power(NULL, 5.0) &&
	      !qam_point_fits_power(&bad_in_phase, 5.0) &&
	      !qam_point_fits_power(&finite, 0.0) &&
	      !qam_point_fits_power(&finite, NAN),
	      "QAM point power bounds changed");
	CHECK(!qam_point_nonzero_power(NULL, &power) && power == 7.0,
	      "null power input changed output");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid_power); ++index)
		CHECK(!qam_point_nonzero_power(&invalid_power[index], &power) &&
		      power == 7.0,
		      "invalid power case %zu changed output", index);
	CHECK(qam_point_nonzero_power(&finite, &power) && power == 5.0 &&
	      qam_point_nonzero_power(&finite, NULL) &&
	      qam_point_nonzero_power(&above, NULL) &&
	      qam_point_nonzero_power(&alias_i, &alias_i.in_phase) &&
	      alias_i.in_phase == 5.0 && alias_i.quadrature == -2.0 &&
	      qam_point_nonzero_power(&alias_q, &alias_q.quadrature) &&
	      alias_q.in_phase == 1.0 && alias_q.quadrature == 5.0,
	      "nonzero power boundary, optional output, or aliasing changed");
	CHECK(!qam_point_nonzero_power(&rejected_alias, &rejected_alias.in_phase) &&
	      rejected_alias.in_phase == 1e-6 && rejected_alias.quadrature == 0.0,
	      "rejected aliased power changed the input point");
	return true;
}

static bool test_point_quotient(void)
{
	static const struct {
		struct qam_point numerator, denominator, expected;
	} cases[] = {
		{ { 6, 2 }, { 2, 0 }, { 3, 1 } },
		{ { 6, 2 }, { 0, 2 }, { 1, -3 } },
		{ { 2, 4 }, { 1, 1 }, { 3, 1 } },
		{ { 2, 4 }, { -1, 1 }, { 1, -3 } },
		{ { 0x1p480, 0 }, { 0x1p240, 0 }, { 0x1p240, 0 } },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		struct qam_point result = qam_point_quotient(
			&cases[i].numerator, &cases[i].denominator,
			qam_point_power(&cases[i].denominator));

		CHECK(result.in_phase == cases[i].expected.in_phase &&
		      result.quadrature == cases[i].expected.quadrature,
		      "raw quotient case %zu changed the complex division", i);
	}
	return true;
}

static bool test_point_correlation(void)
{
	static const struct {
		struct qam_point reference, sample, expected;
	} cases[] = {
		{ { 1, 2 }, { 3, 4 }, { 11, -2 } },
		{ { 3, 4 }, { 1, 2 }, { 11, 2 } },
		{ { -1, 2 }, { 3, 4 }, { 5, -10 } },
		{ { 0x1p511, 0 }, { 0x1p511, 0 }, { 0x1p1022, 0 } },
		{ { 0x1p-530, 0 }, { 0x1p-530, 0 }, { 0x1p-1060, 0 } },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		struct qam_point result = qam_point_correlation(
			&cases[i].reference, &cases[i].sample);
		CHECK(result.in_phase == cases[i].expected.in_phase &&
		      result.quadrature == cases[i].expected.quadrature,
		      "correlation case %zu conjugated the wrong operand", i);
	}
	for (unsigned signs = 0; signs < 16; ++signs) {
		struct qam_point a = { signs & 1 ? -0.0 : 0.0, signs & 2 ? -0.0 : 0.0 };
		struct qam_point b = { signs & 4 ? -0.0 : 0.0, signs & 8 ? -0.0 : 0.0 };
		struct qam_point expected = {
			a.in_phase * b.in_phase + a.quadrature * b.quadrature,
			a.in_phase * b.quadrature - a.quadrature * b.in_phase,
		};
		struct qam_point result = qam_point_correlation(&a, &b);
		CHECK(memcmp(&result, &expected, sizeof(result)) == 0,
		      "correlation changed signed zero %u", signs);
	}
	struct qam_point large = { DBL_MAX, 0.0 }, sample = { 2.0, 3.0 };
	struct qam_point result = qam_point_correlation(&large, &sample);
	CHECK(isinf(result.in_phase) && isinf(result.quadrature),
	      "raw correlation unexpectedly changed overflow policy");
	large.in_phase = NAN;
	result = qam_point_correlation(&large, &sample);
	CHECK(isnan(result.in_phase) && isnan(result.quadrature),
	      "raw correlation unexpectedly rejected NaN");
	return true;
}

static bool test_flat_gain_prediction(void)
{
	static const struct qam_point gains[] = {
		{ 1.0, 0.0 },
		{ 0.7, -0.3 },
		{ -1.2, 0.4 },
	};
	static const struct qam_point transmitted[] = {
		{ 0.4, 0.2 },
		{ -0.7, 0.1 },
		{ 0.25, -0.8 },
		{ -0.5, -0.5 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(gains); ++fixture) {
		struct qam_gain tracker;
		struct qam_point reference = complex_product(
			&gains[fixture], &transmitted[0]);

		CHECK(qam_gain_init_from_reference(
			      &tracker, &reference, &transmitted[0], 0.125) ==
			      QAM_GAIN_OK &&
		      qam_gain_is_valid(&tracker) &&
		      points_are_close(&tracker.channel_gain, &gains[fixture]),
		      "QAM gain fixture %zu did not initialize", fixture);
		for (size_t point = 0; point < ARRAY_LENGTH(transmitted);
		     ++point) {
			struct qam_point expected = complex_product(
				&gains[fixture], &transmitted[point]);
			struct qam_point predicted;

			CHECK(qam_gain_predict(
				      &tracker, &transmitted[point], &predicted) ==
				      QAM_GAIN_OK &&
			      points_are_close(&predicted, &expected),
			      "QAM gain fixture %zu changed point %zu", fixture,
			      point);
		}
	}
	return true;
}

static bool test_flat_gain_product_boundaries(void)
{
	const struct qam_point gain = { 2.0, -0.5 };
	const struct qam_point inputs[] = {
		{ 1.5, -2.0 }, { -0.0, -0.0 }, { 0.0, -0.0 },
		{ DBL_MIN, -DBL_MIN },
	};
	struct qam_gain tracker;
	struct qam_gain saved;
	struct qam_point point = { DBL_MAX, DBL_MAX };
	struct qam_point saved_point = point;
	struct qam_point expected = complex_product(&gain, &gain);

	CHECK(qam_gain_init(&tracker, &gain, 0.5) == QAM_GAIN_OK,
	      "product boundary gain did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(inputs); ++index) {
		struct qam_point actual = inputs[index];
		struct qam_point product = complex_product(&gain, &actual);

		CHECK(qam_gain_predict(&tracker, &actual, &actual) == QAM_GAIN_OK &&
		      actual.in_phase == product.in_phase &&
		      actual.quadrature == product.quadrature &&
		      signbit(actual.in_phase) == signbit(product.in_phase) &&
		      signbit(actual.quadrature) == signbit(product.quadrature),
		      "in-place gain product changed exact result %zu", index);
	}
	saved = tracker;
	CHECK(qam_gain_predict(&tracker, &point, &point) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_predict(&tracker, NULL, &point) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0 &&
	      memcmp(&tracker, &saved, sizeof(tracker)) == 0,
	      "overflow or null input changed gain state/output");
	CHECK(qam_gain_predict(
		      &tracker, &tracker.channel_gain, &tracker.channel_gain) ==
		      QAM_GAIN_OK &&
	      tracker.channel_gain.in_phase == expected.in_phase &&
	      tracker.channel_gain.quadrature == expected.quadrature,
	      "owner-aliased gain product changed");
	return true;
}

static bool test_flat_gain_adaptation(void)
{
	static const struct qam_point transmitted[] = {
		{ 0.5, 0.25 },
		{ -0.75, 0.5 },
		{ 0.4, -0.9 },
	};
	const struct qam_point initial = { 1.0, 0.0 };
	const struct qam_point actual = { 0.65, -0.35 };
	struct qam_point expected = initial;
	struct qam_gain tracker;

	CHECK(qam_gain_init(&tracker, &initial, 0.25) ==
		      QAM_GAIN_OK,
	      "QAM gain adaptation did not initialize");
	for (size_t update = 0; update < 24; ++update) {
		const struct qam_point *sent =
			&transmitted[update % ARRAY_LENGTH(transmitted)];
		struct qam_point received =
			complex_product(&actual, sent);

		expected.in_phase +=
			(actual.in_phase - expected.in_phase) * 0.25;
		expected.quadrature +=
			(actual.quadrature - expected.quadrature) * 0.25;
		CHECK(qam_gain_track(
			      &tracker, &received, sent) ==
			      QAM_GAIN_OK &&
		      points_are_close(&tracker.channel_gain, &expected),
		      "QAM gain adaptation changed update %zu", update);
	}
	{
		struct qam_gain frozen;
		struct qam_point received =
			complex_product(&actual, &transmitted[0]);

		CHECK(qam_gain_init(&frozen, &initial, 0.0) ==
			      QAM_GAIN_OK &&
		      qam_gain_track(
			      &frozen, &received, &transmitted[0]) ==
			      QAM_GAIN_OK &&
		      points_are_close(&frozen.channel_gain, &initial),
		      "zero QAM gain adaptation changed its estimate");
	}
	return true;
}

static bool test_flat_gain_validation(void)
{
	struct qam_gain tracker;
	struct qam_gain saved;
	struct qam_point gain = { 0.8, -0.2 };
	struct qam_point zero = { 0 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point overflow = { DBL_MAX, 0.0 };
	struct qam_point output = { 7.0, -9.0 };
	struct qam_point saved_output = output;

	memset(&tracker, 0xa5, sizeof(tracker));
	saved = tracker;
	CHECK(qam_gain_init(NULL, &gain, 0.5) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init(&tracker, NULL, 0.5) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init(&tracker, &zero, 0.5) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init(&tracker, &invalid, 0.5) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init(&tracker, &gain, -0.1) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init(&tracker, &gain, 1.1) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_init_from_reference(
		      &tracker, &gain, &zero, 0.5) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      memcmp(&tracker, &saved, sizeof(tracker)) == 0,
	      "invalid QAM gain initialization changed state");
	CHECK(qam_gain_init(&tracker, &gain, 0.5) ==
		      QAM_GAIN_OK &&
	      qam_gain_is_valid(&tracker) &&
	      !qam_gain_is_valid(NULL),
	      "QAM gain validation fixture did not initialize");
	saved = tracker;
	CHECK(qam_gain_predict(NULL, &gain, &output) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_predict(&tracker, &invalid, &output) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_predict(&tracker, &gain, NULL) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_track(NULL, &gain, &gain) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_track(&tracker, &zero, &gain) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_track(&tracker, &invalid, &gain) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_track(&tracker, &overflow, &gain) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      qam_gain_track(&tracker, &gain, &zero) ==
		      QAM_GAIN_INVALID_ARGUMENT &&
	      memcmp(&tracker, &saved, sizeof(tracker)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "invalid QAM gain operation changed state or output");
	tracker = saved;
	tracker.channel_gain = zero;
	CHECK(!qam_gain_is_valid(&tracker),
	      "QAM gain accepted a zero estimate");
	tracker = saved;
	tracker.adaptation = NAN;
	CHECK(!qam_gain_is_valid(&tracker) &&
	      strcmp(qam_gain_result_name(
		     QAM_GAIN_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(qam_gain_result_name(
		     (enum qam_gain_result)2),
		     "unknown") == 0,
	      "QAM gain accepted corrupt state or changed result names");
	return true;
}

static bool test_generated_pulse(void)
{
	double at_zero = 0.0;
	double at_singularity = 0.0;
	double reflected = 0.0;
	double ordinary = 0.0;
	double expected_zero = 1.0 + 0.75 * (4.0 / TEST_PI - 1.0);

	CHECK(root_raised_cosine(0.0, 0.75, &at_zero) ==
		      QAM_OK &&
		      fabs(at_zero - expected_zero) < 1e-12,
	      "QAM pulse changed its zero-time limit");
	CHECK(root_raised_cosine(
		      1.0 / 3.0, 0.75, &at_singularity) ==
		      QAM_OK &&
		      isfinite(at_singularity),
	      "QAM pulse did not handle its removable singularity");
	CHECK(root_raised_cosine(0.625, 0.75, &ordinary) ==
		      QAM_OK &&
		      root_raised_cosine(-0.625, 0.75, &reflected) ==
			      QAM_OK &&
		      fabs(ordinary - reflected) < 1e-12,
	      "QAM pulse lost even symmetry");
	return true;
}

static bool test_exact_symbol_clock(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_modulator modulator;
	unsigned int accepted_count = 0;
	int16_t peak = 0;

	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
		      qam_modulator_can_accept_symbol(&modulator),
	      "QAM symbol-clock fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE; ++index) {
		struct qam_point point = quadrant_point(
			(uint8_t)(accepted_count & 3u),
			(double)(accepted_count % 4u + 1u) / 4.0);
		int16_t sample;
		bool accepted;

		CHECK(qam_modulator_sample(
			      &modulator, &point,
			      &sample, &accepted) == QAM_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE,
		      "QAM modulator exceeded its waveform bound");
		if (accepted)
			accepted_count++;
		if (abs(sample) > peak)
			peak = (int16_t)abs(sample);
	}
	CHECK(accepted_count == TEST_SYMBOL_RATE &&
		      modulator.symbol_phase_units == 0 &&
		      qam_modulator_can_accept_symbol(&modulator) &&
		      peak > TEST_AMPLITUDE / 8,
	      "QAM rational symbol clock drifted over one second");
	return true;
}

static bool test_exact_carrier_clock(void)
{
	struct qam_channel channel = v22_channel(1);
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	const struct qam_point point = { 1.0, 0.0 };
	const uint32_t carrier_numerator = 96000;
	const uint32_t carrier_denominator = 49;
	uint64_t phase_numerator = (uint64_t)carrier_numerator << 32;
	uint64_t phase_denominator =
		(uint64_t)TEST_SAMPLE_RATE * carrier_denominator;
	uint64_t represented;
	uint64_t error;

	channel.carrier_frequency_numerator = carrier_numerator;
	channel.carrier_frequency_denominator = carrier_denominator;
	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
		      qam_demodulator_init(&demodulator, &channel) ==
			      QAM_OK &&
		      modulator.carrier_phase_step ==
			      demodulator.carrier_phase_step,
	      "QAM fractional-carrier endpoints did not initialize");
	represented =
		(uint64_t)modulator.carrier_phase_step * phase_denominator;
	error = represented > phase_numerator ? represented - phase_numerator :
						phase_numerator - represented;
	CHECK(error * 2u <= phase_denominator,
	      "QAM fractional carrier was not rounded to the nearest phase step");
	for (unsigned int index = 0; index < carrier_denominator; ++index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      QAM_OK &&
		      qam_demodulator_sample(
			      &demodulator, sample, &received, &ready) ==
			      QAM_OK,
		      "QAM fractional carrier failed at sample %u", index);
	}
	CHECK(modulator.carrier_phase == demodulator.carrier_phase &&
		      llabs((long long)(int32_t)modulator.carrier_phase) <= 25,
	      "QAM fractional carrier drifted across its 49-sample cycle");
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
	struct qam_channel channel = v22_channel(carrier_hz);
	struct qam_modulator modulator;
	const struct qam_point point = { 1.0, 0.0 };
	int16_t samples[TEST_SAMPLE_RATE];

	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_OK,
	      "QAM carrier fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
		bool accepted;

		CHECK(qam_modulator_sample(
			      &modulator, &point, &samples[index], &accepted) ==
			      QAM_OK,
		      "QAM carrier fixture failed at sample %zu", index);
	}
	CHECK(tone_projection(samples + TEST_SAMPLE_RATE / 4,
			      TEST_SAMPLE_RATE * 3 / 4, carrier_hz) >
		      tone_projection(samples + TEST_SAMPLE_RATE / 4,
			      TEST_SAMPLE_RATE * 3 / 4, reject_hz) * 1000.0,
	      "QAM modulator did not retain its %u Hz carrier", carrier_hz);
	return true;
}

static bool test_channel_carriers(void)
{
	return check_carrier(1200, 2400) && check_carrier(2400, 1200);
}

static bool test_floating_transmit_path(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_modulator floating;
	struct qam_modulator integer;
	unsigned int accepted_count = 0;

	CHECK(qam_modulator_init(
		      &floating, &channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
	      qam_modulator_init(
		      &integer, &channel, TEST_AMPLITUDE) ==
		      QAM_OK,
	      "QAM floating transmit fixtures did not initialize");
	for (unsigned int index = 0; index < 1000; ++index) {
		struct qam_point point = quadrant_point(
			(uint8_t)(accepted_count & 3u),
			(double)(accepted_count % 4u + 1u) / 4.0);
		double floating_sample;
		int16_t integer_sample;
		long expected;
		bool floating_accepted;
		bool integer_accepted;

		CHECK(qam_modulator_sample_f64(
			      &floating, &point, &floating_sample,
			      &floating_accepted) == QAM_OK &&
		      qam_modulator_sample(
			      &integer, &point, &integer_sample,
			      &integer_accepted) == QAM_OK &&
		      isfinite(floating_sample) &&
		      floating_accepted == integer_accepted &&
		      qam_modulators_equal(&floating, &integer),
		      "QAM floating transmit path diverged at sample %u", index);
		expected = lround(floating_sample * TEST_AMPLITUDE);
		if (expected > TEST_AMPLITUDE)
			expected = TEST_AMPLITUDE;
		if (expected < -TEST_AMPLITUDE)
			expected = -TEST_AMPLITUDE;
		CHECK(integer_sample == expected,
		      "QAM integer scaling changed at sample %u", index);
		if (floating_accepted)
			accepted_count++;
	}
	CHECK(accepted_count == 75,
	      "QAM floating path accepted %u symbols instead of 75",
	      accepted_count);
	return true;
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static int16_t impair_sample(int16_t sample, bool impaired, uint32_t *random)
{
	long value;

	if (!impaired)
		return sample;
	value = lround((double)sample * 0.58) + 175;
	value += (long)(fixture_random(random) % 401u) - 200;
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static void build_phase_changes(uint8_t *changes, size_t count)
{
	uint32_t random = UINT32_C(0x51f22d79);

	for (size_t index = 0; index < count; ++index)
		changes[index] = (uint8_t)(fixture_random(&random) >> 30);
}

static size_t find_pattern(
	const uint8_t *haystack, size_t haystack_count,
	const uint8_t *needle, size_t needle_count)
{
	if (needle_count > haystack_count)
		return SIZE_MAX;
	for (size_t offset = 0; offset <= haystack_count - needle_count;
	     ++offset)
		if (memcmp(haystack + offset, needle, needle_count) == 0)
			return offset;
	return SIZE_MAX;
}

static bool check_receive_loopback(
	uint32_t transmit_carrier_hz, uint32_t receive_carrier_hz,
	uint32_t transmit_symbol_rate, uint32_t receive_symbol_rate,
	double rolloff, unsigned int leading_silence, bool impaired,
	size_t symbol_count, size_t sync_symbols, size_t payload_symbols)
{
	struct qam_channel transmit_channel =
		v22_channel(transmit_carrier_hz);
	struct qam_channel receive_channel =
		v22_channel(receive_carrier_hz);
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	struct qam_measurement measurement;
	uint8_t changes[TRACKING_SYMBOL_COUNT];
	uint8_t decoded[TRACKING_DECODED_CAPACITY];
	uint32_t random = UINT32_C(0x912ca775);
	struct qam_point previous_point = { 0 };
	uint8_t quadrant = 0;
	size_t accepted = 0;
	size_t decoded_count = 0;
	size_t sync_offset;
	size_t payload_errors = SIZE_MAX;
	bool has_previous_point = false;
	bool exact_clock_stayed_nominal = true;
	size_t sample_limit = leading_silence +
		((symbol_count + 16u) * TEST_SAMPLE_RATE +
		 transmit_symbol_rate - 1u) /
			transmit_symbol_rate;

	CHECK(transmit_symbol_rate != 0 && receive_symbol_rate != 0 &&
		      symbol_count <= ARRAY_LENGTH(changes) &&
		      sync_symbols + payload_symbols <= symbol_count,
	      "QAM receive-loopback fixture is invalid");
	transmit_channel.symbol_rate_numerator = transmit_symbol_rate;
	receive_channel.symbol_rate_numerator = receive_symbol_rate;
	transmit_channel.rolloff = rolloff;
	receive_channel.rolloff = rolloff;
	build_phase_changes(changes, symbol_count);
	CHECK(qam_modulator_init(
		      &modulator, &transmit_channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
		      qam_demodulator_init(
			      &demodulator, &receive_channel) ==
			      QAM_OK,
	      "QAM receive-loopback endpoints did not initialize");
	for (size_t sample_index = 0; sample_index < sample_limit;
	     ++sample_index) {
		struct qam_point received_point;
		int16_t sample = 0;
		bool ready;

		if (sample_index >= leading_silence) {
			struct qam_point transmit_point = quadrant_point(
				quadrant,
				0.55 + 0.15 * (double)(accepted % 4u));
			bool symbol_accepted;

			CHECK(qam_modulator_sample(
				      &modulator, &transmit_point, &sample,
				      &symbol_accepted) == QAM_OK,
			      "QAM loopback modulator failed");
			if (symbol_accepted && accepted < symbol_count) {
				quadrant =
					(uint8_t)((quadrant + changes[accepted]) & 3u);
				accepted++;
			}
		}
		sample = impair_sample(sample, impaired, &random);
		CHECK(qam_demodulator_sample(
			      &demodulator, sample, &received_point, &ready) ==
			      QAM_OK,
		      "QAM loopback demodulator failed");
		if (transmit_symbol_rate == receive_symbol_rate &&
		    qam_demodulator_measure(&demodulator).timing_locked &&
		    demodulator.recovery_step_units != receive_symbol_rate)
			exact_clock_stayed_nominal = false;
		if (ready) {
			uint8_t quarter_turns;

			CHECK(isfinite(received_point.in_phase) &&
				      isfinite(received_point.quadrature),
			      "QAM loopback produced a non-finite point");
			if (has_previous_point) {
				CHECK(decoded_count < ARRAY_LENGTH(decoded),
				      "QAM loopback produced unbounded symbols");
				CHECK(qam_nearest_quarter_turn(
					      &previous_point, &received_point,
					      &quarter_turns) == QAM_OK,
				      "QAM loopback phase decision failed");
				decoded[decoded_count++] = quarter_turns;
			}
			previous_point = received_point;
			has_previous_point = true;
		}
	}
	CHECK(accepted == symbol_count,
	      "QAM loopback did not consume its symbol fixture");
	sync_offset = find_pattern(
		decoded, decoded_count,
		changes + symbol_count - sync_symbols - payload_symbols,
		sync_symbols);
	if (sync_offset != SIZE_MAX &&
	    sync_offset + sync_symbols + payload_symbols <= decoded_count) {
		payload_errors = 0;
		for (size_t index = 0; index < payload_symbols; ++index)
			if (decoded[sync_offset + sync_symbols + index] !=
			    changes[symbol_count - payload_symbols + index])
				payload_errors++;
	}
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + sync_symbols + payload_symbols <=
			      decoded_count &&
		      memcmp(decoded + sync_offset + sync_symbols,
			     changes + symbol_count - payload_symbols,
			     payload_symbols) == 0,
	      "QAM receiver changed its post-acquisition payload "
	      "(%u/%u symbols, decoded=%zu, sync=%zu, errors=%zu)",
	      transmit_symbol_rate, receive_symbol_rate, decoded_count,
	      sync_offset, payload_errors);
	measurement = qam_demodulator_measure(&demodulator);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "QAM receiver did not retain carrier measurements");
	CHECK(demodulator.recovery_step_units == transmit_symbol_rate,
	      "QAM receiver tracked %llu instead of %u symbol-rate units",
	      (unsigned long long)demodulator.recovery_step_units,
	      transmit_symbol_rate);
	CHECK(exact_clock_stayed_nominal,
	      "QAM receiver perturbed an exact recovered symbol clock");
	return true;
}

static bool test_receive_loopback(void)
{
	return check_receive_loopback(
		       1200, 1200, TEST_SYMBOL_RATE, TEST_SYMBOL_RATE, 0.75, 0,
		       false, TEST_SYMBOL_COUNT, TEST_SYNC_SYMBOLS,
		       TEST_PAYLOAD_SYMBOLS) &&
	       check_receive_loopback(
		       2400, 2400, TEST_SYMBOL_RATE, TEST_SYMBOL_RATE, 0.75, 0,
		       false, TEST_SYMBOL_COUNT, TEST_SYNC_SYMBOLS,
		       TEST_PAYLOAD_SYMBOLS);
}

static bool test_receive_tolerance(void)
{
	return check_receive_loopback(
		       1207, 1200, TEST_SYMBOL_RATE, TEST_SYMBOL_RATE, 0.75, 19,
		       true, TEST_SYMBOL_COUNT, TEST_SYNC_SYMBOLS,
		       TEST_PAYLOAD_SYMBOLS) &&
	       check_receive_loopback(
		       2393, 2400, TEST_SYMBOL_RATE, TEST_SYMBOL_RATE, 0.75, 31,
		       true, TEST_SYMBOL_COUNT, TEST_SYNC_SYMBOLS,
		       TEST_PAYLOAD_SYMBOLS);
}

static bool test_receive_clock_tracking(void)
{
	return check_receive_loopback(
		       1807, 1800, 2401, 2400, 0.25, 17, true,
		       TRACKING_SYMBOL_COUNT, TRACKING_SYNC_SYMBOLS,
		       TRACKING_PAYLOAD_SYMBOLS) &&
	       check_receive_loopback(
		       1793, 1800, 2399, 2400, 0.25, 29, true,
		       TRACKING_SYMBOL_COUNT, TRACKING_SYNC_SYMBOLS,
		       TRACKING_PAYLOAD_SYMBOLS);
}

static bool force_timing_candidate(
	struct qam_demodulator *demodulator, size_t target)
{
	struct qam_point received;
	uint32_t minimum_symbols =
		(demodulator->tracking_window_symbols * 3u + 3u) / 4u;
	bool ready;

	if (target >= demodulator->timing_candidate_count)
		return false;
	demodulator->timing_sample_count =
		demodulator->timing_sample_target - 1u;
	for (size_t index = 0;
	     index < demodulator->timing_candidate_count; ++index) {
		demodulator->timing[index].symbol_count = minimum_symbols;
		demodulator->timing[index].symbol_power_sum =
			minimum_symbols * (index == target ? 2.0e12 : 1.0e12);
	}
	return qam_demodulator_sample(
		       demodulator, 0, &received, &ready) == QAM_OK &&
	       demodulator->timing_rate_reference_index == target;
}

static bool test_filtered_clock_tracking(void)
{
	struct qam_channel channel = {
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 1800,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.2,
		.pulse_span_symbols = 6,
	};
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	struct qam_point point = { 0.6, 0.0 };

	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) == QAM_OK &&
	      qam_demodulator_init_with_timing_windows(
		      &demodulator, &channel, 64, 256) == QAM_OK &&
	      qam_demodulator_set_timing_power_hysteresis(
		      &demodulator, 1.01) == QAM_OK,
	      "filtered-clock fixture did not initialize");
	for (size_t sample_index = 0;
	     sample_index < demodulator.timing_sample_target +
				    TEST_SAMPLE_RATE;
	     ++sample_index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) == QAM_OK,
		      "filtered-clock transmitter failed");
		if (accepted)
			point = quadrant_point(
				(uint8_t)(sample_index & 3u), 0.6);
		CHECK(qam_demodulator_sample(
			      &demodulator, sample, &received, &ready) == QAM_OK,
		      "filtered-clock receiver failed");
		if (qam_demodulator_has_timing_lock(&demodulator))
			break;
	}
	CHECK(qam_demodulator_has_timing_lock(&demodulator) &&
		      demodulator.timing_candidate_count == 8,
	      "filtered-clock fixture did not acquire timing");
	demodulator.timing_reference_index = 0;
	demodulator.locked_phase_units = demodulator.timing[0].phase_units;
	demodulator.recovery_pending = false;
	for (size_t selection = 0; selection < 4; ++selection) {
		size_t target = selection % 2u == 0 ? 7u : 0u;

		CHECK(force_timing_candidate(&demodulator, target),
		      "filtered-clock fixture did not select candidate %zu",
		      target);
	}
	CHECK(demodulator.recovery_step_units == 3000 &&
	      demodulator.timing_reference_index == 0,
	      "alternating phase estimates perturbed an exact clock");
	demodulator.timing_reference_index = 0;
	demodulator.locked_phase_units = demodulator.timing[0].phase_units;
	demodulator.recovery_step_units = 3000;
	demodulator.recovery_pending = false;
	demodulator.timing_forward_error_units = 0;
	demodulator.timing_backward_error_units = 0;
	demodulator.timing_error_windows = 0;
	demodulator.timing_forward_windows = 0;
	demodulator.timing_backward_windows = 0;
	CHECK(force_timing_candidate(&demodulator, 0) &&
	      force_timing_candidate(&demodulator, 0) &&
	      force_timing_candidate(&demodulator, 3) &&
	      demodulator.recovery_step_units == 3000 &&
	      demodulator.timing_reference_index == 0,
	      "one phase outlier perturbed an exact clock");
	CHECK(force_timing_candidate(&demodulator, 3) &&
	      force_timing_candidate(&demodulator, 3) &&
	      demodulator.recovery_step_units == 3000 &&
	      demodulator.timing_reference_index == 3,
	      "stable phase evidence did not move the energy reference");
	return true;
}

static bool test_silence_does_not_lock(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_demodulator demodulator;

	CHECK(qam_demodulator_init(&demodulator, &channel) ==
		      QAM_OK,
	      "QAM silence fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE / 2; ++index) {
		struct qam_point point;
		bool ready;

		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &point, &ready) ==
			      QAM_OK &&
			      !ready,
		      "QAM silence produced a symbol at sample %u", index);
	}
	CHECK(!qam_demodulator_has_timing_lock(&demodulator),
	      "QAM silence produced a timing lock");
	return true;
}

static bool test_timing_windows(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_demodulator defaults;
	struct qam_demodulator demodulator;
	struct qam_demodulator saved;
	struct qam_modulator modulator;
	struct qam_point point = { 0.75, 0.0 };
	uint32_t acquisition_samples;
	uint32_t signal_samples = 0;
	bool locked = false;

	CHECK(qam_demodulator_init(&defaults, &channel) == QAM_OK &&
	      defaults.acquisition_window_symbols ==
		      QAM_DEFAULT_TIMING_WINDOW_SYMBOLS &&
	      defaults.tracking_window_symbols ==
		      QAM_DEFAULT_TIMING_WINDOW_SYMBOLS &&
	      qam_demodulator_init_with_timing_windows(
		      &demodulator, &channel, 64, 384) == QAM_OK &&
	      qam_modulator_init(&modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
	      demodulator.timing_sample_target <
		      defaults.timing_sample_target,
	      "QAM timing windows did not initialize");
	acquisition_samples = demodulator.timing_sample_target;
	for (uint32_t sample_index = 0;
	     sample_index < acquisition_samples / 2u; ++sample_index) {
		struct qam_point received;
		bool ready;

		CHECK(qam_demodulator_sample(
			      &demodulator, 8, &received, &ready) == QAM_OK &&
			      !ready,
		      "quiet QAM acquisition failed at %u", sample_index);
	}
	CHECK(!qam_demodulator_measure(&demodulator).timing_locked &&
	      !demodulator.acquisition_started &&
	      demodulator.timing_sample_count == 0,
	      "QAM acquisition window advanced before carrier onset");
	for (uint32_t sample_index = 0;
	     sample_index < acquisition_samples / 2u; ++sample_index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) == QAM_OK &&
		      qam_demodulator_sample(
			      &demodulator, sample, &received, &ready) == QAM_OK &&
		      !ready,
		      "QAM carrier-tail fixture failed at %u", sample_index);
	}
	for (uint32_t sample_index = 0;
	     sample_index < acquisition_samples; ++sample_index) {
		struct qam_point received;
		bool ready;

		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received, &ready) == QAM_OK &&
		      !ready,
		      "QAM carrier tail produced a symbol at %u", sample_index);
	}
	CHECK(!qam_demodulator_measure(&demodulator).timing_locked &&
	      !demodulator.acquisition_started &&
	      demodulator.timing_sample_count == 0,
	      "QAM acquired a carrier that ended before its timing window");
	for (uint32_t sample_index = 0;
	     sample_index < acquisition_samples +
				channel.sample_rate;
	     ++sample_index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) == QAM_OK &&
		      qam_demodulator_sample(
			      &demodulator, sample, &received, &ready) == QAM_OK,
		      "QAM timing-window fixture failed at %u", sample_index);
		if (accepted)
			point = quadrant_point(
				(uint8_t)((sample_index / 7u) & 3u), 0.75);
		if (qam_demodulator_measure(&demodulator).timing_locked) {
			locked = true;
			signal_samples = sample_index + 1u;
			break;
		}
	}
	CHECK(locked && signal_samples >= acquisition_samples &&
	      demodulator.timing_sample_count == 0 &&
	      demodulator.timing_sample_target > acquisition_samples &&
	      demodulator.acquisition_window_symbols == 64 &&
	      demodulator.tracking_window_symbols == 384,
	      "QAM did not measure a carrier-relative acquisition window");
	CHECK(qam_demodulator_set_timing_power_hysteresis(
		      &demodulator, 1.01) == QAM_OK &&
	      demodulator.timing_power_hysteresis == 1.01 &&
	      qam_demodulator_reset(&demodulator) == QAM_OK &&
	      !qam_demodulator_measure(&demodulator).timing_locked &&
	      !demodulator.acquisition_started &&
	      demodulator.timing_sample_target == acquisition_samples &&
	      demodulator.acquisition_window_symbols == 64 &&
	      demodulator.tracking_window_symbols == 384 &&
	      demodulator.timing_power_hysteresis == 1.01,
	      "QAM reset did not preserve its timing-window policy");
	saved = demodulator;
	CHECK(qam_demodulator_init_with_timing_windows(
		      &demodulator, &channel,
		      QAM_MIN_TIMING_WINDOW_SYMBOLS - 1u, 384) ==
		      QAM_INVALID_ARGUMENT &&
	      qam_demodulator_init_with_timing_windows(
		      &demodulator, &channel, 64,
		      QAM_MAX_TIMING_WINDOW_SYMBOLS + 1u) ==
		      QAM_INVALID_ARGUMENT &&
	      qam_demodulator_set_timing_power_hysteresis(
		      &demodulator, 0.99) == QAM_INVALID_ARGUMENT &&
	      qam_demodulator_set_timing_power_hysteresis(
		      &demodulator, NAN) == QAM_INVALID_ARGUMENT &&
	      qam_demodulators_equal(&demodulator, &saved),
	      "invalid QAM timing policy changed the receiver");
	return true;
}

static bool test_configurable_point_power(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_modulator bounded;
	struct qam_modulator wider;
	struct qam_modulator before;
	const struct qam_point wider_point = { 1.0, 1.0 };
	const struct qam_point excessive_point = { 1.0, 1.1 };
	double sample = -99.0;
	bool accepted = false;

	CHECK(qam_modulator_init(
		      &bounded, &channel, TEST_AMPLITUDE) ==
		      QAM_OK &&
		      bounded.maximum_point_power == 9.0 / 5.0 &&
	      qam_modulator_init_with_point_power_limit(
		      &wider, &channel, TEST_AMPLITUDE, 2.0) ==
		      QAM_OK &&
		      wider.maximum_point_power == 2.0,
	      "QAM did not retain its selected point-power bound");
	before = bounded;
	CHECK(qam_modulator_sample_f64(
		      &bounded, &wider_point, &sample, &accepted) ==
		      QAM_INVALID_ARGUMENT &&
		      qam_modulators_equal(&bounded, &before) &&
		      sample == -99.0 && !accepted,
	      "default QAM bound accepted a wider constellation point");
	CHECK(qam_modulator_sample_f64(
		      &wider, &wider_point, &sample, &accepted) ==
		      QAM_OK &&
		      accepted && isfinite(sample),
	      "explicit QAM bound rejected its boundary point");
	before = wider;
	sample = -99.0;
	accepted = false;
	CHECK(qam_modulator_sample_f64(
		      &wider, &excessive_point, &sample, &accepted) ==
		      QAM_INVALID_ARGUMENT &&
		      qam_modulators_equal(&wider, &before) &&
		      sample == -99.0 && !accepted,
	      "explicit QAM bound accepted an excessive point");
	before = wider;
	CHECK(qam_modulator_init_with_point_power_limit(
		      &wider, &channel, TEST_AMPLITUDE, 0.0) ==
		      QAM_INVALID_ARGUMENT &&
	      qam_modulator_init_with_point_power_limit(
		      &wider, &channel, TEST_AMPLITUDE, NAN) ==
		      QAM_INVALID_ARGUMENT &&
	      qam_modulator_init_with_point_power_limit(
		      &wider, &channel, TEST_AMPLITUDE, INFINITY) ==
		      QAM_INVALID_ARGUMENT &&
		      qam_modulators_equal(&wider, &before),
	      "invalid QAM point-power limit mutated state");
	wider.maximum_point_power = NAN;
	CHECK(!qam_modulator_is_valid(&wider),
	      "QAM accepted a corrupt point-power limit");
	return true;
}

static bool test_matched_filter_ring_wrap(void)
{
	static const struct {
		uint32_t sample_rate;
		uint32_t symbol_rate;
		unsigned int span;
	} shapes[] = {
		{ 8000, 8000, 2 }, /* Minimum three-tap filter. */
		{ 8000, 600, 6 },  /* V.22's 81-tap filter. */
		{ 5100, 200, 10 }, /* Maximum 256-tap filter. */
	};
	static const int16_t samples[] = { 0, INT16_MIN, INT16_MAX };

	for (size_t shape = 0; shape < ARRAY_LENGTH(shapes); ++shape) {
		struct qam_channel channel = v22_channel(1200);
		struct qam_demodulator initialized;

		channel.sample_rate = shapes[shape].sample_rate;
		channel.symbol_rate_numerator = shapes[shape].symbol_rate;
		channel.pulse_span_symbols = shapes[shape].span;
		CHECK(qam_demodulator_init(&initialized, &channel) == QAM_OK,
		      "matched-filter ring fixture did not initialize");
		for (size_t head = 0; head < initialized.matched_filter_count;
		     ++head) {
			struct qam_demodulator receiver = initialized;
			size_t count = receiver.matched_filter_count;

			receiver.history_count = count;
			receiver.history_index = head;
			for (size_t index = 0; index < count; ++index) {
				receiver.in_phase_history[index] = (int)(index % 17) - 8;
				receiver.quadrature_history[index] = (int)(index % 13) - 6;
			}
			for (size_t sample = 0; sample < ARRAY_LENGTH(samples);
			     ++sample) {
				struct qam_point point;
				struct qam_point expected = { 0 };
				const struct qam_point *matched;
				bool ready;

				CHECK(qam_demodulator_sample(&receiver, samples[sample],
					&point, &ready) == QAM_OK &&
				      receiver.matched_history_count != 0 &&
				      receiver.history_index == (head + sample + 1) % count,
				      "matched-filter ring failed at %zu/%zu/%zu",
				      shape, head, sample);
				/* Direct convolution retains tap order across ring wrap. */
				for (size_t tap = 0; tap < count; ++tap) {
					size_t index =
						(receiver.history_index + count - 1 - tap) % count;
					double coefficient =
						receiver.matched_coefficients[tap];

					expected.in_phase +=
						receiver.in_phase_history[index] * coefficient;
					expected.quadrature +=
						receiver.quadrature_history[index] * coefficient;
				}
				matched = &receiver.matched_history[
					receiver.matched_history_count - 1];
				CHECK(matched->in_phase == expected.in_phase &&
				      matched->quadrature == expected.quadrature,
				      "matched-filter sum changed at %zu/%zu/%zu",
				      shape, head, sample);
			}
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct qam_channel channel = v22_channel(1200);
	struct qam_modulator modulator;
	struct qam_demodulator demodulator;
	struct qam_point point = { 1.0, 0.0 };
	struct qam_point bounded_point = { sqrt(9.0 / 5.0), 0.0 };
	struct qam_point invalid_point = { 1.0, 1.0 };
	struct qam_point received_point;
	uint32_t carrier_phase_step;
	int16_t sample = 0;
	bool accepted = false;
	bool floating_accepted = false;
	double floating_sample = -99.0;
	double pulse = 0.0;

	CHECK(root_raised_cosine(0.0, 0.0, &pulse) ==
		      QAM_INVALID_ARGUMENT &&
		      root_raised_cosine(0.0, 0.75, NULL) ==
			      QAM_INVALID_ARGUMENT &&
		      qam_modulator_init(NULL, &channel, 1) ==
			      QAM_INVALID_ARGUMENT,
	      "QAM accepted an invalid initialization argument");
	channel.carrier_frequency_numerator = TEST_SAMPLE_RATE / 2;
	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier at Nyquist");
	channel = v22_channel(1200);
	channel.carrier_frequency_denominator = 0;
	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier with no denominator");
	channel = v22_channel(1200);
	CHECK(qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      QAM_OK,
	      "QAM corruption fixture did not initialize");
	carrier_phase_step = modulator.carrier_phase_step;
	modulator.carrier_phase_step++;
	CHECK(!qam_modulator_is_valid(&modulator) &&
		      qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier phase step inconsistent with its channel");
	modulator.carrier_phase_step = carrier_phase_step;
	CHECK(qam_demodulator_init(
		      &demodulator, &channel) == QAM_OK &&
		      qam_demodulator_reset(&demodulator) ==
			      QAM_OK,
		      "QAM demodulator reset rejected valid state");
	{
		uint64_t recovery_step = demodulator.recovery_step_units;
		size_t reference_index = demodulator.timing_reference_index;
		size_t rate_reference_index =
			demodulator.timing_rate_reference_index;

		demodulator.recovery_step_units = 0;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted a corrupt recovery step");
		demodulator.recovery_step_units = recovery_step;
		demodulator.timing_reference_index =
			demodulator.timing_candidate_count;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted a corrupt timing reference");
		demodulator.timing_reference_index = reference_index;
		demodulator.timing_rate_reference_index =
			demodulator.timing_candidate_count;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted a corrupt rate reference");
		demodulator.timing_rate_reference_index = rate_reference_index;
		demodulator.recovery_phase_units = 1;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted an incomplete phase correction");
		demodulator.recovery_phase_units = 0;
		demodulator.timing_error_windows = UINT8_MAX;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted a corrupt clock-filter window count");
		demodulator.timing_error_windows = 0;
		demodulator.timing_forward_error_units = 1;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted an orphaned clock-filter error");
		demodulator.timing_forward_error_units = 0;
		demodulator.timing_error_windows = 1;
		demodulator.timing_forward_windows = 2;
		CHECK(qam_demodulator_sample(
			      &demodulator, 0, &received_point, &accepted) ==
			      QAM_INVALID_ARGUMENT,
		      "QAM accepted excess directional clock evidence");
		demodulator.timing_error_windows = 0;
		demodulator.timing_forward_windows = 0;
	}
	CHECK(qam_modulator_sample(
		      &modulator, &bounded_point, &sample, &accepted) ==
		      QAM_OK &&
		      accepted && sample >= -TEST_AMPLITUDE &&
		      sample <= TEST_AMPLITUDE,
	      "QAM rejected a bounded super-unit constellation point");
	CHECK(qam_modulator_sample(
		      &modulator, &invalid_point, &sample, &accepted) ==
		      QAM_INVALID_ARGUMENT &&
	      qam_modulator_sample(
			      &modulator, NULL, &sample, &accepted) ==
			      QAM_INVALID_ARGUMENT,
	      "QAM accepted an unbounded constellation point");
	{
		struct qam_modulator before = modulator;

		CHECK(qam_modulator_sample_f64(
			      &modulator, &invalid_point, &floating_sample,
			      &floating_accepted) ==
			      QAM_INVALID_ARGUMENT &&
		      qam_modulator_sample_f64(
			      &modulator, &point, NULL, &floating_accepted) ==
			      QAM_INVALID_ARGUMENT &&
		      qam_modulator_sample_f64(
			      &modulator, &point, &floating_sample, NULL) ==
			      QAM_INVALID_ARGUMENT &&
		      qam_modulators_equal(&modulator, &before) &&
		      floating_sample == -99.0 && !floating_accepted,
		      "invalid QAM floating sample mutated state");
	}
	modulator.active_count = QAM_MAX_ACTIVE_SYMBOLS + 1;
	CHECK(!qam_modulator_can_accept_symbol(&modulator) &&
		      qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      QAM_INVALID_ARGUMENT &&
		      strcmp(qam_result_name(
			     QAM_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "QAM accepted corrupt active-symbol state");
	demodulator.history_index = demodulator.matched_filter_count;
	CHECK(qam_demodulator_sample(
		      &demodulator, 0, &received_point, &accepted) ==
		      QAM_INVALID_ARGUMENT &&
	      !qam_demodulator_has_timing_lock(&demodulator) &&
	      !qam_demodulator_has_timing_lock(NULL) &&
		      !qam_demodulator_measure(&demodulator)
			       .timing_locked,
		      "QAM accepted corrupt matched-filter state");
	demodulator.history_index = 0;
	demodulator.matched_history[0].in_phase = NAN;
	CHECK(qam_demodulator_sample(
		      &demodulator, 0, &received_point, &accepted) ==
		      QAM_INVALID_ARGUMENT,
	      "QAM accepted corrupt interpolation state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "coordinate quantization", test_coordinate_quantization },
		{ "point validation", test_point_validation },
		{ "point correlation", test_point_correlation },
		{ "point quotient", test_point_quotient },
		{ "flat gain prediction", test_flat_gain_prediction },
		{ "flat gain product boundaries", test_flat_gain_product_boundaries },
		{ "flat gain adaptation", test_flat_gain_adaptation },
		{ "flat gain validation", test_flat_gain_validation },
		{ "generated pulse", test_generated_pulse },
		{ "exact symbol clock", test_exact_symbol_clock },
		{ "exact carrier clock", test_exact_carrier_clock },
		{ "channel carriers", test_channel_carriers },
		{ "floating transmit path", test_floating_transmit_path },
		{ "receive loopback", test_receive_loopback },
		{ "matched-filter ring wrap", test_matched_filter_ring_wrap },
		{ "receive tolerance", test_receive_tolerance },
		{ "receive clock tracking", test_receive_clock_tracking },
		{ "filtered clock tracking", test_filtered_clock_tracking },
		{ "silence does not lock", test_silence_does_not_lock },
		{ "timing windows", test_timing_windows },
		{ "configurable point power", test_configurable_point_power },
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
