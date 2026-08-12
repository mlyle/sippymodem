// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_SELECTION_H
#define V34_PHASE2_SELECTION_H

#include "v34_info0.h"
#include "v34_phase3_profile.h"

#include <stdbool.h>

enum v34_phase2_selection_result {
	V34_PHASE2_SELECTION_OK,
	V34_PHASE2_SELECTION_INVALID_ARGUMENT,
	V34_PHASE2_SELECTION_INCONSISTENT_INFO1C,
	V34_PHASE2_SELECTION_INCONSISTENT_INFO1A,
};

/* INFO1c describes the answerer's possible transmit profiles. A usable entry
 * must be supported by both endpoints and by the answerer's advertised
 * transmitter configuration. Power-reduction requests are likewise bounded
 * by the answerer's INFO0. */
bool v34_phase2_info1c_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c);

/* INFO1a selects one direction from INFO1c and supplies the caller's selected
 * transmit profile. Both rates must respect each endpoint's INFO0 asymmetry
 * limit. */
bool v34_phase2_info1a_is_consistent(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a);

/* Validate the complete Phase 2 selection and derive both transmitter-owned
 * Phase 3 profiles atomically. The outputs must be distinct. */
enum v34_phase2_selection_result v34_phase2_select_profiles(
	const struct v34_info0 *caller_info0,
	const struct v34_info0 *answerer_info0,
	const struct v34_info1c *info1c,
	const struct v34_info1a *info1a,
	struct v34_phase3_profile *caller_transmit,
	struct v34_phase3_profile *answerer_transmit);

const char *v34_phase2_selection_result_name(
	enum v34_phase2_selection_result result);

#endif /* V34_PHASE2_SELECTION_H */
