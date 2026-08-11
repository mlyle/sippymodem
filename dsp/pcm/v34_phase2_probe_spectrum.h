// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_H
#define SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_H

#include "v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_probe_spectrum_result {
	SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_spectrum_accumulator {
	double in_phase_sum;
	double quadrature_sum;
};

struct sippymodem_v34_phase2_probe_spectrum {
	struct sippymodem_v34_phase2_probe_spectrum_accumulator harmonics
		[SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC];
	uint32_t samples_observed;
	uint32_t window_start_sample;
};

struct sippymodem_v34_phase2_probe_spectrum_bin {
	double in_phase_peak;
	double quadrature_peak;
	double mean_square;
};

/* Element zero represents 150 Hz and each subsequent element advances by
 * SIPPYMODEM_V34_PHASE2_PROBE_TONE_SPACING_HZ. */
struct sippymodem_v34_phase2_probe_spectrum_measurement {
	struct sippymodem_v34_phase2_probe_spectrum_bin harmonics
		[SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC];
	uint32_t sample_count;
};

enum sippymodem_v34_phase2_probe_spectrum_result
sippymodem_v34_phase2_probe_spectrum_init(
	struct sippymodem_v34_phase2_probe_spectrum *spectrum);
bool sippymodem_v34_phase2_probe_spectrum_is_valid(
	const struct sippymodem_v34_phase2_probe_spectrum *spectrum);

/* Clear the current measurement window while retaining the harmonic phase
 * established by all samples observed since initialization. */
enum sippymodem_v34_phase2_probe_spectrum_result
sippymodem_v34_phase2_probe_spectrum_restart_window(
	struct sippymodem_v34_phase2_probe_spectrum *spectrum);
enum sippymodem_v34_phase2_probe_spectrum_result
sippymodem_v34_phase2_probe_spectrum_sample(
	struct sippymodem_v34_phase2_probe_spectrum *spectrum,
	int16_t sample);

/* Complex peak amplitudes use the first observed sample as phase zero. The
 * normalization is exact for windows spanning whole 160-sample periods. */
enum sippymodem_v34_phase2_probe_spectrum_result
sippymodem_v34_phase2_probe_spectrum_measure(
	const struct sippymodem_v34_phase2_probe_spectrum *spectrum,
	struct sippymodem_v34_phase2_probe_spectrum_measurement *measurement);

const char *sippymodem_v34_phase2_probe_spectrum_result_name(
	enum sippymodem_v34_phase2_probe_spectrum_result result);

#endif
