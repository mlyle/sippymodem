// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_BRANCHES_H
#define V34_BRANCHES_H

#include "v34_lattice.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_MAX_BRANCH_COUNT = 32,
};

enum v34_branches_result {
	V34_BRANCHES_OK,
	V34_BRANCHES_INVALID_ARGUMENT,
	V34_BRANCHES_NUMERIC_FAILURE,
};

struct v34_branch {
	struct v34_lattice_candidate
		symbol[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_convolutional_input convolutional_input;
	uint8_t convolutional_output;
	double squared_error;
};

struct v34_branch_set {
	enum v34_trellis_selection selection;
	uint8_t count;
	/* Scheduled V0(m), shared by every branch in this interval. */
	uint8_t inversion_bit;
	struct v34_branch
		branch[V34_MAX_BRANCH_COUNT];
};

bool v34_branch_set_is_valid(
	const struct v34_branch_set *set);

/* Reduce two complete 2-D subset slices into the nearest point pair for every
 * trellis selection. Array indices pack Y0, Y1, and Y2 in bits 0..2; the
 * 32-state code adds Y4 in bit 3, while the 64-state code adds Y3 and Y4 in
 * bits 3 and 4. Inputs unused by a selected code are minimized over.
 * Metric ties select smaller total point magnitude and then the smaller label
 * pair. `inversion_bit` is the scheduled V0(m) and is retained in the set.
 * The destination is unchanged on failure. */
enum v34_branches_result v34_build_branch_set(
	const struct v34_subset_slice
		slices[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	enum v34_trellis_selection selection,
	struct v34_branch_set *set);

#endif /* V34_BRANCHES_H */
