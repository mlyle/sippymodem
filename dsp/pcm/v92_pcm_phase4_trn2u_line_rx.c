// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase4_trn2u_line_rx.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert((int)V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS <=
		       (int)QAM_EQUALIZER_MAX_TAP_COUNT,
	       "TRN2u equalizer must fit shared storage");
_Static_assert(V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY > 0,
	       "TRN2u target queue must not be empty");

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool target_is_member(
	const struct v92_phase4_demodulator *demodulator,
	enum g711_law law, int16_t target)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		demodulator->constellation);
	unsigned int magnitudes = bits == 0u ? 0u : 1u << (bits - 1u);

	for (uint8_t sign = 0u; sign <= 1u; ++sign) {
		for (unsigned int magnitude = 0u;
		     magnitude < magnitudes; ++magnitude) {
			int16_t level;

			if (v92_phase4_modulator_level(
				    demodulator->constellation,
				    demodulator->lu,
				    (uint8_t)magnitude, sign, &level) !=
			    V92_PHASE4_MODULATOR_OK)
				return false;
			if (quantize(law, level) == target)
				return true;
		}
	}
	return false;
}

static bool targets_are_valid(
	const struct v92_phase4_demodulator *demodulator,
	enum g711_law law, const int16_t *targets)
{
	for (size_t index = 0;
	     index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		if (!target_is_member(demodulator, law, targets[index]))
			return false;
	}
	return true;
}

bool v92_pcm_phase4_trn2u_line_rx_is_valid(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	return receiver != NULL &&
	       qam_equalizer_is_valid(&receiver->equalizer) &&
	       receiver->equalizer.tap_count ==
		       V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS &&
	       receiver->equalizer.decision_delay ==
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       receiver->equalizer.fill ==
		       receiver->equalizer.decision_delay + 1u &&
	       !receiver->equalizer.can_adapt &&
	       v92_pcm_phase4_trn2u_is_valid(&receiver->reference) &&
	       v92_phase4_demodulator_is_valid(&receiver->demodulator) &&
	       receiver->demodulator.constellation ==
		       receiver->reference.modulator.constellation &&
	       receiver->demodulator.lu == receiver->reference.modulator.lu &&
	       receiver->trained_symbols <=
		       UINT32_MAX -
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       receiver->reference.emitted_symbols ==
		       receiver->trained_symbols +
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       receiver->symbol_errors <= receiver->trained_symbols &&
	       receiver->coding_errors <= receiver->trained_symbols &&
	       receiver->starting_input_samples <= receiver->input_samples &&
	       receiver->input_samples - receiver->starting_input_samples ==
		       receiver->trained_symbols &&
	       isfinite(receiver->squared_error_sum) &&
	       receiver->squared_error_sum >= 0.0 &&
	       qam_adaptation_is_valid(receiver->adaptation) &&
	       g711_law_is_valid(receiver->law) &&
	       receiver->expected_index <
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       (receiver->polarity == -1 || receiver->polarity == 1) &&
	       targets_are_valid(
		       &receiver->demodulator, receiver->law,
		       receiver->expected);
}

bool v92_pcm_phase4_trn2u_line_rx_can_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	return v92_pcm_phase4_trn2u_line_rx_is_valid(receiver) &&
	       !receiver->failed &&
	       receiver->reference.emitted_symbols >=
		       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	       receiver->reference.emitted_symbols %
		       V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u;
}

bool v92_pcm_phase4_trn2u_line_rx_has_failed(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	return v92_pcm_phase4_trn2u_line_rx_is_valid(receiver) &&
	       receiver->failed;
}

bool v92_pcm_phase4_trn2u_line_handoff_is_valid(
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	return handoff != NULL &&
	       qam_equalizer_is_valid(&handoff->equalizer) &&
	       handoff->equalizer.tap_count ==
		       V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS &&
	       handoff->equalizer.decision_delay ==
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       handoff->equalizer.fill ==
		       handoff->equalizer.decision_delay + 1u &&
	       !handoff->equalizer.can_adapt &&
	       v92_phase4_demodulator_is_valid(&handoff->demodulator) &&
	       handoff->training_symbols >=
		       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	       handoff->training_symbols %
		       V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u &&
	       handoff->input_samples >= handoff->training_symbols &&
	       qam_adaptation_is_valid(handoff->adaptation) &&
	       g711_law_is_valid(handoff->law) &&
	       (handoff->polarity == -1 || handoff->polarity == 1) &&
	       targets_are_valid(
		       &handoff->demodulator, handoff->law,
		       handoff->pending_training_targets);
}

enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_pcm_phase4_trn2u_line_handoff generated;

	if (!v92_pcm_phase4_trn2u_line_rx_is_valid(receiver) ||
	    handoff == NULL)
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED;
	if (!v92_pcm_phase4_trn2u_line_rx_can_handoff(receiver))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE;
	generated = (struct v92_pcm_phase4_trn2u_line_handoff){
		.equalizer = receiver->equalizer,
		.demodulator = receiver->demodulator,
		.input_samples = receiver->input_samples,
		.training_symbols = receiver->reference.emitted_symbols,
		.adaptation = receiver->adaptation,
		.law = receiver->law,
		.polarity = receiver->polarity,
	};
	for (size_t index = 0;
	     index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		generated.pending_training_targets[index] = receiver->expected[
			(receiver->expected_index + index) %
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY];
	}
	if (!v92_pcm_phase4_trn2u_line_handoff_is_valid(&generated))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE;
	*handoff = generated;
	return V92_PCM_PHASE4_TRN2U_LINE_RX_OK;
}

static bool initialize_targets(
	struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	for (size_t index = 0;
	     index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t level;

		if (v92_pcm_phase4_trn2u_next(
			    &receiver->reference, &level) !=
		    V92_PCM_PHASE4_TRN2U_OK)
			return false;
		receiver->expected[index] = quantize(receiver->law, level);
	}
	return true;
}

enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_init(
	struct v92_pcm_phase4_trn2u_line_rx *receiver,
	const struct v92_pcm_phase3_cpt_line_handoff *handoff,
	enum v92_constellation_size constellation)
{
	struct v92_pcm_phase4_trn2u_line_rx initialized = { 0 };
	unsigned int bits;
	int16_t outer_level;
	long rounded_level;

	if (receiver == NULL ||
	    !v92_pcm_phase3_cpt_line_handoff_is_valid(handoff))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT;
	rounded_level = lround(handoff->signal_level);
	bits = v92_phase4_modulator_bits_per_symbol(constellation);
	if (rounded_level <= 0 || rounded_level > INT16_MAX || bits == 0 ||
	    v92_phase4_modulator_level(
		    constellation, (int16_t)rounded_level,
		    (uint8_t)((1u << (bits - 1u)) - 1u), 0u,
		    &outer_level) != V92_PHASE4_MODULATOR_OK)
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT;
	initialized = (struct v92_pcm_phase4_trn2u_line_rx){
		.equalizer = handoff->equalizer,
		.starting_input_samples = handoff->input_samples,
		.input_samples = handoff->input_samples,
		.adaptation = handoff->adaptation,
		.law = handoff->parameters.law,
		.polarity = handoff->polarity,
	};
	if (qam_equalizer_extend_span(
		    &initialized.equalizer,
		    V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS) !=
		    QAM_EQUALIZER_OK ||
	    v92_pcm_phase4_trn2u_init(
		    &initialized.reference, constellation,
		    handoff->previous_sign, (int16_t)rounded_level) !=
		    V92_PCM_PHASE4_TRN2U_OK ||
	    v92_phase4_demodulator_init(
		    &initialized.demodulator, constellation,
		    handoff->previous_sign, (int16_t)rounded_level) !=
		    V92_PHASE4_MODULATOR_OK ||
	    !initialize_targets(&initialized) ||
	    !v92_pcm_phase4_trn2u_line_rx_is_valid(&initialized))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PCM_PHASE4_TRN2U_LINE_RX_OK;
}

static enum v92_pcm_phase4_trn2u_line_rx_result fail(
	struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	receiver->failed = true;
	return V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE;
}

enum v92_pcm_phase4_trn2u_line_rx_result
v92_pcm_phase4_trn2u_line_rx_sample(
	struct v92_pcm_phase4_trn2u_line_rx *receiver, int16_t sample)
{
	struct v92_pcm_phase4_trn2u_line_rx next;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point input;
	struct qam_point target;
	double error;
	unsigned int bits;
	uint8_t decoded_group;
	uint8_t training_group;
	int16_t next_expected;
	int16_t selected;
	int16_t selected_level;
	int16_t raw_expected;
	bool ready;

	if (!v92_pcm_phase4_trn2u_line_rx_is_valid(receiver))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED;
	if (receiver->input_samples == UINT64_MAX ||
	    receiver->trained_symbols ==
		    UINT32_MAX - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY)
		return V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE;
	next = *receiver;
	if (v92_pcm_phase4_trn2u_next(
		    &next.reference, &raw_expected) !=
	    V92_PCM_PHASE4_TRN2U_OK)
		return fail(receiver);
	next_expected = next.expected[next.expected_index];
	next.expected[next.expected_index] = quantize(next.law, raw_expected);
	next.expected_index = (uint8_t)(
		(next.expected_index + 1u) %
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY);
	input = (struct qam_point){
		.in_phase = next.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_observe(
		    &next.equalizer, &input, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready || v92_phase4_demodulator_next(
			      &next.demodulator, observation.output.in_phase,
			      &decoded_group, &selected_level) !=
			      V92_PHASE4_MODULATOR_OK)
		return fail(receiver);
	selected = quantize(next.law, selected_level);
	bits = v92_phase4_modulator_bits_per_symbol(
		next.demodulator.constellation);
	if (bits == 0u)
		return fail(receiver);
	training_group = (uint8_t)((1u << bits) - 1u);
	target = (struct qam_point){ .in_phase = next_expected };
	error = observation.output.in_phase - target.in_phase;
	if (!isfinite(error) ||
	    !isfinite(next.squared_error_sum + error * error))
		return fail(receiver);
	if (selected == next_expected) {
		if (qam_equalizer_adapt_observation(
			    &next.equalizer, &observation, &target,
			    next.adaptation) != QAM_EQUALIZER_OK)
			return fail(receiver);
	} else {
		next.symbol_errors++;
	}
	if (decoded_group != training_group)
		next.coding_errors++;
	next.squared_error_sum += error * error;
	next.input_samples++;
	next.trained_symbols++;
	if (!v92_pcm_phase4_trn2u_line_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	return v92_pcm_phase4_trn2u_line_rx_can_handoff(receiver) ?
		       V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED :
		       V92_PCM_PHASE4_TRN2U_LINE_RX_OK;
}

const char *v92_pcm_phase4_trn2u_line_rx_result_name(
	enum v92_pcm_phase4_trn2u_line_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE4_TRN2U_LINE_RX_OK:
		return "ok";
	case V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED:
		return "aligned";
	case V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED:
		return "failed";
	case V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
