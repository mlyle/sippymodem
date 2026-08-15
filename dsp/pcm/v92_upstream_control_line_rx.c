// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_control_line_rx.h"

#include <stddef.h>
#include <stdint.h>

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool line_state_is_valid(
	const struct v92_upstream_control_line_rx *receiver)
{
	unsigned int expected_alignment =
		receiver->line.demodulator.constellation ==
				V92_CONSTELLATION_4_POINT ?
			V92_STATUS_UPDATE_4_POINT_ALIGNMENT :
			V92_STATUS_UPDATE_8_POINT_ALIGNMENT;

	return v92_pcm_phase4_trn2u_line_handoff_is_valid(&receiver->line) &&
	       v92_control_rx_is_valid(&receiver->control) &&
	       receiver->control.direction == V92_CONTROL_UPSTREAM &&
	       receiver->control.alignment_bits == expected_alignment &&
	       receiver->training_tail_remaining <=
		       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
}

bool v92_upstream_control_line_rx_is_valid(
	const struct v92_upstream_control_line_rx *receiver)
{
	return receiver != NULL && line_state_is_valid(receiver);
}

bool v92_upstream_control_line_rx_has_failed(
	const struct v92_upstream_control_line_rx *receiver)
{
	return v92_upstream_control_line_rx_is_valid(receiver) &&
	       receiver->failed;
}

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_init(
	struct v92_upstream_control_line_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_upstream_control_line_rx initialized = { 0 };

	if (receiver == NULL ||
	    !v92_pcm_phase4_trn2u_line_handoff_is_valid(handoff))
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT;
	initialized = (struct v92_upstream_control_line_rx){
		.line = *handoff,
		.training_tail_remaining =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
	};
	if (v92_control_rx_init_upstream(
		    &initialized.control,
		    initialized.line.demodulator.constellation) !=
		    V92_CONTROL_RX_OK ||
	    !v92_upstream_control_line_rx_is_valid(&initialized))
		return V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_UPSTREAM_CONTROL_LINE_RX_OK;
}

static enum v92_upstream_control_line_rx_result fail(
	struct v92_upstream_control_line_rx *receiver)
{
	receiver->failed = true;
	return V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR;
}

static bool receive_group(
	struct v92_control_rx *control, uint8_t group, unsigned int bits,
	struct v92_control_event *event)
{
	struct v92_control_event emitted = { 0 };

	for (unsigned int bit = 0u; bit < bits; ++bit) {
		struct v92_control_event current;

		if (v92_control_rx_receive_bit(
			    control, (uint8_t)((group >> bit) & 1u),
			    &current) != V92_CONTROL_RX_OK)
			return false;
		if (current.type == V92_CONTROL_EVENT_NONE)
			continue;
		if (emitted.type != V92_CONTROL_EVENT_NONE)
			return false;
		emitted = current;
	}
	*event = emitted;
	return true;
}

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_sample(
	struct v92_upstream_control_line_rx *receiver, int16_t sample,
	struct v92_control_event *event)
{
	struct v92_upstream_control_line_rx next;
	struct v92_control_event emitted = { 0 };
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point input;
	struct qam_point target;
	unsigned int bits;
	uint8_t group;
	int16_t nearest_level;
	bool ready;

	if (!v92_upstream_control_line_rx_is_valid(receiver) || event == NULL)
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_UPSTREAM_CONTROL_LINE_RX_FAILED;
	if (receiver->line.input_samples == UINT64_MAX)
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){
		.in_phase = next.line.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_observe(
		    &next.line.equalizer, &input, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready || v92_phase4_demodulator_next(
			      &next.line.demodulator,
			      observation.output.in_phase,
			      &group, &nearest_level) !=
			      V92_PHASE4_MODULATOR_OK)
		return fail(receiver);
	if (next.training_tail_remaining != 0u) {
		size_t target_index =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY -
			next.training_tail_remaining;
		int16_t selected = quantize(next.line.law, nearest_level);

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
			.in_phase = quantize(next.line.law, nearest_level),
		};
		if (qam_equalizer_adapt_observation(
			    &next.line.equalizer, &observation, &target,
			    next.line.adaptation) != QAM_EQUALIZER_OK)
			return fail(receiver);
	}
	bits = v92_phase4_modulator_bits_per_symbol(
		next.line.demodulator.constellation);
	if (bits == 0u || !receive_group(
				  &next.control, group, bits, &emitted))
		return fail(receiver);
	next.line.input_samples++;
	if (!v92_upstream_control_line_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	*event = emitted;
	return emitted.type == V92_CONTROL_EVENT_NONE ?
		       V92_UPSTREAM_CONTROL_LINE_RX_OK :
		       V92_UPSTREAM_CONTROL_LINE_RX_EVENT;
}

const char *v92_upstream_control_line_rx_result_name(
	enum v92_upstream_control_line_rx_result result)
{
	switch (result) {
	case V92_UPSTREAM_CONTROL_LINE_RX_OK:
		return "ok";
	case V92_UPSTREAM_CONTROL_LINE_RX_EVENT:
		return "event";
	case V92_UPSTREAM_CONTROL_LINE_RX_FAILED:
		return "failed";
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
