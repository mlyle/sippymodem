// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_16_STATE_BRANCHES_H
#define V34_16_STATE_BRANCHES_H

#include "qam.h"
#include "v34_16_state_survivor.h"
#include "v34_analytic_filter.h"
#include "v34_frame_schedule.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_16_state_branches_result {
	V34_16_STATE_BRANCHES_OK,
	V34_16_STATE_BRANCHES_INVALID_ARGUMENT,
	V34_16_STATE_BRANCHES_UNSUPPORTED_SELECTION,
	V34_16_STATE_BRANCHES_OUT_OF_RANGE,
};

/* The fixed branch metric is selected by the negotiated symbol rate, primary
 * rate, and shaping mode. It is independent of the auxiliary-channel bit. */
struct v34_16_state_branch_profile {
	enum v34_symbol_rate symbol_rate;
	uint16_t metric;
	uint8_t primary_data_rate_2400_bps_units;
	uint8_t distance_shift;
	bool expanded_shaping;
};

enum v34_16_state_branches_result v34_16_state_branch_profile_init(
	struct v34_16_state_branch_profile *profile,
	const struct v34_frame_geometry *geometry, bool expanded_shaping);

bool v34_16_state_branch_profile_is_valid(
	const struct v34_16_state_branch_profile *profile);

/* Convert an already conditioned integer-grid estimate to the negotiated
 * fixed metric. This is the explicit handoff from the current floating-point
 * channel owner; fixed branch slicing itself performs no rescaling. */
enum v34_16_state_branches_result v34_16_state_branch_quantize(
	const struct v34_16_state_branch_profile *profile,
	const struct qam_point *normalized,
	struct v34_analytic_sample *fixed);

/* Build the eight branches in the order consumed by the fixed 16-state
 * survivor. Each coordinate is an odd integer-grid value; each metric uses
 * the negotiated fixed scaling and uint16_t wrap semantics. */
enum v34_16_state_branches_result v34_16_state_branches_build(
	const struct v34_16_state_branch_profile *profile,
	const struct v34_analytic_sample
		samples[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct v34_16_state_branch
		branches[V34_16_STATE_BRANCH_COUNT]);

#endif /* V34_16_STATE_BRANCHES_H */
