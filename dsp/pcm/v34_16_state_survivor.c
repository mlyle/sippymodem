// SPDX-License-Identifier: BSD-3-Clause

#include "v34_16_state_survivor.h"

#include "memory_region.h"

#include <stddef.h>
#include <string.h>

static const uint8_t BRANCH_TABLE[64] = {
	0, 1, 2, 3, 2, 3, 0, 1, 1, 0, 3, 2, 3, 2, 1, 0,
	4, 5, 6, 7, 6, 7, 4, 5, 5, 4, 7, 6, 7, 6, 5, 4,
	2, 3, 0, 1, 0, 1, 2, 3, 3, 2, 1, 0, 1, 0, 3, 2,
	6, 7, 4, 5, 4, 5, 6, 7, 7, 6, 5, 4, 5, 4, 7, 6,
};

static bool header_is_valid(const struct v34_16_state_survivor *survivor)
{
	return survivor != NULL &&
	       survivor->path_index < V34_16_STATE_TRACEBACK_INTERVALS &&
	       survivor->path_intervals <= V34_16_STATE_TRACEBACK_INTERVALS &&
	       (survivor->path_intervals == V34_16_STATE_TRACEBACK_INTERVALS ||
		survivor->path_index == survivor->path_intervals);
}

static bool step_is_valid(const struct v34_16_state_traceback_step *step)
{
	for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state) {
		if (step->predecessor[state] >= V34_16_STATE_COUNT ||
		    !v34_survivor_decision_is_valid(&step->decision[state]))
			return false;
	}
	return true;
}

static bool scores_are_normalized(
	const struct v34_16_state_survivor *survivor)
{
	for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state)
		if (survivor->score[state] == 0)
			return true;
	return false;
}

bool v34_16_state_survivor_is_valid(
	const struct v34_16_state_survivor *survivor)
{
	if (!header_is_valid(survivor) || !scores_are_normalized(survivor))
		return false;
	for (uint8_t interval = 0; interval < survivor->path_intervals;
	     ++interval) {
		if (!step_is_valid(&survivor->path[interval]))
			return false;
	}
	return true;
}

enum v34_16_state_survivor_result v34_16_state_survivor_init(
	struct v34_16_state_survivor *survivor)
{
	struct v34_16_state_survivor initialized = { 0 };

	if (survivor == NULL)
		return V34_16_STATE_SURVIVOR_INVALID_ARGUMENT;
	*survivor = initialized;
	return V34_16_STATE_SURVIVOR_OK;
}
static bool branches_are_valid(
	const struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT])
{
	uint8_t inversion;

	if (branches == NULL)
		return false;
	inversion = branches[0].decision.inversion_bit;
	for (uint8_t branch = 0; branch < V34_16_STATE_BRANCH_COUNT;
	     ++branch) {
		const struct v34_survivor_decision *decision =
			&branches[branch].decision;

		if (!v34_survivor_decision_is_valid(decision) ||
		    decision->inversion_bit != inversion ||
		    decision->convolutional_output != (branch >> 2u))
			return false;
	}
	return true;
}

static uint8_t previous_index(uint8_t index)
{
	return index == 0 ? V34_16_STATE_TRACEBACK_INTERVALS - 1u :
			    (uint8_t)(index - 1u);
}

static const struct v34_16_state_traceback_step *path_step(
	const struct v34_16_state_survivor *survivor,
	const struct v34_16_state_traceback_step *newest,
	uint8_t newest_index, uint8_t index)
{
	return index == newest_index ? newest : &survivor->path[index];
}

static bool traceback_decision(
	const struct v34_16_state_survivor *survivor,
	const struct v34_16_state_traceback_step *newest,
	uint8_t newest_index, uint8_t state,
	struct v34_survivor_decision *decision)
{
	uint8_t index = newest_index;

	for (uint8_t distance = 1;
	     distance < V34_16_STATE_TRACEBACK_INTERVALS; ++distance) {
		const struct v34_16_state_traceback_step *step = path_step(
			survivor, newest, newest_index, index);

		if (step->predecessor[state] >= V34_16_STATE_COUNT ||
		    !v34_survivor_decision_is_valid(&step->decision[state]))
			return false;
		state = step->predecessor[state];
		index = previous_index(index);
	}
	const struct v34_16_state_traceback_step *step = path_step(
		survivor, newest, newest_index, index);

	if (step->predecessor[state] >= V34_16_STATE_COUNT ||
	    !v34_survivor_decision_is_valid(&step->decision[state]))
		return false;
	*decision = step->decision[state];
	return true;
}

static uint8_t select_paths(
	const struct v34_16_state_survivor *survivor,
	const struct v34_16_state_branch branches[V34_16_STATE_BRANCH_COUNT],
	uint16_t next_score[V34_16_STATE_COUNT],
	struct v34_16_state_traceback_step *step)
{
	uint16_t minimum = UINT16_C(0x7fff);
	uint8_t best_zero_state = 0;
	size_t table_index = 0;
	int selected_previous = 3;

	for (int source_state = V34_16_STATE_COUNT - 1; source_state >= 0;
	     --source_state) {
		uint16_t best_score = UINT16_C(0x7fff);
		int best_previous = selected_previous;
		uint8_t best_branch = BRANCH_TABLE[
			table_index + (size_t)(3 - best_previous)];
		uint8_t destination = (uint8_t)(
			V34_16_STATE_COUNT - 1 - source_state);
		uint8_t score_index = (uint8_t)(3 - (source_state >> 2));

		for (int previous = 3; previous >= 0; --previous) {
			uint8_t branch = BRANCH_TABLE[table_index++];
			uint16_t score = (uint16_t)(
				survivor->score[score_index] +
				branches[branch].metric);

			if (score < best_score) {
				best_score = score;
				best_previous = previous;
				best_branch = branch;
			}
			score_index = (uint8_t)(score_index + 4u);
		}
		selected_previous = best_previous;
		next_score[destination] = best_score;
		step->predecessor[destination] = (uint8_t)(
			best_previous * -4 - (source_state >> 2) + 15);
		step->decision[destination] = branches[best_branch].decision;
		if (best_score < minimum)
			minimum = best_score;
	}
	for (uint8_t state = 0; state < V34_16_STATE_COUNT; ++state) {
		next_score[state] = (uint16_t)(next_score[state] - minimum);
		if (next_score[state] == 0)
			best_zero_state = state;
	}
	return best_zero_state;
}

static bool regions_overlap(
	const struct v34_16_state_survivor *survivor,
	const struct v34_16_state_branch *branches,
	const struct v34_survivor_decision *current,
	const struct v34_survivor_decision *traceback,
	const bool *ready)
{
	return memory_regions_overlap(
		       survivor, survivor == NULL ? 0 : sizeof(*survivor),
		       branches,
		       sizeof(*branches) * V34_16_STATE_BRANCH_COUNT) ||
	       memory_regions_overlap(
		       survivor, survivor == NULL ? 0 : sizeof(*survivor),
		       current, sizeof(*current)) ||
	       memory_regions_overlap(
		       survivor, survivor == NULL ? 0 : sizeof(*survivor),
		       traceback, sizeof(*traceback)) ||
	       memory_regions_overlap(
		       survivor, survivor == NULL ? 0 : sizeof(*survivor),
		       ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       branches,
		       sizeof(*branches) * V34_16_STATE_BRANCH_COUNT,
		       current, sizeof(*current)) ||
	       memory_regions_overlap(
		       branches,
		       sizeof(*branches) * V34_16_STATE_BRANCH_COUNT,
		       traceback, sizeof(*traceback)) ||
	       memory_regions_overlap(
		       branches,
		       sizeof(*branches) * V34_16_STATE_BRANCH_COUNT,
		       ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       current, sizeof(*current), traceback,
		       sizeof(*traceback)) ||
	       memory_regions_overlap(
		       current, sizeof(*current), ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       traceback, sizeof(*traceback), ready, sizeof(*ready));
}

enum v34_16_state_survivor_result v34_16_state_survivor_advance(
	struct v34_16_state_survivor *survivor,
	const struct v34_16_state_branch
		branches[V34_16_STATE_BRANCH_COUNT],
	struct v34_survivor_decision *current,
	struct v34_survivor_decision *traceback,
	bool *traceback_ready)
{
	struct v34_16_state_traceback_step generated_step;
	struct v34_survivor_decision generated_traceback;
	uint16_t next_score[V34_16_STATE_COUNT];
	uint8_t current_state;
	uint8_t newest_index;
	uint8_t next_intervals;
	bool generated_ready;

	if (branches == NULL || current == NULL || traceback == NULL ||
	    traceback_ready == NULL ||
	    regions_overlap(
		    survivor, branches, current, traceback, traceback_ready))
		return V34_16_STATE_SURVIVOR_INVALID_ARGUMENT;
	if (!header_is_valid(survivor) || !scores_are_normalized(survivor))
		return V34_16_STATE_SURVIVOR_INVALID_STATE;
	if (!branches_are_valid(branches))
		return V34_16_STATE_SURVIVOR_INVALID_ARGUMENT;
	current_state = select_paths(
		survivor, branches, next_score, &generated_step);
	newest_index = survivor->path_index;
	next_intervals = survivor->path_intervals;
	if (next_intervals < V34_16_STATE_TRACEBACK_INTERVALS)
		next_intervals++;
	generated_ready =
		next_intervals == V34_16_STATE_TRACEBACK_INTERVALS;
	if (generated_ready &&
	    !traceback_decision(
		    survivor, &generated_step, newest_index, current_state,
		    &generated_traceback))
		return V34_16_STATE_SURVIVOR_INVALID_STATE;
	survivor->path[newest_index] = generated_step;
	memcpy(survivor->score, next_score, sizeof(next_score));
	survivor->path_index = (uint8_t)(
		(newest_index + 1u) % V34_16_STATE_TRACEBACK_INTERVALS);
	survivor->path_intervals = next_intervals;
	*current = generated_step.decision[current_state];
	*traceback_ready = generated_ready;
	if (generated_ready)
		*traceback = generated_traceback;
	return V34_16_STATE_SURVIVOR_OK;
}
