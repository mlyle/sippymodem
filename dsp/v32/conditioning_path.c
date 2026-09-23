// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning_path.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool decoder_is_valid(const struct v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       v32_scrambler_is_valid(&decoder->descrambler) &&
	       decoder->phase_quadrant < 4;
}

static bool path_header_is_valid(
	const struct v32_conditioning_path *path)
{
	const struct v32_equalizer *equalizer;
	bool complete;

	if (path == NULL ||
	    !v32_conditioning_receiver_is_valid(&path->conditioning))
		return false;
	equalizer = &path->receive.equalizer;
	complete = path->conditioning.phase == V32_CONDITIONING_READY;
	if (!path->conditioning.has_previous_quadrant)
		return !complete && !equalizer->decision_pending &&
		       path->observation_epoch == equalizer->epoch;
	if (path->observation_epoch !=
	    path->conditioning.observation_epoch)
		return false;
	if (!complete)
		return !equalizer->decision_pending &&
		       path->observation_epoch == equalizer->epoch;
	if (!equalizer->decision_pending)
		return path->observation_epoch == equalizer->epoch;
	return path->observation_epoch != UINT64_MAX &&
	       path->observation_epoch + 1u == equalizer->epoch;
}

bool v32_conditioning_path_is_valid(
	const struct v32_conditioning_path *path)
{
	return path_header_is_valid(path) &&
	       v32_receive_path_is_valid(&path->receive);
}

bool v32_conditioning_path_is_complete(
	const struct v32_conditioning_path *path)
{
	return v32_conditioning_path_is_valid(path) &&
	       path->conditioning.phase == V32_CONDITIONING_READY;
}

enum v32_conditioning_path_result v32_conditioning_path_init(
	struct v32_conditioning_path *path,
	enum v32_transmitter_role source_role,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols)
{
	struct v32_conditioning_path initialized;

	if (path == NULL)
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (v32_receive_path_init(
		    &initialized.receive, near_delay_symbols,
		    round_trip_symbols) != V32_RECEIVE_PATH_OK ||
	    v32_conditioning_receiver_init(
		    &initialized.conditioning, source_role) !=
		    V32_CONDITIONING_RECEIVER_OK)
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	initialized.observation_epoch = initialized.receive.equalizer.epoch;
	if (!v32_conditioning_path_is_valid(&initialized))
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	*path = initialized;
	return V32_CONDITIONING_PATH_OK;
}

enum v32_conditioning_path_result v32_conditioning_path_init_from_receive(
	struct v32_conditioning_path *path,
	enum v32_transmitter_role source_role,
	const struct v32_receive_path *receive)
{
	struct v32_conditioning_path initialized;

	if (path == NULL || receive == NULL || memory_regions_overlap(
		    path, sizeof(*path), receive, sizeof(*receive)) ||
	    !v32_receive_path_is_valid(receive))
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.receive = *receive;
	if (v32_receive_path_reacquire_conditioning(
		    &initialized.receive) != V32_RECEIVE_PATH_OK ||
	    v32_conditioning_receiver_init(
		    &initialized.conditioning, source_role) !=
		    V32_CONDITIONING_RECEIVER_OK)
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	initialized.observation_epoch = initialized.receive.equalizer.epoch;
	if (!v32_conditioning_path_is_valid(&initialized))
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	*path = initialized;
	return V32_CONDITIONING_PATH_OK;
}

static enum v32_conditioning_path_result commit_observations(
	struct v32_conditioning_path *path,
	enum v32_receive_path_result receive_result,
	struct v32_equalizer_observation *observation)
{
	while (receive_result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		struct v32_conditioning_receiver next_conditioning;
		struct v32_conditioning_symbol_decision decision;
		enum v32_conditioning_receiver_result conditioning_result;

		next_conditioning = path->conditioning;
		conditioning_result = v32_conditioning_receiver_observe(
			&next_conditioning, observation, &decision);
		if (conditioning_result != V32_CONDITIONING_RECEIVER_OK)
			return V32_CONDITIONING_PATH_INVALID_STATE;
		if (next_conditioning.phase == V32_CONDITIONING_READY) {
			receive_result = v32_receive_path_commit_adaptation(
				&path->receive, &decision.equalizer,
				V32_EQUALIZER_ADAPTATION_FINE, observation);
		} else {
			receive_result = v32_receive_path_commit(
				&path->receive, &decision.equalizer, observation);
		}
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return V32_CONDITIONING_PATH_INVALID_STATE;
		path->conditioning = next_conditioning;
		path->observation_epoch =
			decision.equalizer.observation.epoch;
		if (path->conditioning.phase == V32_CONDITIONING_READY)
			return path_header_is_valid(path) ?
				       V32_CONDITIONING_PATH_COMPLETE :
				       V32_CONDITIONING_PATH_INVALID_STATE;
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_CONDITIONING_PATH_INVALID_ARGUMENT :
			       V32_CONDITIONING_PATH_INVALID_STATE;
	return path_header_is_valid(path) ?
		       V32_CONDITIONING_PATH_OK :
		       V32_CONDITIONING_PATH_INVALID_STATE;
}

enum v32_conditioning_path_result v32_conditioning_path_sample(
	struct v32_conditioning_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference)
{
	struct v32_equalizer_observation observation;
	enum v32_receive_path_result receive_result;

	if (!path_header_is_valid(path))
		return V32_CONDITIONING_PATH_INVALID_STATE;
	if (path->conditioning.phase == V32_CONDITIONING_READY)
		return v32_receive_path_is_valid(&path->receive) ?
			       V32_CONDITIONING_PATH_COMPLETE :
			       V32_CONDITIONING_PATH_INVALID_STATE;
	if (accepted_reference != NULL &&
	    memory_regions_overlap(
		    path, sizeof(*path), accepted_reference,
		    sizeof(*accepted_reference)))
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	receive_result = v32_receive_path_sample(
		&path->receive, received, accepted_reference, &observation);
	return commit_observations(path, receive_result, &observation);
}

bool v32_conditioning_path_handoff_is_valid(
	const struct v32_conditioning_path_handoff *handoff)
{
	return handoff != NULL &&
	       v32_receive_path_is_valid(&handoff->receive) &&
	       decoder_is_valid(&handoff->decoder) &&
	       v32_receive_path_handoff_observation_matches(
		       &handoff->receive, handoff->observation_ready,
		       &handoff->observation);
}

enum v32_conditioning_path_result v32_conditioning_path_handoff(
	const struct v32_conditioning_path *path,
	struct v32_conditioning_path_handoff *handoff)
{
	struct v32_conditioning_path_handoff generated = { 0 };

	if (!v32_conditioning_path_is_complete(path))
		return V32_CONDITIONING_PATH_INVALID_STATE;
	if (handoff == NULL || memory_regions_overlap(
		    path, sizeof(*path), handoff, sizeof(*handoff)))
		return V32_CONDITIONING_PATH_INVALID_ARGUMENT;
	generated.receive = path->receive;
	if (v32_conditioning_receiver_framing_state(
		    &path->conditioning, &generated.decoder) !=
	    V32_CONDITIONING_RECEIVER_OK)
		return V32_CONDITIONING_PATH_INVALID_STATE;
	generated.observation_ready =
		generated.receive.equalizer.decision_pending;
	if (generated.observation_ready)
		generated.observation =
			generated.receive.equalizer.pending_observation;
	if (!v32_conditioning_path_handoff_is_valid(&generated))
		return V32_CONDITIONING_PATH_INVALID_STATE;
	*handoff = generated;
	return V32_CONDITIONING_PATH_OK;
}
