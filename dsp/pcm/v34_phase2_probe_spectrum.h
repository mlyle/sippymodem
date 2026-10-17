// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_SPECTRUM_H
#define V34_PHASE2_PROBE_SPECTRUM_H

#include "v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_probe_spectrum_result {
	V34_PHASE2_PROBE_SPECTRUM_OK,
	V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT,
};

struct v34_phase2_probe_spectrum_accumulator {
	double in_phase_sum;
	double quadrature_sum;
};

struct v34_phase2_probe_spectrum {
	struct v34_phase2_probe_spectrum_accumulator harmonics
		[V34_PHASE2_PROBE_HIGHEST_HARMONIC];
	uint32_t samples_observed;
};

struct v34_phase2_probe_spectrum_bin {
	double in_phase_peak;
	double quadrature_peak;
	double mean_square;
};

/* Element zero represents 150 Hz and each subsequent element advances by
 * V34_PHASE2_PROBE_TONE_SPACING_HZ. */
struct v34_phase2_probe_spectrum_measurement {
	struct v34_phase2_probe_spectrum_bin harmonics
		[V34_PHASE2_PROBE_HIGHEST_HARMONIC];
	uint32_t sample_count;
};

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_init(
	struct v34_phase2_probe_spectrum *spectrum);

/* Replace a sample at its absolute phase in a one-period observation without
 * changing its length. Sliding windows remove and add samples one period
 * apart at the same phase. Bin powers are invariant under the resulting
 * rotation relative to the window's new start. */
enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_replace(
	struct v34_phase2_probe_spectrum *spectrum, uint16_t period_index,
	int16_t outgoing, int16_t incoming);

bool v34_phase2_probe_spectrum_is_valid(
	const struct v34_phase2_probe_spectrum *spectrum);

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_sample(
	struct v34_phase2_probe_spectrum *spectrum,
	int16_t sample);

/* Complex peak amplitudes use the first observed sample as phase zero. The
 * normalization is exact for windows spanning whole 160-sample periods. */
enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_measure(
	const struct v34_phase2_probe_spectrum *spectrum,
	struct v34_phase2_probe_spectrum_measurement *measurement);

#endif
