// SPDX-License-Identifier: BSD-3-Clause

#include "rate_path.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

bool v32_rate_path_header_is_valid(const struct v32_rate_path *path)
{
	return path != NULL &&
	       v32_receive_path_header_is_valid(&path->receive) &&
	       v32_uncoded_receiver_is_valid(&path->four_point) &&
	       v32_rate_signal_detector_is_valid(&path->detector) &&
	       path->receive.equalizer.adaptation ==
		       V32_EQUALIZER_ADAPTATION_FINE &&
	       v32_receive_path_epoch_matches(
		       &path->receive, path->observation_epoch);
}

bool v32_rate_path_is_valid(const struct v32_rate_path *path)
{
	return v32_rate_path_header_is_valid(path) &&
	       v32_receive_path_is_valid(&path->receive);
}

bool v32_rate_path_is_complete(const struct v32_rate_path *path)
{
	return v32_rate_path_is_valid(path) && path->detector.aligned;
}

static enum v32_rate_path_result commit_observations(
	struct v32_rate_path *path,
	enum v32_receive_path_result receive_result,
	struct v32_equalizer_observation *observation)
{
	while (receive_result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		struct v32_uncoded_receiver next_four_point;
		struct v32_rate_signal_detector next_detector;
		struct v32_uncoded_symbol symbol;
		struct v32_rate_offer offer;
		enum v32_rate_signal_event event;

		next_four_point = path->four_point;
		next_detector = path->detector;
		if (v32_uncoded_receiver_observe(
			    &next_four_point, V32_4800_BITS_PER_SYMBOL,
			    observation, true, &symbol) !=
			    V32_UNCODED_RECEIVER_OK ||
		    v32_rate_signal_detector_observe(
			    &next_detector, symbol.group, &event, &offer) !=
			    V32_RATE_SIGNAL_OK)
			return V32_RATE_PATH_INVALID_STATE;
		receive_result = v32_receive_path_commit(
			&path->receive, &symbol.equalizer, observation);
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return V32_RATE_PATH_INVALID_STATE;
		path->four_point = next_four_point;
		path->detector = next_detector;
		path->observation_epoch = symbol.equalizer.observation.epoch;
		if (event == V32_END_SIGNAL_DETECTED)
			return V32_RATE_PATH_INVALID_STATE;
		if (event == V32_CLEARDOWN_SIGNAL_DETECTED)
			return V32_RATE_PATH_CLEARDOWN;
		if (event == V32_RATE_SIGNAL_DETECTED)
			return v32_rate_path_header_is_valid(path) ?
				       V32_RATE_PATH_COMPLETE :
				       V32_RATE_PATH_INVALID_STATE;
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_RATE_PATH_INVALID_ARGUMENT :
			       V32_RATE_PATH_INVALID_STATE;
	return v32_rate_path_header_is_valid(path) ? V32_RATE_PATH_OK :
					  V32_RATE_PATH_INVALID_STATE;
}

enum v32_rate_path_result v32_rate_path_init(
	struct v32_rate_path *path,
	const struct v32_conditioning_path_handoff *conditioning)
{
	struct v32_rate_path initialized;
	enum v32_rate_path_result result = V32_RATE_PATH_OK;

	if (path == NULL || conditioning == NULL ||
	    memory_regions_overlap(
		    path, sizeof(*path), conditioning, sizeof(*conditioning)))
		return V32_RATE_PATH_INVALID_ARGUMENT;
	if (!v32_conditioning_path_handoff_is_valid(conditioning))
		return V32_RATE_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.receive = conditioning->receive;
	if (v32_uncoded_receiver_init(
		    &initialized.four_point, &conditioning->decoder) !=
		    V32_UNCODED_RECEIVER_OK ||
	    v32_rate_signal_detector_init(&initialized.detector) !=
		    V32_RATE_SIGNAL_OK)
		return V32_RATE_PATH_INVALID_ARGUMENT;
	initialized.observation_epoch = conditioning->observation_ready ?
		conditioning->observation.epoch - 1u :
		conditioning->receive.equalizer.epoch;
	if (!v32_rate_path_header_is_valid(&initialized))
		return V32_RATE_PATH_INVALID_ARGUMENT;
	if (conditioning->observation_ready) {
		struct v32_equalizer_observation observation =
			conditioning->observation;

		result = commit_observations(
			&initialized, V32_RECEIVE_PATH_OBSERVATION_READY,
			&observation);
		if (result != V32_RATE_PATH_OK &&
		    result != V32_RATE_PATH_COMPLETE)
			return result;
	}
	if (!v32_rate_path_is_valid(&initialized))
		return V32_RATE_PATH_INVALID_STATE;
	*path = initialized;
	return result;
}

enum v32_rate_path_result v32_rate_path_sample(
	struct v32_rate_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference)
{
	struct v32_equalizer_observation observation;
	enum v32_receive_path_result receive_result;

	if (!v32_rate_path_header_is_valid(path))
		return V32_RATE_PATH_INVALID_STATE;
	if (path->detector.aligned)
		return v32_receive_path_is_valid(&path->receive) ?
			       V32_RATE_PATH_COMPLETE :
			       V32_RATE_PATH_INVALID_STATE;
	if (accepted_reference != NULL &&
	    memory_regions_overlap(
		    path, sizeof(*path), accepted_reference,
		    sizeof(*accepted_reference)))
		return V32_RATE_PATH_INVALID_ARGUMENT;
	receive_result = v32_receive_path_sample(
		&path->receive, received, accepted_reference, &observation);
	return commit_observations(path, receive_result, &observation);
}

bool v32_rate_path_handoff_is_valid(
	const struct v32_rate_path_handoff *handoff)
{
	struct v32_rate_offer decoded;

	return handoff != NULL &&
	       v32_receive_path_is_valid(&handoff->receive) &&
	       v32_uncoded_receiver_is_valid(&handoff->four_point) &&
	       v32_rate_signal_detector_is_valid(&handoff->detector) &&
	       v32_rate_signal_detector_offer(&handoff->detector, &decoded) &&
	       v32_rate_offers_match(&handoff->offer, &decoded) &&
	       handoff->receive.equalizer.adaptation ==
		       V32_EQUALIZER_ADAPTATION_FINE &&
	       v32_receive_path_epoch_matches(
		       &handoff->receive, handoff->observation_epoch) &&
	       v32_receive_path_handoff_observation_matches(
		       &handoff->receive, handoff->observation_ready,
		       &handoff->observation);
}

enum v32_rate_path_result v32_rate_path_handoff(
	const struct v32_rate_path *path,
	struct v32_rate_path_handoff *handoff)
{
	struct v32_rate_path_handoff generated = { 0 };

	if (!v32_rate_path_is_complete(path))
		return V32_RATE_PATH_INVALID_STATE;
	if (handoff == NULL || memory_regions_overlap(
		    path, sizeof(*path), handoff, sizeof(*handoff)))
		return V32_RATE_PATH_INVALID_ARGUMENT;
	generated.receive = path->receive;
	generated.four_point = path->four_point;
	generated.detector = path->detector;
	if (!v32_rate_signal_detector_offer(
		    &generated.detector, &generated.offer))
		return V32_RATE_PATH_INVALID_STATE;
	generated.observation_epoch = path->observation_epoch;
	generated.observation_ready =
		generated.receive.equalizer.decision_pending;
	if (generated.observation_ready)
		generated.observation =
			generated.receive.equalizer.pending_observation;
	if (!v32_rate_path_handoff_is_valid(&generated))
		return V32_RATE_PATH_INVALID_STATE;
	*handoff = generated;
	return V32_RATE_PATH_OK;
}
