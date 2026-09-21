// SPDX-License-Identifier: BSD-3-Clause

#include "end_path.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool offer_is_valid(
	const struct v32_rate_offer *offer,
	enum v32_rate_signal_type type)
{
	uint16_t word;

	return v32_rate_signal_encode(type, offer, &word) ==
	       V32_RATE_SIGNAL_OK;
}

static bool detector_is_reset(
	const struct v32_rate_signal_detector *detector)
{
	return detector->window == 0 && detector->locked_rate_word == 0 &&
	       detector->dibits_seen == 0 &&
	       detector->dibits_since_boundary == 0 && !detector->aligned;
}

static bool adaptation_is_valid(
	enum v32_equalizer_adaptation adaptation)
{
	return adaptation == V32_EQUALIZER_ADAPTATION_DISABLED ||
	       adaptation == V32_EQUALIZER_ADAPTATION_FINE;
}

static bool observation_epoch_is_valid(
	const struct v32_receive_path *receive, uint64_t observation_epoch)
{
	if (!receive->equalizer.decision_pending)
		return observation_epoch == receive->equalizer.epoch;
	return observation_epoch != UINT64_MAX &&
	       observation_epoch + 1u ==
		       receive->equalizer.pending_observation.epoch;
}

static bool detector_state_is_valid(const struct v32_end_path *path)
{
	struct v32_rate_offer offer;

	if (path->end_received)
		return detector_is_reset(&path->detector);
	if (!path->detector.aligned)
		return true;
	return v32_rate_signal_detector_offer(&path->detector, &offer) &&
	       (v32_rate_offers_match(&offer, &path->initial_offer) ||
		v32_rate_offers_match(&offer, &path->selected_offer));
}

static bool adaptation_state_is_valid(const struct v32_end_path *path)
{
	enum v32_equalizer_adaptation current =
		path->receive.equalizer.adaptation;

	if (!adaptation_is_valid(path->target_adaptation) ||
	    !adaptation_is_valid(current))
		return false;
	if (current == path->target_adaptation)
		return true;
	return !path->end_received &&
	       path->receive.equalizer.decision_pending;
}

static bool path_header_is_valid(const struct v32_end_path *path)
{
	return path != NULL &&
	       v32_four_point_receiver_is_valid(&path->four_point) &&
	       v32_rate_signal_detector_is_valid(&path->detector) &&
	       offer_is_valid(&path->initial_offer, V32_RATE_SIGNAL) &&
	       offer_is_valid(&path->selected_offer, V32_END_SIGNAL) &&
	       (path->selected_offer.rates & path->initial_offer.rates) ==
		       path->selected_offer.rates &&
	       detector_state_is_valid(path) &&
	       adaptation_state_is_valid(path) &&
	       observation_epoch_is_valid(
		       &path->receive, path->observation_epoch);
}

bool v32_end_path_is_valid(const struct v32_end_path *path)
{
	return path_header_is_valid(path) &&
	       v32_receive_path_is_valid(&path->receive);
}

bool v32_end_path_is_complete(const struct v32_end_path *path)
{
	return v32_end_path_is_valid(path) && path->end_received;
}

static bool event_is_allowed(
	const struct v32_end_path *path,
	enum v32_rate_signal_event event,
	const struct v32_rate_offer *offer)
{
	if (event == V32_RATE_SIGNAL_NO_EVENT)
		return true;
	if (event == V32_END_SIGNAL_DETECTED)
		return v32_rate_offers_match(offer, &path->selected_offer);
	return v32_rate_offers_match(offer, &path->initial_offer) ||
	       v32_rate_offers_match(offer, &path->selected_offer);
}

static enum v32_end_path_result commit_observations(
	struct v32_end_path *path,
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
			return V32_END_PATH_INVALID_STATE;
		if (!event_is_allowed(path, event, &offer))
			return V32_END_PATH_SEQUENCE_REJECTED;
		if (path->receive.equalizer.adaptation ==
		    path->target_adaptation) {
			receive_result = v32_receive_path_commit(
				&path->receive, &symbol.equalizer, observation);
		} else {
			receive_result = v32_receive_path_commit_adaptation(
				&path->receive, &symbol.equalizer,
				path->target_adaptation, observation);
		}
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return V32_END_PATH_INVALID_STATE;
		path->four_point = next_four_point;
		path->detector = next_detector;
		path->observation_epoch = symbol.equalizer.observation.epoch;
		if (event == V32_END_SIGNAL_DETECTED) {
			path->end_received = true;
			return path_header_is_valid(path) ?
				       V32_END_PATH_COMPLETE :
				       V32_END_PATH_INVALID_STATE;
		}
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_END_PATH_INVALID_ARGUMENT :
			       V32_END_PATH_INVALID_STATE;
	return path_header_is_valid(path) ? V32_END_PATH_OK :
					 V32_END_PATH_INVALID_STATE;
}

enum v32_end_path_result v32_end_path_init(
	struct v32_end_path *path,
	const struct v32_rate_path_handoff *rate,
	const struct v32_rate_offer *selected_offer,
	enum v32_equalizer_adaptation adaptation)
{
	struct v32_end_path initialized;
	enum v32_end_path_result result = V32_END_PATH_OK;

	if (path == NULL || rate == NULL || selected_offer == NULL ||
	    memory_regions_overlap(path, sizeof(*path), rate, sizeof(*rate)) ||
	    memory_regions_overlap(
		    path, sizeof(*path), selected_offer,
		    sizeof(*selected_offer)) ||
	    !v32_rate_path_handoff_is_valid(rate) ||
	    !offer_is_valid(selected_offer, V32_END_SIGNAL) ||
	    (selected_offer->rates & rate->offer.rates) !=
		    selected_offer->rates ||
	    !adaptation_is_valid(adaptation))
		return V32_END_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.receive = rate->receive;
	initialized.four_point = rate->four_point;
	initialized.detector = rate->detector;
	initialized.initial_offer = rate->offer;
	initialized.selected_offer = *selected_offer;
	initialized.observation_epoch = rate->observation_ready ?
		rate->observation.epoch - 1u :
		rate->receive.equalizer.epoch;
	initialized.target_adaptation = adaptation;
	if (!rate->observation_ready &&
	    initialized.receive.equalizer.adaptation != adaptation &&
	    v32_receive_path_set_adaptation(
		    &initialized.receive, adaptation) !=
		    V32_RECEIVE_PATH_OK)
		return V32_END_PATH_INVALID_STATE;
	if (!path_header_is_valid(&initialized))
		return V32_END_PATH_INVALID_ARGUMENT;
	if (rate->observation_ready) {
		struct v32_equalizer_observation observation =
			rate->observation;

		result = commit_observations(
			&initialized, V32_RECEIVE_PATH_OBSERVATION_READY,
			&observation);
		if (result != V32_END_PATH_OK &&
		    result != V32_END_PATH_COMPLETE)
			return result;
	}
	if (!v32_end_path_is_valid(&initialized))
		return V32_END_PATH_INVALID_STATE;
	*path = initialized;
	return result;
}

enum v32_end_path_result v32_end_path_set_adaptation(
	struct v32_end_path *path,
	enum v32_equalizer_adaptation adaptation)
{
	enum v32_receive_path_result result;

	if (!v32_end_path_is_valid(path) || path->end_received)
		return V32_END_PATH_INVALID_STATE;
	if (!adaptation_is_valid(adaptation))
		return V32_END_PATH_INVALID_ARGUMENT;
	if (path->receive.equalizer.decision_pending) {
		path->target_adaptation = adaptation;
		return V32_END_PATH_OK;
	}
	if (path->receive.equalizer.adaptation == adaptation) {
		path->target_adaptation = adaptation;
		return V32_END_PATH_OK;
	}
	result = v32_receive_path_set_adaptation(
		&path->receive, adaptation);
	if (result == V32_RECEIVE_PATH_INVALID_ARGUMENT)
		return V32_END_PATH_INVALID_ARGUMENT;
	if (result != V32_RECEIVE_PATH_OK)
		return V32_END_PATH_INVALID_STATE;
	path->target_adaptation = adaptation;
	return path_header_is_valid(path) ? V32_END_PATH_OK :
					 V32_END_PATH_INVALID_STATE;
}

enum v32_end_path_result v32_end_path_sample(
	struct v32_end_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference)
{
	struct v32_equalizer_observation observation;
	enum v32_receive_path_result receive_result;

	if (!path_header_is_valid(path))
		return V32_END_PATH_INVALID_STATE;
	if (path->end_received)
		return v32_receive_path_is_valid(&path->receive) ?
			       V32_END_PATH_COMPLETE :
			       V32_END_PATH_INVALID_STATE;
	if (accepted_reference != NULL &&
	    memory_regions_overlap(
		    path, sizeof(*path), accepted_reference,
		    sizeof(*accepted_reference)))
		return V32_END_PATH_INVALID_ARGUMENT;
	receive_result = v32_receive_path_sample(
		&path->receive, received, accepted_reference, &observation);
	return commit_observations(path, receive_result, &observation);
}

bool v32_end_path_handoff_is_valid(
	const struct v32_end_path_handoff *handoff)
{
	bool pending;

	if (handoff == NULL ||
	    !v32_receive_path_is_valid(&handoff->receive) ||
	    !v32_four_point_receiver_is_valid(&handoff->four_point) ||
	    !offer_is_valid(&handoff->selected_offer, V32_END_SIGNAL) ||
	    !adaptation_is_valid(handoff->receive.equalizer.adaptation) ||
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

enum v32_end_path_result v32_end_path_handoff(
	const struct v32_end_path *path,
	struct v32_end_path_handoff *handoff)
{
	struct v32_end_path_handoff generated = { 0 };

	if (!v32_end_path_is_complete(path))
		return V32_END_PATH_INVALID_STATE;
	if (handoff == NULL || memory_regions_overlap(
		    path, sizeof(*path), handoff, sizeof(*handoff)))
		return V32_END_PATH_INVALID_ARGUMENT;
	generated.receive = path->receive;
	generated.four_point = path->four_point;
	generated.selected_offer = path->selected_offer;
	generated.observation_epoch = path->observation_epoch;
	generated.observation_ready =
		generated.receive.equalizer.decision_pending;
	if (generated.observation_ready)
		generated.observation =
			generated.receive.equalizer.pending_observation;
	if (!v32_end_path_handoff_is_valid(&generated))
		return V32_END_PATH_INVALID_STATE;
	*handoff = generated;
	return V32_END_PATH_OK;
}
