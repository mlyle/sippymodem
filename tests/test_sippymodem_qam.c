// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_qam.h>

#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_SYMBOL_RATE = 600,
	TEST_AMPLITUDE = 12000,
	TEST_SYMBOL_COUNT = 640,
	TEST_DECODED_CAPACITY = 512,
	TEST_SYNC_SYMBOLS = 32,
	TEST_PAYLOAD_SYMBOLS = 128,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static struct sippymodem_qam_channel v22_channel(uint32_t carrier_hz)
{
	return (struct sippymodem_qam_channel){
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = TEST_SYMBOL_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = carrier_hz,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.75,
		.pulse_span_symbols = 6,
	};
}

static struct sippymodem_qam_point quadrant_point(
	uint8_t quadrant, double magnitude)
{
	switch (quadrant & 3u) {
	case 0:
		return (struct sippymodem_qam_point){ magnitude, 0.0 };
	case 1:
		return (struct sippymodem_qam_point){ 0.0, magnitude };
	case 2:
		return (struct sippymodem_qam_point){ -magnitude, 0.0 };
	default:
		return (struct sippymodem_qam_point){ 0.0, -magnitude };
	}
}

static bool test_generated_pulse(void)
{
	double at_zero = 0.0;
	double at_singularity = 0.0;
	double reflected = 0.0;
	double ordinary = 0.0;
	double expected_zero = 1.0 + 0.75 * (4.0 / TEST_PI - 1.0);

	CHECK(sippymodem_root_raised_cosine(0.0, 0.75, &at_zero) ==
		      SIPPYMODEM_QAM_OK &&
		      fabs(at_zero - expected_zero) < 1e-12,
	      "QAM pulse changed its zero-time limit");
	CHECK(sippymodem_root_raised_cosine(
		      1.0 / 3.0, 0.75, &at_singularity) ==
		      SIPPYMODEM_QAM_OK &&
		      isfinite(at_singularity),
	      "QAM pulse did not handle its removable singularity");
	CHECK(sippymodem_root_raised_cosine(0.625, 0.75, &ordinary) ==
		      SIPPYMODEM_QAM_OK &&
		      sippymodem_root_raised_cosine(-0.625, 0.75, &reflected) ==
			      SIPPYMODEM_QAM_OK &&
		      fabs(ordinary - reflected) < 1e-12,
	      "QAM pulse lost even symmetry");
	return true;
}

static bool test_exact_symbol_clock(void)
{
	struct sippymodem_qam_channel channel = v22_channel(1200);
	struct sippymodem_qam_modulator modulator;
	unsigned int accepted_count = 0;
	int16_t peak = 0;

	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_modulator_can_accept_symbol(&modulator),
	      "QAM symbol-clock fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE; ++index) {
		struct sippymodem_qam_point point = quadrant_point(
			(uint8_t)(accepted_count & 3u),
			(double)(accepted_count % 4u + 1u) / 4.0);
		int16_t sample;
		bool accepted;

		CHECK(sippymodem_qam_modulator_sample(
			      &modulator, &point,
			      &sample, &accepted) == SIPPYMODEM_QAM_OK &&
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
		      sippymodem_qam_modulator_can_accept_symbol(&modulator) &&
		      peak > TEST_AMPLITUDE / 8,
	      "QAM rational symbol clock drifted over one second");
	return true;
}

static bool test_exact_carrier_clock(void)
{
	struct sippymodem_qam_channel channel = v22_channel(1);
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	const struct sippymodem_qam_point point = { 1.0, 0.0 };
	const uint32_t carrier_numerator = 96000;
	const uint32_t carrier_denominator = 49;
	uint64_t phase_numerator = (uint64_t)carrier_numerator << 32;
	uint64_t phase_denominator =
		(uint64_t)TEST_SAMPLE_RATE * carrier_denominator;
	uint64_t represented;
	uint64_t error;

	channel.carrier_frequency_numerator = carrier_numerator;
	channel.carrier_frequency_denominator = carrier_denominator;
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_demodulator_init(&demodulator, &channel) ==
			      SIPPYMODEM_QAM_OK &&
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
		struct sippymodem_qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(sippymodem_qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_demodulator_sample(
			      &demodulator, sample, &received, &ready) ==
			      SIPPYMODEM_QAM_OK,
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
	struct sippymodem_qam_channel channel = v22_channel(carrier_hz);
	struct sippymodem_qam_modulator modulator;
	const struct sippymodem_qam_point point = { 1.0, 0.0 };
	int16_t samples[TEST_SAMPLE_RATE];

	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK,
	      "QAM carrier fixture did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
		bool accepted;

		CHECK(sippymodem_qam_modulator_sample(
			      &modulator, &point, &samples[index], &accepted) ==
			      SIPPYMODEM_QAM_OK,
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
	struct sippymodem_qam_channel channel = v22_channel(1200);
	struct sippymodem_qam_modulator floating;
	struct sippymodem_qam_modulator integer;
	unsigned int accepted_count = 0;

	CHECK(sippymodem_qam_modulator_init(
		      &floating, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK &&
	      sippymodem_qam_modulator_init(
		      &integer, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK,
	      "QAM floating transmit fixtures did not initialize");
	for (unsigned int index = 0; index < 1000; ++index) {
		struct sippymodem_qam_point point = quadrant_point(
			(uint8_t)(accepted_count & 3u),
			(double)(accepted_count % 4u + 1u) / 4.0);
		double floating_sample;
		int16_t integer_sample;
		long expected;
		bool floating_accepted;
		bool integer_accepted;

		CHECK(sippymodem_qam_modulator_sample_f64(
			      &floating, &point, &floating_sample,
			      &floating_accepted) == SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_modulator_sample(
			      &integer, &point, &integer_sample,
			      &integer_accepted) == SIPPYMODEM_QAM_OK &&
		      isfinite(floating_sample) &&
		      floating_accepted == integer_accepted &&
		      memcmp(&floating, &integer, sizeof(floating)) == 0,
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

static void build_phase_changes(uint8_t changes[TEST_SYMBOL_COUNT])
{
	uint32_t random = UINT32_C(0x51f22d79);

	for (size_t index = 0; index < TEST_SYMBOL_COUNT; ++index)
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
	unsigned int leading_silence, bool impaired)
{
	struct sippymodem_qam_channel transmit_channel =
		v22_channel(transmit_carrier_hz);
	struct sippymodem_qam_channel receive_channel =
		v22_channel(receive_carrier_hz);
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_qam_measurement measurement;
	uint8_t changes[TEST_SYMBOL_COUNT];
	uint8_t decoded[TEST_DECODED_CAPACITY];
	uint32_t random = UINT32_C(0x912ca775);
	struct sippymodem_qam_point previous_point = { 0 };
	uint8_t quadrant = 0;
	size_t accepted = 0;
	size_t decoded_count = 0;
	size_t sync_offset;
	bool has_previous_point = false;
	size_t sample_limit = leading_silence +
		(TEST_SYMBOL_COUNT + 16u) * TEST_SAMPLE_RATE / TEST_SYMBOL_RATE;

	build_phase_changes(changes);
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &transmit_channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_demodulator_init(
			      &demodulator, &receive_channel) ==
			      SIPPYMODEM_QAM_OK,
	      "QAM receive-loopback endpoints did not initialize");
	for (size_t sample_index = 0; sample_index < sample_limit;
	     ++sample_index) {
		struct sippymodem_qam_point received_point;
		int16_t sample = 0;
		bool ready;

		if (sample_index >= leading_silence) {
			struct sippymodem_qam_point transmit_point = quadrant_point(
				quadrant,
				0.55 + 0.15 * (double)(accepted % 4u));
			bool symbol_accepted;

			CHECK(sippymodem_qam_modulator_sample(
				      &modulator, &transmit_point, &sample,
				      &symbol_accepted) == SIPPYMODEM_QAM_OK,
			      "QAM loopback modulator failed");
			if (symbol_accepted && accepted < TEST_SYMBOL_COUNT) {
				quadrant =
					(uint8_t)((quadrant + changes[accepted]) & 3u);
				accepted++;
			}
		}
		sample = impair_sample(sample, impaired, &random);
		CHECK(sippymodem_qam_demodulator_sample(
			      &demodulator, sample, &received_point, &ready) ==
			      SIPPYMODEM_QAM_OK,
		      "QAM loopback demodulator failed");
		if (ready) {
			uint8_t quarter_turns;

			CHECK(isfinite(received_point.in_phase) &&
				      isfinite(received_point.quadrature),
			      "QAM loopback produced a non-finite point");
			if (has_previous_point) {
				CHECK(decoded_count < ARRAY_LENGTH(decoded),
				      "QAM loopback produced unbounded symbols");
				CHECK(sippymodem_qam_nearest_quarter_turn(
					      &previous_point, &received_point,
					      &quarter_turns) == SIPPYMODEM_QAM_OK,
				      "QAM loopback phase decision failed");
				decoded[decoded_count++] = quarter_turns;
			}
			previous_point = received_point;
			has_previous_point = true;
		}
	}
	CHECK(accepted == TEST_SYMBOL_COUNT,
	      "QAM loopback did not consume its symbol fixture");
	sync_offset = find_pattern(
		decoded, decoded_count,
		changes + TEST_SYMBOL_COUNT -
			TEST_SYNC_SYMBOLS - TEST_PAYLOAD_SYMBOLS,
		TEST_SYNC_SYMBOLS);
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + TEST_SYNC_SYMBOLS + TEST_PAYLOAD_SYMBOLS <=
			      decoded_count &&
		      memcmp(decoded + sync_offset + TEST_SYNC_SYMBOLS,
			     changes + TEST_SYMBOL_COUNT - TEST_PAYLOAD_SYMBOLS,
			     TEST_PAYLOAD_SYMBOLS) == 0,
	      "QAM receiver changed its post-acquisition payload");
	measurement = sippymodem_qam_demodulator_measure(&demodulator);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "QAM receiver did not retain carrier measurements");
	return true;
}

static bool test_receive_loopback(void)
{
	return check_receive_loopback(1200, 1200, 0, false) &&
	       check_receive_loopback(2400, 2400, 0, false);
}

static bool test_receive_tolerance(void)
{
	return check_receive_loopback(1207, 1200, 19, true) &&
	       check_receive_loopback(2393, 2400, 31, true);
}

static bool test_silence_does_not_lock(void)
{
	struct sippymodem_qam_channel channel = v22_channel(1200);
	struct sippymodem_qam_demodulator demodulator;

	CHECK(sippymodem_qam_demodulator_init(&demodulator, &channel) ==
		      SIPPYMODEM_QAM_OK,
	      "QAM silence fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE / 2; ++index) {
		struct sippymodem_qam_point point;
		bool ready;

		CHECK(sippymodem_qam_demodulator_sample(
			      &demodulator, 0, &point, &ready) ==
			      SIPPYMODEM_QAM_OK &&
			      !ready,
		      "QAM silence produced a symbol at sample %u", index);
	}
	CHECK(!sippymodem_qam_demodulator_measure(&demodulator).timing_locked,
	      "QAM silence produced a timing lock");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_qam_channel channel = v22_channel(1200);
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_qam_point point = { 1.0, 0.0 };
	struct sippymodem_qam_point bounded_point = { sqrt(3.0 / 2.0), 0.0 };
	struct sippymodem_qam_point invalid_point = { 1.0, 1.0 };
	struct sippymodem_qam_point received_point;
	uint32_t carrier_phase_step;
	int16_t sample = 0;
	bool accepted = false;
	bool floating_accepted = false;
	double floating_sample = -99.0;
	double pulse = 0.0;

	CHECK(sippymodem_root_raised_cosine(0.0, 0.0, &pulse) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      sippymodem_root_raised_cosine(0.0, 0.75, NULL) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      sippymodem_qam_modulator_init(NULL, &channel, 1) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted an invalid initialization argument");
	channel.carrier_frequency_numerator = TEST_SAMPLE_RATE / 2;
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier at Nyquist");
	channel = v22_channel(1200);
	channel.carrier_frequency_denominator = 0;
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier with no denominator");
	channel = v22_channel(1200);
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK,
	      "QAM corruption fixture did not initialize");
	carrier_phase_step = modulator.carrier_phase_step;
	modulator.carrier_phase_step++;
	CHECK(!sippymodem_qam_modulator_is_valid(&modulator) &&
		      sippymodem_qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted a carrier phase step inconsistent with its channel");
	modulator.carrier_phase_step = carrier_phase_step;
	CHECK(sippymodem_qam_demodulator_init(
		      &demodulator, &channel) == SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_demodulator_reset(&demodulator) ==
			      SIPPYMODEM_QAM_OK,
		      "QAM demodulator reset rejected valid state");
	CHECK(sippymodem_qam_modulator_sample(
		      &modulator, &bounded_point, &sample, &accepted) ==
		      SIPPYMODEM_QAM_OK &&
		      accepted && sample >= -TEST_AMPLITUDE &&
		      sample <= TEST_AMPLITUDE,
	      "QAM rejected a bounded super-unit constellation point");
	CHECK(sippymodem_qam_modulator_sample(
		      &modulator, &invalid_point, &sample, &accepted) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
	      sippymodem_qam_modulator_sample(
			      &modulator, NULL, &sample, &accepted) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted an unbounded constellation point");
	{
		struct sippymodem_qam_modulator before = modulator;

		CHECK(sippymodem_qam_modulator_sample_f64(
			      &modulator, &invalid_point, &floating_sample,
			      &floating_accepted) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      sippymodem_qam_modulator_sample_f64(
			      &modulator, &point, NULL, &floating_accepted) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      sippymodem_qam_modulator_sample_f64(
			      &modulator, &point, &floating_sample, NULL) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      memcmp(&modulator, &before, sizeof(modulator)) == 0 &&
		      floating_sample == -99.0 && !floating_accepted,
		      "invalid QAM floating sample mutated state");
	}
	modulator.active_count = SIPPYMODEM_QAM_MAX_ACTIVE_SYMBOLS + 1;
	CHECK(!sippymodem_qam_modulator_can_accept_symbol(&modulator) &&
		      sippymodem_qam_modulator_sample(
			      &modulator, &point, &sample, &accepted) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      strcmp(sippymodem_qam_result_name(
			     SIPPYMODEM_QAM_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "QAM accepted corrupt active-symbol state");
	demodulator.history_index = demodulator.matched_filter_count;
	CHECK(sippymodem_qam_demodulator_sample(
		      &demodulator, 0, &received_point, &accepted) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      !sippymodem_qam_demodulator_measure(&demodulator)
			       .timing_locked,
		      "QAM accepted corrupt matched-filter state");
	demodulator.history_index = 0;
	demodulator.previous_in_phase = NAN;
	CHECK(sippymodem_qam_demodulator_sample(
		      &demodulator, 0, &received_point, &accepted) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT,
	      "QAM accepted corrupt interpolation state");
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
		{ "exact carrier clock", test_exact_carrier_clock },
		{ "channel carriers", test_channel_carriers },
		{ "floating transmit path", test_floating_transmit_path },
		{ "receive loopback", test_receive_loopback },
		{ "receive tolerance", test_receive_tolerance },
		{ "silence does not lock", test_silence_does_not_lock },
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
