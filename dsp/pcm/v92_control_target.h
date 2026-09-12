// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CONTROL_TARGET_H
#define V92_CONTROL_TARGET_H

#include "g711.h"
#include "v92_phase4_modulator.h"

/* Nearest quantized level for this nominal control symbol, allowing LU to
 * lie anywhere in its measured G.711 cell. Only constellation and LU are
 * read from the demodulator. Coding, nominal decisions, and residual evidence
 * remain the caller's responsibility. Collapsed nominal magnitudes retain
 * the nominal quantized target. Invalid calls leave target unchanged. */
bool v92_control_target(
	enum g711_law law, const struct v92_phase4_demodulator *demodulator,
	int16_t nominal, double observed, int16_t *target);

#endif
