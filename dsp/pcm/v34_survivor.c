// SPDX-License-Identifier: BSD-3-Clause

#include "v34_survivor.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static uint8_t trellis_state_count(
	enum sippymodem_v34_trellis_selection selection)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return 16;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return 32;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return 64;
	}
	return 0;
}

static bool grid_point_label(
	const struct sippymodem_v34_grid_point *point, uint8_t *label)
{
	return point->in_phase >= -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->in_phase <= SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature >= -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature <= SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       sippymodem_v34_grid_subset_label(point, label) ==
		       SIPPYMODEM_V34_TRELLIS_OK;
}

bool sippymodem_v34_survivor_decision_is_valid(
	const struct sippymodem_v34_survivor_decision *decision)
{
	uint8_t label[2];

	if (decision == NULL || decision->convolutional_output > 1u ||
	    decision->inversion_bit > 1u)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (!grid_point_label(
			    &decision->channel_output[symbol], &label[symbol]))
			return false;
	}
	return decision->convolutional_output ==
	       ((label[0] ^ label[1] ^ decision->inversion_bit) & 1u);
}

static bool traceback_step_is_valid(
	const struct sippymodem_v34_survivor_traceback_step *step,
	uint8_t state_count, uint8_t branch_count)
{
	uint8_t inversion_bit = step->branch[0].inversion_bit;

	for (uint8_t state = 0; state < state_count; ++state) {
		if (step->predecessor[state] >= state_count ||
		    step->branch_index[state] >= branch_count)
			return false;
	}
	for (uint8_t branch = 0; branch < branch_count; ++branch) {
		if (step->branch[branch].inversion_bit != inversion_bit ||
		    step->branch[branch].convolutional_output != (branch & 1u) ||
		    !sippymodem_v34_survivor_decision_is_valid(
			    &step->branch[branch]))
			return false;
	}
	return true;
}

bool sippymodem_v34_survivor_is_valid(
	const struct sippymodem_v34_survivor *survivor)
{
	uint8_t state_count;
	uint8_t branch_count;
	bool has_normalized_path = false;

	if (survivor == NULL)
		return false;
	state_count = trellis_state_count(survivor->selection);
	branch_count = (uint8_t)(state_count / 2u);
	if (state_count == 0 ||
	    survivor->path_index >=
		    SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS ||
	    survivor->path_intervals >
		    SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS ||
	    (survivor->path_intervals <
		     SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS &&
	     survivor->path_index != survivor->path_intervals))
		return false;
	for (uint8_t state = 0; state < state_count; ++state) {
		double metric = survivor->path_metric[state];

		if (metric == DBL_MAX)
			continue;
		if (!isfinite(metric) || metric < 0.0)
			return false;
		if (metric == 0.0)
			has_normalized_path = true;
	}
	if (!has_normalized_path)
		return false;
	for (uint8_t index = 0; index < survivor->path_intervals; ++index) {
		if (!traceback_step_is_valid(
			    &survivor->path[index], state_count, branch_count))
			return false;
	}
	return true;
}

enum sippymodem_v34_survivor_result sippymodem_v34_survivor_init(
	struct sippymodem_v34_survivor *survivor,
	enum sippymodem_v34_trellis_selection selection)
{
	struct sippymodem_v34_survivor initialized;

	if (survivor == NULL || trellis_state_count(selection) == 0)
		return SIPPYMODEM_V34_SURVIVOR_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	for (uint8_t state = 1;
	     state < SIPPYMODEM_V34_MAX_TRELLIS_STATES; ++state)
		initialized.path_metric[state] = DBL_MAX;
	initialized.selection = selection;
	*survivor = initialized;
	return SIPPYMODEM_V34_SURVIVOR_OK;
}

static struct sippymodem_v34_survivor_decision branch_decision(
	const struct sippymodem_v34_branch *branch, uint8_t inversion_bit)
{
	struct sippymodem_v34_survivor_decision decision = {
		.convolutional_output = branch->convolutional_output,
		.inversion_bit = inversion_bit,
	};

	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		decision.channel_output[symbol] =
			branch->symbol[symbol].channel_output;
	return decision;
}

static bool select_paths(
	const struct sippymodem_v34_survivor *survivor,
	const struct sippymodem_v34_branch_set *branches,
	double next_metric[SIPPYMODEM_V34_MAX_TRELLIS_STATES],
	struct sippymodem_v34_survivor_traceback_step *step)
{
	uint8_t state_count = trellis_state_count(survivor->selection);
	double minimum = DBL_MAX;

	for (uint8_t state = 0; state < SIPPYMODEM_V34_MAX_TRELLIS_STATES;
	     ++state)
		next_metric[state] = DBL_MAX;
	memset(step, 0, sizeof(*step));
	for (uint8_t branch = 0; branch < branches->count; ++branch)
		step->branch[branch] = branch_decision(
			&branches->branch[branch], branches->inversion_bit);

	for (uint8_t predecessor = 0; predecessor < state_count;
	     ++predecessor) {
		struct sippymodem_v34_convolutional_encoder origin = {
			.state = predecessor,
			.selection = survivor->selection,
		};
		uint8_t convolutional_output;

		if (survivor->path_metric[predecessor] == DBL_MAX)
			continue;
		if (sippymodem_v34_convolutional_encoder_output(
			    &origin, &convolutional_output) !=
		    SIPPYMODEM_V34_TRELLIS_OK)
			return false;
		for (uint8_t branch = 0; branch < branches->count; ++branch) {
			struct sippymodem_v34_convolutional_encoder next = origin;
			double metric;

			if (branches->branch[branch].convolutional_output !=
			    convolutional_output)
				continue;
			if (sippymodem_v34_convolutional_encoder_advance(
				    &next,
				    &branches->branch[branch].convolutional_input) !=
			    SIPPYMODEM_V34_TRELLIS_OK)
				return false;
			metric = survivor->path_metric[predecessor] +
				 branches->branch[branch].squared_error;
			if (!isfinite(metric) || metric >= next_metric[next.state])
				continue;
			next_metric[next.state] = metric;
			step->predecessor[next.state] = predecessor;
			step->branch_index[next.state] = branch;
		}
	}
	for (uint8_t state = 0; state < state_count; ++state) {
		if (next_metric[state] < minimum)
			minimum = next_metric[state];
	}
	if (minimum == DBL_MAX)
		return false;
	for (uint8_t state = 0; state < state_count; ++state) {
		if (next_metric[state] != DBL_MAX)
			next_metric[state] -= minimum;
	}
	return true;
}

static uint8_t best_state(const double *metric, uint8_t state_count)
{
	uint8_t best = 0;

	for (uint8_t state = 1; state < state_count; ++state) {
		if (metric[state] < metric[best])
			best = state;
	}
	return best;
}

static uint8_t previous_path_index(uint8_t index)
{
	return index == 0 ? SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS - 1u :
			    (uint8_t)(index - 1u);
}

static const struct sippymodem_v34_survivor_traceback_step *path_step(
	const struct sippymodem_v34_survivor *survivor,
	const struct sippymodem_v34_survivor_traceback_step *newest,
	uint8_t newest_index, uint8_t index)
{
	return index == newest_index ? newest : &survivor->path[index];
}

static struct sippymodem_v34_survivor_decision traceback_decision(
	const struct sippymodem_v34_survivor *survivor,
	const struct sippymodem_v34_survivor_traceback_step *newest,
	uint8_t newest_index, uint8_t state)
{
	uint8_t index = newest_index;

	for (uint8_t distance = 1;
	     distance < SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS;
	     ++distance) {
		const struct sippymodem_v34_survivor_traceback_step *step =
			path_step(survivor, newest, newest_index, index);

		state = step->predecessor[state];
		index = previous_path_index(index);
	}
	const struct sippymodem_v34_survivor_traceback_step *step =
		path_step(survivor, newest, newest_index, index);

	return step->branch[step->branch_index[state]];
}

enum sippymodem_v34_survivor_result sippymodem_v34_survivor_advance(
	struct sippymodem_v34_survivor *survivor,
	const struct sippymodem_v34_branch_set *branches,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_survivor_decision *traceback,
	bool *traceback_ready)
{
	struct sippymodem_v34_survivor_traceback_step generated_step;
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_v34_survivor_decision generated_traceback;
	double next_metric[SIPPYMODEM_V34_MAX_TRELLIS_STATES];
	uint8_t state_count;
	uint8_t current_state;
	uint8_t newest_index;
	uint8_t next_intervals;
	bool generated_ready;

	if (!sippymodem_v34_survivor_is_valid(survivor) ||
	    !sippymodem_v34_branch_set_is_valid(branches) ||
	    branches->selection != survivor->selection || current == NULL ||
	    traceback == NULL || traceback_ready == NULL)
		return SIPPYMODEM_V34_SURVIVOR_INVALID_ARGUMENT;
	if (!select_paths(
		    survivor, branches, next_metric, &generated_step))
		return SIPPYMODEM_V34_SURVIVOR_NUMERIC_FAILURE;
	state_count = trellis_state_count(survivor->selection);
	current_state = best_state(next_metric, state_count);
	generated_current = generated_step.branch
		[generated_step.branch_index[current_state]];
	newest_index = survivor->path_index;
	next_intervals = survivor->path_intervals;
	if (next_intervals < SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS)
		next_intervals++;
	generated_ready =
		next_intervals == SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS;
	if (generated_ready)
		generated_traceback = traceback_decision(
			survivor, &generated_step, newest_index, current_state);

	survivor->path[newest_index] = generated_step;
	memcpy(survivor->path_metric, next_metric, sizeof(next_metric));
	survivor->path_index = (uint8_t)(
		(newest_index + 1u) %
		SIPPYMODEM_V34_SURVIVOR_TRACEBACK_INTERVALS);
	survivor->path_intervals = next_intervals;
	*current = generated_current;
	*traceback_ready = generated_ready;
	if (generated_ready)
		*traceback = generated_traceback;
	return SIPPYMODEM_V34_SURVIVOR_OK;
}

const char *sippymodem_v34_survivor_result_name(
	enum sippymodem_v34_survivor_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_SURVIVOR_OK:
		return "ok";
	case SIPPYMODEM_V34_SURVIVOR_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_SURVIVOR_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
