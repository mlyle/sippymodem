// SPDX-License-Identifier: BSD-3-Clause

#include "v34_lattice.h"

#include "v34_trellis.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

enum {
	V34_LATTICE_CLASS_PERIOD = 4,
	V34_LATTICE_CLASS_ONE = 1,
	V34_LATTICE_CLASS_THREE = 3,
};

static int class_coordinate(int class_index, int residue)
{
	return residue + V34_LATTICE_CLASS_PERIOD * class_index;
}

static int16_t nearest_class_coordinate(double estimate, int residue)
{
	int minimum = -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT;
	int maximum = SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT;
	double position;
	int lower_index;
	int lower;
	int upper;
	double lower_error;
	double upper_error;

	while (((uint16_t)minimum & 3u) != (uint16_t)residue)
		minimum++;
	while (((uint16_t)maximum & 3u) != (uint16_t)residue)
		maximum--;
	if (estimate <= minimum)
		return (int16_t)minimum;
	if (estimate >= maximum)
		return (int16_t)maximum;
	position = (estimate - residue) / V34_LATTICE_CLASS_PERIOD;
	lower_index = (int)floor(position);
	lower = class_coordinate(lower_index, residue);
	upper = class_coordinate(lower_index + 1, residue);
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
	in_phase[0] = nearest_class_coordinate(
		channel_estimate->in_phase, V34_LATTICE_CLASS_ONE);
	in_phase[1] = nearest_class_coordinate(
		channel_estimate->in_phase, V34_LATTICE_CLASS_THREE);
	quadrature[0] = nearest_class_coordinate(
		channel_estimate->quadrature, V34_LATTICE_CLASS_ONE);
	quadrature[1] = nearest_class_coordinate(
		channel_estimate->quadrature, V34_LATTICE_CLASS_THREE);
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
