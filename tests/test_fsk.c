// SPDX-License-Identifier: BSD-3-Clause

#include <fsk.h>
#include <v21.h>

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

static const struct fsk_channel channel_1200_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.symbol_rate_numerator = 1200,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = 1300,
	.space_frequency_hz = 2100,
	.detector_window = 11,
	.metric_smoothing_shift = 1,
};

static const struct fsk_channel channel_75_baud = {
	.sample_rate = TEST_SAMPLE_RATE,
	.symbol_rate_numerator = 75,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = 390,
	.space_frequency_hz = 450,
	.detector_window = 128,
	.metric_smoothing_shift = 1,
};

static const struct fsk_channel channel_500_over_11_baud = {
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
	struct fsk_measurement measurement;
};

static uint64_t hash_byte(uint64_t hash, uint8_t byte)
{
	return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
	for (unsigned int shift = 0; shift < 64; shift += 8)
		hash = hash_byte(hash, (uint8_t)(value >> shift));
	return hash;
}

static int16_t word_to_s16(uint16_t word)
{
	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

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
	const struct fsk_channel *channel,
	bool impaired,
	const uint8_t sequence[SEQUENCE_BITS],
	struct loopback_result *result)
{
	struct fsk_transmitter transmitter;
	struct fsk_receiver receiver;
	uint32_t random = UINT32_C(0x6d2b79f5);
	size_t transmitted = 0;
	size_t sample_limit =
		(SEQUENCE_BITS + 2) * channel->sample_rate *
		channel->symbol_rate_denominator /
		channel->symbol_rate_numerator;

	memset(result, 0, sizeof(*result));
	CHECK(fsk_transmitter_init(
		      &transmitter, channel, TEST_AMPLITUDE) ==
		      FSK_OK &&
		      fsk_receiver_init(&receiver, channel) ==
			      FSK_OK,
	      "FSK loopback endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < sample_limit && transmitted < SEQUENCE_BITS;
	     ++sample_index) {
		int16_t sample;
		uint8_t decoded = 0;
		bool consumed;
		bool ready;

		CHECK(fsk_transmitter_sample(
		      &transmitter, sequence[transmitted], &sample, &consumed) ==
			      FSK_OK,
		      "FSK transmitter rejected a valid symbol");
		sample = impair_sample(sample, impaired, &random);
		CHECK(fsk_receiver_sample(
		      &receiver, sample, &decoded, &ready) ==
			      FSK_OK,
		      "FSK receiver rejected a valid sample");
		if (ready) {
			CHECK(result->decoded_count < DECODED_CAPACITY,
			      "FSK loopback produced unbounded bits");
			result->decoded[result->decoded_count++] = decoded;
		}
		if (consumed)
			transmitted++;
	}
	result->measurement = fsk_receiver_measure(&receiver);
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
	const struct fsk_channel *channel, bool impaired)
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
	struct fsk_transmitter transmitter;
	struct fsk_receiver receiver;
	struct fsk_channel invalid = v21_channel_1;
	unsigned int consumed_bits = 0;
	int16_t sample = 0;
	uint8_t bit = 0;
	bool consumed = false;
	bool ready = false;

	CHECK(fsk_transmitter_init(
		      &transmitter, &v21_channel_1, TEST_AMPLITUDE) ==
		      FSK_OK &&
		      fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK transmitter clock fixture failed");
	for (unsigned int index = 0;
	     index < v21_channel_1.sample_rate;
	     ++index) {
		CHECK(fsk_transmitter_sample(
		      &transmitter, (uint8_t)(consumed_bits & 1u),
		      &sample, &consumed) == FSK_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE,
		      "FSK transmitter exceeded its bounded waveform");
		if (consumed)
			consumed_bits++;
	}
	CHECK(consumed_bits ==
		      v21_channel_1.symbol_rate_numerator &&
		      transmitter.symbol_accumulator == 0 &&
		      fsk_transmitter_can_accept_bit(&transmitter),
	      "FSK symbol clock drifted over one exact second");
	transmitter.symbol_accumulator = transmitter.channel.sample_rate;
	CHECK(!fsk_transmitter_can_accept_bit(&transmitter) &&
		      fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      FSK_INVALID_ARGUMENT,
	      "FSK transmitter accepted a corrupted fractional phase");
	transmitter.symbol_accumulator = 0;
	transmitter.channel.symbol_rate_numerator =
		transmitter.channel.sample_rate + 1;
	CHECK(!fsk_transmitter_can_accept_bit(&transmitter) &&
		      fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      FSK_INVALID_ARGUMENT,
	      "FSK transmitter accepted a corrupted symbol rate");
	CHECK(fsk_receiver_init(
		      &receiver, &v21_channel_2) ==
		      FSK_OK,
	      "FSK receiver validation fixture failed");
	fsk_receiver_reset(&receiver);
	CHECK(receiver.channel.mark_frequency_hz ==
		      v21_channel_2.mark_frequency_hz &&
		      !fsk_receiver_measure(&receiver).ready,
	      "FSK receiver reset discarded its configuration");
	invalid.space_frequency_hz = invalid.sample_rate / 2;
	transmitter.tones.mark_phase_step = 0;
	CHECK(fsk_transmitter_init(
		      NULL, &v21_channel_1, TEST_AMPLITUDE) ==
		      FSK_INVALID_ARGUMENT &&
		      fsk_transmitter_init(
			      &transmitter, &v21_channel_1, 0) ==
			      FSK_INVALID_ARGUMENT &&
		      fsk_receiver_init(&receiver, &invalid) ==
			      FSK_INVALID_ARGUMENT &&
		      fsk_transmitter_sample(
			      &transmitter, 1, &sample, &consumed) ==
			      FSK_INVALID_ARGUMENT &&
		      fsk_receiver_sample(
			      NULL, sample, &bit, &ready) ==
			      FSK_INVALID_ARGUMENT &&
		      !fsk_receiver_measure(NULL).ready,
	      "FSK API accepted invalid state");
	return true;
}

static bool test_fractional_symbol_clock(void)
{
	struct fsk_transmitter transmitter;
	uint64_t exact_interval =
		(uint64_t)channel_500_over_11_baud.sample_rate *
		channel_500_over_11_baud.symbol_rate_denominator;
	unsigned int consumed_bits = 0;

	CHECK(fsk_transmitter_init(
		      &transmitter, &channel_500_over_11_baud,
		      TEST_AMPLITUDE) == FSK_OK,
	      "fractional FSK clock did not initialize");
	for (uint64_t index = 0; index < exact_interval; ++index) {
		int16_t sample;
		bool consumed;

		CHECK(fsk_transmitter_sample(
		      &transmitter, (uint8_t)(consumed_bits & 1u),
		      &sample, &consumed) == FSK_OK,
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
	struct fsk_tone_generator generator;
	struct fsk_receiver receiver;
	struct fsk_measurement measurement;
	int16_t sample = 0;
	uint8_t decision = 0;
	bool ready = false;

	CHECK(fsk_tone_generator_init(
		      &generator, &channel_500_over_11_baud,
		      TEST_AMPLITUDE) == FSK_OK &&
		      fsk_receiver_init(
			      &receiver, &channel_500_over_11_baud) ==
			      FSK_OK,
	      "continuous FSK tone endpoints did not initialize");
	for (size_t index = 0;
	     index < channel_500_over_11_baud.detector_window * 3;
	     ++index) {
		CHECK(fsk_tone_generator_sample(
			      &generator, 1, &sample) == FSK_OK &&
			      sample >= -TEST_AMPLITUDE &&
			      sample <= TEST_AMPLITUDE &&
			      fsk_receiver_detect(
				      &receiver, sample, &decision, &ready) ==
				      FSK_OK,
		      "continuous mark tone was rejected");
	}
	measurement = fsk_receiver_measure(&receiver);
	CHECK(ready && decision == 1 && measurement.ready &&
		      measurement.mark_power > measurement.space_power &&
		      measurement.dominance_q8 >= 180,
	      "continuous mark tone was not distinguished");
	for (size_t index = 0;
	     index < channel_500_over_11_baud.detector_window * 3;
	     ++index) {
		CHECK(fsk_tone_generator_sample(
			      &generator, 0, &sample) == FSK_OK &&
			      fsk_receiver_detect(
				      &receiver, sample, &decision, &ready) ==
				      FSK_OK,
		      "continuous space tone was rejected");
	}
	measurement = fsk_receiver_measure(&receiver);
	CHECK(ready && decision == 0 &&
		      measurement.space_power > measurement.mark_power &&
		      measurement.dominance_q8 >= 180,
	      "continuous space tone was not distinguished");
	generator.mark_phase_step = 0;
	CHECK(fsk_tone_generator_sample(
		      &generator, 1, &sample) ==
		      FSK_INVALID_ARGUMENT &&
		      fsk_tone_generator_init(
			      NULL, &channel_500_over_11_baud,
			      TEST_AMPLITUDE) ==
			      FSK_INVALID_ARGUMENT &&
		      fsk_receiver_detect(
			      NULL, sample, &decision, &ready) ==
			      FSK_INVALID_ARGUMENT,
	      "continuous FSK tone API accepted invalid state");
	return true;
}

static bool test_v21_channel_pairs(void)
{
	return check_payload(&v21_channel_1, false) &&
	       check_payload(&v21_channel_2, false) &&
	       check_payload(&v21_channel_1, true) &&
	       check_payload(&v21_channel_2, true);
}

static bool test_v21_profiles(void)
{
	CHECK(v21_channel_1.sample_rate ==
		      V21_SAMPLE_RATE &&
		      v21_channel_1.symbol_rate_numerator ==
			      V21_BIT_RATE &&
		      v21_channel_1.symbol_rate_denominator == 1 &&
		      v21_channel_1.mark_frequency_hz == 980 &&
		      v21_channel_1.space_frequency_hz == 1180 &&
		      v21_channel_2.sample_rate ==
			      V21_SAMPLE_RATE &&
		      v21_channel_2.symbol_rate_numerator ==
			      V21_BIT_RATE &&
		      v21_channel_2.symbol_rate_denominator == 1 &&
		      v21_channel_2.mark_frequency_hz == 1650 &&
		      v21_channel_2.space_frequency_hz == 1850,
	      "V.21 profiles do not match the channel definitions");
	return true;
}

static bool test_fixed_detector_arithmetic(void)
{
	struct fsk_receiver receiver;
	uint64_t coefficient_hash = UINT64_C(14695981039346656037);
	uint64_t stream_hash = UINT64_C(14695981039346656037);
	uint32_t random = UINT32_C(0x7a31c29d);

	CHECK(fsk_receiver_init(&receiver, &v21_channel_2) == FSK_OK,
	      "fixed FSK detector did not initialize");
	for (size_t tap = 0; tap < v21_channel_2.detector_window; ++tap) {
		const int16_t values[] = {
			receiver.mark.in_phase[tap],
			receiver.mark.quadrature[tap],
			receiver.space.in_phase[tap],
			receiver.space.quadrature[tap],
		};

		for (size_t index = 0; index < ARRAY_LENGTH(values); ++index)
			coefficient_hash = hash_u64(
				coefficient_hash, (uint16_t)values[index]);
	}
	for (size_t index = 0; index < 4096; ++index) {
		uint8_t bit = 0;
		bool ready = false;
		int16_t sample;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		sample = word_to_s16((uint16_t)(random >> 16));
		CHECK(fsk_receiver_sample(
			      &receiver, sample, &bit, &ready) == FSK_OK,
		      "fixed FSK detector rejected sample %zu", index);
		stream_hash = hash_u64(stream_hash, (uint16_t)sample);
		stream_hash = hash_u64(
			stream_hash, (uint64_t)receiver.mark_power);
		stream_hash = hash_u64(
			stream_hash, (uint64_t)receiver.space_power);
		stream_hash = hash_u64(
			stream_hash, (uint64_t)receiver.smoothed_metric);
		stream_hash = hash_byte(stream_hash, ready);
		stream_hash = hash_byte(stream_hash, bit);
	}
	CHECK(coefficient_hash == UINT64_C(0xeecab44adea3eb7f) &&
		      stream_hash == UINT64_C(0x63c7974c663f5b97) &&
		      receiver.emitted_bits == 121 &&
		      receiver.smoothed_metric == INT64_C(-48866622206990),
	      "fixed FSK detector arithmetic changed: %#llx/%#llx",
	      (unsigned long long)coefficient_hash,
	      (unsigned long long)stream_hash);
	return true;
}

static bool test_configurable_rate_and_tone_pair(void)
{
	return check_payload(&channel_1200_baud, false) &&
	       check_payload(&channel_75_baud, false);
}

static int64_t linear_projection_power(
	const int16_t *samples, const struct fsk_tone_projection *projection,
	size_t count)
{
	int64_t in_phase = 0;
	int64_t quadrature = 0;

	for (size_t tap = 0; tap < count; ++tap) {
		in_phase += (int64_t)floor(
			(double)samples[tap] * projection->in_phase[tap] / 256.0);
		quadrature += (int64_t)floor(
			(double)samples[tap] * projection->quadrature[tap] / 256.0);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool test_detector_ring_wrap(void)
{
	uint32_t random = UINT32_C(0x971a463b);

	for (size_t window = 3; window <= FSK_MAX_DETECTOR_WINDOW; ++window) {
		struct fsk_channel channel = v21_channel_1;
		struct fsk_receiver receiver;
		int16_t newest_first[FSK_MAX_DETECTOR_WINDOW] = { 0 };

		channel.detector_window = window;
		CHECK(fsk_receiver_init(&receiver, &channel) == FSK_OK,
		      "detector window %zu did not initialize", window);
		for (size_t index = 0; index < 3 * window; ++index) {
			int16_t sample = index % 3 == 0 ? INT16_MIN :
				index % 3 == 1 ? INT16_MAX :
				word_to_s16((uint16_t)(fixture_random(&random) >> 16));
			uint64_t line_power = 0;
			uint8_t bit;
			bool ready;

			memmove(newest_first + 1, newest_first,
				(window - 1) * sizeof(newest_first[0]));
			newest_first[0] = sample;
			CHECK(fsk_receiver_detect(&receiver, sample, &bit, &ready) ==
			      FSK_OK && ready == (index + 1 >= window) &&
			      receiver.history_index == (index + 1) % window,
			      "detector ring failed at window %zu sample %zu", window, index);
			if (!ready) {
				CHECK(receiver.mark_power == 0 && receiver.space_power == 0 &&
				      receiver.line_power == 0, "detector power changed during warmup");
				continue;
			}
			for (size_t tap = 0; tap < window; ++tap)
				line_power += (uint64_t)((int64_t)newest_first[tap] *
					newest_first[tap]);
			CHECK(receiver.mark_power == linear_projection_power(
				newest_first, &receiver.mark, window) &&
			      receiver.space_power == linear_projection_power(
				newest_first, &receiver.space, window) &&
			      receiver.line_power == line_power,
			      "detector projection changed at window %zu sample %zu",
			      window, index);
		}
	}
	return true;
}

static bool test_measurement_qualification(void)
{
	struct fsk_measurement measurement = {
		.mark_power = 2,
		.space_power = 1,
		.line_power = 1000,
		.dominance_q8 = 180,
		.ready = true,
	};

	CHECK(!fsk_measurement_tone_present(NULL, 1000, 180),
	      "null measurement qualified");
	CHECK(fsk_measurement_tone_present(&measurement, 1000, 180) &&
	      fsk_measurement_mark_present(&measurement, 1000, 180),
	      "threshold boundaries did not qualify");
	measurement.ready = false;
	CHECK(!fsk_measurement_tone_present(&measurement, 1000, 180),
	      "unready measurement qualified");
	measurement.ready = true;
	CHECK(!fsk_measurement_tone_present(&measurement, 1001, 180) &&
	      !fsk_measurement_tone_present(&measurement, 1000, 181),
	      "below-threshold measurement qualified");
	measurement.mark_power = measurement.space_power;
	CHECK(fsk_measurement_tone_present(&measurement, 1000, 180) &&
	      !fsk_measurement_mark_present(&measurement, 1000, 180),
	      "tone tie qualified as mark");
	return true;
}

static bool test_channel_matching(void)
{
	const struct fsk_channel original = channel_500_over_11_baud;
	struct fsk_channel changed[7];
	struct fsk_channel padded[2];
	struct fsk_channel saved;
	const struct fsk_channel zero = { 0 };

	CHECK(!fsk_channels_match(NULL, NULL) &&
	      !fsk_channels_match(&original, NULL) &&
	      !fsk_channels_match(NULL, &original), "missing FSK channel matched");
	CHECK(fsk_channels_match(&original, &original) &&
	      fsk_channels_match(&zero, &zero),
	      "FSK matching imposed a geometry validation policy");
	for (size_t i = 0; i < ARRAY_LENGTH(changed); ++i)
		changed[i] = original;
	changed[0].sample_rate++;
	changed[1].symbol_rate_numerator++;
	changed[2].symbol_rate_denominator++;
	changed[3].mark_frequency_hz++;
	changed[4].space_frequency_hz++;
	changed[5].detector_window++;
	changed[6].metric_smoothing_shift++;
	for (size_t i = 0; i < ARRAY_LENGTH(changed); ++i) {
		memcpy(&saved, &changed[i], sizeof(saved));
		CHECK(!fsk_channels_match(&original, &changed[i]) &&
		      !fsk_channels_match(&changed[i], &original),
		      "FSK field %zu was ignored", i);
		CHECK(memcmp(&saved, &changed[i], sizeof(saved)) == 0,
		      "FSK matching changed its input");
	}
	changed[0] = original;
	changed[0].symbol_rate_numerator *= 2;
	changed[0].symbol_rate_denominator *= 2;
	CHECK(!fsk_channels_match(&original, &changed[0]),
	      "FSK matching normalized configured symbol-rate units");
	for (size_t i = 0; i < ARRAY_LENGTH(padded); ++i) {
		memset(&padded[i], i ? 0xa5 : 0x5a, sizeof(padded[i]));
		padded[i].sample_rate = original.sample_rate;
		padded[i].symbol_rate_numerator = original.symbol_rate_numerator;
		padded[i].symbol_rate_denominator = original.symbol_rate_denominator;
		padded[i].mark_frequency_hz = original.mark_frequency_hz;
		padded[i].space_frequency_hz = original.space_frequency_hz;
		padded[i].detector_window = original.detector_window;
		padded[i].metric_smoothing_shift = original.metric_smoothing_shift;
	}
	CHECK(fsk_channels_match(&padded[0], &padded[1]),
	      "FSK matching compared struct padding");
	return true;
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
		{ "measurement qualification", test_measurement_qualification },
		{ "configured channel matching", test_channel_matching },
		{ "fixed detector arithmetic", test_fixed_detector_arithmetic },
		{ "detector ring wrap", test_detector_ring_wrap },
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
