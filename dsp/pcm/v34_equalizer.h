// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_EQUALIZER_H
#define V34_EQUALIZER_H

#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_EQUALIZER_TAPS = 80,
	V34_EQUALIZER_CENTER_FIRST = 36,
	V34_EQUALIZER_CENTER_TAPS = 8,
	V34_EQUALIZER_IDENTITY_TAP = 40,
};

enum v34_equalizer_result {
	V34_EQUALIZER_OK,
	V34_EQUALIZER_INVALID_ARGUMENT,
	V34_EQUALIZER_INVALID_STATE,
};

struct v34_equalizer_accumulator {
	int32_t in_phase;
	int32_t quadrature;
};

/* Owned two-samples-per-symbol complex equalizer. Coefficients are Q12;
 * residues retain the low half of each Q16 adaptation word. */
struct v34_equalizer {
	int16_t in_phase_delay[V34_EQUALIZER_TAPS];
	int16_t quadrature_delay[V34_EQUALIZER_TAPS];
	int16_t in_phase_coefficient[V34_EQUALIZER_TAPS];
	int16_t quadrature_coefficient[V34_EQUALIZER_TAPS];
	uint16_t in_phase_residue[V34_EQUALIZER_TAPS];
	uint16_t quadrature_residue[V34_EQUALIZER_TAPS];
	uint32_t index;
	bool initialized;
};

enum v34_equalizer_result v34_equalizer_init(
	struct v34_equalizer *equalizer);
bool v34_equalizer_is_valid(const struct v34_equalizer *equalizer);

/* Clear only the eight center coefficients while preserving their residues. */
enum v34_equalizer_result v34_equalizer_clear_center(
	struct v34_equalizer *equalizer);

enum v34_equalizer_result v34_equalizer_push(
	struct v34_equalizer *equalizer, struct v34_analytic_sample sample);

/* Evaluate the delay line without mutating it. The wide accumulator preserves
 * exact modulo-2^32 fixed-point arithmetic for the receive controller. */
enum v34_equalizer_result v34_equalizer_filter(
	const struct v34_equalizer *equalizer,
	struct v34_equalizer_accumulator *output);

/* Adapt every tap while retaining the low Q16 residue. */
enum v34_equalizer_result v34_equalizer_adapt(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error);

/* Adapt only the center eight taps, rounding each update to a Q12 word. */
enum v34_equalizer_result v34_equalizer_adapt_center(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error);

#endif /* V34_EQUALIZER_H */
