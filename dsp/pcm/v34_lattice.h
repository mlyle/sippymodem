// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_LATTICE_H
#define SIPPYMODEM_V34_LATTICE_H

#include "sippymodem_qam.h"
#include "v34_trellis.h"

#include <stdint.h>

enum {
	SIPPYMODEM_V34_LATTICE_CANDIDATE_COUNT = 4,
};

enum sippymodem_v34_lattice_result {
	SIPPYMODEM_V34_LATTICE_OK,
	SIPPYMODEM_V34_LATTICE_INVALID_ARGUMENT,
	SIPPYMODEM_V34_LATTICE_NUMERIC_FAILURE,
};

struct sippymodem_v34_lattice_candidate {
	struct sippymodem_v34_grid_point channel_output;
	uint8_t subset_label;
	double squared_error;
};

struct sippymodem_v34_lattice_slice {
	struct sippymodem_v34_lattice_candidate
		candidate[SIPPYMODEM_V34_LATTICE_CANDIDATE_COUNT];
};

struct sippymodem_v34_subset_slice {
	/* Indexed by the three-bit Figure 9 subset label. */
	struct sippymodem_v34_lattice_candidate
		subset[SIPPYMODEM_V34_SUBSET_LABEL_COUNT];
};

/* Slice an already conditioned estimate of y(n), expressed in integer-grid
 * units, to the four surrounding odd-lattice paths. Path order is
 * (1,1), (1,3), (3,3), (3,1) modulo four on the two axes. Each coordinate is
 * bounded to the legal channel interval; an exact same-class tie selects the
 * coordinate with smaller magnitude. The destination is unchanged on
 * failure. Front-end scaling and nonlinear inversion precede this boundary. */
enum sippymodem_v34_lattice_result sippymodem_v34_slice_odd_lattice(
	const struct sippymodem_qam_point *channel_estimate,
	struct sippymodem_v34_lattice_slice *slice);

/* Find the nearest bounded odd-grid point in each of Figure 9's eight
 * subsets. Exact metric ties select the point with smaller magnitude, then
 * the lexicographically smaller coordinates. The destination is unchanged on
 * failure. */
enum sippymodem_v34_lattice_result sippymodem_v34_slice_grid_subsets(
	const struct sippymodem_qam_point *channel_estimate,
	struct sippymodem_v34_subset_slice *slice);

const char *sippymodem_v34_lattice_result_name(
	enum sippymodem_v34_lattice_result result);

#endif /* SIPPYMODEM_V34_LATTICE_H */
