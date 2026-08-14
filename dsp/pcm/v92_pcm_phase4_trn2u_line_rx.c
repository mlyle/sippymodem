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
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	int16_t target)
{
	for (size_t index = 0; index < receiver->level_count; ++index) {
		if (receiver->levels[index] == target)
			return true;
	}
	return false;
}

static bool levels_are_valid(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	unsigned int expected_count =
		(unsigned int)receiver->reference.modulator.constellation;
	unsigned int magnitudes = expected_count / 2u;

	if (receiver->level_count != expected_count ||
	    receiver->level_count > V92_PCM_PHASE4_TRN2U_LINE_MAX_LEVELS)
		return false;
	for (size_t index = 0; index < magnitudes; ++index) {
		if (receiver->levels[index] <= 0 ||
		    receiver->levels[magnitudes + index] >= 0 ||
		    (index != 0u &&
		     (receiver->levels[index] <= receiver->levels[index - 1u] ||
		      receiver->levels[magnitudes + index] >=
			      receiver->levels[magnitudes + index - 1u])))
			return false;
	}
	return true;
}

static bool targets_are_valid(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	for (size_t index = 0;
	     index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		if (!target_is_member(receiver, receiver->expected[index]))
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
	       receiver->trained_symbols <=
		       UINT32_MAX -
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       receiver->reference.emitted_symbols ==
		       receiver->trained_symbols +
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       receiver->symbol_errors <= receiver->trained_symbols &&
	       receiver->starting_input_samples <= receiver->input_samples &&
	       receiver->input_samples - receiver->starting_input_samples ==
		       receiver->trained_symbols &&
	       isfinite(receiver->squared_error_sum) &&
	       receiver->squared_error_sum >= 0.0 &&
	       isfinite(receiver->adaptation) && receiver->adaptation >= 0.0 &&
	       receiver->adaptation <= 1.0 &&
	       g711_law_is_valid(receiver->law) &&
	       receiver->expected_index <
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       (receiver->polarity == -1 || receiver->polarity == 1) &&
	       levels_are_valid(receiver) &&
	       targets_are_valid(receiver);
}

bool v92_pcm_phase4_trn2u_line_rx_can_handoff(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	return v92_pcm_phase4_trn2u_line_rx_is_valid(receiver) &&
	       !receiver->failed &&
	       receiver->trained_symbols >=
		       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	       receiver->trained_symbols %
		       V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u;
}

bool v92_pcm_phase4_trn2u_line_rx_has_failed(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	return v92_pcm_phase4_trn2u_line_rx_is_valid(receiver) &&
	       receiver->failed;
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

static bool initialize_levels(
	struct v92_pcm_phase4_trn2u_line_rx *receiver)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		receiver->reference.modulator.constellation);
	unsigned int magnitudes = 1u << (bits - 1u);
	size_t index = 0;

	for (uint8_t sign = 0; sign <= 1u; ++sign) {
		for (uint8_t magnitude = 0;
		     magnitude < magnitudes; ++magnitude) {
			int16_t level;

			if (v92_phase4_modulator_level(
				    receiver->reference.modulator.constellation,
				    receiver->reference.modulator.lu,
				    magnitude, sign, &level) !=
			    V92_PHASE4_MODULATOR_OK)
				return false;
			receiver->levels[index++] = quantize(receiver->law, level);
		}
	}
	receiver->level_count = (uint8_t)index;
	return levels_are_valid(receiver);
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
	    !initialize_levels(&initialized) ||
	    !initialize_targets(&initialized) ||
	    !v92_pcm_phase4_trn2u_line_rx_is_valid(&initialized))
		return V92_PCM_PHASE4_TRN2U_LINE_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PCM_PHASE4_TRN2U_LINE_RX_OK;
}

static bool nearest_target(
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	double value, int16_t *nearest)
{
	double best_error = INFINITY;
	int16_t best = 0;

	if (!isfinite(value) || nearest == NULL || receiver->level_count == 0)
		return false;
	for (size_t index = 0; index < receiver->level_count; ++index) {
		double error = fabs(value - receiver->levels[index]);

		if (error < best_error) {
			best_error = error;
			best = receiver->levels[index];
		}
	}
	*nearest = best;
	return true;
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
	int16_t next_expected;
	int16_t selected;
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
	    !ready ||
	    !nearest_target(&next, observation.output.in_phase, &selected))
		return fail(receiver);
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
