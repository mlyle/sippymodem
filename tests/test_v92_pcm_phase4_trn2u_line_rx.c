// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase4_trn2u_line_rx.h>
#include <v92_control_target.h>
#include <v92_upstream_rx.h>

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
	int8_t polarity, int16_t lu,
	struct v92_pcm_phase3_cpt_line_handoff *handoff,
	struct v92_pcm_phase4_trn2u *transmitter,
	struct line_channel *channel)
{
	struct qam_point reference = { .in_phase = lu };
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
		    transmitter, constellation, previous_sign, lu) !=
		    V92_PCM_PHASE4_TRN2U_OK)
		return false;
	for (size_t index = 0;
	     index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t raw;
		int16_t line;
		struct qam_point input;

		if (index == 0u) {
			raw = previous_sign != 0u ? -lu : lu;
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
		.signal_level = lu,
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
		      echo_gain_q15, polarity, TEST_LU, &handoff, &transmitter,
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

static bool check_control_symbol(
	enum g711_law law, enum v92_constellation_size constellation,
	int16_t lu, unsigned magnitude, unsigned sign)
{
	struct v92_phase4_demodulator d = { .constellation = constellation, .lu = lu };
	struct g711_interval interval;
	int16_t nominal, reachable[128];
	size_t count = 0u;

	if (v92_phase4_modulator_level(constellation, lu, magnitude, sign, &nominal) !=
	    V92_PHASE4_MODULATOR_OK)
		return true;
	for (unsigned other = 0u; other < (unsigned)constellation / 2u; ++other) {
		int16_t raw, actual;
		if (other != magnitude && v92_phase4_modulator_level(constellation,
				lu, other, sign, &raw) == V92_PHASE4_MODULATOR_OK && raw == nominal) {
			CHECK(v92_control_target(law, &d, nominal, 40000.0, &actual) &&
			      actual == quantize(law, nominal), "collapsed magnitude changed target");
			return true;
		}
	}
	CHECK(g711_quantization_interval(law, lu, &interval), "LU cell failed");
	/* Exhaust every integer LU, independent of the binary search. */
	for (int candidate = interval.lower > 0 ? interval.lower : 1;
	     candidate <= interval.upper; ++candidate) {
		int16_t raw;
		if (v92_phase4_modulator_level(constellation, candidate, magnitude,
				sign, &raw) != V92_PHASE4_MODULATOR_OK)
			continue;
		raw = quantize(law, raw);
		if (!count || reachable[count - 1u] != raw) {
			CHECK(count < ARRAY_LENGTH(reachable), "target list overflow");
			reachable[count++] = raw;
		}
	}
	CHECK(count != 0u, "empty control alphabet");
	for (size_t j = 0u; j < count; ++j) {
		double midpoint = j ? (reachable[j - 1u] + reachable[j]) / 2.0 : 0.0;
		double observations[] = { reachable[j], midpoint,
			midpoint - 0.25, midpoint + 0.25, -40000.0, 40000.0 };
		for (size_t k = 0u; k < ARRAY_LENGTH(observations); ++k) {
			double observed = observations[k];
			int16_t expected = quantize(law, nominal), actual;
			for (size_t n = 0u; n < count; ++n)
				if (fabs(observed - reachable[n]) < fabs(observed - expected))
					expected = reachable[n];
			CHECK(v92_control_target(law, &d, nominal, observed, &actual) &&
			      actual == expected, "control target law=%u LU=%d raw=%d obs=%.2f",
			      law, lu, nominal, observed);
		}
	}
	return true;
}

static bool test_control_targets(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (int input = 1; input <= INT16_MAX;) {
			struct g711_interval interval;
			CHECK(g711_quantization_interval(law, input, &interval), "LU cell failed");
			int levels[] = { input, quantize(law, input), interval.upper };
			input = interval.upper + 1;
			for (size_t i = 0; i < ARRAY_LENGTH(levels); ++i)
				for (unsigned points = 4u; points <= 8u; points += 4u)
					for (unsigned symbol = 0u; symbol < points; ++symbol)
						CHECK(check_control_symbol(law,
							      (enum v92_constellation_size)points, levels[i],
							      symbol / 2u, symbol % 2u),
						      "control symbol search changed");
		}
	}
	struct v92_phase4_demodulator d = { .constellation = V92_CONSTELLATION_4_POINT, .lu = 5471 };
	int16_t target = 123, nominal;
	CHECK(v92_phase4_modulator_level(d.constellation, d.lu, 0u, 0u, &nominal) ==
		      V92_PHASE4_MODULATOR_OK &&
	      !v92_control_target(G711_LAW_PCMU, NULL, nominal, 0.0, &target) &&
	      !v92_control_target(G711_LAW_PCMU, &d, nominal, NAN, &target) &&
	      !v92_control_target(G711_LAW_PCMU, &d, nominal, INFINITY, &target) &&
	      !v92_control_target((enum g711_law)99, &d, nominal, 0.0, &target) &&
	      !v92_control_target(G711_LAW_PCMU, &d, 0, 0.0, &target) &&
	      !v92_control_target(G711_LAW_PCMU, &d, nominal, 0.0, NULL) &&
	      target == 123, "target rejection changed");
	d.lu = 0;
	CHECK(!v92_control_target(G711_LAW_PCMU, &d, nominal, 0.0, &target) &&
	      target == 123, "invalid LU changed target");
	d.lu = 5471;
	d.constellation = (enum v92_constellation_size)99;
	CHECK(!v92_control_target(G711_LAW_PCMU, &d, nominal, 0.0, &target) &&
	      target == 123, "invalid constellation changed target");
	return true;
}

static bool test_quantized_lu_training(void)
{
	bool saw_residual = false;

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned points = 4u; points <= 8u; points += 4u) {
			enum v92_constellation_size constellation = (enum v92_constellation_size)points;
			struct v92_pcm_phase3_cpt_line_handoff handoff;
			struct v92_pcm_phase4_trn2u transmitter;
			struct v92_pcm_phase4_trn2u_line_rx receiver;
			struct v92_pcm_phase4_trn2u_line_handoff trained;
			struct v92_upstream_rx control;
			struct line_channel channel;
			int16_t lu = 5471;
			CHECK(make_handoff(law, constellation, 0u, 0.05, 0, -1, lu,
					&handoff, &transmitter, &channel), "quantized fixture failed");
			handoff.signal_level = quantize(law, lu);
			CHECK(v92_pcm_phase4_trn2u_line_rx_init(&receiver, &handoff, constellation) ==
				      V92_PCM_PHASE4_TRN2U_LINE_RX_OK, "quantized training failed");
			for (unsigned n = 0u; n < RECEIVED_TRAINING_SYMBOLS; ++n) {
				int16_t raw;
				CHECK(v92_pcm_phase4_trn2u_next(&transmitter, &raw) == V92_PCM_PHASE4_TRN2U_OK,
				      "quantized transmitter failed");
				enum v92_pcm_phase4_trn2u_line_rx_result result =
					v92_pcm_phase4_trn2u_line_rx_sample(&receiver, channel_sample(&channel, law, raw));
				CHECK(result == V92_PCM_PHASE4_TRN2U_LINE_RX_OK ||
				      result == V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED, "quantized receiver failed");
			}
			CHECK(receiver.symbol_errors == 0u && receiver.coding_errors == 0u &&
			      v92_pcm_phase4_trn2u_line_rx_handoff(&receiver, &trained) ==
				      V92_PCM_PHASE4_TRN2U_LINE_RX_OK &&
			      v92_upstream_rx_init(&control, &trained) == V92_UPSTREAM_RX_OK &&
			      memcmp(&control.line.equalizer, &receiver.equalizer, sizeof(receiver.equalizer)) == 0,
			      "quantized handoff changed decisions or FIR history");
			saw_residual |= receiver.squared_error_sum > 0.0;
			/* Keep the real pending tail, then exercise both known-symbol and
			 * decision-directed control adaptation with scrambled ones. */
			for (unsigned n = 0u; n < 1200u; ++n) {
				int16_t raw;
				struct v92_upstream_decision decision;
				uint8_t ones = points - 1u;
				CHECK(v92_pcm_phase4_trn2u_next(&transmitter, &raw) == V92_PCM_PHASE4_TRN2U_OK,
				      "control transmitter failed");
				int16_t line = channel_sample(&channel, law, raw);
				CHECK((n % 2u ? v92_upstream_rx_sample(&control, line, &decision) :
					v92_upstream_rx_sample_known(&control, line, ones, &decision)) ==
					      V92_UPSTREAM_RX_OK && decision.group == ones, "control coding changed");
			}
			for (size_t tap = 0u; tap < control.line.equalizer.tap_count; ++tap)
				CHECK(control.line.equalizer.coefficients[tap].in_phase ==
					      (tap == control.line.equalizer.decision_delay ? 1.0 : 0.0) &&
				      control.line.equalizer.coefficients[tap].quadrature == 0.0,
				      "quantized LU invented a channel tap: law=%u points=%u tap=%zu", law, points, tap);
		}
	}
	CHECK(saw_residual, "nominal quantization residual was hidden");
	return true;
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
		      0.0, 0, 1, TEST_LU, &handoff, &transmitter, &channel),
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
		{ "quantized control targets", test_control_targets },
		{ "quantized LU training", test_quantized_lu_training },
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
