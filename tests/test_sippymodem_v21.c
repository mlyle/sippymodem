// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v21.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	TRAINING_BITS = 48,
	SYNC_BITS = 32,
	PAYLOAD_BITS = 192,
	TAIL_BITS = 24,
	SEQUENCE_BITS = TRAINING_BITS + SYNC_BITS + PAYLOAD_BITS + TAIL_BITS,
	DECODED_CAPACITY = 384,
};

struct loopback_result {
	uint8_t decoded[DECODED_CAPACITY];
	size_t decoded_count;
	struct sippymodem_v21_measurement measurement;
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
	enum sippymodem_v21_channel channel,
	bool impaired,
	const uint8_t sequence[SEQUENCE_BITS],
	struct loopback_result *result)
{
	struct sippymodem_v21_transmitter transmitter;
	struct sippymodem_v21_receiver receiver;
	uint32_t random = UINT32_C(0x6d2b79f5);
	size_t transmitted = 0;
	size_t sample_limit =
		(SEQUENCE_BITS + 2) * SIPPYMODEM_V21_SAMPLE_RATE /
		SIPPYMODEM_V21_BAUD;

	memset(result, 0, sizeof(*result));
	CHECK(sippymodem_v21_transmitter_init(&transmitter, channel) ==
		      SIPPYMODEM_V21_OK &&
		      sippymodem_v21_receiver_init(&receiver, channel) ==
			      SIPPYMODEM_V21_OK,
	      "V.21 loopback endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < sample_limit && transmitted < SEQUENCE_BITS;
	     ++sample_index) {
		int16_t sample;
		uint8_t decoded = 0;
		bool consumed;
		bool ready;

		CHECK(sippymodem_v21_transmitter_sample(
		      &transmitter, sequence[transmitted], &sample, &consumed) ==
			      SIPPYMODEM_V21_OK,
		      "V.21 transmitter rejected a valid symbol");
		sample = impair_sample(sample, impaired, &random);
		CHECK(sippymodem_v21_receiver_sample(
		      &receiver, sample, &decoded, &ready) ==
			      SIPPYMODEM_V21_OK,
		      "V.21 receiver rejected a valid sample");
		if (ready) {
			CHECK(result->decoded_count < DECODED_CAPACITY,
			      "V.21 loopback produced unbounded bits");
			result->decoded[result->decoded_count++] = decoded;
		}
		if (consumed)
			transmitted++;
	}
	result->measurement = sippymodem_v21_receiver_measure(&receiver);
	CHECK(transmitted == SEQUENCE_BITS,
	      "V.21 rational symbol clock did not consume the fixture");
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
	enum sippymodem_v21_channel channel, bool impaired)
{
	uint8_t sequence[SEQUENCE_BITS];
	struct loopback_result result;
	size_t sync_offset;

	build_sequence(sequence);
	CHECK(run_loopback(channel, impaired, sequence, &result),
	      "V.21 loopback fixture failed");
	sync_offset = find_pattern(
		result.decoded, result.decoded_count,
		sequence + TRAINING_BITS, SYNC_BITS);
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + SYNC_BITS + PAYLOAD_BITS <=
			      result.decoded_count,
	      "V.21 receiver did not acquire the synchronization word");
	CHECK(memcmp(result.decoded + sync_offset + SYNC_BITS,
		     sequence + TRAINING_BITS + SYNC_BITS,
		     PAYLOAD_BITS) == 0,
	      "V.21 payload changed after carrier acquisition");
	CHECK(result.measurement.ready &&
		      result.measurement.line_power != 0 &&
		      result.measurement.dominance_q8 >= 190,
	      "V.21 tone measurement did not retain carrier evidence");
	return true;
}

static bool test_exact_symbol_clock_and_validation(void)
{
	struct sippymodem_v21_transmitter transmitter;
	struct sippymodem_v21_receiver receiver;
	unsigned int consumed_bits = 0;
	int16_t sample = 0;
	uint8_t bit = 0;
	bool consumed = false;
	bool ready = false;

	CHECK(sippymodem_v21_transmitter_init(
		      &transmitter, SIPPYMODEM_V21_CHANNEL_1) ==
		      SIPPYMODEM_V21_OK,
	      "V.21 transmitter clock fixture failed");
	for (unsigned int index = 0; index < SIPPYMODEM_V21_SAMPLE_RATE;
	     ++index) {
		CHECK(sippymodem_v21_transmitter_sample(
		      &transmitter, (uint8_t)(consumed_bits & 1u),
		      &sample, &consumed) == SIPPYMODEM_V21_OK &&
			      sample >= -8192 && sample <= 8192,
		      "V.21 transmitter exceeded its bounded waveform");
		if (consumed)
			consumed_bits++;
	}
	CHECK(consumed_bits == SIPPYMODEM_V21_BAUD &&
		      transmitter.baud_accumulator == 0,
	      "V.21 symbol clock drifted over one exact second");
	CHECK(sippymodem_v21_receiver_init(
		      &receiver, SIPPYMODEM_V21_CHANNEL_2) ==
		      SIPPYMODEM_V21_OK,
	      "V.21 receiver validation fixture failed");
	sippymodem_v21_receiver_reset(&receiver);
	transmitter.mark_phase_step = 0;
	CHECK(sippymodem_v21_transmitter_init(NULL,
		      SIPPYMODEM_V21_CHANNEL_1) ==
		      SIPPYMODEM_V21_INVALID_ARGUMENT &&
		      sippymodem_v21_receiver_init(
			      &receiver,
			      (enum sippymodem_v21_channel)99) ==
			      SIPPYMODEM_V21_INVALID_ARGUMENT &&
		      sippymodem_v21_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      SIPPYMODEM_V21_INVALID_ARGUMENT &&
		      sippymodem_v21_receiver_sample(
			      NULL, sample, &bit, &ready) ==
			      SIPPYMODEM_V21_INVALID_ARGUMENT &&
		      !sippymodem_v21_receiver_measure(&receiver).ready &&
		      strcmp(sippymodem_v21_result_name(
			     SIPPYMODEM_V21_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.21 API accepted invalid state");
	return true;
}

static bool test_clean_channel_pairs(void)
{
	return check_payload(SIPPYMODEM_V21_CHANNEL_1, false) &&
	       check_payload(SIPPYMODEM_V21_CHANNEL_2, false);
}

static bool test_bounded_line_impairment(void)
{
	return check_payload(SIPPYMODEM_V21_CHANNEL_1, true) &&
	       check_payload(SIPPYMODEM_V21_CHANNEL_2, true);
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact symbol clock and validation",
		  test_exact_symbol_clock_and_validation },
		{ "clean channel pairs", test_clean_channel_pairs },
		{ "bounded line impairment", test_bounded_line_impairment },
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
