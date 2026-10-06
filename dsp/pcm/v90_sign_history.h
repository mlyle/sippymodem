// SPDX-License-Identifier: BSD-3-Clause
#ifndef V90_SIGN_HISTORY_H
#define V90_SIGN_HISTORY_H

#include "packed_bits.h"
#include "v90_cp.h"

/* V.90 5.4.5 uses one six-symbol data frame, divided into Sr shaping frames
 * when Sr is nonzero. Keep the unused mode and history positions canonical. */
static inline bool v90_sign_history_is_valid(uint8_t sign_redundancy,
	uint8_t previous_odd, uint8_t previous_pcm_sign,
	const uint8_t previous_shaping_signs[V90_CP_FRAME_INTERVALS])
{
	if (sign_redundancy > 3 || previous_odd > 1 || previous_pcm_sign > 1)
		return false;
	size_t active = sign_redundancy ? V90_CP_FRAME_INTERVALS / sign_redundancy : 0;
	return unpacked_bits_are_binary(previous_shaping_signs, active) &&
	       memory_region_is_zero(previous_shaping_signs + active,
		V90_CP_FRAME_INTERVALS - active) &&
	       (sign_redundancy ? previous_pcm_sign == 0 : previous_odd == 0);
}

#endif
