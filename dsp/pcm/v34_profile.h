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

/* Source-supported V.34 line capabilities shared with PCM negotiation.
 * Protocol/role restrictions are applied by the profile that advertises them;
 * rate selection and INFO0 acknowledgment remain with their existing owners. */
static inline struct v34_info0 v34_profile_info0(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.constellation_1664_supported = true,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

/* Build the role-specific V.34 call policy. A zero symbol rate enables
 * negotiation across every supported cadence; a nonzero rate is exact. */
enum v34_profile_result v34_profile_init(
	struct v34_call_configuration *configuration,
	uint16_t *enabled_rate_mask, enum v34_station_role role,
	unsigned int minimum_bit_rate, unsigned int maximum_bit_rate,
	unsigned int symbol_rate);

#endif /* V34_PROFILE_H */
