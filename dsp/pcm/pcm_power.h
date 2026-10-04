// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_POWER_H
#define PCM_POWER_H
#include <stdint.h>

/* V.90 Table 15 mean-square limits in 16-bit linear PCM units. V.91 uses
 * the same entries for codes 1..31; its code zero means unspecified. */
static inline uint32_t pcm_power_limit(uint8_t code)
{
	static const uint16_t rms[] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380,
		6020, 5684, 5368, 5068, 4784, 4516, 4264, 4024,
		3800, 3588, 3388, 3196, 3020, 2852, 2692, 2540,
	};
	return code < 32 ? (uint32_t)rms[code] * rms[code] : 0;
}
#endif
