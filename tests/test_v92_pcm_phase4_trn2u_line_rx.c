// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase4_trn2u_line_rx.h>

#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 6000,
	CHANNEL_HISTORY = 64,
	ECHO_DELAY = 40,
	ECHO_GAIN_Q15 = 8192,
	EXTRA_ALIGNED_SYMBOLS = 12,
	RECEIVED_TRAINING_SYMBOLS =
		V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY +
		EXTRA_ALIGNED_SYMBOLS,
};

struct line_channel {
	int16_t history[CHANNEL_HISTORY];
	size_t write_index;
	int echo_gain_q15;
	int8_t polarity;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters make_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 24u);
	add_ucode(&parameters.transmit_constellations[0], 80u);
	return parameters;
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static int16_t clamp_sample(int32_t sample)
{
	if (sample < INT16_MIN)
		return INT16_MIN;
	if (sample > INT16_MAX)
		return INT16_MAX;
	return (int16_t)sample;
}

static int16_t channel_sample(
	struct line_channel *channel, enum g711_law law, int16_t raw)
{
	int16_t wire = quantize(law, raw);
	size_t delayed_index =
		(channel->write_index + CHANNEL_HISTORY - ECHO_DELAY) %
		CHANNEL_HISTORY;
	int32_t mixed = wire +
		((int32_t)channel->echo_gain_q15 *
		 channel->history[delayed_index]) /
			INT32_C(32768);

	channel->history[channel->write_index] = wire;
	channel->write_index =
		(channel->write_index + 1u) % CHANNEL_HISTORY;
	if (channel->polarity < 0)
		mixed = -mixed;
	return clamp_sample(mixed);
}

static bool make_handoff(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, double adaptation, int echo_gain_q15,
	int8_t polarity, struct v92_pcm_phase3_cpt_line_handoff *handoff,
	struct v92_pcm_phase4_trn2u *transmitter,
	struct line_channel *channel)
{
	struct qam_point reference = { .in_phase = TEST_LU };
	struct qam_equalizer equalizer;
	struct qam_point output = { 0 };
	bool ready = false;

	*channel = (struct line_channel){
		.echo_gain_q15 = echo_gain_q15,
		.polarity = polarity,
	};
	if (qam_equalizer_init_geometry(
		    &equalizer, &reference, &reference,
		    V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS,
		    V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY) !=
		    QAM_EQUALIZER_OK ||
	    v92_pcm_phase4_trn2u_init(
		    transmitter, constellation, previous_sign, TEST_LU) !=
		    V92_PCM_PHASE4_TRN2U_OK)
		return false;
	for (size_t index = 0;
	     index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t raw;
		int16_t line;
		struct qam_point input;

		if (index == 0u) {
			raw = previous_sign != 0u ? -TEST_LU : TEST_LU;
		} else if (v92_pcm_phase4_trn2u_next(
				   transmitter, &raw) !=
			   V92_PCM_PHASE4_TRN2U_OK) {
			return false;
		}
		line = channel_sample(channel, law, raw);
		input = (struct qam_point){
			.in_phase = polarity > 0 ? line : -(double)line,
		};
		if (qam_equalizer_push(
			    &equalizer, &input, &output, &ready) !=
		    QAM_EQUALIZER_OK)
			return false;
	}
	if (!ready || qam_equalizer_adapt(
			      &equalizer, &output, 0.0) != QAM_EQUALIZER_OK)
		return false;
	*handoff = (struct v92_pcm_phase3_cpt_line_handoff){
		.equalizer = equalizer,
		.parameters = make_parameters(law),
		.input_samples = UINT64_C(4096),
		.signal_level = TEST_LU,
		.adaptation = adaptation,
		.input_advance_symbols =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
		.previous_sign = previous_sign,
		.polarity = polarity,
	};
	return v92_pcm_phase3_cpt_line_handoff_is_valid(handoff) &&
	       transmitter->emitted_symbols ==
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
}

static bool run_line(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, double adaptation, int echo_gain_q15,
	int8_t polarity)
{
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_rx receiver;
	struct v92_phase4_modulator decoded_oracle;
	struct line_channel channel;
	double first_half_error = 0.0;
	uint64_t starting_samples;

	CHECK(make_handoff(
		      law, constellation, previous_sign, adaptation,
		      echo_gain_q15, polarity, &handoff, &transmitter,
		      &channel) &&
	      v92_pcm_phase4_trn2u_line_rx_init(
		      &receiver, &handoff, constellation) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
	      "TRN2u line fixture did not initialize");
	CHECK(v92_phase4_modulator_init(
		      &decoded_oracle, constellation, previous_sign,
		      TEST_LU) == V92_PHASE4_MODULATOR_OK,
	      "TRN2u coding oracle did not initialize");
	starting_samples = receiver.input_samples;
	CHECK(v92_pcm_phase4_trn2u_line_rx_is_valid(&receiver) &&
	      !v92_pcm_phase4_trn2u_line_rx_can_handoff(&receiver) &&
	      !v92_pcm_phase4_trn2u_line_rx_has_failed(&receiver) &&
	      receiver.equalizer.tap_count ==
		      V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS &&
	      receiver.equalizer.decision_delay ==
		      handoff.equalizer.decision_delay &&
	      receiver.reference.emitted_symbols ==
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
	      "TRN2u line boundary state changed");
	for (size_t tap = 0; tap < handoff.equalizer.tap_count; ++tap) {
		CHECK(memcmp(
			      &receiver.equalizer.coefficients[tap],
			      &handoff.equalizer.coefficients[tap],
			      sizeof(receiver.equalizer.coefficients[tap])) == 0 &&
		      memcmp(
			      &receiver.equalizer.history[tap],
			      &handoff.equalizer.history[tap],
			      sizeof(receiver.equalizer.history[tap])) == 0,
		      "TRN2u line changed warm tap %zu", tap);
	}
	for (size_t tap = handoff.equalizer.tap_count;
	     tap < receiver.equalizer.tap_count; ++tap) {
		CHECK(receiver.equalizer.coefficients[tap].in_phase == 0.0 &&
		      receiver.equalizer.coefficients[tap].quadrature == 0.0 &&
		      receiver.equalizer.history[tap].in_phase == 0.0 &&
		      receiver.equalizer.history[tap].quadrature == 0.0,
		      "TRN2u line exposed nonzero extension tap %zu", tap);
	}
	for (size_t symbol = 0;
	     symbol < RECEIVED_TRAINING_SYMBOLS;
	     ++symbol) {
		int16_t raw;
		int16_t decoded_raw;
		int16_t line;
		enum v92_pcm_phase4_trn2u_line_rx_result result;
		uint32_t trained = (uint32_t)symbol + 1u;
		uint32_t physical = trained +
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
		bool aligned =
			physical >= V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
			physical % V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u;

		CHECK(v92_pcm_phase4_trn2u_next(&transmitter, &raw) ==
			      V92_PCM_PHASE4_TRN2U_OK,
		      "TRN2u transmitter failed at %zu", symbol);
		line = channel_sample(&channel, law, raw);
		result = v92_pcm_phase4_trn2u_line_rx_sample(
			&receiver, line);
		CHECK(v92_phase4_modulator_next(
			      &decoded_oracle,
			      (uint8_t)((1u <<
				       v92_phase4_modulator_bits_per_symbol(
					       constellation)) - 1u),
			      &decoded_raw) == V92_PHASE4_MODULATOR_OK,
		      "TRN2u coding oracle failed at %zu", symbol);
		CHECK(echo_gain_q15 != 0 ||
		      (receiver.demodulator.descrambler.history ==
			       decoded_oracle.scrambler.history &&
		       receiver.demodulator.previous_encoded_sign ==
			       decoded_oracle.previous_encoded_sign),
		      "TRN2u coding continuity changed at %zu", symbol);
		CHECK(result ==
			      (aligned ?
				       V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED :
				       V92_PCM_PHASE4_TRN2U_LINE_RX_OK) &&
		      v92_pcm_phase4_trn2u_line_rx_can_handoff(
			      &receiver) == aligned,
		      "TRN2u line boundary changed at %zu: %s", symbol,
		      v92_pcm_phase4_trn2u_line_rx_result_name(result));
		if (trained ==
		    V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS / 2u)
			first_half_error = receiver.squared_error_sum;
	}
	CHECK(receiver.trained_symbols == RECEIVED_TRAINING_SYMBOLS &&
	      receiver.input_samples ==
		      starting_samples + receiver.trained_symbols &&
	      receiver.reference.emitted_symbols ==
		      V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS +
			      EXTRA_ALIGNED_SYMBOLS &&
	      receiver.expected_index ==
		      receiver.trained_symbols %
			      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      receiver.demodulator.descrambler.history ==
		      decoded_oracle.scrambler.history &&
	      receiver.demodulator.previous_encoded_sign ==
		      decoded_oracle.previous_encoded_sign &&
	      v92_pcm_phase4_trn2u_line_rx_can_handoff(&receiver),
	      "TRN2u line counters lost chronology");
	if (echo_gain_q15 == 0) {
		CHECK(receiver.symbol_errors == 0u &&
		      receiver.coding_errors == 0u &&
		      receiver.squared_error_sum == 0.0,
		      "exact TRN2u line accumulated %u/%u errors and %.0f energy",
		      receiver.symbol_errors, receiver.coding_errors,
		      receiver.squared_error_sum);
	} else {
		double second_half_error =
			receiver.squared_error_sum - first_half_error;

		CHECK(first_half_error > 0.0 && second_half_error >= 0.0 &&
		      second_half_error < first_half_error * 0.75 &&
		      receiver.symbol_errors < receiver.trained_symbols / 50u &&
		      receiver.coding_errors < receiver.trained_symbols / 25u &&
		      fabs(receiver.equalizer.coefficients[ECHO_DELAY + 10u]
				   .in_phase) > 0.01,
		      "extended TRN2u equalizer did not converge: %.0f then %.0f, "
		      "%u/%u decisions",
		      first_half_error, second_half_error, receiver.symbol_errors,
		      receiver.coding_errors);
	}
	return true;
}

static bool test_exact_lines(void)
{
	return run_line(
		       G711_LAW_PCMU, V92_CONSTELLATION_4_POINT,
		       0u, 0.0, 0, 1) &&
	       run_line(
		       G711_LAW_PCMA, V92_CONSTELLATION_8_POINT,
		       1u, 0.0, 0, -1);
}

static bool test_long_postcursor(void)
{
	return run_line(
		G711_LAW_PCMU, V92_CONSTELLATION_8_POINT,
		1u, 0.25, ECHO_GAIN_Q15, 1);
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase3_cpt_line_handoff invalid_handoff;
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_rx receiver;
	struct v92_pcm_phase4_trn2u_line_rx before;
	struct v92_pcm_phase4_trn2u_line_rx untouched;
	struct v92_pcm_phase4_trn2u_line_rx saved_untouched;
	struct v92_pcm_phase4_trn2u_line_handoff line_handoff;
	struct v92_pcm_phase4_trn2u_line_handoff saved_line_handoff;
	struct line_channel channel;
	int16_t raw;
	int16_t line;
	static const char *const names[] = {
		"ok", "aligned", "failed", "invalid argument",
		"invalid state", "numeric failure",
	};

	memset(&untouched, 0xa5, sizeof(untouched));
	saved_untouched = untouched;
	CHECK(make_handoff(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u,
		      0.0, 0, 1, &handoff, &transmitter, &channel),
	      "contract handoff did not initialize");
	invalid_handoff = handoff;
	invalid_handoff.previous_sign = 2u;
	CHECK(v92_pcm_phase4_trn2u_line_rx_init(
		      NULL, &handoff, V92_CONSTELLATION_4_POINT) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init(
		      &untouched, NULL, V92_CONSTELLATION_4_POINT) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init(
		      &untouched, &invalid_handoff,
		      V92_CONSTELLATION_4_POINT) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init(
		      &untouched, &handoff,
		      (enum v92_constellation_size)6) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved_untouched, sizeof(untouched)) == 0,
	      "invalid TRN2u line initialization changed output");
	CHECK(v92_pcm_phase4_trn2u_line_rx_init(
		      &receiver, &handoff, V92_CONSTELLATION_4_POINT) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_OK &&
	      v92_pcm_phase4_trn2u_next(&transmitter, &raw) ==
		      V92_PCM_PHASE4_TRN2U_OK,
	      "contract receiver did not initialize");
	memset(&line_handoff, 0xa5, sizeof(line_handoff));
	saved_line_handoff = line_handoff;
	CHECK(v92_pcm_phase4_trn2u_line_rx_handoff(
		      NULL, &line_handoff) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_handoff(
		      &receiver, NULL) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_handoff(
		      &receiver, &line_handoff) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE &&
	      memcmp(
		      &line_handoff, &saved_line_handoff,
		      sizeof(line_handoff)) == 0,
	      "premature TRN2u line handoff changed output");
	line = channel_sample(&channel, G711_LAW_PCMU, raw);
	before = receiver;
	CHECK(v92_pcm_phase4_trn2u_line_rx_sample(NULL, line) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "null TRN2u line sample changed state");
	receiver.failed = true;
	before = receiver;
	CHECK(v92_pcm_phase4_trn2u_line_rx_has_failed(&receiver) &&
	      !v92_pcm_phase4_trn2u_line_rx_can_handoff(&receiver) &&
	      v92_pcm_phase4_trn2u_line_rx_handoff(
		      &receiver, &line_handoff) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED &&
	      v92_pcm_phase4_trn2u_line_rx_sample(&receiver, line) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(
		      &line_handoff, &saved_line_handoff,
		      sizeof(line_handoff)) == 0,
	      "failed TRN2u line receiver was not stable");
	receiver = before;
	receiver.failed = false;
	receiver.demodulator.previous_encoded_sign = 2u;
	before = receiver;
	CHECK(!v92_pcm_phase4_trn2u_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase4_trn2u_line_rx_sample(&receiver, line) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid TRN2u decoder state was not transactional");
	receiver.demodulator.previous_encoded_sign = 0u;
	receiver.trained_symbols =
		UINT32_MAX - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	receiver.reference.emitted_symbols = UINT32_MAX;
	receiver.input_samples = receiver.starting_input_samples +
		receiver.trained_symbols;
	before = receiver;
	CHECK(v92_pcm_phase4_trn2u_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase4_trn2u_line_rx_sample(&receiver, line) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "overflowing TRN2u line receiver was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_pcm_phase4_trn2u_line_rx_result_name(
				      (enum v92_pcm_phase4_trn2u_line_rx_result)
					      index),
			      names[index]) == 0,
		      "TRN2u line result name %zu changed", index);
	}
	CHECK(strcmp(
		      v92_pcm_phase4_trn2u_line_rx_result_name(
			      (enum v92_pcm_phase4_trn2u_line_rx_result)99),
		      "unknown") == 0 &&
	      !v92_pcm_phase4_trn2u_line_rx_is_valid(NULL) &&
	      !v92_pcm_phase4_trn2u_line_rx_can_handoff(NULL) &&
	      !v92_pcm_phase4_trn2u_line_rx_has_failed(NULL) &&
	      !v92_pcm_phase4_trn2u_line_handoff_is_valid(NULL),
	      "TRN2u line validation contracts changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact four- and eight-point lines", test_exact_lines },
		{ "long postcursor adaptation", test_long_postcursor },
		{ "contracts", test_contracts },
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
