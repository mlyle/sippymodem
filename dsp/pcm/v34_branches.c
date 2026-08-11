// SPDX-License-Identifier: BSD-3-Clause

#include "v34_branches.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static uint8_t branch_count(
	enum sippymodem_v34_trellis_selection selection)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return 8;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return 16;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return 32;
	}
	return 0;
}

static bool grid_point_is_bounded(
	const struct sippymodem_v34_grid_point *point)
{
	return point->in_phase >= -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->in_phase <= SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature >= -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT &&
	       point->quadrature <= SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT;
}

static bool subset_slice_is_valid(
	const struct sippymodem_v34_subset_slice *slice)
{
	if (slice == NULL)
		return false;
	for (uint8_t label = 0; label < SIPPYMODEM_V34_SUBSET_LABEL_COUNT;
	     ++label) {
		const struct sippymodem_v34_lattice_candidate *candidate =
			&slice->subset[label];
		uint8_t actual_label;

		if (candidate->subset_label != label ||
		    !grid_point_is_bounded(&candidate->channel_output) ||
		    !isfinite(candidate->squared_error) ||
		    candidate->squared_error < 0.0 ||
		    sippymodem_v34_grid_subset_label(
			    &candidate->channel_output, &actual_label) !=
			    SIPPYMODEM_V34_TRELLIS_OK ||
		    actual_label != label)
			return false;
	}
	return true;
}

static uint8_t subset_index(
	const struct sippymodem_v34_convolutional_input *input,
	uint8_t convolutional_output,
	enum sippymodem_v34_trellis_selection selection)
{
	uint8_t index = convolutional_output | (uint8_t)(input->y1 << 1) |
			(uint8_t)(input->y2 << 2);

	if (selection == SIPPYMODEM_V34_TRELLIS_32_STATE)
		return (uint8_t)(index | (uint8_t)(input->y4 << 3));
	if (selection == SIPPYMODEM_V34_TRELLIS_64_STATE)
		return (uint8_t)(index | (uint8_t)(input->y3 << 3) |
				 (uint8_t)(input->y4 << 4));
	return index;
}

static int32_t branch_magnitude_squared(
	const struct sippymodem_v34_branch *branch)
{
	int32_t magnitude = 0;

	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct sippymodem_v34_grid_point *point =
			&branch->symbol[symbol].channel_output;

		magnitude += (int32_t)point->in_phase * point->in_phase +
			     (int32_t)point->quadrature * point->quadrature;
	}
	return magnitude;
}

static bool branch_precedes(
	const struct sippymodem_v34_branch *candidate,
	const struct sippymodem_v34_branch *current)
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

enum sippymodem_v34_branches_result sippymodem_v34_build_branch_set(
	const struct sippymodem_v34_subset_slice
		slices[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	enum sippymodem_v34_trellis_selection selection,
	struct sippymodem_v34_branch_set *set)
{
	struct sippymodem_v34_branch_set generated = {
		.selection = selection,
		.count = branch_count(selection),
	};
	bool found[SIPPYMODEM_V34_MAX_BRANCH_COUNT] = { false };

	if (generated.count == 0 || inversion_bit > 1u || set == NULL ||
	    slices == NULL || !subset_slice_is_valid(&slices[0]) ||
	    !subset_slice_is_valid(&slices[1]))
		return SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT;
	for (uint8_t first = 0; first < SIPPYMODEM_V34_SUBSET_LABEL_COUNT;
	     ++first) {
		for (uint8_t second = 0;
		     second < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++second) {
			struct sippymodem_v34_branch candidate = {
				.symbol = {
					slices[0].subset[first],
					slices[1].subset[second],
				},
			};
			uint8_t index;

			if (sippymodem_v34_convolutional_input_from_subset_labels(
				    first, second,
				    &candidate.convolutional_input) !=
				    SIPPYMODEM_V34_TRELLIS_OK)
				return SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT;
			/* Each label's low bit is its quarter-turn parity. Their
			 * XOR is U0 xor C0, so applying V0 recovers Y0. */
			candidate.convolutional_output =
				(uint8_t)((first ^ second ^ inversion_bit) & 1u);
			candidate.squared_error =
				candidate.symbol[0].squared_error +
				candidate.symbol[1].squared_error;
			if (!isfinite(candidate.squared_error))
				return SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE;
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
			return SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT;
	}
	*set = generated;
	return SIPPYMODEM_V34_BRANCHES_OK;
}

const char *sippymodem_v34_branches_result_name(
	enum sippymodem_v34_branches_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_BRANCHES_OK:
		return "ok";
	case SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
