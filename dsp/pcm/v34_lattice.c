// SPDX-License-Identifier: BSD-3-Clause

#include "v34_lattice.h"
#include "sample_math.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

enum {
	V34_LATTICE_SUBSET_PERIOD = 8,
};

_Static_assert((V34_CHANNEL_COORDINATE_LIMIT + 1) %
	       V34_LATTICE_SUBSET_PERIOD == 0,
	       "lattice endpoints must bound complete residue periods");

static int16_t nearest_residue_coordinate(
	double estimate, int residue)
{
	/* For residues 1, 3, 5, 7, complete periods put the endpoints at
	 * residue - (limit + 1) and residue + (limit + 1) - period. */
	int minimum = residue - (V34_CHANNEL_COORDINATE_LIMIT + 1);
	int maximum = residue + V34_CHANNEL_COORDINATE_LIMIT + 1 -
		V34_LATTICE_SUBSET_PERIOD;
	double position;
	int lower_index;
	int lower;
	int upper;
	double lower_error;
	double upper_error;

	if (estimate <= minimum)
		return (int16_t)minimum;
	if (estimate >= maximum)
		return (int16_t)maximum;
	position = (estimate - residue) / V34_LATTICE_SUBSET_PERIOD;
	lower_index = (int)floor(position);
	lower = residue + V34_LATTICE_SUBSET_PERIOD * lower_index;
	upper = lower + V34_LATTICE_SUBSET_PERIOD;
	lower_error = fabs(estimate - lower);
	upper_error = fabs(estimate - upper);
	if (upper_error < lower_error ||
	    (upper_error == lower_error && abs(upper) < abs(lower)))
		return (int16_t)upper;
	return (int16_t)lower;
}

static bool candidate_error(
	const struct qam_point *observation,
	const struct v34_grid_point *candidate,
	const struct v34_power_normalizer *normalizer,
	double *squared_error)
{
	struct qam_point point;

	if (normalizer != NULL) {
		struct v34_q7_point precoded = {
			.in_phase = (int32_t)candidate->in_phase * V34_Q7_SCALE,
			.quadrature = (int32_t)candidate->quadrature * V34_Q7_SCALE,
		};

		if (v34_nonlinear_encode(
			    normalizer->nonlinear_parameter,
			    normalizer->precoded_mean_squared_magnitude,
			    &precoded, &point) != V34_NONLINEAR_OK)
			return false;
		point.in_phase *= normalizer->normalization_gain;
		point.quadrature *= normalizer->normalization_gain;
	} else {
		point = (struct qam_point){
			.in_phase = candidate->in_phase,
			.quadrature = candidate->quadrature,
		};
	}
	double generated = qam_point_distance_squared(observation, &point);

	if (!isfinite(generated))
		return false;
	*squared_error = generated;
	return true;
}

static bool candidate_precedes(
	const struct v34_lattice_candidate *candidate,
	const struct v34_lattice_candidate *current)
{
	int32_t candidate_magnitude;
	int32_t current_magnitude;

	if (candidate->squared_error != current->squared_error)
		return candidate->squared_error < current->squared_error;
	candidate_magnitude = (int32_t)sample_pair_power_s16(candidate->channel_output.in_phase,
		candidate->channel_output.quadrature);
	current_magnitude = (int32_t)sample_pair_power_s16(current->channel_output.in_phase,
		current->channel_output.quadrature);
	if (candidate_magnitude != current_magnitude)
		return candidate_magnitude < current_magnitude;
	if (candidate->channel_output.in_phase !=
	    current->channel_output.in_phase)
		return candidate->channel_output.in_phase <
		       current->channel_output.in_phase;
	return candidate->channel_output.quadrature <
	       current->channel_output.quadrature;
}

static enum v34_lattice_result slice_grid_subsets(
	const struct qam_point *channel_estimate,
	const struct qam_point *normalized_observation,
	const struct v34_power_normalizer *normalizer,
	struct v34_subset_slice *slice)
{
	struct v34_subset_slice generated = { 0 };
	bool found[V34_SUBSET_LABEL_COUNT] = { false };
	const struct qam_point *observation = normalized_observation == NULL ?
		channel_estimate : normalized_observation;

	if (!point_is_finite(channel_estimate) || slice == NULL ||
	    ((normalized_observation == NULL) != (normalizer == NULL)) ||
	    (normalized_observation != NULL &&
	     (!point_is_finite(normalized_observation) ||
	      !v34_power_normalizer_is_valid(normalizer))))
		return V34_LATTICE_INVALID_ARGUMENT;
	for (int in_phase_residue = 1; in_phase_residue < 8;
	     in_phase_residue += 2) {
		int16_t in_phase = nearest_residue_coordinate(
			channel_estimate->in_phase, in_phase_residue);

		for (int quadrature_residue = 1; quadrature_residue < 8;
		     quadrature_residue += 2) {
			struct v34_lattice_candidate candidate = {
				.channel_output = {
					.in_phase = in_phase,
					.quadrature = nearest_residue_coordinate(
						channel_estimate->quadrature, quadrature_residue),
				},
			};
			uint8_t label;

			if (v34_grid_subset_label(
				    &candidate.channel_output, &label) !=
				    V34_TRELLIS_OK)
				return V34_LATTICE_INVALID_ARGUMENT;
			candidate.subset_label = label;
			if (!candidate_error(
				    observation,
				    &candidate.channel_output, normalizer,
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

enum v34_lattice_result v34_slice_grid_subsets(
	const struct qam_point *channel_estimate,
	struct v34_subset_slice *slice)
{
	return slice_grid_subsets(channel_estimate, NULL, NULL, slice);
}

enum v34_lattice_result v34_slice_grid_subsets_projected(
	const struct qam_point *channel_estimate,
	const struct qam_point *normalized_observation,
	const struct v34_power_normalizer *normalizer,
	struct v34_subset_slice *slice)
{
	return slice_grid_subsets(
		channel_estimate, normalized_observation, normalizer, slice);
}
