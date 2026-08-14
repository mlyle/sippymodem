// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_MODE_H
#define V90_UPSTREAM_MODE_H

#include "v34_data_mode.h"
#include "v34_phase3_profile.h"
#include "v90_cp.h"
#include "v90_mp.h"

enum v90_upstream_mode_result {
	V90_UPSTREAM_MODE_OK,
	V90_UPSTREAM_MODE_INVALID_ARGUMENT,
	V90_UPSTREAM_MODE_INVALID_PARAMETERS,
	V90_UPSTREAM_MODE_NO_COMMON_RATE,
};

/* Select the conventional upstream direction common to final CP, MP, and
 * the trained analogue-modem profile. The output changes only on success. */
enum v90_upstream_mode_result v90_upstream_mode_select(
	const struct v90_cp_parameters *cp,
	const struct v90_mp_record *mp,
	const struct v34_phase3_profile *transmitting_profile,
	struct v34_data_direction_parameters *direction);

#endif /* V90_UPSTREAM_MODE_H */
