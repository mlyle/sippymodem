// SPDX-License-Identifier: BSD-3-Clause

#include "b1_path.h"

#include "decision.h"
#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool selected_mode(
	const struct v32_rate_offer *offer, enum v32_b1_mode *mode,
	uint8_t *data_bits_per_symbol)
{
	uint16_t word;

	if (offer == NULL || mode == NULL || data_bits_per_symbol == NULL ||
	    v32_rate_signal_encode(V32_END_SIGNAL, offer, &word) !=
		    V32_RATE_SIGNAL_OK)
		return false;
	switch (offer->rates) {
	case V32_RATE_CAPABILITY_4800:
		*mode = V32_B1_MODE_FOUR_POINT;
		*data_bits_per_symbol = V32_4800_BITS_PER_SYMBOL;
		return true;
	case V32_RATE_CAPABILITY_7200:
		*mode = V32_B1_MODE_TRELLIS;
		*data_bits_per_symbol = V32_7200_DATA_BITS;
		return true;
	case V32_RATE_CAPABILITY_9600:
		*mode = offer->trellis_at_highest_rate ?
			V32_B1_MODE_TRELLIS : V32_B1_MODE_UNCODED;
		*data_bits_per_symbol = V32_9600_BITS_PER_SYMBOL;
		return true;
	case V32_RATE_CAPABILITY_12000:
		*mode = V32_B1_MODE_TRELLIS;
		*data_bits_per_symbol = V32_12000_DATA_BITS;
		return true;
	case V32_RATE_CAPABILITY_14400:
		*mode = V32_B1_MODE_TRELLIS;
		*data_bits_per_symbol = V32_14400_DATA_BITS;
		return true;
	}
	return false;
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

static bool active_receiver_is_valid(const struct v32_b1_path *path)
{
	switch (path->mode) {
	case V32_B1_MODE_FOUR_POINT:
		return path->data_bits_per_symbol == V32_4800_BITS_PER_SYMBOL &&
		       v32_four_point_receiver_is_valid(
			       &path->receiver.four_point);
	case V32_B1_MODE_UNCODED:
		return path->data_bits_per_symbol == V32_9600_BITS_PER_SYMBOL &&
		       v32_uncoded_receiver_is_valid(&path->receiver.uncoded);
	case V32_B1_MODE_TRELLIS:
		return path->data_bits_per_symbol >= V32_TRELLIS_MIN_DATA_BITS &&
		       path->data_bits_per_symbol <= V32_TRELLIS_MAX_DATA_BITS &&
		       v32_trellis_receiver_is_valid(&path->receiver.trellis) &&
		       path->receiver.trellis.decoder.data_bits_per_symbol ==
			       path->data_bits_per_symbol &&
		       path->receiver.trellis.b1_observations ==
			       path->observations &&
		       path->receiver.trellis.b1_matches == path->b1_matches;
	}
	return false;
}

static bool path_header_is_valid(const struct v32_b1_path *path)
{
	enum v32_b1_mode selected;
	uint8_t data_bits;
	enum v32_equalizer_adaptation adaptation;

	if (path == NULL || path->observations > V32_B1_PATH_SYMBOLS ||
	    !selected_mode(&path->selected_offer, &selected, &data_bits) ||
	    selected != path->mode || data_bits != path->data_bits_per_symbol ||
	    !active_receiver_is_valid(path) ||
	    !observation_epoch_is_valid(
		    &path->receive, path->observation_epoch))
		return false;
	adaptation = path->receive.equalizer.adaptation;
	if (adaptation != V32_EQUALIZER_ADAPTATION_DISABLED &&
	    adaptation != V32_EQUALIZER_ADAPTATION_FINE)
		return false;
	return path->observations != V32_B1_PATH_SYMBOLS ||
	       adaptation == V32_EQUALIZER_ADAPTATION_FINE;
}

bool v32_b1_path_is_valid(const struct v32_b1_path *path)
{
	return path_header_is_valid(path) &&
	       v32_receive_path_is_valid(&path->receive);
}

bool v32_b1_path_is_complete(const struct v32_b1_path *path)
{
	return v32_b1_path_is_valid(path) &&
	       path->observations == V32_B1_PATH_SYMBOLS;
}

static bool initialize_receiver(
	struct v32_b1_path *path,
	const struct v32_four_point_receiver *four_point)
{
	switch (path->mode) {
	case V32_B1_MODE_FOUR_POINT:
		path->receiver.four_point = *four_point;
		return true;
	case V32_B1_MODE_UNCODED:
		return v32_uncoded_receiver_init(
			       &path->receiver.uncoded,
			       &four_point->decoder.descrambler) ==
		       V32_UNCODED_RECEIVER_OK;
	case V32_B1_MODE_TRELLIS:
		return v32_trellis_receiver_init(
			       &path->receiver.trellis,
			       &four_point->decoder.descrambler,
			       path->data_bits_per_symbol) ==
		       V32_TRELLIS_RECEIVER_OK;
	}
	return false;
}

static enum v32_b1_path_result decide_observation(
	const struct v32_b1_path *path,
	const struct v32_equalizer_observation *observation,
	union v32_b1_receiver *next_receiver,
	struct v32_equalizer_decision *equalizer, bool *b1_matches)
{
	bool adapt = path->receive.equalizer.adaptation !=
		     V32_EQUALIZER_ADAPTATION_DISABLED;

	*next_receiver = path->receiver;
	switch (path->mode) {
	case V32_B1_MODE_FOUR_POINT: {
		struct v32_four_point_symbol symbol;

		if (v32_four_point_receiver_observe(
			    &next_receiver->four_point, observation, adapt,
			    &symbol) != V32_FOUR_POINT_RECEIVER_OK)
			return V32_B1_PATH_INVALID_STATE;
		*b1_matches = *b1_matches && symbol.dibit == 3;
		*equalizer = symbol.equalizer;
		return V32_B1_PATH_OK;
	}
	case V32_B1_MODE_UNCODED: {
		struct v32_uncoded_symbol symbol;

		if (v32_uncoded_receiver_observe(
			    &next_receiver->uncoded, observation, adapt,
			    &symbol) != V32_UNCODED_RECEIVER_OK)
			return V32_B1_PATH_INVALID_STATE;
		*b1_matches = *b1_matches && symbol.group == 15;
		*equalizer = symbol.equalizer;
		return V32_B1_PATH_OK;
	}
	case V32_B1_MODE_TRELLIS: {
		struct v32_tcm_decision decision;

		if (v32_decide_tcm(
			    observation, path->data_bits_per_symbol, adapt,
			    &decision) != V32_DECISION_OK ||
		    v32_trellis_receiver_track_b1(
			    &next_receiver->trellis, observation) !=
			    V32_TRELLIS_RECEIVER_OK)
			return V32_B1_PATH_INVALID_STATE;
		*b1_matches = next_receiver->trellis.b1_matches;
		*equalizer = decision.equalizer;
		return V32_B1_PATH_OK;
	}
	}
	return V32_B1_PATH_INVALID_STATE;
}

static enum v32_b1_path_result commit_observations(
	struct v32_b1_path *path,
	enum v32_receive_path_result receive_result,
	struct v32_equalizer_observation *observation)
{
	while (receive_result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		union v32_b1_receiver next_receiver;
		struct v32_equalizer_decision decision;
		enum v32_b1_path_result result;
		uint8_t next_observations;
		bool b1_matches = path->b1_matches;

		if (path->observations == V32_B1_PATH_SYMBOLS)
			return V32_B1_PATH_COMPLETE;
		result = decide_observation(
			path, observation, &next_receiver, &decision,
			&b1_matches);
		if (result != V32_B1_PATH_OK)
			return result;
		next_observations = (uint8_t)(path->observations + 1u);
		if (next_observations == V32_B1_PATH_SYMBOLS &&
		    path->receive.equalizer.adaptation !=
			    V32_EQUALIZER_ADAPTATION_FINE) {
			receive_result = v32_receive_path_commit_adaptation(
				&path->receive, &decision,
				V32_EQUALIZER_ADAPTATION_FINE, observation);
		} else {
			receive_result = v32_receive_path_commit(
				&path->receive, &decision, observation);
		}
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return V32_B1_PATH_INVALID_STATE;
		path->receiver = next_receiver;
		path->observations = next_observations;
		path->observation_epoch = decision.observation.epoch;
		path->b1_matches = b1_matches;
		if (path->observations == V32_B1_PATH_SYMBOLS)
			return path_header_is_valid(path) ?
				       V32_B1_PATH_COMPLETE :
				       V32_B1_PATH_INVALID_STATE;
	}
	if (receive_result != V32_RECEIVE_PATH_OK)
		return receive_result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			       V32_B1_PATH_INVALID_ARGUMENT :
			       V32_B1_PATH_INVALID_STATE;
	return path_header_is_valid(path) ? V32_B1_PATH_OK :
					 V32_B1_PATH_INVALID_STATE;
}

enum v32_b1_path_result v32_b1_path_init(
	struct v32_b1_path *path,
	const struct v32_end_path_handoff *end)
{
	struct v32_b1_path initialized;
	enum v32_b1_path_result result = V32_B1_PATH_OK;

	if (path == NULL || end == NULL ||
	    memory_regions_overlap(path, sizeof(*path), end, sizeof(*end)) ||
	    !v32_end_path_handoff_is_valid(end))
		return V32_B1_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.receive = end->receive;
	initialized.selected_offer = end->selected_offer;
	initialized.b1_matches = true;
	if (!selected_mode(
		    &initialized.selected_offer, &initialized.mode,
		    &initialized.data_bits_per_symbol) ||
	    !initialize_receiver(&initialized, &end->four_point))
		return V32_B1_PATH_INVALID_ARGUMENT;
	initialized.observation_epoch = end->observation_ready ?
		end->observation.epoch - 1u : end->receive.equalizer.epoch;
	if (!path_header_is_valid(&initialized))
		return V32_B1_PATH_INVALID_ARGUMENT;
	if (end->observation_ready) {
		struct v32_equalizer_observation observation = end->observation;

		result = commit_observations(
			&initialized, V32_RECEIVE_PATH_OBSERVATION_READY,
			&observation);
		if (result != V32_B1_PATH_OK &&
		    result != V32_B1_PATH_COMPLETE)
			return result;
	}
	if (!v32_b1_path_is_valid(&initialized))
		return V32_B1_PATH_INVALID_STATE;
	*path = initialized;
	return result;
}

enum v32_b1_path_result v32_b1_path_sample(
	struct v32_b1_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference)
{
	struct v32_equalizer_observation observation;
	enum v32_receive_path_result receive_result;

	if (!path_header_is_valid(path))
		return V32_B1_PATH_INVALID_STATE;
	if (path->observations == V32_B1_PATH_SYMBOLS)
		return v32_receive_path_is_valid(&path->receive) ?
			       V32_B1_PATH_COMPLETE : V32_B1_PATH_INVALID_STATE;
	if (accepted_reference != NULL &&
	    memory_regions_overlap(
		    path, sizeof(*path), accepted_reference,
		    sizeof(*accepted_reference)))
		return V32_B1_PATH_INVALID_ARGUMENT;
	receive_result = v32_receive_path_sample(
		&path->receive, received, accepted_reference, &observation);
	return commit_observations(path, receive_result, &observation);
}
