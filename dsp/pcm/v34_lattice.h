// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_LATTICE_H
#define V34_LATTICE_H

#include "qam.h"
#include "v34_symbol_power.h"
#include "v34_trellis.h"

#include <stdint.h>

enum v34_lattice_result {
	V34_LATTICE_OK,
	V34_LATTICE_INVALID_ARGUMENT,
	V34_LATTICE_NUMERIC_FAILURE,
};

struct v34_lattice_candidate {
	struct v34_grid_point channel_output;
	uint8_t subset_label;
	double squared_error;
};

struct v34_subset_slice {
	/* Indexed by the three-bit Figure 9 subset label. */
	struct v34_lattice_candidate
		subset[V34_SUBSET_LABEL_COUNT];
};

/* Find the nearest bounded odd-grid point in each of Figure 9's eight
 * subsets. Exact metric ties select the point with smaller magnitude, then
 * the lexicographically smaller coordinates. The destination is unchanged on
 * failure. */
enum v34_lattice_result v34_slice_grid_subsets(
	const struct qam_point *channel_estimate,
	struct v34_subset_slice *slice);

/* Slice on the same odd lattice while scoring candidate symbols in the
 * normalized, nonlinear-encoded signal plane.  This form applies when the
 * precoder coefficients are zero, so channel_output is also x(n). */
enum v34_lattice_result v34_slice_grid_subsets_projected(
	const struct qam_point *channel_estimate,
	const struct qam_point *normalized_observation,
	const struct v34_power_normalizer *normalizer,
	struct v34_subset_slice *slice);

#endif /* V34_LATTICE_H */
