// SPDX-License-Identifier: BSD-3-Clause

#include "rate_path.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool observation_epoch_is_valid(
	const struct v32_receive_path *receive, uint64_t observation_epoch)
{
	if (!receive->equalizer.decision_pending)
		return observation_epoch == receive->equalizer.epoch;
	return observation_epoch != UINT64_MAX &&
	       observation_epoch + 1u ==
		       receive->equalizer.pending_observation.epoch;
}

static bool path_header_is_valid(const struct v32_rate_path *path)
{
	return path != NULL &&
	       v32_four_point_receiver_is_valid(&path->four_point) &&
	       v32_rate_signal_detector_is_valid(&path->detector) &&
	       path->receive.equalizer.adaptation ==
		       V32_EQUALIZER_ADAPTATION_FINE &&
	       observation_epoch_is_valid(
		       &path->receive, path->observation_epoch);
}

bool v32_rate_path_is_valid(const struct v32_rate_path *path)
{
	return path_header_is_valid(path) &&
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
		struct v32_four_point_receiver next_four_point;
		struct v32_rate_signal_detector next_detector;
		struct v32_four_point_symbol symbol;
		struct v32_rate_offer offer;
		enum v32_rate_signal_event event;

		next_four_point = path->four_point;
		next_detector = path->detector;
		if (v32_four_point_receiver_observe(
			    &next_four_point, observation, true, &symbol) !=
			    V32_FOUR_POINT_RECEIVER_OK ||
		    v32_rate_signal_detector_observe(
			    &next_detector, symbol.dibit, &event, &offer) !=
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
		if (event == V32_RATE_SIGNAL_DETECTED)
			return path_header_is_valid(path) ?
				       V32_RATE_PATH_COMPLETE :
				       V32_RATE_PATH_INVALID_STATE;
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_RATE_PATH_INVALID_ARGUMENT :
			       V32_RATE_PATH_INVALID_STATE;
	return path_header_is_valid(path) ? V32_RATE_PATH_OK :
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
	if (v32_four_point_receiver_init(
		    &initialized.four_point, &conditioning->decoder) !=
		    V32_FOUR_POINT_RECEIVER_OK ||
	    v32_rate_signal_detector_init(&initialized.detector) !=
		    V32_RATE_SIGNAL_OK)
		return V32_RATE_PATH_INVALID_ARGUMENT;
	initialized.observation_epoch = conditioning->observation_ready ?
		conditioning->observation.epoch - 1u :
		conditioning->receive.equalizer.epoch;
	if (!path_header_is_valid(&initialized))
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

	if (!path_header_is_valid(path))
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
	bool pending;
	struct v32_rate_offer decoded;

	if (handoff == NULL ||
	    !v32_receive_path_is_valid(&handoff->receive) ||
	    !v32_four_point_receiver_is_valid(&handoff->four_point) ||
	    !v32_rate_signal_detector_is_valid(&handoff->detector) ||
	    !v32_rate_signal_detector_offer(&handoff->detector, &decoded) ||
	    !v32_rate_offers_match(&handoff->offer, &decoded) ||
	    handoff->receive.equalizer.adaptation !=
		    V32_EQUALIZER_ADAPTATION_FINE ||
	    !observation_epoch_is_valid(
		    &handoff->receive, handoff->observation_epoch))
		return false;
	pending = handoff->receive.equalizer.decision_pending;
	if (handoff->observation_ready != pending)
		return false;
	if (!pending) {
		static const struct v32_equalizer_observation zero;

		return memcmp(
			       &handoff->observation, &zero, sizeof(zero)) == 0;
	}
	return memcmp(
		       &handoff->observation,
		       &handoff->receive.equalizer.pending_observation,
		       sizeof(handoff->observation)) == 0;
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
