// SPDX-License-Identifier: BSD-3-Clause

#ifndef FIXED_PHASE_H
#define FIXED_PHASE_H

#include <stdint.h>

/* One full turn is 0x8000 phase units. */
uint16_t fixed_phase_atan2_u15(int16_t x, int16_t y);

struct fixed_phase_q14 {
	int16_t sine;
	int16_t cosine;
};

/* Preserve the modem's signed phase interpolation over the complete s16
 * domain.  Returning the pair by value leaves no partial output state. */
struct fixed_phase_q14 fixed_phase_sin_cos_q14(int16_t phase);

#endif
