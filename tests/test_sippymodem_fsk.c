// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_fsk.h>

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

static const struct sippymodem_fsk_channel v21_channel_1 = {
	.sample_rate = TEST_SAMPLE_RATE,
	.baud = 300,
	.mark_frequency_hz = 980,
	.space_frequency_hz = 1180,
	.detector_window = 27,
	.metric_smoothing_shift = 2,
};

static const struct sippymodem_fsk_channel v21_channel_2 = {
	.sample_rate = TEST_SAMPLE_RATE,
	.baud = 300,
	.mark_frequency_hz = 1650,
	.space_frequency_hz = 1850,
	.detector_window = 27,
	.metric_smoothing_shift = 2,
};

static const struct sippymodem_fsk_channel channel_1200_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.baud = 1200,
	.mark_frequency_hz = 1300,
	.space_frequency_hz = 2100,
	.detector_window = 11,
	.metric_smoothing_shift = 1,
};

static const struct sippymodem_fsk_channel channel_75_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.baud = 75,
	.mark_frequency_hz = 390,
	.space_frequency_hz = 450,
	.detector_window = 128,
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
		(SEQUENCE_BITS + 2) * channel->sample_rate / channel->baud;

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
	struct sippymodem_fsk_channel invalid = v21_channel_1;
	unsigned int consumed_bits = 0;
	int16_t sample = 0;
	uint8_t bit = 0;
	bool consumed = false;
	bool ready = false;

	CHECK(sippymodem_fsk_transmitter_init(
		      &transmitter, &v21_channel_1, TEST_AMPLITUDE) ==
		      SIPPYMODEM_FSK_OK &&
		      sippymodem_fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK transmitter clock fixture failed");
	for (unsigned int index = 0; index < v21_channel_1.sample_rate;
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
	CHECK(consumed_bits == v21_channel_1.baud &&
		      transmitter.baud_accumulator == 0 &&
		      sippymodem_fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK symbol clock drifted over one exact second");
	transmitter.channel.baud = transmitter.channel.sample_rate + 1;
	CHECK(!sippymodem_fsk_transmitter_can_accept_bit(&transmitter) &&
		      sippymodem_fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      SIPPYMODEM_FSK_INVALID_ARGUMENT,
	      "FSK transmitter accepted a corrupted symbol rate");
	CHECK(sippymodem_fsk_receiver_init(&receiver, &v21_channel_2) ==
		      SIPPYMODEM_FSK_OK,
	      "FSK receiver validation fixture failed");
	sippymodem_fsk_receiver_reset(&receiver);
	CHECK(receiver.channel.mark_frequency_hz ==
		      v21_channel_2.mark_frequency_hz &&
		      !sippymodem_fsk_receiver_measure(&receiver).ready,
	      "FSK receiver reset discarded its configuration");
	invalid.space_frequency_hz = invalid.sample_rate / 2;
	transmitter.mark_phase_step = 0;
	CHECK(sippymodem_fsk_transmitter_init(
		      NULL, &v21_channel_1, TEST_AMPLITUDE) ==
		      SIPPYMODEM_FSK_INVALID_ARGUMENT &&
		      sippymodem_fsk_transmitter_init(
			      &transmitter, &v21_channel_1, 0) ==
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

static bool test_v21_channel_pairs(void)
{
	return check_payload(&v21_channel_1, false) &&
	       check_payload(&v21_channel_2, false) &&
	       check_payload(&v21_channel_1, true) &&
	       check_payload(&v21_channel_2, true);
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
