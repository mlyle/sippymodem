// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CONTROL_TARGET_H
#define V92_CONTROL_TARGET_H

#include "g711.h"
#include "qam_equalizer.h"
#include "v92_phase4_modulator.h"

/* Nearest quantized level for this nominal control symbol, allowing LU to
 * lie anywhere in its measured G.711 cell. Only constellation and LU are
 * read from the demodulator. Coding, nominal decisions, and residual evidence
 * remain the caller's responsibility. Collapsed nominal magnitudes retain
 * the nominal quantized target. Invalid calls leave target unchanged. */
bool v92_control_target(
	enum g711_law law, const struct v92_phase4_demodulator *demodulator,
	int16_t nominal, double observed, int16_t *target);

/* Resolve a pending FIR observation in equalized PCM units. Bias is added
 * after filtering, so physical history remains available to channel fitting.
 * Known training may learn the bias as a constant LU-sized basis in the same
 * normalized-LMS update; control decisions retain it. The three objects must
 * be disjoint. Rejection changes neither the equalizer nor the bias. */
bool v92_control_adapt(struct qam_equalizer *equalizer,
	enum g711_law law, const struct v92_phase4_demodulator *demodulator,
	int16_t nominal, double adaptation, double *output_bias, bool learn_bias);

#endif
