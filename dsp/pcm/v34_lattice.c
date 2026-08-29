// SPDX-License-Identifier: BSD-3-Clause

#include "v34_lattice.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

enum {
	V34_LATTICE_SUBSET_PERIOD = 8,
};

static int class_coordinate(int class_index, int residue, int period)
{
	return residue + period * class_index;
}

static int coordinate_residue(int coordinate, int period)
{
	int residue = coordinate % period;

	return residue < 0 ? residue + period : residue;
}

static int16_t nearest_residue_coordinate(
	double estimate, int residue, int period)
{
	int minimum = -V34_CHANNEL_COORDINATE_LIMIT;
	int maximum = V34_CHANNEL_COORDINATE_LIMIT;
	double position;
	int lower_index;
	int lower;
	int upper;
	double lower_error;
	double upper_error;

	while (coordinate_residue(minimum, period) != residue)
		minimum++;
	while (coordinate_residue(maximum, period) != residue)
		maximum--;
	if (estimate <= minimum)
		return (int16_t)minimum;
	if (estimate >= maximum)
		return (int16_t)maximum;
	position = (estimate - residue) / period;
	lower_index = (int)floor(position);
	lower = class_coordinate(lower_index, residue, period);
	upper = class_coordinate(lower_index + 1, residue, period);
	lower_error = fabs(estimate - lower);
	upper_error = fabs(estimate - upper);
	if (upper_error < lower_error ||
	    (upper_error == lower_error && abs(upper) < abs(lower)))
		return (int16_t)upper;
	return (int16_t)lower;
}

static bool candidate_error(
	const struct qam_point *estimate,
	const struct v34_grid_point *candidate, double *squared_error)
{
	double in_phase = estimate->in_phase - candidate->in_phase;
	double quadrature = estimate->quadrature - candidate->quadrature;
	double generated = in_phase * in_phase + quadrature * quadrature;

	if (!isfinite(generated))
		return false;
	*squared_error = generated;
	return true;
}

static int32_t point_magnitude_squared(
	const struct v34_grid_point *point)
{
	return (int32_t)point->in_phase * point->in_phase +
	       (int32_t)point->quadrature * point->quadrature;
}

static bool candidate_precedes(
	const struct v34_lattice_candidate *candidate,
	const struct v34_lattice_candidate *current)
{
	int32_t candidate_magnitude;
	int32_t current_magnitude;

	if (candidate->squared_error != current->squared_error)
		return candidate->squared_error < current->squared_error;
	candidate_magnitude = point_magnitude_squared(&candidate->channel_output);
	current_magnitude = point_magnitude_squared(&current->channel_output);
	if (candidate_magnitude != current_magnitude)
		return candidate_magnitude < current_magnitude;
	if (candidate->channel_output.in_phase !=
	    current->channel_output.in_phase)
		return candidate->channel_output.in_phase <
		       current->channel_output.in_phase;
	return candidate->channel_output.quadrature <
	       current->channel_output.quadrature;
}

enum v34_lattice_result v34_slice_grid_subsets(
	const struct qam_point *channel_estimate,
	struct v34_subset_slice *slice)
{
	struct v34_subset_slice generated = { 0 };
	bool found[V34_SUBSET_LABEL_COUNT] = { false };

	if (channel_estimate == NULL || slice == NULL ||
	    !isfinite(channel_estimate->in_phase) ||
	    !isfinite(channel_estimate->quadrature))
		return V34_LATTICE_INVALID_ARGUMENT;
	for (int in_phase_residue = 1; in_phase_residue < 8;
	     in_phase_residue += 2) {
		int16_t in_phase = nearest_residue_coordinate(
			channel_estimate->in_phase, in_phase_residue,
			V34_LATTICE_SUBSET_PERIOD);

		for (int quadrature_residue = 1; quadrature_residue < 8;
		     quadrature_residue += 2) {
			struct v34_lattice_candidate candidate = {
				.channel_output = {
					.in_phase = in_phase,
					.quadrature = nearest_residue_coordinate(
						channel_estimate->quadrature,
						quadrature_residue,
						V34_LATTICE_SUBSET_PERIOD),
				},
			};
			uint8_t label;

			if (v34_grid_subset_label(
				    &candidate.channel_output, &label) !=
				    V34_TRELLIS_OK)
				return V34_LATTICE_INVALID_ARGUMENT;
			candidate.subset_label = label;
			if (!candidate_error(
				    channel_estimate, &candidate.channel_output,
				    &candidate.squared_error))
				return V34_LATTICE_NUMERIC_FAILURE;
			if (!found[label] || candidate_precedes(
						     &candidate,
						     &generated.subset[label])) {
				generated.subset[label] = candidate;
				found[label] = true;
			}
		}
	}
	for (uint8_t label = 0; label < V34_SUBSET_LABEL_COUNT;
	     ++label) {
		if (!found[label])
			return V34_LATTICE_INVALID_ARGUMENT;
	}
	*slice = generated;
	return V34_LATTICE_OK;
}
