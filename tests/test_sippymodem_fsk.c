// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_fsk.h>
#include <sippymodem_v21.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_AMPLITUDE = 8192,
	TRAINING_BITS = 48,
	SYNC_BITS = 32,
	PAYLOAD_BITS = 192,
	TAIL_BITS = 24,
	SEQUENCE_BITS = TRAINING_BITS + SYNC_BITS + PAYLOAD_BITS + TAIL_BITS,
	DECODED_CAPACITY = 384,
};

static const struct sippymodem_fsk_channel channel_1200_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.symbol_rate_numerator = 1200,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = 1300,
	.space_frequency_hz = 2100,
	.detector_window = 11,
	.metric_smoothing_shift = 1,
};

static const struct sippymodem_fsk_channel channel_75_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.symbol_rate_numerator = 75,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = 390,
	.space_frequency_hz = 450,
	.detector_window = 128,
	.metric_smoothing_shift = 1,
};

static const struct sippymodem_fsk_channel channel_500_over_11_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.symbol_rate_numerator = 500,
	.symbol_rate_denominator = 11,
	.mark_frequency_hz = 1400,
	.space_frequency_hz = 1800,
	.detector_window = 64,
	.metric_smoothing_shift = 1,
};

struct loopback_result {
	uint8_t decoded[DECODED_CAPACITY];
	size_t decoded_count;
	struct sippymodem_fsk_measurement measurement;
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static void build_sequence(uint8_t sequence[SEQUENCE_BITS])
{
	uint32_t random = UINT32_C(0x21f5a17b);

	memset(sequence, 1, TRAINING_BITS);
	for (size_t index = 0; index < SYNC_BITS + PAYLOAD_BITS; ++index)
		sequence[TRAINING_BITS + index] =
			(uint8_t)(fixture_random(&random) >> 31);
	memset(sequence + TRAINING_BITS + SYNC_BITS + PAYLOAD_BITS, 1,
	       TAIL_BITS);
}

static int16_t impair_sample(
	int16_t sample, bool impaired, uint32_t *random)
{
	long value;

	if (!impaired)
		return sample;
	value = lround((double)sample * 0.58) + 275;
	value += (long)(fixture_random(random) % 501) - 250;
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static bool run_loopback(
	const struct sippymodem_fsk_channel *channel,
	bool impaired,
	const uint8_t sequence[SEQUENCE_BITS],
	struct loopback_result *result)
{
	struct sippymodem_fsk_transmitter transmitter;
	struct sippymodem_fsk_receiver receiver;
	uint32_t random = UINT32_C(0x6d2b79f5);
	size_t transmitted = 0;
	size_t sample_limit =
		(SEQUENCE_BITS + 2) * channel->sample_rate *
		channel->symbol_rate_denominator /
		channel->symbol_rate_numerator;

	memset(result, 0, sizeof(*result));
	CHECK(sippymodem_fsk_transmitter_init(
		      &transmitter, channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_FSK_OK &&
		      sippymodem_fsk_receiver_init(&receiver, channel) ==
			      SIPPYMODEM_FSK_OK,
	      "FSK loopback endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < sample_limit && transmitted < SEQUENCE_BITS;
	     ++sample_index) {
		int16_t sample;
		uint8_t decoded = 0;
		bool consumed;
		bool ready;

		CHECK(sippymodem_fsk_transmitter_sample(
		      &transmitter, sequence[transmitted], &sample, &consumed) ==
			      SIPPYMODEM_FSK_OK,
		      "FSK transmitter rejected a valid symbol");
		sample = impair_sample(sample, impaired, &random);
		CHECK(sippymodem_fsk_receiver_sample(
		      &receiver, sample, &decoded, &ready) ==
			      SIPPYMODEM_FSK_OK,
		      "FSK receiver rejected a valid sample");
		if (ready) {
			CHECK(result->decoded_count < DECODED_CAPACITY,
			      "FSK loopback produced unbounded bits");
			result->decoded[result->decoded_count++] = decoded;
		}
		if (consumed)
			transmitted++;
	}
	result->measurement = sippymodem_fsk_receiver_measure(&receiver);
	CHECK(transmitted == SEQUENCE_BITS,
	      "FSK rational symbol clock did not consume the fixture");
	return true;
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

static bool check_payload(
	const struct sippymodem_fsk_channel *channel, bool impaired)
{
	uint8_t sequence[SEQUENCE_BITS];
	struct loopback_result result;
	size_t sync_offset;

	build_sequence(sequence);
	CHECK(run_loopback(channel, impaired, sequence, &result),
	      "FSK loopback fixture failed");
	sync_offset = find_pattern(
		result.decoded, result.decoded_count,
		sequence + TRAINING_BITS, SYNC_BITS);
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + SYNC_BITS + PAYLOAD_BITS <=
			      result.decoded_count,
	      "FSK receiver did not acquire the synchronization word");
	CHECK(memcmp(result.decoded + sync_offset + SYNC_BITS,
		     sequence + TRAINING_BITS + SYNC_BITS,
		     PAYLOAD_BITS) == 0,
	      "FSK payload changed after carrier acquisition");
	CHECK(result.measurement.ready &&
		      result.measurement.line_power != 0 &&
		      result.measurement.dominance_q8 >= 180,
	      "FSK measurement did not retain carrier evidence");
	return true;
}

static bool test_exact_symbol_clock_and_validation(void)
{
	struct sippymodem_fsk_transmitter transmitter;
	struct sippymodem_fsk_receiver receiver;
	struct sippymodem_fsk_channel invalid = sippymodem_v21_channel_1;
	unsigned int consumed_bits = 0;
	int16_t sample = 0;
	uint8_t bit = 0;
	bool consumed = false;
	bool ready = false;

	CHECK(sippymodem_fsk_transmitter_init(
		      &transmitter, &sippymodem_v21_channel_1, TEST_AMPLITUDE) ==
		      SIPPYMODEM_FSK_OK &&
		      sippymodem_fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK transmitter clock fixture failed");
	for (unsigned int index = 0;
	     index < sippymodem_v21_channel_1.sample_rate;
	     ++index) {
		CHECK(sippymodem_fsk_transmitter_sample(
		      &transmitter, (uint8_t)(consumed_bits & 1u),
		      &sample, &consumed) == SIPPYMODEM_FSK_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE,
		      "FSK transmitter exceeded its bounded waveform");
		if (consumed)
			consumed_bits++;
	}
	CHECK(consumed_bits ==
		      sippymodem_v21_channel_1.symbol_rate_numerator &&
		      transmitter.symbol_accumulator == 0 &&
		      sippymodem_fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK symbol clock drifted over one exact second");
	transmitter.symbol_accumulator = transmitter.channel.sample_rate;
	CHECK(!sippymodem_fsk_transmitter_can_accept_bit(&transmitter) &&
		      sippymodem_fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT,
	      "FSK transmitter accepted a corrupted fractional phase");
	transmitter.symbol_accumulator = 0;
	transmitter.channel.symbol_rate_numerator =
		transmitter.channel.sample_rate + 1;
	CHECK(!sippymodem_fsk_transmitter_can_accept_bit(&transmitter) &&
		      sippymodem_fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT,
	      "FSK transmitter accepted a corrupted symbol rate");
	CHECK(sippymodem_fsk_receiver_init(
		      &receiver, &sippymodem_v21_channel_2) ==
		      SIPPYMODEM_FSK_OK,
	      "FSK receiver validation fixture failed");
	sippymodem_fsk_receiver_reset(&receiver);
	CHECK(receiver.channel.mark_frequency_hz ==
		      sippymodem_v21_channel_2.mark_frequency_hz &&
		      !sippymodem_fsk_receiver_measure(&receiver).ready,
	      "FSK receiver reset discarded its configuration");
	invalid.space_frequency_hz = invalid.sample_rate / 2;
	transmitter.tones.mark_phase_step = 0;
	CHECK(sippymodem_fsk_transmitter_init(
		      NULL, &sippymodem_v21_channel_1, TEST_AMPLITUDE) ==
		      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_transmitter_init(
			      &transmitter, &sippymodem_v21_channel_1, 0) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_receiver_init(&receiver, &invalid) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_receiver_sample(
			      NULL, sample, &bit, &ready) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      !sippymodem_fsk_receiver_measure(NULL).ready &&
		      strcmp(sippymodem_fsk_result_name(
			     SIPPYMODEM_FSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "FSK API accepted invalid state");
	return true;
}

static bool test_fractional_symbol_clock(void)
{
	struct sippymodem_fsk_transmitter transmitter;
	uint64_t exact_interval =
		(uint64_t)channel_500_over_11_baud.sample_rate *
		channel_500_over_11_baud.symbol_rate_denominator;
	unsigned int consumed_bits = 0;

	CHECK(sippymodem_fsk_transmitter_init(
		      &transmitter, &channel_500_over_11_baud,
		      TEST_AMPLITUDE) == SIPPYMODEM_FSK_OK,
	      "fractional FSK clock did not initialize");
	for (uint64_t index = 0; index < exact_interval; ++index) {
		int16_t sample;
		bool consumed;

		CHECK(sippymodem_fsk_transmitter_sample(
		      &transmitter, (uint8_t)(consumed_bits & 1u),
		      &sample, &consumed) == SIPPYMODEM_FSK_OK,
		      "fractional FSK clock rejected a valid symbol");
		if (consumed)
			consumed_bits++;
	}
	CHECK(consumed_bits ==
		      channel_500_over_11_baud.symbol_rate_numerator &&
		      transmitter.symbol_accumulator == 0,
	      "500/11-baud clock drifted over its exact 11-second period");
	return check_payload(&channel_500_over_11_baud, false);
}

static bool test_continuous_tone_detection(void)
{
	struct sippymodem_fsk_tone_generator generator;
	struct sippymodem_fsk_receiver receiver;
	struct sippymodem_fsk_measurement measurement;
	int16_t sample = 0;
	uint8_t decision = 0;
	bool ready = false;

	CHECK(sippymodem_fsk_tone_generator_init(
		      &generator, &channel_500_over_11_baud,
		      TEST_AMPLITUDE) == SIPPYMODEM_FSK_OK &&
		      sippymodem_fsk_receiver_init(
			      &receiver, &channel_500_over_11_baud) ==
			      SIPPYMODEM_FSK_OK,
	      "continuous FSK tone endpoints did not initialize");
	for (size_t index = 0;
	     index < channel_500_over_11_baud.detector_window * 3;
	     ++index) {
		CHECK(sippymodem_fsk_tone_generator_sample(
			      &generator, 1, &sample) == SIPPYMODEM_FSK_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE &&
			      sippymodem_fsk_receiver_detect(
				      &receiver, sample, &decision, &ready) ==
				      SIPPYMODEM_FSK_OK,
		      "continuous mark tone was rejected");
	}
	measurement = sippymodem_fsk_receiver_measure(&receiver);
	CHECK(ready && decision == 1 && measurement.ready &&
		      measurement.mark_power > measurement.space_power &&
		      measurement.dominance_q8 >= 180,
	      "continuous mark tone was not distinguished");
	for (size_t index = 0;
	     index < channel_500_over_11_baud.detector_window * 3;
	     ++index) {
		CHECK(sippymodem_fsk_tone_generator_sample(
			      &generator, 0, &sample) == SIPPYMODEM_FSK_OK &&
			      sippymodem_fsk_receiver_detect(
				      &receiver, sample, &decision, &ready) ==
				      SIPPYMODEM_FSK_OK,
		      "continuous space tone was rejected");
	}
	measurement = sippymodem_fsk_receiver_measure(&receiver);
	CHECK(ready && decision == 0 &&
		      measurement.space_power > measurement.mark_power &&
		      measurement.dominance_q8 >= 180,
	      "continuous space tone was not distinguished");
	generator.mark_phase_step = 0;
	CHECK(sippymodem_fsk_tone_generator_sample(
		      &generator, 1, &sample) ==
		      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_tone_generator_init(
			      NULL, &channel_500_over_11_baud,
			      TEST_AMPLITUDE) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_receiver_detect(
			      NULL, sample, &decision, &ready) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT,
	      "continuous FSK tone API accepted invalid state");
	return true;
}

static bool test_v21_channel_pairs(void)
{
	return check_payload(&sippymodem_v21_channel_1, false) &&
	       check_payload(&sippymodem_v21_channel_2, false) &&
	       check_payload(&sippymodem_v21_channel_1, true) &&
	       check_payload(&sippymodem_v21_channel_2, true);
}

static bool test_v21_profiles(void)
{
	CHECK(sippymodem_v21_channel_1.sample_rate ==
		      SIPPYMODEM_V21_SAMPLE_RATE &&
		      sippymodem_v21_channel_1.symbol_rate_numerator ==
			      SIPPYMODEM_V21_BIT_RATE &&
		      sippymodem_v21_channel_1.symbol_rate_denominator == 1 &&
		      sippymodem_v21_channel_1.mark_frequency_hz == 980 &&
		      sippymodem_v21_channel_1.space_frequency_hz == 1180 &&
		      sippymodem_v21_channel_2.sample_rate ==
			      SIPPYMODEM_V21_SAMPLE_RATE &&
		      sippymodem_v21_channel_2.symbol_rate_numerator ==
			      SIPPYMODEM_V21_BIT_RATE &&
		      sippymodem_v21_channel_2.symbol_rate_denominator == 1 &&
		      sippymodem_v21_channel_2.mark_frequency_hz == 1650 &&
		      sippymodem_v21_channel_2.space_frequency_hz == 1850,
	      "V.21 profiles do not match the channel definitions");
	return true;
}

static bool test_configurable_rate_and_tone_pair(void)
{
	return check_payload(&channel_1200_baud, false) &&
	       check_payload(&channel_75_baud, false);
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact symbol clock and validation",
		  test_exact_symbol_clock_and_validation },
		{ "fractional symbol clock", test_fractional_symbol_clock },
		{ "continuous tone detection", test_continuous_tone_detection },
		{ "V.21 profiles", test_v21_profiles },
		{ "V.21 channel pairs", test_v21_channel_pairs },
		{ "configurable rate and tone pair",
		  test_configurable_rate_and_tone_pair },
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
