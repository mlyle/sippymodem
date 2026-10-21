// SPDX-License-Identifier: BSD-3-Clause

#include "v34_branches.h"
#include "sample_math.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static bool subset_slice_is_valid(
	const struct v34_subset_slice *slice)
{
	if (slice == NULL)
		return false;
	for (uint8_t label = 0; label < V34_SUBSET_LABEL_COUNT;
	     ++label) {
		const struct v34_lattice_candidate *candidate =
			&slice->subset[label];
		uint8_t actual_label;

		if (candidate->subset_label != label ||
		    !v34_grid_point_is_bounded(&candidate->channel_output) ||
		    !isfinite(candidate->squared_error) ||
		    candidate->squared_error < 0.0 ||
		    v34_grid_subset_label(
			    &candidate->channel_output, &actual_label) !=
			    V34_TRELLIS_OK ||
		    actual_label != label)
			return false;
	}
	return true;
}

static uint8_t subset_index(
	const struct v34_convolutional_input *input,
	uint8_t convolutional_output,
	enum v34_trellis_selection selection)
{
	uint8_t index = convolutional_output | (uint8_t)(input->y1 << 1) |
			(uint8_t)(input->y2 << 2);

	if (selection == V34_TRELLIS_32_STATE)
		return (uint8_t)(index | (uint8_t)(input->y4 << 3));
	if (selection == V34_TRELLIS_64_STATE)
		return (uint8_t)(index | (uint8_t)(input->y3 << 3) |
				 (uint8_t)(input->y4 << 4));
	return index;
}

static int32_t branch_magnitude_squared(
	const struct v34_branch *branch)
{
	int32_t magnitude = 0;

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_grid_point *point =
			&branch->symbol[symbol].channel_output;

		magnitude += (int32_t)sample_pair_power_s16(point->in_phase, point->quadrature);
	}
	return magnitude;
}

static bool branch_precedes(
	const struct v34_branch *candidate,
	const struct v34_branch *current)
{
	int32_t magnitude;
	int32_t current_magnitude;

	if (candidate->squared_error != current->squared_error)
		return candidate->squared_error < current->squared_error;
	magnitude = branch_magnitude_squared(candidate);
	current_magnitude = branch_magnitude_squared(current);
	if (magnitude != current_magnitude)
		return magnitude < current_magnitude;
	if (candidate->symbol[0].subset_label !=
	    current->symbol[0].subset_label)
		return candidate->symbol[0].subset_label <
		       current->symbol[0].subset_label;
	return candidate->symbol[1].subset_label <
	       current->symbol[1].subset_label;
}

bool v34_branch_set_is_valid(
	const struct v34_branch_set *set)
{
	uint8_t count;

	if (set == NULL)
		return false;
	count = v34_trellis_branch_count(set->selection);
	if (count == 0 || set->count != count || set->inversion_bit > 1u)
		return false;
	for (uint8_t index = 0; index < count; ++index) {
		const struct v34_branch *branch = &set->branch[index];
		struct v34_convolutional_input derived;

		/* Match the stored double precision even on x87. */
		if (branch->convolutional_output > 1u ||
		    !v34_convolutional_input_is_valid(
			    &branch->convolutional_input) ||
		    !isfinite(branch->squared_error) ||
		    branch->squared_error < 0.0 ||
		    branch->squared_error !=
			    (double)(branch->symbol[0].squared_error +
				     branch->symbol[1].squared_error))
			return false;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			const struct v34_lattice_candidate *candidate =
				&branch->symbol[symbol];
			uint8_t actual_label;

			if (candidate->subset_label >=
				    V34_SUBSET_LABEL_COUNT ||
			    !v34_grid_point_is_bounded(
				    &candidate->channel_output) ||
			    !isfinite(candidate->squared_error) ||
			    candidate->squared_error < 0.0 ||
			    v34_grid_subset_label(
				    &candidate->channel_output, &actual_label) !=
				    V34_TRELLIS_OK ||
			    actual_label != candidate->subset_label)
				return false;
		}
		if (v34_convolutional_input_from_subset_labels(
			    branch->symbol[0].subset_label,
			    branch->symbol[1].subset_label, &derived) !=
			    V34_TRELLIS_OK ||
		    derived.y1 != branch->convolutional_input.y1 ||
		    derived.y2 != branch->convolutional_input.y2 ||
		    derived.y3 != branch->convolutional_input.y3 ||
		    derived.y4 != branch->convolutional_input.y4 ||
		    branch->convolutional_output !=
			    ((branch->symbol[0].subset_label ^
			      branch->symbol[1].subset_label ^
			      set->inversion_bit) &
			     1u) ||
		    subset_index(
			    &branch->convolutional_input,
			    branch->convolutional_output, set->selection) != index)
			return false;
	}
	return true;
}

enum v34_branches_result v34_build_branch_set(
	const struct v34_subset_slice
		slices[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	enum v34_trellis_selection selection,
	struct v34_branch_set *set)
{
	struct v34_branch_set generated = {
		.selection = selection,
		.count = v34_trellis_branch_count(selection),
		.inversion_bit = inversion_bit,
	};
	bool found[V34_MAX_BRANCH_COUNT] = { false };

	if (generated.count == 0 || inversion_bit > 1u || set == NULL ||
	    slices == NULL || !subset_slice_is_valid(&slices[0]) ||
	    !subset_slice_is_valid(&slices[1]))
		return V34_BRANCHES_INVALID_ARGUMENT;
	for (uint8_t first = 0; first < V34_SUBSET_LABEL_COUNT;
	     ++first) {
		for (uint8_t second = 0;
		     second < V34_SUBSET_LABEL_COUNT; ++second) {
			struct v34_branch candidate = {
				.symbol = {
					slices[0].subset[first],
					slices[1].subset[second],
				},
			};
			uint8_t index;

			if (v34_convolutional_input_from_subset_labels(
				    first, second,
				    &candidate.convolutional_input) !=
				    V34_TRELLIS_OK)
				return V34_BRANCHES_INVALID_ARGUMENT;
			/* Each label's low bit is its quarter-turn parity. Their
			 * XOR is U0 xor C0, so applying V0 recovers Y0. */
			candidate.convolutional_output =
				(uint8_t)((first ^ second ^ inversion_bit) & 1u);
			candidate.squared_error =
				candidate.symbol[0].squared_error +
				candidate.symbol[1].squared_error;
			if (!isfinite(candidate.squared_error))
				return V34_BRANCHES_NUMERIC_FAILURE;
			index = subset_index(
				&candidate.convolutional_input,
				candidate.convolutional_output, selection);
			if (!found[index] || branch_precedes(
						     &candidate,
						     &generated.branch[index])) {
				generated.branch[index] = candidate;
				found[index] = true;
			}
		}
	}
	for (uint8_t index = 0; index < generated.count; ++index) {
		if (!found[index])
			return V34_BRANCHES_INVALID_ARGUMENT;
	}
	*set = generated;
	return V34_BRANCHES_OK;
}
