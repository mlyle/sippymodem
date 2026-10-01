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

/* One full turn is 0x8000 phase units; signed inputs wrap modulo a turn.
 * Interpolated Q14 components are bounded by +/-16384, with at most two
 * Q14 units of error from the analytic sine and cosine. */
struct fixed_phase_q14 fixed_phase_sin_cos_q14(int16_t phase);

/* Unsigned carrier phase uses 0x10000 units per full turn. */
struct fixed_phase_q14 fixed_phase_sin_cos_u16_q14(uint16_t phase);

/* The modem's compact 256-point cycle uses a positive Q14 endpoint and a
 * signed negative endpoint. */
int16_t fixed_phase_cosine_u8_q14(uint8_t phase);

#endif
