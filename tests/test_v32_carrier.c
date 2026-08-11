// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <qam.h>
#include <constellation.h>
#include <framing.h>

#include "test.h"

#include <float.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_SYMBOL_RATE = 2400,
	TEST_CARRIER_HZ = 1800,
	TEST_AMPLITUDE = 12000,
	TEST_TRAINING_DIBITS = 384,
	TEST_SYNC_DIBITS = 32,
	TEST_SYNC_TAIL_DIBITS = 16,
	TEST_PAYLOAD_DIBITS = 192,
	TEST_TRAILING_DIBITS = 64,
	TEST_SOURCE_DIBITS = TEST_TRAINING_DIBITS + TEST_SYNC_DIBITS +
			      TEST_PAYLOAD_DIBITS + TEST_TRAILING_DIBITS,
	TEST_DECODED_CAPACITY = TEST_SOURCE_DIBITS + 128,
};

_Static_assert(
	(TEST_SYNC_DIBITS - TEST_SYNC_TAIL_DIBITS) * 2 >=
		SIPPYMODEM_V32_SCRAMBLER_BITS,
	"sync prefix must replace the descrambler history");

enum test_codec {
	TEST_LINEAR_PCM,
	TEST_PCMU,
	TEST_PCMA,
};

static struct sippymodem_qam_channel v32_channel(uint32_t carrier_hz)
{
	return (struct sippymodem_qam_channel){
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = TEST_SYMBOL_RATE,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = carrier_hz,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.25,
		.pulse_span_symbols = 6,
	};
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static void build_source(uint8_t source[TEST_SOURCE_DIBITS])
{
	uint32_t random = UINT32_C(0x480032a1);

	for (size_t index = 0; index < TEST_SOURCE_DIBITS; ++index) {
		bool mark = index < TEST_TRAINING_DIBITS ||
			    index >= TEST_TRAINING_DIBITS + TEST_SYNC_DIBITS +
					     TEST_PAYLOAD_DIBITS;

		source[index] = mark ? 3u :
				       (uint8_t)(fixture_random(&random) >> 30);
	}
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

static size_t longest_mark_run(const uint8_t *dibits, size_t count)
{
	size_t longest = 0;
	size_t current = 0;

	for (size_t index = 0; index < count; ++index) {
		current = dibits[index] == 3 ? current + 1u : 0;
		if (current > longest)
			longest = current;
	}
	return longest;
}

static int16_t transcode_sample(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_LINEAR_PCM:
		return sample;
	case TEST_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	case TEST_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	}
	return 0;
}

static bool check_carrier(
	enum test_codec codec, uint32_t transmit_carrier_hz,
	size_t raw_silence, size_t media_silence,
	enum sippymodem_v32_transmitter_role transmitter_role)
{
	struct sippymodem_qam_channel transmit_channel =
		v32_channel(transmit_carrier_hz);
	struct sippymodem_qam_channel receive_channel =
		v32_channel(TEST_CARRIER_HZ);
	struct sippymodem_qam_modulator modulator;
	struct sippymodem_qam_demodulator demodulator;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_qam_measurement measurement;
	struct sippymodem_qam_point transmit_point;
	struct sippymodem_qam_point previous_receive_point = { 0 };
	uint8_t source[TEST_SOURCE_DIBITS];
	uint8_t decoded[TEST_DECODED_CAPACITY];
	uint8_t relative_quadrant = 0;
	size_t transmitted = 0;
	size_t decoded_count = 0;
	size_t sync_offset;
	size_t leading_silence = raw_silence + media_silence;
	bool has_previous_receive_point = false;
	size_t sample_count = leading_silence +
		((TEST_SOURCE_DIBITS + 48u) * TEST_SAMPLE_RATE +
		 TEST_SYMBOL_RATE - 1u) /
			TEST_SYMBOL_RATE;

	build_source(source);
	CHECK(sippymodem_qam_modulator_init(
		      &modulator, &transmit_channel, TEST_AMPLITUDE) ==
		      SIPPYMODEM_QAM_OK &&
		      sippymodem_qam_demodulator_init(
			      &demodulator, &receive_channel) ==
			      SIPPYMODEM_QAM_OK &&
		      sippymodem_v32_symbol_encoder_init(
			      &encoder, transmitter_role, 0) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_symbol_decoder_init(
			      &decoder, transmitter_role, 0) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_4800_constellation_point(
			      0, &transmit_point) ==
			      SIPPYMODEM_V32_CONSTELLATION_OK,
	      "V.32 carrier endpoints did not initialize");
	for (size_t sample_index = 0; sample_index < sample_count;
	     ++sample_index) {
		struct sippymodem_qam_point receive_point;
		int16_t sample = 0;
		bool ready;

		if (sample_index >= leading_silence) {
			bool can_accept =
				sippymodem_qam_modulator_can_accept_symbol(
					&modulator);
			bool accepted;

			if (can_accept) {
				uint8_t input = transmitted < TEST_SOURCE_DIBITS ?
							source[transmitted] : 3u;
				uint8_t quadrant;
				enum sippymodem_v32_framing_result framing_result;

				if (transmitted < TEST_TRAINING_DIBITS)
					framing_result =
						sippymodem_v32_trn_symbol_encode(
							&encoder, input, &quadrant);
				else
					framing_result =
						sippymodem_v32_4800_symbol_encode(
						&encoder, input, &quadrant);
				CHECK(framing_result == SIPPYMODEM_V32_FRAMING_OK &&
					      sippymodem_v32_4800_constellation_point(
						      quadrant, &transmit_point) ==
						      SIPPYMODEM_V32_CONSTELLATION_OK,
				      "V.32 carrier could not encode symbol %zu",
				      transmitted);
			}
			CHECK(sippymodem_qam_modulator_sample(
				      &modulator, &transmit_point, &sample,
				      &accepted) == SIPPYMODEM_QAM_OK &&
				      accepted == can_accept,
			      "V.32 carrier failed at sample %zu", sample_index);
			if (accepted)
				transmitted++;
		}
		if (sample_index >= raw_silence)
			sample = transcode_sample(sample, codec);
		CHECK(sippymodem_qam_demodulator_sample(
			      &demodulator, sample, &receive_point, &ready) ==
			      SIPPYMODEM_QAM_OK,
		      "V.32 recovery failed at sample %zu", sample_index);
		if (ready && has_previous_receive_point) {
			uint8_t output;
			uint8_t quarter_turns;

			CHECK(sippymodem_qam_nearest_quarter_turn(
				      &previous_receive_point, &receive_point,
				      &quarter_turns) == SIPPYMODEM_QAM_OK,
			      "V.32 carrier phase decision failed");
			relative_quadrant =
				(uint8_t)((relative_quadrant + quarter_turns) & 3u);
			CHECK(decoded_count < ARRAY_LENGTH(decoded) &&
			      sippymodem_v32_4800_symbol_decode(
				      &decoder, relative_quadrant, &output) ==
				      SIPPYMODEM_V32_FRAMING_OK,
			      "V.32 carrier produced an invalid symbol "
			      "(codec %d, carrier %u, silence %zu+%zu, role %d, "
			      "decoded %zu)",
			      codec, transmit_carrier_hz, raw_silence,
			      media_silence,
			      transmitter_role, decoded_count);
			decoded[decoded_count++] = output;
		}
		if (ready) {
			previous_receive_point = receive_point;
			has_previous_receive_point = true;
		}
	}
	CHECK(transmitted >= TEST_SOURCE_DIBITS,
	      "V.32 carrier did not consume its source");
	CHECK(longest_mark_run(decoded, decoded_count) >= TEST_TRAILING_DIBITS,
	      "V.32 carrier recovered only %zu consecutive mark dibits",
	      longest_mark_run(decoded, decoded_count));
	sync_offset = find_pattern(
		decoded, decoded_count,
		source + TEST_TRAINING_DIBITS + TEST_SYNC_DIBITS -
			TEST_SYNC_TAIL_DIBITS,
		TEST_SYNC_TAIL_DIBITS);
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + TEST_SYNC_TAIL_DIBITS +
			      TEST_PAYLOAD_DIBITS <=
			      decoded_count &&
		      memcmp(decoded + sync_offset + TEST_SYNC_TAIL_DIBITS,
			     source + TEST_TRAINING_DIBITS + TEST_SYNC_DIBITS,
			     TEST_PAYLOAD_DIBITS) == 0,
	      "V.32 carrier changed its recovered payload");
	measurement = sippymodem_qam_demodulator_measure(&demodulator);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "V.32 carrier did not retain timing and power measurements");
	return true;
}

static bool test_phase_quantizer_validation(void)
{
	struct sippymodem_qam_point axes[] = {
		{ 1.0, 0.0 },
		{ 0.0, 1.0 },
		{ -1.0, 0.0 },
		{ 0.0, -1.0 },
	};
	struct sippymodem_qam_point huge = { DBL_MAX, DBL_MAX };
	uint8_t quarter_turns = UINT8_MAX;

	for (uint8_t expected = 0; expected < ARRAY_LENGTH(axes); ++expected)
		CHECK(sippymodem_qam_nearest_quarter_turn(
			      &axes[0], &axes[expected], &quarter_turns) ==
			      SIPPYMODEM_QAM_OK &&
			      quarter_turns == expected,
		      "QAM phase quantizer changed turn %u", expected);
	quarter_turns = UINT8_MAX;
	CHECK(sippymodem_qam_nearest_quarter_turn(
		      NULL, &axes[0], &quarter_turns) ==
		      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      sippymodem_qam_nearest_quarter_turn(
			      &huge, &huge, &quarter_turns) ==
			      SIPPYMODEM_QAM_INVALID_ARGUMENT &&
		      quarter_turns == UINT8_MAX,
	      "QAM phase quantizer accepted invalid points or changed output");
	return true;
}

static bool test_linear_carrier(void)
{
	return check_carrier(
		TEST_LINEAR_PCM, TEST_CARRIER_HZ, 0, 0,
		SIPPYMODEM_V32_CALLING_TRANSMITTER);
}

static bool test_pcmu_carrier(void)
{
	return check_carrier(
		TEST_PCMU, TEST_CARRIER_HZ + 7u, 0, 13,
		SIPPYMODEM_V32_CALLING_TRANSMITTER);
}

static bool test_pcma_carrier(void)
{
	return check_carrier(
		TEST_PCMA, TEST_CARRIER_HZ - 7u, 0, 26,
		SIPPYMODEM_V32_ANSWERING_TRANSMITTER) &&
	       check_carrier(
		       TEST_PCMA, TEST_CARRIER_HZ, 160, 160,
		       SIPPYMODEM_V32_ANSWERING_TRANSMITTER) &&
	       check_carrier(
		       TEST_PCMA, TEST_CARRIER_HZ, 0, 0,
		       SIPPYMODEM_V32_CALLING_TRANSMITTER);
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "linear carrier", test_linear_carrier },
		{ "PCMU carrier", test_pcmu_carrier },
		{ "PCMA carrier", test_pcma_carrier },
		{ "phase quantizer validation", test_phase_quantizer_validation },
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
