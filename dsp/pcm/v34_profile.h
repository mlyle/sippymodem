// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PROFILE_H
#define V34_PROFILE_H

#include "v34_call.h"

#include <stdint.h>

enum {
	V34_MIN_BIT_RATE = 2400,
	V34_MAX_BIT_RATE = 33600,
	V34_RATE_STEP = 2400,
};

enum v34_profile_result {
	V34_PROFILE_OK,
	V34_PROFILE_INVALID_ARGUMENT,
	V34_PROFILE_UNSUPPORTED_RATE,
};

/* Build the role-specific V.34 call policy. A zero symbol rate enables
 * negotiation across every supported cadence; a nonzero rate is exact. */
enum v34_profile_result v34_profile_init(
	struct v34_call_configuration *configuration,
	uint16_t *enabled_rate_mask, enum v34_station_role role,
	unsigned int minimum_bit_rate, unsigned int maximum_bit_rate,
	unsigned int symbol_rate);

#endif /* V34_PROFILE_H */
