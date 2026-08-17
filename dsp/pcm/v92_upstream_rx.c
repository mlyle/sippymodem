// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_rx.h"

#include "memory_region.h"

#include <stddef.h>

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

bool v92_upstream_rx_is_valid(const struct v92_upstream_rx *receiver)
{
	return receiver != NULL &&
	       v92_pcm_phase4_trn2u_line_handoff_is_valid(&receiver->line) &&
	       receiver->training_tail_remaining <=
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
}

bool v92_upstream_rx_has_failed(const struct v92_upstream_rx *receiver)
{
	return v92_upstream_rx_is_valid(receiver) && receiver->failed;
}

enum v92_upstream_rx_result v92_upstream_rx_init(
	struct v92_upstream_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_upstream_rx initialized;

	if (receiver == NULL ||
	    !v92_pcm_phase4_trn2u_line_handoff_is_valid(handoff))
		return V92_UPSTREAM_RX_INVALID_ARGUMENT;
	initialized = (struct v92_upstream_rx){
		.line = *handoff,
		.training_tail_remaining =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
	};
	if (!v92_upstream_rx_is_valid(&initialized))
		return V92_UPSTREAM_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_UPSTREAM_RX_OK;
}

static enum v92_upstream_rx_result fail(struct v92_upstream_rx *receiver)
{
	receiver->failed = true;
	return V92_UPSTREAM_RX_NUMERIC_FAILURE;
}

enum v92_upstream_rx_result v92_upstream_rx_sample(
	struct v92_upstream_rx *receiver, int16_t sample,
	struct v92_upstream_decision *decision)
{
	struct v92_upstream_rx next;
	struct v92_upstream_decision generated;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point input;
	struct qam_point target;
	bool ready;

	if (receiver == NULL || decision == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), decision, sizeof(*decision)))
		return V92_UPSTREAM_RX_INVALID_ARGUMENT;
	if (!v92_upstream_rx_is_valid(receiver))
		return V92_UPSTREAM_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_UPSTREAM_RX_FAILED;
	if (receiver->line.input_samples == UINT64_MAX)
		return V92_UPSTREAM_RX_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){
		.in_phase = next.line.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_observe(
		    &next.line.equalizer, &input, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready ||
	    v92_phase4_demodulator_next(
		    &next.line.demodulator, observation.output.in_phase,
		    &generated.group, &generated.nearest_level) !=
		    V92_PHASE4_MODULATOR_OK)
		return fail(receiver);
	if (next.training_tail_remaining != 0u) {
		size_t target_index =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY -
			next.training_tail_remaining;
		int16_t selected = quantize(
			next.line.law, generated.nearest_level);

		target = (struct qam_point){
			.in_phase = next.line.pending_training_targets[target_index],
		};
		next.training_tail_remaining--;
		if (selected == (int16_t)target.in_phase &&
		    qam_equalizer_adapt_observation(
			    &next.line.equalizer, &observation, &target,
			    next.line.adaptation) != QAM_EQUALIZER_OK)
			return fail(receiver);
	} else {
		target = (struct qam_point){
			.in_phase = quantize(
				next.line.law, generated.nearest_level),
		};
		if (qam_equalizer_adapt_observation(
			    &next.line.equalizer, &observation, &target,
			    next.line.adaptation) != QAM_EQUALIZER_OK)
			return fail(receiver);
	}
	generated.equalized_level = observation.output.in_phase;
	next.line.input_samples++;
	if (!v92_upstream_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	*decision = generated;
	return V92_UPSTREAM_RX_OK;
}

const char *v92_upstream_rx_result_name(enum v92_upstream_rx_result result)
{
	switch (result) {
	case V92_UPSTREAM_RX_OK:
		return "ok";
	case V92_UPSTREAM_RX_FAILED:
		return "failed";
	case V92_UPSTREAM_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_UPSTREAM_RX_INVALID_STATE:
		return "invalid state";
	case V92_UPSTREAM_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
