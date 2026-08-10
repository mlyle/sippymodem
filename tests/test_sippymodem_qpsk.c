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
	TEST_SYMBOL_COUNT = 384,
	TEST_DECODED_CAPACITY = 512,
	TEST_SYNC_SYMBOLS = 32,
	TEST_PAYLOAD_SYMBOLS = 128,
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
	struct sippymodem_qpsk_channel transmit_channel =
		v22_channel(transmit_carrier_hz);
	struct sippymodem_qpsk_channel receive_channel =
		v22_channel(receive_carrier_hz);
	struct sippymodem_qpsk_modulator modulator;
	struct sippymodem_qpsk_demodulator demodulator;
	struct sippymodem_qpsk_measurement measurement;
	uint8_t changes[TEST_SYMBOL_COUNT];
	uint8_t decoded[TEST_DECODED_CAPACITY];
	uint32_t random = UINT32_C(0x912ca775);
	uint8_t quadrant = 0;
	size_t accepted = 0;
	size_t decoded_count = 0;
	size_t sync_offset;
	size_t sample_limit = leading_silence +
		(TEST_SYMBOL_COUNT + 16u) * TEST_SAMPLE_RATE / TEST_SYMBOL_RATE;

	build_phase_changes(changes);
	CHECK(sippymodem_qpsk_modulator_init(
		      &modulator, &transmit_channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QPSK_OK &&
		      sippymodem_qpsk_demodulator_init(
			      &demodulator, &receive_channel) ==
			      SIPPYMODEM_QPSK_OK,
	      "QPSK receive-loopback endpoints did not initialize");
	for (size_t sample_index = 0; sample_index < sample_limit;
	     ++sample_index) {
		int16_t sample = 0;
		uint8_t phase_change = 0;
		bool ready;

		if (sample_index >= leading_silence) {
			bool symbol_accepted;

			CHECK(sippymodem_qpsk_modulator_sample(
				      &modulator, quadrant, &sample,
				      &symbol_accepted) == SIPPYMODEM_QPSK_OK,
			      "QPSK loopback modulator failed");
			if (symbol_accepted && accepted < TEST_SYMBOL_COUNT) {
				quadrant =
					(uint8_t)((quadrant + changes[accepted]) & 3u);
				accepted++;
			}
		}
		sample = impair_sample(sample, impaired, &random);
		CHECK(sippymodem_qpsk_demodulator_sample(
			      &demodulator, sample, &phase_change, &ready) ==
			      SIPPYMODEM_QPSK_OK,
		      "QPSK loopback demodulator failed");
		if (ready) {
			CHECK(decoded_count < ARRAY_LENGTH(decoded),
			      "QPSK loopback produced unbounded symbols");
			decoded[decoded_count++] = phase_change;
		}
	}
	CHECK(accepted == TEST_SYMBOL_COUNT,
	      "QPSK loopback did not consume its symbol fixture");
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
	      "QPSK receiver changed its post-acquisition payload");
	measurement = sippymodem_qpsk_demodulator_measure(&demodulator);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "QPSK receiver did not retain carrier measurements");
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
	struct sippymodem_qpsk_channel channel = v22_channel(1200);
	struct sippymodem_qpsk_demodulator demodulator;

	CHECK(sippymodem_qpsk_demodulator_init(&demodulator, &channel) ==
		      SIPPYMODEM_QPSK_OK,
	      "QPSK silence fixture did not initialize");
	for (unsigned int index = 0; index < TEST_SAMPLE_RATE / 2; ++index) {
		uint8_t phase_change;
		bool ready;

		CHECK(sippymodem_qpsk_demodulator_sample(
			      &demodulator, 0, &phase_change, &ready) ==
			      SIPPYMODEM_QPSK_OK &&
			      !ready,
		      "QPSK silence produced a symbol at sample %u", index);
	}
	CHECK(!sippymodem_qpsk_demodulator_measure(&demodulator).timing_locked,
	      "QPSK silence produced a timing lock");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_qpsk_channel channel = v22_channel(1200);
	struct sippymodem_qpsk_modulator modulator;
	struct sippymodem_qpsk_demodulator demodulator;
	int16_t sample = 0;
	uint8_t value = 0;
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
	CHECK(sippymodem_qpsk_demodulator_init(
		      &demodulator, &channel) == SIPPYMODEM_QPSK_OK &&
		      sippymodem_qpsk_demodulator_reset(&demodulator) ==
			      SIPPYMODEM_QPSK_OK,
	      "QPSK demodulator reset rejected valid state");
	modulator.active_count = SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS + 1;
	CHECK(!sippymodem_qpsk_modulator_can_accept_symbol(&modulator) &&
		      sippymodem_qpsk_modulator_sample(
			      &modulator, 0, &sample, &accepted) ==
			      SIPPYMODEM_QPSK_INVALID_ARGUMENT &&
		      strcmp(sippymodem_qpsk_result_name(
			     SIPPYMODEM_QPSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "QPSK accepted corrupt active-symbol state");
	demodulator.history_index = demodulator.matched_filter_count;
	CHECK(sippymodem_qpsk_demodulator_sample(
		      &demodulator, 0, &value, &accepted) ==
		      SIPPYMODEM_QPSK_INVALID_ARGUMENT &&
		      !sippymodem_qpsk_demodulator_measure(&demodulator)
			       .timing_locked,
	      "QPSK accepted corrupt matched-filter state");
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
