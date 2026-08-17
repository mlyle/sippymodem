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

bool v92_upstream_rx_expected_level(
	const struct v92_upstream_rx *receiver, uint8_t input_group,
	int16_t *level)
{
	const struct v92_phase4_demodulator *demodulator;
	struct v92_phase4_modulator reference;
	unsigned int bits;
	int16_t generated;

	if (!v92_upstream_rx_is_valid(receiver) || receiver->failed ||
	    level == NULL)
		return false;
	demodulator = &receiver->line.demodulator;
	bits = v92_phase4_modulator_bits_per_symbol(
		demodulator->constellation);
	if (bits == 0u || input_group >= (1u << bits))
		return false;
	reference = (struct v92_phase4_modulator){
		.scrambler = demodulator->descrambler,
		.constellation = demodulator->constellation,
		.lu = demodulator->lu,
		.previous_encoded_sign = demodulator->previous_encoded_sign,
	};
	if (v92_phase4_modulator_next(
		    &reference, input_group, &generated) !=
	    V92_PHASE4_MODULATOR_OK)
		return false;
	*level = generated;
	return true;
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

static enum v92_upstream_rx_result sample(
	struct v92_upstream_rx *receiver, int16_t input_sample,
	bool known, uint8_t expected_group,
	struct v92_upstream_decision *decision)
{
	struct v92_upstream_rx next;
	struct v92_upstream_decision generated;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point input;
	struct qam_point target;
	int16_t expected_level = 0;
	unsigned int bits;
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
	bits = v92_phase4_modulator_bits_per_symbol(
		receiver->line.demodulator.constellation);
	if (known && (bits == 0u || expected_group >= (1u << bits)))
		return V92_UPSTREAM_RX_INVALID_ARGUMENT;
	if (known && receiver->training_tail_remaining == 0u &&
	    !v92_upstream_rx_expected_level(
		    receiver, expected_group, &expected_level))
		return V92_UPSTREAM_RX_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){
		.in_phase = next.line.polarity > 0 ?
			input_sample : -(double)input_sample,
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
	} else if (known) {
		target = (struct qam_point){
			.in_phase = quantize(next.line.law, expected_level),
		};
		if (qam_equalizer_adapt_observation(
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

enum v92_upstream_rx_result v92_upstream_rx_sample(
	struct v92_upstream_rx *receiver, int16_t input_sample,
	struct v92_upstream_decision *decision)
{
	return sample(receiver, input_sample, false, 0u, decision);
}

enum v92_upstream_rx_result v92_upstream_rx_sample_known(
	struct v92_upstream_rx *receiver, int16_t input_sample,
	uint8_t expected_group, struct v92_upstream_decision *decision)
{
	return sample(
		receiver, input_sample, true, expected_group, decision);
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
