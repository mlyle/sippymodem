// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_BRANCHES_H
#define SIPPYMODEM_V34_BRANCHES_H

#include "v34_lattice.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_MAX_BRANCH_COUNT = 32,
};

enum sippymodem_v34_branches_result {
	SIPPYMODEM_V34_BRANCHES_OK,
	SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT,
	SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE,
};

struct sippymodem_v34_branch {
	struct sippymodem_v34_lattice_candidate
		symbol[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_convolutional_input convolutional_input;
	uint8_t convolutional_output;
	double squared_error;
};

struct sippymodem_v34_branch_set {
	enum sippymodem_v34_trellis_selection selection;
	uint8_t count;
	/* Scheduled V0(m), shared by every branch in this interval. */
	uint8_t inversion_bit;
	struct sippymodem_v34_branch
		branch[SIPPYMODEM_V34_MAX_BRANCH_COUNT];
};

bool sippymodem_v34_branch_set_is_valid(
	const struct sippymodem_v34_branch_set *set);

/* Reduce two complete 2-D subset slices into the nearest point pair for every
 * trellis-visible 4-D subset. Array indices pack Y0, Y1, and Y2 in bits 0..2;
 * the 32-state code adds Y4 in bit 3, while the 64-state code adds Y3 and Y4
 * in bits 3 and 4. Inputs unused by a selected code are minimized over.
 * Metric ties select smaller total point magnitude and then the smaller label
 * pair. `inversion_bit` is the scheduled V0(m) and is retained in the set.
 * The destination is unchanged on failure. */
enum sippymodem_v34_branches_result sippymodem_v34_build_branch_set(
	const struct sippymodem_v34_subset_slice
		slices[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	enum sippymodem_v34_trellis_selection selection,
	struct sippymodem_v34_branch_set *set);

const char *sippymodem_v34_branches_result_name(
	enum sippymodem_v34_branches_result result);

#endif /* SIPPYMODEM_V34_BRANCHES_H */
