// SPDX-License-Identifier: BSD-3-Clause

#include "receive_path.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static bool buffer_state_is_valid(const struct v32_receive_path *path)
{
	if (path->recovered_index > path->recovered_count ||
	    path->recovered_count > V32_RECEIVE_PATH_RECOVERED_CAPACITY)
		return false;
	if (path->recovered_index == path->recovered_count &&
	    path->recovered_count != 0)
		return false;
	if (!path->equalizer.decision_pending)
		return path->recovered_index == 0 && path->recovered_count == 0;
	return true;
}

static bool path_header_is_valid(const struct v32_receive_path *path)
{
	return path != NULL && v32_line_receiver_is_valid(&path->line) &&
	       v32_equalizer_is_valid(&path->equalizer) &&
	       (!path->echo.echo.adapting || path->echo_training) &&
	       buffer_state_is_valid(path);
}

bool v32_receive_path_is_valid(const struct v32_receive_path *path)
{
	return path_header_is_valid(path) &&
	       v32_echo_frontend_is_valid(&path->echo);
}

enum v32_receive_path_result v32_receive_path_init(
	struct v32_receive_path *path,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols)
{
	struct v32_receive_path initialized;

	if (path == NULL)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (v32_echo_frontend_init(
		    &initialized.echo, near_delay_symbols,
		    round_trip_symbols) != V32_ECHO_OK ||
	    v32_line_receiver_init(&initialized.line) !=
		    V32_LINE_RECEIVER_OK ||
	    v32_equalizer_init(&initialized.equalizer) != V32_EQUALIZER_OK ||
	    !v32_receive_path_is_valid(&initialized))
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	*path = initialized;
	return V32_RECEIVE_PATH_OK;
}

static bool receive_boundary_is_open(const struct v32_receive_path *path)
{
	return !path->equalizer.decision_pending &&
	       path->recovered_index == 0 && path->recovered_count == 0;
}

enum v32_receive_path_result v32_receive_path_begin_echo_training(
	struct v32_receive_path *path)
{
	struct v32_receive_path next;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!receive_boundary_is_open(path))
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (path->echo_training || path->echo.echo.adapting)
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (path->echo.echo.reference_pending)
		return V32_RECEIVE_PATH_INVALID_STATE;
	next = *path;
	next.echo_training = true;
	if (!v32_receive_path_is_valid(&next))
		return V32_RECEIVE_PATH_INVALID_STATE;
	*path = next;
	return V32_RECEIVE_PATH_OK;
}

enum v32_receive_path_result v32_receive_path_start_echo_adaptation(
	struct v32_receive_path *path, double adaptation)
{
	struct v32_echo_frontend next_echo;
	enum v32_echo_result result;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!receive_boundary_is_open(path))
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (!path->echo_training)
		return V32_RECEIVE_PATH_INVALID_STATE;
	next_echo = path->echo;
	result = v32_echo_frontend_start_adaptation(
		&next_echo, adaptation);
	if (result == V32_ECHO_INVALID_ARGUMENT)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	if (result != V32_ECHO_OK ||
	    !v32_echo_frontend_is_valid(&next_echo))
		return V32_RECEIVE_PATH_INVALID_STATE;
	path->echo = next_echo;
	return V32_RECEIVE_PATH_OK;
}

enum v32_receive_path_result v32_receive_path_finish_echo_training(
	struct v32_receive_path *path)
{
	struct v32_receive_path next;
	struct v32_echo_frontend next_echo;
	enum v32_echo_result result;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!receive_boundary_is_open(path))
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (!path->echo_training)
		return V32_RECEIVE_PATH_INVALID_STATE;
	next_echo = path->echo;
	result = v32_echo_frontend_freeze(&next_echo);
	if (result != V32_ECHO_OK ||
	    !v32_echo_frontend_is_valid(&next_echo))
		return V32_RECEIVE_PATH_INVALID_STATE;
	next = *path;
	next.echo = next_echo;
	next.echo_training = false;
	if (!v32_receive_path_is_valid(&next))
		return V32_RECEIVE_PATH_INVALID_STATE;
	*path = next;
	return V32_RECEIVE_PATH_OK;
}

static enum v32_receive_path_result change_gain_lifecycle(
	struct v32_receive_path *path, bool freeze)
{
	struct v32_receive_path next;
	enum v32_line_receiver_result result;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!receive_boundary_is_open(path))
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (path->echo_training || path->echo.echo.adapting)
		return V32_RECEIVE_PATH_INVALID_STATE;
	next = *path;
	result = freeze ?
		v32_line_receiver_freeze_gain(&next.line) :
		v32_line_receiver_reacquire_gain(&next.line);
	if (result != V32_LINE_RECEIVER_OK ||
	    !v32_receive_path_is_valid(&next))
		return V32_RECEIVE_PATH_INVALID_STATE;
	*path = next;
	return V32_RECEIVE_PATH_OK;
}

enum v32_receive_path_result v32_receive_path_freeze_gain(
	struct v32_receive_path *path)
{
	return change_gain_lifecycle(path, true);
}

enum v32_receive_path_result v32_receive_path_reacquire_gain(
	struct v32_receive_path *path)
{
	return change_gain_lifecycle(path, false);
}

enum v32_receive_path_result v32_receive_path_reacquire_conditioning(
	struct v32_receive_path *path)
{
	struct v32_receive_path next;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!receive_boundary_is_open(path))
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (path->echo_training || path->echo.echo.adapting ||
	    !path->line.input_paused)
		return V32_RECEIVE_PATH_INVALID_STATE;
	next = *path;
	if (v32_line_receiver_reacquire_timing(&next.line) !=
		    V32_LINE_RECEIVER_OK ||
	    v32_equalizer_set_adaptation(
		    &next.equalizer, V32_EQUALIZER_ADAPTATION_COARSE) !=
		    V32_EQUALIZER_OK ||
	    v32_equalizer_set_carrier_tracking(
		    &next.equalizer, true) != V32_EQUALIZER_OK ||
	    v32_equalizer_restart_carrier_acquisition(&next.equalizer) !=
		    V32_EQUALIZER_OK ||
	    !v32_receive_path_is_valid(&next))
		return V32_RECEIVE_PATH_INVALID_STATE;
	/* Timing now starts a new three-sample/symbol stream. Its first
	 * observation cannot inherit the old stream's partial-symbol cursor. */
	next.equalizer.samples_until_observation = 1;
	*path = next;
	return V32_RECEIVE_PATH_OK;
}

static enum v32_receive_path_result drain_recovered(
	struct v32_equalizer *equalizer,
	const int16_t *recovered, size_t *index, size_t count,
	struct v32_equalizer_observation *observation)
{
	while (*index < count) {
		struct v32_equalizer_observation next_observation;
		size_t consumed;
		enum v32_equalizer_result result = v32_equalizer_process(
			equalizer, recovered + *index, count - *index,
			&consumed, &next_observation);

		if (result != V32_EQUALIZER_OK &&
		    result != V32_EQUALIZER_OBSERVATION_READY)
			return V32_RECEIVE_PATH_INVALID_STATE;
		if (consumed == 0)
			return V32_RECEIVE_PATH_INVALID_STATE;
		*index += consumed;
		if (result == V32_EQUALIZER_OBSERVATION_READY) {
			*observation = next_observation;
			return V32_RECEIVE_PATH_OBSERVATION_READY;
		}
	}
	return V32_RECEIVE_PATH_OK;
}

static bool sample_arguments_are_separate(
	const struct v32_receive_path *path,
	const struct v32_echo_reference *reference,
	const struct v32_equalizer_observation *observation)
{
	return observation != NULL &&
	       !memory_regions_overlap(
		       path, sizeof(*path), observation, sizeof(*observation)) &&
	       (reference == NULL ||
		(!memory_regions_overlap(
			reference, sizeof(*reference), path, sizeof(*path)) &&
		 !memory_regions_overlap(
			reference, sizeof(*reference),
			observation, sizeof(*observation))));
}

enum v32_receive_path_result v32_receive_path_sample(
	struct v32_receive_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	struct v32_equalizer_observation *observation)
{
	struct v32_line_receiver next_line;
	struct v32_equalizer next_equalizer;
	struct v32_equalizer_observation next_observation;
	int16_t recovered[V32_RECEIVE_PATH_RECOVERED_CAPACITY];
	double cleaned;
	size_t recovered_count;
	size_t recovered_index = 0;
	bool cleaned_ready;
	enum v32_echo_result echo_result;
	enum v32_receive_path_result result;

	if (!path_header_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (path->equalizer.decision_pending)
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (!sample_arguments_are_separate(
		    path, accepted_reference, observation))
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;

	next_line = path->line;
	next_equalizer = path->equalizer;
	echo_result = v32_echo_frontend_sample(
		&path->echo, received, accepted_reference,
		&cleaned, &cleaned_ready);
	if (echo_result == V32_ECHO_INVALID_ARGUMENT)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	if (echo_result != V32_ECHO_OK)
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!cleaned_ready)
		return V32_RECEIVE_PATH_OK;
	if (v32_line_receiver_process(
		    &next_line, &cleaned, 1,
		    path->echo_training, recovered,
		    V32_RECEIVE_PATH_RECOVERED_CAPACITY,
		    &recovered_count) != V32_LINE_RECEIVER_OK)
		return V32_RECEIVE_PATH_INVALID_STATE;
	result = drain_recovered(
		&next_equalizer, recovered, &recovered_index,
		recovered_count, &next_observation);
	if (result != V32_RECEIVE_PATH_OK &&
	    result != V32_RECEIVE_PATH_OBSERVATION_READY)
		return result;

	path->line = next_line;
	path->equalizer = next_equalizer;
	memset(path->recovered, 0, sizeof(path->recovered));
	if (recovered_index < recovered_count) {
		memcpy(path->recovered, recovered,
		       recovered_count * sizeof(*recovered));
		path->recovered_index = recovered_index;
		path->recovered_count = recovered_count;
	} else {
		path->recovered_index = 0;
		path->recovered_count = 0;
	}
	if (!path_header_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (result == V32_RECEIVE_PATH_OBSERVATION_READY)
		*observation = next_observation;
	return result;
}

enum v32_receive_path_result v32_receive_path_sample_paused(
	struct v32_receive_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	double *cleaned, bool *cleaned_ready)
{
	struct v32_echo_frontend next_echo;
	double next_cleaned;
	bool next_ready;
	enum v32_echo_result result;

	if (!path_header_is_valid(path) || !receive_boundary_is_open(path) ||
	    !path->line.input_paused || path->echo_training ||
	    path->echo.echo.adapting)
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (cleaned == NULL || cleaned_ready == NULL ||
	    memory_regions_overlap(
		    path, sizeof(*path), cleaned, sizeof(*cleaned)) ||
	    memory_regions_overlap(
		    path, sizeof(*path), cleaned_ready,
		    sizeof(*cleaned_ready)) ||
	    memory_regions_overlap(
		    cleaned, sizeof(*cleaned), cleaned_ready,
		    sizeof(*cleaned_ready)) ||
	    (accepted_reference != NULL &&
	     (memory_regions_overlap(
		     path, sizeof(*path), accepted_reference,
		     sizeof(*accepted_reference)) ||
	      memory_regions_overlap(
		     accepted_reference, sizeof(*accepted_reference),
		     cleaned, sizeof(*cleaned)) ||
	      memory_regions_overlap(
		     accepted_reference, sizeof(*accepted_reference),
		     cleaned_ready, sizeof(*cleaned_ready)))))
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	next_echo = path->echo;
	result = v32_echo_frontend_sample(
		&next_echo, received, accepted_reference,
		&next_cleaned, &next_ready);
	if (result == V32_ECHO_INVALID_ARGUMENT)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	if (result != V32_ECHO_OK ||
	    !v32_echo_frontend_is_valid(&next_echo))
		return V32_RECEIVE_PATH_INVALID_STATE;
	path->echo = next_echo;
	if (next_ready)
		*cleaned = next_cleaned;
	*cleaned_ready = next_ready;
	return V32_RECEIVE_PATH_OK;
}

static bool commit_arguments_are_separate(
	const struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	const struct v32_equalizer_observation *observation)
{
	return decision != NULL && observation != NULL &&
	       !memory_regions_overlap(
		       path, sizeof(*path), decision, sizeof(*decision)) &&
	       !memory_regions_overlap(
		       path, sizeof(*path), observation, sizeof(*observation)) &&
	       !memory_regions_overlap(
		       decision, sizeof(*decision),
		       observation, sizeof(*observation));
}

static enum v32_receive_path_result commit_decision(
	struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	bool change_adaptation,
	enum v32_equalizer_adaptation adaptation,
	struct v32_equalizer_observation *observation)
{
	struct v32_equalizer next_equalizer;
	struct v32_equalizer_observation next_observation;
	size_t next_index;
	enum v32_receive_path_result result;

	if (!path_header_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!path->equalizer.decision_pending)
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (!commit_arguments_are_separate(path, decision, observation))
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	next_equalizer = path->equalizer;
	next_index = path->recovered_index;
	if (v32_equalizer_commit(&next_equalizer, decision) !=
		    V32_EQUALIZER_OK)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	if (change_adaptation &&
	    v32_equalizer_set_adaptation(
		    &next_equalizer, adaptation) != V32_EQUALIZER_OK)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	result = drain_recovered(
		&next_equalizer, path->recovered, &next_index,
		path->recovered_count, &next_observation);
	if (result != V32_RECEIVE_PATH_OK &&
	    result != V32_RECEIVE_PATH_OBSERVATION_READY)
		return result;

	path->equalizer = next_equalizer;
	if (next_index == path->recovered_count) {
		memset(path->recovered, 0, sizeof(path->recovered));
		path->recovered_index = 0;
		path->recovered_count = 0;
	} else {
		path->recovered_index = next_index;
	}
	if (!path_header_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	if (result == V32_RECEIVE_PATH_OBSERVATION_READY)
		*observation = next_observation;
	return result;
}

enum v32_receive_path_result v32_receive_path_commit(
	struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	struct v32_equalizer_observation *observation)
{
	return commit_decision(
		path, decision, false,
		V32_EQUALIZER_ADAPTATION_DISABLED, observation);
}

enum v32_receive_path_result v32_receive_path_commit_adaptation(
	struct v32_receive_path *path,
	const struct v32_equalizer_decision *decision,
	enum v32_equalizer_adaptation adaptation,
	struct v32_equalizer_observation *observation)
{
	return commit_decision(
		path, decision, true, adaptation, observation);
}

enum v32_receive_path_result v32_receive_path_set_adaptation(
	struct v32_receive_path *path,
	enum v32_equalizer_adaptation adaptation)
{
	enum v32_equalizer_result result;

	if (!v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	result = v32_equalizer_set_adaptation(
		&path->equalizer, adaptation);
	if (result == V32_EQUALIZER_DECISION_PENDING)
		return V32_RECEIVE_PATH_DECISION_PENDING;
	if (result == V32_EQUALIZER_INVALID_ARGUMENT)
		return V32_RECEIVE_PATH_INVALID_ARGUMENT;
	if (result != V32_EQUALIZER_OK ||
	    !v32_receive_path_is_valid(path))
		return V32_RECEIVE_PATH_INVALID_STATE;
	return V32_RECEIVE_PATH_OK;
}
