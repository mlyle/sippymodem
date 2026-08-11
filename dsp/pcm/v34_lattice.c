// SPDX-License-Identifier: BSD-3-Clause

#include "v34_lattice.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

enum {
	V34_LATTICE_CLASS_PERIOD = 4,
	V34_LATTICE_SUBSET_PERIOD = 8,
	V34_LATTICE_CLASS_ONE = 1,
	V34_LATTICE_CLASS_THREE = 3,
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
	int minimum = -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT;
	int maximum = SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT;
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

static struct sippymodem_v34_grid_point candidate_point(
	const int16_t in_phase[2], const int16_t quadrature[2], uint8_t path)
{
	uint8_t in_phase_index = path >> 1;
	uint8_t quadrature_index = (uint8_t)(in_phase_index ^ (path & 1u));

	return (struct sippymodem_v34_grid_point){
		.in_phase = in_phase[in_phase_index],
		.quadrature = quadrature[quadrature_index],
	};
}

static bool candidate_error(
	const struct sippymodem_qam_point *estimate,
	const struct sippymodem_v34_grid_point *candidate, double *squared_error)
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
	const struct sippymodem_v34_grid_point *point)
{
	return (int32_t)point->in_phase * point->in_phase +
	       (int32_t)point->quadrature * point->quadrature;
}

static bool candidate_precedes(
	const struct sippymodem_v34_lattice_candidate *candidate,
	const struct sippymodem_v34_lattice_candidate *current)
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

enum sippymodem_v34_lattice_result sippymodem_v34_slice_odd_lattice(
	const struct sippymodem_qam_point *channel_estimate,
	struct sippymodem_v34_lattice_slice *slice)
{
	struct sippymodem_v34_lattice_slice generated = { 0 };
	int16_t in_phase[2];
	int16_t quadrature[2];

	if (channel_estimate == NULL || slice == NULL ||
	    !isfinite(channel_estimate->in_phase) ||
	    !isfinite(channel_estimate->quadrature))
		return SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT;
	in_phase[0] = nearest_residue_coordinate(
		channel_estimate->in_phase, V34_LATTICE_CLASS_ONE,
		V34_LATTICE_CLASS_PERIOD);
	in_phase[1] = nearest_residue_coordinate(
		channel_estimate->in_phase, V34_LATTICE_CLASS_THREE,
		V34_LATTICE_CLASS_PERIOD);
	quadrature[0] = nearest_residue_coordinate(
		channel_estimate->quadrature, V34_LATTICE_CLASS_ONE,
		V34_LATTICE_CLASS_PERIOD);
	quadrature[1] = nearest_residue_coordinate(
		channel_estimate->quadrature, V34_LATTICE_CLASS_THREE,
		V34_LATTICE_CLASS_PERIOD);
	for (uint8_t path = 0;
	     path < SIPPYMODEM_V34_LATTICE_CANDIDATE_COUNT; ++path) {
		struct sippymodem_v34_lattice_candidate *candidate =
			&generated.candidate[path];

		candidate->channel_output = candidate_point(
			in_phase, quadrature, path);
		if (sippymodem_v34_grid_subset_label(
			    &candidate->channel_output,
			    &candidate->subset_label) != SIPPYMODEM_V34_TRELLIS_OK)
			return SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT;
		if (!candidate_error(
			    channel_estimate, &candidate->channel_output,
			    &candidate->squared_error))
			return SIPPYMODEM_V34_LATTICE_NUMERIC_FAILURE;
	}
	*slice = generated;
	return SIPPYMODEM_V34_LATTICE_OK;
}

enum sippymodem_v34_lattice_result sippymodem_v34_slice_grid_subsets(
	const struct sippymodem_qam_point *channel_estimate,
	struct sippymodem_v34_subset_slice *slice)
{
	struct sippymodem_v34_subset_slice generated = { 0 };
	bool found[SIPPYMODEM_V34_SUBSET_LABEL_COUNT] = { false };

	if (channel_estimate == NULL || slice == NULL ||
	    !isfinite(channel_estimate->in_phase) ||
	    !isfinite(channel_estimate->quadrature))
		return SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT;
	for (int in_phase_residue = 1; in_phase_residue < 8;
	     in_phase_residue += 2) {
		int16_t in_phase = nearest_residue_coordinate(
			channel_estimate->in_phase, in_phase_residue,
			V34_LATTICE_SUBSET_PERIOD);

		for (int quadrature_residue = 1; quadrature_residue < 8;
		     quadrature_residue += 2) {
			struct sippymodem_v34_lattice_candidate candidate = {
				.channel_output = {
					.in_phase = in_phase,
					.quadrature = nearest_residue_coordinate(
						channel_estimate->quadrature,
						quadrature_residue,
						V34_LATTICE_SUBSET_PERIOD),
				},
			};
			uint8_t label;

			if (sippymodem_v34_grid_subset_label(
				    &candidate.channel_output, &label) !=
				    SIPPYMODEM_V34_TRELLIS_OK)
				return SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT;
			candidate.subset_label = label;
			if (!candidate_error(
				    channel_estimate, &candidate.channel_output,
				    &candidate.squared_error))
				return SIPPYMODEM_V34_LATTICE_NUMERIC_FAILURE;
			if (!found[label] || candidate_precedes(
						     &candidate,
						     &generated.subset[label])) {
				generated.subset[label] = candidate;
				found[label] = true;
			}
		}
	}
	for (uint8_t label = 0; label < SIPPYMODEM_V34_SUBSET_LABEL_COUNT;
	     ++label) {
		if (!found[label])
			return SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT;
	}
	*slice = generated;
	return SIPPYMODEM_V34_LATTICE_OK;
}

const char *sippymodem_v34_lattice_result_name(
	enum sippymodem_v34_lattice_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_LATTICE_OK:
		return "ok";
	case SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_LATTICE_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
