// SPDX-License-Identifier: BSD-3-Clause

#include "v34_line_receiver.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

bool v34_line_receiver_is_valid(
	const struct v34_line_receiver *receiver)
{
	if (receiver == NULL || !receiver->initialized ||
	    !v34_receive_timing_is_valid(&receiver->timing) ||
	    !v34_adaptive_equalizer_is_valid(&receiver->equalizer))
		return false;
	if (!v34_receive_timing_is_acquired_owned(&receiver->timing))
		return receiver->equalizer.epoch == 0u &&
		       receiver->equalizer.inputs_since_observation == 0u &&
		       !receiver->equalizer.decision_pending;
	return true;
}

bool v34_line_receiver_matches_profile(
	const struct v34_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile)
{
	/* Full-field equality with the validated owned profile also validates
	 * the requested profile, including when it aliases the owned copy. */
	return v34_line_receiver_is_valid(receiver) &&
	       v34_phase3_profile_is_equal(
		       &receiver->timing.transmitting_profile, transmitting_profile);
}

enum v34_line_receiver_result v34_line_receiver_init(
	struct v34_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	int16_t initial_receive_gain, bool reanchor_at_s,
	enum v34_adaptive_equalizer_feedback_mode feedback_mode)
{
	struct v34_line_receiver initialized;

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    initial_receive_gain <= 0)
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (v34_receive_timing_init(
		    &initialized.timing, transmitting_profile,
		    initial_receive_gain, reanchor_at_s) !=
			    V34_RECEIVE_TIMING_OK ||
	    v34_adaptive_equalizer_init(&initialized.equalizer) !=
		    V34_ADAPTIVE_EQUALIZER_OK ||
	    v34_adaptive_equalizer_set_feedback_mode(
		    &initialized.equalizer, feedback_mode) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	initialized.initialized = true;
	if (!v34_line_receiver_is_valid(&initialized))
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_LINE_RECEIVER_OK;
}

enum v34_line_receiver_result v34_line_receiver_sample(
	struct v34_line_receiver *receiver, int16_t sample,
	struct v34_adaptive_equalizer_observation *observation)
{
	if (observation == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    observation, sizeof(*observation)))
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	if (!v34_line_receiver_is_valid(receiver))
		return V34_LINE_RECEIVER_INVALID_STATE;
	return v34_line_receiver_sample_owned(receiver, sample, observation);
}

enum v34_line_receiver_result v34_line_receiver_sample_owned(
	struct v34_line_receiver *receiver, int16_t sample,
	struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_receive_timing_samples recovered = { 0 };
	struct v34_adaptive_equalizer_observation generated;
	enum v34_receive_timing_result timing_result;
	bool was_acquired;

	/* A valid line can still have a semantic decision outstanding. */
	if (receiver->equalizer.decision_pending)
		return V34_LINE_RECEIVER_INVALID_STATE;
	was_acquired = v34_receive_timing_is_acquired_owned(&receiver->timing);
	timing_result = v34_receive_timing_sample_owned(
		&receiver->timing, sample, &recovered);
	if (timing_result != V34_RECEIVE_TIMING_OK &&
	    timing_result != V34_RECEIVE_TIMING_SAMPLES_READY)
		return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	/* Carrier confirmation owns the start of the recovered symbol stream.
	 * Do not train on samples produced while acquisition was provisional. */
	if (!was_acquired)
		return v34_receive_timing_is_acquired_owned(&receiver->timing) ?
			V34_LINE_RECEIVER_TIMING_ACQUIRED : V34_LINE_RECEIVER_OK;
	if (timing_result == V34_RECEIVE_TIMING_OK)
		return V34_LINE_RECEIVER_OK;
	for (uint8_t index = 0; index < recovered.count; ++index)
		if (v34_adaptive_equalizer_push(
			    &receiver->equalizer, recovered.sample[index]) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
			return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	if (!recovered.symbol_complete)
		return V34_LINE_RECEIVER_OK;
	if (v34_adaptive_equalizer_observe(
		    &receiver->equalizer, &generated) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	/* A fresh identity equalizer initially publishes its zero-filled delay
	 * line. Keep that physical prefix out of every semantic receiver. */
	if (receiver->equalizer.epoch <
	    ((unsigned int)receiver->equalizer.equalizer.tap_count -
	     receiver->equalizer.equalizer.identity_tap) /
		    V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION) {
		if (v34_adaptive_equalizer_discard(
			    &receiver->equalizer, &generated) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
			return V34_LINE_RECEIVER_NUMERIC_FAILURE;
		return V34_LINE_RECEIVER_OK;
	}
	if (!v34_line_receiver_is_valid(receiver))
		return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	receiver->latest_observation = generated;
	*observation = generated;
	return V34_LINE_RECEIVER_OBSERVATION_READY;
}

static enum v34_line_receiver_result change_receive_epoch(
	struct v34_line_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	enum v34_receive_action_kind action)
{
	struct v34_line_receiver next = *receiver;

	if (v34_adaptive_equalizer_discard(&next.equalizer, observation) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	if (action == V34_RECEIVE_BEGIN_TRACKING) {
		if (v34_receive_timing_freeze_gain(&next.timing) !=
			    V34_RECEIVE_TIMING_OK ||
		    v34_adaptive_equalizer_begin_tracking(&next.equalizer) !=
			    V34_ADAPTIVE_EQUALIZER_OK)
			return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	} else {
		struct v34_line_receiver restarted;
		const struct v34_equalizer *geometry = &next.equalizer.equalizer;

		if (v34_line_receiver_init(
			    &restarted, &next.timing.transmitting_profile,
			    next.timing.gain.gain, false, next.equalizer.feedback_mode) !=
			    V34_LINE_RECEIVER_OK ||
		    v34_equalizer_init_geometry(
			    &restarted.equalizer.equalizer, geometry->tap_count,
			    (geometry->tap_count - geometry->identity_tap) / 2u) !=
			    V34_EQUALIZER_OK)
			return V34_LINE_RECEIVER_NUMERIC_FAILURE;
		next = restarted;
	}
	if (!v34_line_receiver_is_valid(&next))
		return V34_LINE_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	return V34_LINE_RECEIVER_OK;
}

enum v34_line_receiver_result v34_line_receiver_resolve(
	struct v34_line_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_receive_action *action,
	uint16_t decoder_power_q15)
{
	const struct memory_region regions[] = {
		{ receiver, receiver == NULL ? 0 : sizeof(*receiver) },
		{ observation, observation == NULL ? 0 : sizeof(*observation) },
		{ action, action == NULL ? 0 : sizeof(*action) },
	};
	enum v34_adaptive_equalizer_result result;

	if (observation == NULL || action == NULL ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions) ||
	    !v34_receive_action_is_valid(action, observation, decoder_power_q15))
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	if (!v34_line_receiver_is_valid(receiver) ||
	    !receiver->equalizer.decision_pending)
		return V34_LINE_RECEIVER_INVALID_STATE;

	/* A line with a pending decision has acquired timing. Only epoch
	 * changes can modify both timing and equalizer and need a line snapshot.
	 * Ordinary resolution rejects before changing the equalizer. */
	switch (action->action) {
	case V34_RECEIVE_COMMIT:
		result = v34_adaptive_equalizer_commit(
			&receiver->equalizer, &action->equalizer);
		break;
	case V34_RECEIVE_DISCARD:
		result = v34_adaptive_equalizer_discard(
			&receiver->equalizer, observation);
		break;
	case V34_RECEIVE_COAST:
		result = v34_adaptive_equalizer_coast(
			&receiver->equalizer, observation);
		break;
	case V34_RECEIVE_BEGIN_TRACKING:
	case V34_RECEIVE_RESTART_PATH:
		return change_receive_epoch(receiver, observation, action->action);
	default:
		return V34_LINE_RECEIVER_INVALID_ARGUMENT;
	}
	return result == V34_ADAPTIVE_EQUALIZER_OK ?
		V34_LINE_RECEIVER_OK : V34_LINE_RECEIVER_NUMERIC_FAILURE;
}
