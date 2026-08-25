// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_line_transmitter.h"

#include <stddef.h>

static enum v34_phase3_line_transmitter_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V34_PHASE3_LINE_TRANSMITTER_OK;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V34_PHASE3_LINE_TRANSMITTER_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V34_PHASE3_LINE_TRANSMITTER_CLIPPING_RISK;
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
	}
	return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
}

bool v34_phase3_line_transmitter_is_valid(
	const struct v34_phase3_line_transmitter *transmitter)
{
	return transmitter != NULL &&
	       v34_training_line_is_valid(&transmitter->line) &&
	       transmitter->line.profile.md_symbol_count == 0 &&
	       v34_phase3_transmitter_is_valid(&transmitter->symbols) &&
	       transmitter->line.profile.local_role ==
		       transmitter->symbols.local_role;
}

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_init(
	struct v34_phase3_line_transmitter *transmitter,
	const struct v34_phase3_profile *local_profile,
	enum v34_constellation_size remote_constellation,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_phase3_line_transmitter initialized = { 0 };
	enum v34_phase3_line_transmitter_result result;

	if (transmitter == NULL ||
	    !v34_phase3_profile_is_valid(local_profile) ||
	    local_profile->md_symbol_count != 0 ||
	    !v34_constellation_size_is_valid(remote_constellation))
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	result = line_result(v34_training_line_init(
		&initialized.line, local_profile, sample_rate,
		nominal_peak_amplitude));
	if (result != V34_PHASE3_LINE_TRANSMITTER_OK)
		return result;
	if (v34_phase3_transmitter_init(
		    &initialized.symbols, local_profile->local_role,
		    remote_constellation) != V34_PHASE3_TRANSMITTER_OK ||
	    !v34_phase3_line_transmitter_is_valid(&initialized))
		return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = initialized;
	return V34_PHASE3_LINE_TRANSMITTER_OK;
}

static enum v34_phase3_line_transmitter_result control_result(
	struct v34_phase3_line_transmitter *transmitter,
	enum v34_phase3_transmitter_result result)
{
	if (result == V34_PHASE3_TRANSMITTER_INVALID_STATE)
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE;
	if (result != V34_PHASE3_TRANSMITTER_OK ||
	    !v34_phase3_line_transmitter_is_valid(transmitter))
		return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
	return V34_PHASE3_LINE_TRANSMITTER_OK;
}

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_start_j(
	struct v34_phase3_line_transmitter *transmitter)
{
	struct v34_phase3_line_transmitter next;
	enum v34_phase3_line_transmitter_result result;

	if (!v34_phase3_line_transmitter_is_valid(transmitter))
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	next = *transmitter;
	result = control_result(
		&next, v34_phase3_transmitter_start_j(&next.symbols));
	if (result == V34_PHASE3_LINE_TRANSMITTER_OK)
		*transmitter = next;
	return result;
}

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_finish_j(
	struct v34_phase3_line_transmitter *transmitter)
{
	struct v34_phase3_line_transmitter next;
	enum v34_phase3_line_transmitter_result result;

	if (!v34_phase3_line_transmitter_is_valid(transmitter))
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	next = *transmitter;
	result = control_result(
		&next, v34_phase3_transmitter_finish_j(&next.symbols));
	if (result == V34_PHASE3_LINE_TRANSMITTER_OK)
		*transmitter = next;
	return result;
}

bool v34_phase3_line_transmitter_is_handoff_ready(
	const struct v34_phase3_line_transmitter *transmitter)
{
	return v34_phase3_line_transmitter_is_valid(transmitter) &&
	       transmitter->symbols.stage == V34_PHASE3_TRANSMITTER_COMPLETE &&
	       v34_training_line_needs_point(&transmitter->line);
}

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_sample(
	struct v34_phase3_line_transmitter *transmitter, int16_t *sample)
{
	struct v34_phase3_line_transmitter next;
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	enum v34_training_line_result waveform_result;
	int16_t next_sample;
	bool accepting;
	bool accepted;

	if (!v34_phase3_line_transmitter_is_valid(transmitter) || sample == NULL)
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	if (v34_phase3_line_transmitter_is_handoff_ready(transmitter))
		return V34_PHASE3_LINE_TRANSMITTER_COMPLETE;
	next = *transmitter;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		if (v34_phase3_transmitter_next(&next.symbols, &point) !=
		    V34_PHASE3_TRANSMITTER_OK)
			return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
		point_pointer = &point;
	}
	waveform_result = v34_training_line_sample(
		&next.line, point_pointer, &next_sample, &accepted);
	if (waveform_result != V34_TRAINING_LINE_OK)
		return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
	if (accepted != accepting ||
	    !v34_phase3_line_transmitter_is_valid(&next))
		return V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*sample = next_sample;
	return V34_PHASE3_LINE_TRANSMITTER_OK;
}

enum v34_phase3_line_transmitter_result
v34_phase3_line_transmitter_handoff(
	const struct v34_phase3_line_transmitter *transmitter,
	struct v34_training_line *line)
{
	if (!v34_phase3_line_transmitter_is_valid(transmitter) || line == NULL)
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT;
	if (!v34_phase3_line_transmitter_is_handoff_ready(transmitter))
		return V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE;
	*line = transmitter->line;
	return V34_PHASE3_LINE_TRANSMITTER_OK;
}
