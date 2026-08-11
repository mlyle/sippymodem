// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_spectrum.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static const double V34_PHASE2_PROBE_SPECTRUM_TWO_PI =
	6.283185307179586476925286766559;

_Static_assert(
	V34_PHASE2_PROBE_PERIOD_SAMPLES *
		V34_PHASE2_PROBE_TONE_SPACING_HZ ==
		V34_PHASE2_SAMPLE_RATE * 3,
	"V.34 probe spectrum must contain three fundamental cycles");
_Static_assert(
	V34_PHASE2_PROBE_HIGHEST_HARMONIC *
		V34_PHASE2_PROBE_TONE_SPACING_HZ <
		V34_PHASE2_SAMPLE_RATE / 2,
	"V.34 probe harmonics must remain below Nyquist");

bool v34_phase2_probe_spectrum_is_valid(
	const struct v34_phase2_probe_spectrum *spectrum)
{
	if (spectrum == NULL ||
	    spectrum->window_start_sample > spectrum->samples_observed)
		return false;
	for (size_t harmonic = 0;
	     harmonic < V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		if (!isfinite(spectrum->harmonics[harmonic].in_phase_sum) ||
		    !isfinite(spectrum->harmonics[harmonic].quadrature_sum))
			return false;
	}
	return true;
}

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_init(
	struct v34_phase2_probe_spectrum *spectrum)
{
	struct v34_phase2_probe_spectrum initialized = { 0 };

	if (spectrum == NULL ||
	    !v34_phase2_probe_spectrum_is_valid(&initialized))
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	*spectrum = initialized;
	return V34_PHASE2_PROBE_SPECTRUM_OK;
}

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_restart_window(
	struct v34_phase2_probe_spectrum *spectrum)
{
	struct v34_phase2_probe_spectrum restarted;

	if (!v34_phase2_probe_spectrum_is_valid(spectrum))
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	restarted = *spectrum;
	memset(restarted.harmonics, 0, sizeof(restarted.harmonics));
	restarted.window_start_sample = restarted.samples_observed;
	if (!v34_phase2_probe_spectrum_is_valid(&restarted))
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	*spectrum = restarted;
	return V34_PHASE2_PROBE_SPECTRUM_OK;
}

static void accumulate_sample(
	struct v34_phase2_probe_spectrum *spectrum,
	int16_t sample)
{
	uint32_t period_index =
		spectrum->samples_observed %
		V34_PHASE2_PROBE_PERIOD_SAMPLES;
	double angle = V34_PHASE2_PROBE_SPECTRUM_TWO_PI * 3.0 *
		       (double)period_index /
		       (double)V34_PHASE2_PROBE_PERIOD_SAMPLES;
	double fundamental_in_phase = cos(angle);
	double fundamental_quadrature = sin(angle);
	double harmonic_in_phase = 1.0;
	double harmonic_quadrature = 0.0;

	/* Generate z through z^25 from one fundamental basis. Re-deriving z at
	 * every sample prevents oscillator drift without a coefficient table. */
	for (size_t harmonic = 0;
	     harmonic < V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		double next_in_phase =
			harmonic_in_phase * fundamental_in_phase -
			harmonic_quadrature * fundamental_quadrature;
		double next_quadrature =
			harmonic_quadrature * fundamental_in_phase +
			harmonic_in_phase * fundamental_quadrature;

		harmonic_in_phase = next_in_phase;
		harmonic_quadrature = next_quadrature;
		spectrum->harmonics[harmonic].in_phase_sum +=
			(double)sample * harmonic_in_phase;
		spectrum->harmonics[harmonic].quadrature_sum -=
			(double)sample * harmonic_quadrature;
	}
}

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_sample(
	struct v34_phase2_probe_spectrum *spectrum,
	int16_t sample)
{
	struct v34_phase2_probe_spectrum next;

	if (!v34_phase2_probe_spectrum_is_valid(spectrum) ||
	    spectrum->samples_observed == UINT32_MAX)
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	next = *spectrum;
	accumulate_sample(&next, sample);
	next.samples_observed++;
	if (!v34_phase2_probe_spectrum_is_valid(&next))
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	*spectrum = next;
	return V34_PHASE2_PROBE_SPECTRUM_OK;
}

enum v34_phase2_probe_spectrum_result
v34_phase2_probe_spectrum_measure(
	const struct v34_phase2_probe_spectrum *spectrum,
	struct v34_phase2_probe_spectrum_measurement *measurement)
{
	struct v34_phase2_probe_spectrum_measurement measured = { 0 };
	double peak_scale;

	if (!v34_phase2_probe_spectrum_is_valid(spectrum) ||
	    measurement == NULL)
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	measured.sample_count =
		spectrum->samples_observed - spectrum->window_start_sample;
	if (measured.sample_count == 0)
		return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	peak_scale = 2.0 / (double)measured.sample_count;
	for (size_t harmonic = 0;
	     harmonic < V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		struct v34_phase2_probe_spectrum_bin *bin =
			&measured.harmonics[harmonic];

		bin->in_phase_peak =
			spectrum->harmonics[harmonic].in_phase_sum * peak_scale;
		bin->quadrature_peak =
			spectrum->harmonics[harmonic].quadrature_sum * peak_scale;
		bin->mean_square =
			(bin->in_phase_peak * bin->in_phase_peak +
			 bin->quadrature_peak * bin->quadrature_peak) /
			2.0;
		if (!isfinite(bin->in_phase_peak) ||
		    !isfinite(bin->quadrature_peak) ||
		    !isfinite(bin->mean_square))
			return V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT;
	}
	*measurement = measured;
	return V34_PHASE2_PROBE_SPECTRUM_OK;
}

const char *v34_phase2_probe_spectrum_result_name(
	enum v34_phase2_probe_spectrum_result result)
{
	switch (result) {
	case V34_PHASE2_PROBE_SPECTRUM_OK:
		return "ok";
	case V34_PHASE2_PROBE_SPECTRUM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
