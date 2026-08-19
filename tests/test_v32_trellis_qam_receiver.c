// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>
#include <trellis_qam_receiver.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_4800_SYMBOLS = 64,
	TEST_KNOWN_SYMBOLS = 128,
	TEST_PAYLOAD_SYMBOLS = 4096,
	TEST_CHANNEL_TAPS = 4,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static struct qam_point multiply(
	struct qam_point left,
	struct qam_point right)
{
	return (struct qam_point){
		.in_phase = left.in_phase * right.in_phase -
			    left.quadrature * right.quadrature,
		.quadrature = left.in_phase * right.quadrature +
			      left.quadrature * right.in_phase,
	};
}

static struct qam_point apply_channel(
	struct qam_point history[TEST_CHANNEL_TAPS],
	struct qam_point transmitted,
	struct qam_point gain)
{
	static const struct qam_point taps[TEST_CHANNEL_TAPS] = {
		{ 1.0, 0.0 },
		{ 0.14, -0.09 },
		{ -0.06, 0.045 },
		{ 0.025, 0.015 },
	};
	struct qam_point distorted = { 0 };

	for (size_t index = TEST_CHANNEL_TAPS - 1; index != 0; --index)
		history[index] = history[index - 1];
	history[0] = transmitted;
	for (size_t index = 0; index < TEST_CHANNEL_TAPS; ++index) {
		struct qam_point contribution =
			multiply(taps[index], history[index]);

		distorted.in_phase += contribution.in_phase;
		distorted.quadrature += contribution.quadrature;
	}
	return multiply(gain, distorted);
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.001;
}

static bool encode_trellis_point(
	struct v32_scrambler *scrambler,
	struct v32_trellis_encoder *encoder, uint8_t input,
	struct qam_point *point)
{
	uint8_t scrambled;
	uint8_t signal_symbol;

	return v32_scramble_group(
		       scrambler, input, encoder->data_bits_per_symbol,
		       &scrambled) == V32_FRAMING_OK &&
	       v32_trellis_encode(
		       encoder, scrambled, &signal_symbol) ==
		       V32_TRELLIS_OK &&
	       v32_trellis_constellation_point(
		       encoder->data_bits_per_symbol, signal_symbol, point) ==
		       V32_CONSTELLATION_OK;
}

static bool check_transition(
	enum v32_transmitter_role role, uint8_t initial_quadrant,
	uint8_t data_bits)
{
	/* A residual 0.19 Hz carrier error at 2400 baud. */
	static const struct qam_point channel_drift = {
		0.999999875, 0.0005
	};
	struct v32_symbol_encoder framing_encoder;
	struct v32_symbol_decoder framing_decoder;
	struct v32_trellis_encoder trellis_encoder;
	struct v32_trellis_qam_receiver receiver;
	struct qam_point channel = { 0.76, -0.27 };
	struct qam_point channel_history[TEST_CHANNEL_TAPS] = { 0 };
	struct qam_point transmitted;
	struct qam_point received_reference = { 0 };
	uint8_t payload[TEST_PAYLOAD_SYMBOLS];
	uint32_t transmitter_history[TEST_PAYLOAD_SYMBOLS];
	uint32_t random = UINT32_C(0x32b15e00) +
			  (uint32_t)role * 97u + initial_quadrant * 11u +
			  data_bits;
	uint8_t previous_quadrant = initial_quadrant;
	uint8_t mask = (uint8_t)((UINT8_C(1) << data_bits) - 1u);
	unsigned int confirmed_symbols = 0;
	unsigned int delivered = 0;

	CHECK(v32_symbol_encoder_init(
		      &framing_encoder, role, initial_quadrant) ==
		      V32_FRAMING_OK &&
	      v32_symbol_decoder_init(
		      &framing_decoder, role, 0) ==
		      V32_FRAMING_OK,
	      "V.32bis %u-bit transition %u/%u did not initialize",
	      data_bits, role, initial_quadrant);
	for (size_t index = 0; index < TEST_4800_SYMBOLS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t output;
		uint8_t quadrant;
		uint8_t relative_quadrant;
		uint8_t turns;

		CHECK(v32_4800_symbol_encode(
			      &framing_encoder, input, &quadrant) ==
			      V32_FRAMING_OK,
		      "V.32bis prefix generation failed at %zu", index);
		turns = (uint8_t)((quadrant - previous_quadrant) & 3u);
		relative_quadrant =
			(uint8_t)((framing_decoder.phase_quadrant + turns) & 3u);
		CHECK(v32_4800_symbol_decode(
			      &framing_decoder, relative_quadrant, &output) ==
			      V32_FRAMING_OK &&
		      output == input &&
		      v32_4800_constellation_point(
			      quadrant, &transmitted) ==
			      V32_CONSTELLATION_OK,
		      "V.32bis prefix state diverged at %zu", index);
		channel = multiply(channel, channel_drift);
		received_reference =
			apply_channel(channel_history, transmitted, channel);
		previous_quadrant = quadrant;
	}
	CHECK(v32_trellis_encoder_init(
		      &trellis_encoder, data_bits,
		      V32_TRELLIS_INITIAL_DIFFERENTIAL_OUTPUT) ==
		      V32_TRELLIS_OK &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing_decoder, &received_reference,
		      framing_encoder.phase_quadrant,
		      V32_TRELLIS_INITIAL_DIFFERENTIAL_OUTPUT,
		      data_bits) ==
		      V32_TRELLIS_QAM_RECEIVER_OK,
	      "V.32bis %u-bit receiver %u/%u did not initialize", data_bits,
	      role, initial_quadrant);
	for (size_t index = 0; index < TEST_KNOWN_SYMBOLS; ++index) {
		struct qam_point received;
		bool confirmed;

		CHECK(encode_trellis_point(
			      &framing_encoder.scrambler, &trellis_encoder, mask,
			      &transmitted),
		      "V.32bis %u-bit training generation failed at %zu",
		      data_bits, index);
		channel = multiply(channel, channel_drift);
		received = apply_channel(channel_history, transmitted, channel);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(v32_trellis_qam_receiver_track_known(
			      &receiver, &received, mask, &confirmed) ==
			      V32_TRELLIS_QAM_RECEIVER_OK,
		      "V.32bis %u-bit training failed at %zu", data_bits,
		      index);
		if (index >= TEST_KNOWN_SYMBOLS / 2u)
			confirmed_symbols += confirmed;
	}
	CHECK(confirmed_symbols >= TEST_KNOWN_SYMBOLS / 4u,
	      "V.32bis %u-bit receiver confirmed only %u late training symbols",
	      data_bits, confirmed_symbols);
	for (size_t index = 0; index < TEST_PAYLOAD_SYMBOLS; ++index) {
		struct qam_point received;
		uint8_t output = UINT8_MAX;
		bool ready = true;

		payload[index] = (uint8_t)(fixture_random(&random) & mask);
		CHECK(encode_trellis_point(
			      &framing_encoder.scrambler, &trellis_encoder,
			      payload[index], &transmitted),
		      "V.32bis %u-bit payload generation failed at %zu",
		      data_bits, index);
		transmitter_history[index] = framing_encoder.scrambler.history;
		channel = multiply(channel, channel_drift);
		received = apply_channel(channel_history, transmitted, channel);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(v32_trellis_qam_receiver_decode(
			      &receiver, &received, &output, &ready) ==
			      V32_TRELLIS_QAM_RECEIVER_OK,
		      "V.32bis %u-bit payload receive failed at %zu", data_bits,
		      index);
		if (index + 1u < V32_TRELLIS_QAM_RECEIVER_DELAY) {
			CHECK(!ready && output == UINT8_MAX,
			      "V.32bis %u-bit receiver exposed delayed symbol %zu",
			      data_bits, index);
			continue;
		}
		{
			size_t expected =
				index + 1u -
				V32_TRELLIS_QAM_RECEIVER_DELAY;

			CHECK(ready && output == payload[expected] &&
			      receiver.descrambler.history ==
				      transmitter_history[expected],
			      "V.32bis %u-bit receiver %u/%u decoded %u instead of %u at %zu",
			      data_bits, role, initial_quadrant, output,
			      payload[expected], expected);
		}
		delivered++;
	}
	CHECK(delivered ==
		      TEST_PAYLOAD_SYMBOLS -
			      V32_TRELLIS_QAM_RECEIVER_DELAY + 1u &&
	      v32_trellis_qam_receiver_is_valid(&receiver),
	      "V.32bis %u-bit receiver delivered %u symbols", data_bits,
	      delivered);
	return true;
}

static bool test_generated_isi_transition(void)
{
	static const uint8_t widths[] = {
		V32_7200_DATA_BITS,
		V32_9600_TRELLIS_DATA_BITS,
		V32_12000_DATA_BITS,
		V32_14400_DATA_BITS,
	};

	for (enum v32_transmitter_role role =
		     V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		for (uint8_t quadrant = 0;
		     quadrant < QPSK_QUADRANT_COUNT; ++quadrant) {
			for (size_t width = 0; width < ARRAY_LENGTH(widths);
			     ++width) {
				if (!check_transition(role, quadrant, widths[width]))
					return false;
			}
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v32_symbol_decoder framing;
	struct v32_trellis_qam_receiver receiver = { 0 };
	struct v32_trellis_qam_receiver saved;
	struct qam_point point = { 0.8, -0.2 };
	struct qam_point zero = { 0 };
	struct qam_point invalid = { NAN, 0.0 };
	uint8_t output = UINT8_MAX;
	bool ready = true;
	bool confirmed = true;

	CHECK(v32_symbol_decoder_init(
		      &framing, V32_CALLING_TRANSMITTER, 0) ==
		      V32_FRAMING_OK &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &point, 0, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_OK,
	      "V.32bis receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(v32_trellis_qam_receiver_is_valid(&receiver) &&
	      !v32_trellis_qam_receiver_is_valid(NULL) &&
	      v32_trellis_qam_receiver_init(
		      NULL, &framing, &point, 0, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, NULL, &point, 0, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &zero, 0, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &invalid, 0, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &point, 4, 0,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &point, 0, 4,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &point, 0, 0, 2) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_init(
		      &receiver, &framing, &point, 0, 0, 7) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "V.32bis receiver initialization was not transactional");
	CHECK(v32_trellis_qam_receiver_track_known(
		      NULL, &point, 0, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, NULL, 0, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, &zero, 0, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, &invalid, 0, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, &point, 8, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, &point, 0, NULL) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      NULL, &point, &output, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      &receiver, NULL, &output, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      &receiver, &zero, &output, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      &receiver, &invalid, &output, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      &receiver, &point, NULL, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_qam_receiver_decode(
		      &receiver, &point, &output, NULL) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      output == UINT8_MAX && ready && confirmed,
	      "V.32bis receiver invalid input changed state or output");
	CHECK(v32_trellis_qam_receiver_decode(
		      &receiver, &point, &output, &ready) ==
		      V32_TRELLIS_QAM_RECEIVER_OK &&
	      v32_trellis_qam_receiver_track_known(
		      &receiver, &point, 0, &confirmed) ==
		      V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT,
	      "V.32bis receiver accepted training after payload handoff");
	receiver = saved;
	receiver.payload_symbols =
		V32_TRELLIS_QAM_TRACEBACK_SYMBOLS + 1u;
	CHECK(!v32_trellis_qam_receiver_is_valid(&receiver) &&
	      strcmp(v32_trellis_qam_receiver_result_name(
		     V32_TRELLIS_QAM_RECEIVER_OK),
		     "ok") == 0 &&
	      strcmp(v32_trellis_qam_receiver_result_name(
		     V32_TRELLIS_QAM_RECEIVER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v32_trellis_qam_receiver_result_name(
		     (enum v32_trellis_qam_receiver_result)99),
		     "unknown") == 0,
	      "V.32bis receiver accepted corrupt state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated ISI and rate transition",
		  test_generated_isi_transition },
		{ "transactional validation", test_validation },
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
