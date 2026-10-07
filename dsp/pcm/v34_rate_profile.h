// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_RATE_PROFILE_H
#define V34_RATE_PROFILE_H

#include "v34_info1.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_RATE_PROFILE_UNITS = 14,
};

enum v34_rate_profile_result {
	V34_RATE_PROFILE_OK,
	V34_RATE_PROFILE_INVALID_ARGUMENT,
	V34_RATE_PROFILE_UNSUPPORTED_SELECTION,
};

/* Select the implementation's fixed-point level for one negotiated direction.
 * Zero table entries are unsupported combinations, not usable levels.  The
 * destination remains unchanged on every rejection. */
enum v34_rate_profile_result v34_rate_profile_level(
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units, bool expanded_shaping,
	uint16_t *level);

#endif /* V34_RATE_PROFILE_H */
