// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_9600_receiver.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_4800_SYMBOLS = 96,
	TEST_KNOWN_9600_SYMBOLS = 128,
	TEST_9600_SYMBOLS = 1024,
	TEST_CHANNEL_TAPS = 4,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static struct sippymodem_qam_point complex_multiply(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.004;
}

static struct sippymodem_qam_point apply_test_channel(
	struct sippymodem_qam_point history[TEST_CHANNEL_TAPS],
	const struct sippymodem_qam_point *transmitted,
	const struct sippymodem_qam_point *gain)
{
	static const struct sippymodem_qam_point taps[TEST_CHANNEL_TAPS] = {
		{ 1.0, 0.0 },
		{ 0.28, -0.18 },
		{ -0.12, 0.09 },
		{ 0.05, 0.03 },
	};
	struct sippymodem_qam_point distorted = { 0 };

	for (size_t index = TEST_CHANNEL_TAPS - 1; index != 0; --index)
		history[index] = history[index - 1];
	history[0] = *transmitted;
	for (size_t index = 0; index < TEST_CHANNEL_TAPS; ++index) {
		struct sippymodem_qam_point contribution = complex_multiply(
			&taps[index], &history[index]);

		distorted.in_phase += contribution.in_phase;
		distorted.quadrature += contribution.quadrature;
	}
	return complex_multiply(gain, &distorted);
}

static bool check_transition(
	enum sippymodem_v32_transmitter_role role,
	uint8_t initial_transmit_quadrant)
{
	static const struct sippymodem_qam_point drift = {
		.in_phase = 1.000002,
		.quadrature = 0.00004,
	};
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_v32_9600_receiver receiver;
	struct sippymodem_qam_point channel = { 0.72, -0.31 };
	struct sippymodem_qam_point channel_history[TEST_CHANNEL_TAPS] = { 0 };
	struct sippymodem_v32_symbol_encoder encoder_history[
		SIPPYMODEM_QAM_EQUALIZER_DELAY + 1] = { 0 };
	uint8_t input_history[SIPPYMODEM_QAM_EQUALIZER_DELAY + 1] = { 0 };
	struct sippymodem_qam_point transmitted;
	struct sippymodem_qam_point received_reference;
	uint32_t random = UINT32_C(0x32009600) +
			  (uint32_t)role * 4u + initial_transmit_quadrant;
	uint8_t previous_transmit_quadrant = initial_transmit_quadrant;
	unsigned int delivered = 0;

	CHECK(sippymodem_v32_symbol_encoder_init(
		      &encoder, role, initial_transmit_quadrant) ==
		      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_symbol_decoder_init(&decoder, role, 0) ==
		      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_4800_constellation_point(
			      initial_transmit_quadrant, &transmitted) ==
		      SIPPYMODEM_V32_CONSTELLATION_OK,
	      "V.32 9600 transition %u/%u did not initialize", role,
	      initial_transmit_quadrant);
	received_reference = complex_multiply(&channel, &transmitted);
	for (size_t index = 0; index < TEST_4800_SYMBOLS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t output;
		uint8_t transmit_quadrant;
		uint8_t relative_quadrant;
		uint8_t turns;

		CHECK(sippymodem_v32_4800_symbol_encode(
			      &encoder, input, &transmit_quadrant) ==
			      SIPPYMODEM_V32_FRAMING_OK,
		      "V.32 4800 prefix %u/%u failed at %zu", role,
		      initial_transmit_quadrant, index);
		turns = (uint8_t)((transmit_quadrant -
				   previous_transmit_quadrant) &
				  3u);
		relative_quadrant =
			(uint8_t)((decoder.phase_quadrant + turns) & 3u);
		CHECK(sippymodem_v32_4800_symbol_decode(
			      &decoder, relative_quadrant, &output) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
			      output == input &&
			      sippymodem_v32_4800_constellation_point(
				      transmit_quadrant, &transmitted) ==
				      SIPPYMODEM_V32_CONSTELLATION_OK,
		      "V.32 4800 prefix %u/%u changed symbol %zu", role,
		      initial_transmit_quadrant, index);
		channel = complex_multiply(&channel, &drift);
		received_reference = complex_multiply(&channel, &transmitted);
		previous_transmit_quadrant = transmit_quadrant;
	}
	CHECK(sippymodem_v32_9600_receiver_init(
		      &receiver, &decoder, &received_reference) ==
		      SIPPYMODEM_V32_9600_RECEIVER_OK,
	      "V.32 9600 receiver %u/%u did not initialize", role,
	      initial_transmit_quadrant);
	for (size_t index = 0; index < TEST_KNOWN_9600_SYMBOLS; ++index) {
		struct sippymodem_qam_point received;
		uint8_t symbol;

		CHECK(sippymodem_v32_9600_symbol_encode(
			      &encoder, 15, &symbol) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
			      sippymodem_v32_constellation_point(
				      symbol, &transmitted) ==
				      SIPPYMODEM_V32_CONSTELLATION_OK,
		      "V.32 9600 known fixture %u/%u failed at %zu", role,
		      initial_transmit_quadrant, index);
		channel = complex_multiply(&channel, &drift);
		received = apply_test_channel(
			channel_history, &transmitted, &channel);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(sippymodem_v32_9600_receiver_track_known(
			      &receiver, &received, 15) ==
			      SIPPYMODEM_V32_9600_RECEIVER_OK,
		      "V.32 9600 known tracking %u/%u failed at %zu", role,
		      initial_transmit_quadrant, index);
	}
	for (size_t index = 0; index < TEST_9600_SYMBOLS; ++index) {
		struct sippymodem_qam_point received;
		uint8_t input = (uint8_t)(fixture_random(&random) & 15u);
		uint8_t output;
		uint8_t symbol;
		bool ready;

		for (size_t delayed = SIPPYMODEM_QAM_EQUALIZER_DELAY;
		     delayed != 0; --delayed) {
			input_history[delayed] = input_history[delayed - 1];
			encoder_history[delayed] = encoder_history[delayed - 1];
		}
		input_history[0] = input;

		CHECK(sippymodem_v32_9600_symbol_encode(
			      &encoder, input, &symbol) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
			      sippymodem_v32_constellation_point(
				      symbol, &transmitted) ==
				      SIPPYMODEM_V32_CONSTELLATION_OK,
			      "V.32 9600 fixture %u/%u failed at %zu", role,
		      initial_transmit_quadrant, index);
		encoder_history[0] = encoder;
		channel = complex_multiply(&channel, &drift);
		received = apply_test_channel(
			channel_history, &transmitted, &channel);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(sippymodem_v32_9600_receiver_decode(
			      &receiver, &received, &output, &ready) ==
			      SIPPYMODEM_V32_9600_RECEIVER_OK,
		      "V.32 9600 receiver %u/%u changed symbol %zu", role,
		      initial_transmit_quadrant, index);
		if (index < SIPPYMODEM_QAM_EQUALIZER_DELAY) {
			CHECK(!ready,
			      "V.32 9600 receiver %u/%u exposed delayed symbol %zu",
			      role, initial_transmit_quadrant, index);
			continue;
		}
		CHECK(ready &&
		      output == input_history[SIPPYMODEM_QAM_EQUALIZER_DELAY] &&
		      receiver.decoder.descrambler.history ==
			      encoder_history[SIPPYMODEM_QAM_EQUALIZER_DELAY]
				      .scrambler.history &&
		      receiver.decoder.phase_quadrant ==
			      (uint8_t)((encoder_history[
						      SIPPYMODEM_QAM_EQUALIZER_DELAY]
					      .phase_quadrant -
					      initial_transmit_quadrant) &
				     3u),
		      "V.32 9600 receiver %u/%u changed delayed symbol %zu",
		      role, initial_transmit_quadrant, index);
		delivered++;
	}
	CHECK(delivered == TEST_9600_SYMBOLS - SIPPYMODEM_QAM_EQUALIZER_DELAY,
	      "V.32 9600 receiver %u/%u delivered %u symbols", role,
	      initial_transmit_quadrant, delivered);
	return true;
}

static bool test_transition_tracking(void)
{
	for (enum sippymodem_v32_transmitter_role role =
		     SIPPYMODEM_V32_CALLING_TRANSMITTER;
	     role <= SIPPYMODEM_V32_ANSWERING_TRANSMITTER; ++role) {
		for (uint8_t quadrant = 0;
		     quadrant < SIPPYMODEM_QPSK_QUADRANT_COUNT; ++quadrant)
			if (!check_transition(role, quadrant))
				return false;
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_v32_9600_receiver receiver = { 0 };
	struct sippymodem_v32_9600_receiver saved;
	struct sippymodem_qam_point reference = { 0.8, -0.2 };
	struct sippymodem_qam_point zero = { 0.0, 0.0 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };
	uint8_t output = UINT8_MAX;
	bool ready = true;
	bool saved_ready = ready;

	CHECK(sippymodem_v32_symbol_decoder_init(
		      &decoder, SIPPYMODEM_V32_CALLING_TRANSMITTER, 0) ==
		      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_9600_receiver_init(
			      &receiver, &decoder, &reference) ==
		      SIPPYMODEM_V32_9600_RECEIVER_OK,
	      "V.32 9600 validation fixture did not initialize");
	saved = receiver;
	CHECK(sippymodem_v32_9600_receiver_is_valid(&receiver) &&
		      !sippymodem_v32_9600_receiver_is_valid(NULL) &&
		      sippymodem_v32_9600_receiver_init(
			      NULL, &decoder, &reference) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_init(
			      &receiver, NULL, &reference) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_init(
			      &receiver, &decoder, &zero) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_init(
			      &receiver, &decoder, &invalid) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      sippymodem_v32_9600_receiver_decode(
			      NULL, &reference, &output, &ready) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, NULL, &output, &ready) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, &zero, &output, &ready) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, &invalid, &output, &ready) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, &reference, NULL, &ready) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, &reference, &output, NULL) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_track_known(
			      NULL, &reference, 15) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_track_known(
			      &receiver, NULL, 15) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_track_known(
			      &receiver, &zero, 15) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_track_known(
			      &receiver, &invalid, 15) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_9600_receiver_track_known(
			      &receiver, &reference,
			      SIPPYMODEM_V32_9600_SYMBOL_COUNT) ==
			      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      output == UINT8_MAX && ready == saved_ready,
	      "V.32 9600 invalid input was not transactional");
	receiver.equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(!sippymodem_v32_9600_receiver_is_valid(&receiver) &&
		      sippymodem_v32_9600_receiver_decode(
			      &receiver, &reference, &output, &ready) ==
		      SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT &&
		      output == UINT8_MAX &&
		      strcmp(sippymodem_v32_9600_receiver_result_name(
			     SIPPYMODEM_V32_9600_RECEIVER_OK),
			     "ok") == 0 &&
		      strcmp(sippymodem_v32_9600_receiver_result_name(
			     SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT),
			     "invalid argument") == 0 &&
		      strcmp(sippymodem_v32_9600_receiver_result_name(
			     (enum sippymodem_v32_9600_receiver_result)99),
			     "unknown") == 0,
	      "V.32 9600 receiver accepted corrupt state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "4800-to-9600 channel tracking", test_transition_tracking },
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
