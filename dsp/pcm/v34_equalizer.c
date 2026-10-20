// SPDX-License-Identifier: BSD-3-Clause

#include "v34_equalizer.h"

#include "fir.h"
#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool v34_equalizer_is_valid(const struct v34_equalizer *equalizer)
{
	return equalizer != NULL && equalizer->initialized &&
	       equalizer->tap_count >= V34_EQUALIZER_CENTER_TAPS &&
	       equalizer->tap_count <= V34_EQUALIZER_MAX_TAPS &&
	       equalizer->identity_tap >= V34_EQUALIZER_CENTER_TAPS / 2u &&
	       equalizer->identity_tap <=
		       equalizer->tap_count - V34_EQUALIZER_CENTER_TAPS / 2u &&
	       (equalizer->tap_count - equalizer->identity_tap) % 2u == 0u &&
	       equalizer->index < equalizer->tap_count;
}

enum v34_equalizer_result v34_equalizer_init(
	struct v34_equalizer *equalizer)
{
	return v34_equalizer_init_geometry(
		equalizer, V34_EQUALIZER_TAPS,
		(V34_EQUALIZER_TAPS - V34_EQUALIZER_IDENTITY_TAP) / 2u);
}

enum v34_equalizer_result v34_equalizer_init_geometry(
	struct v34_equalizer *equalizer, uint16_t tap_count,
	uint16_t decision_delay_symbols)
{
	struct v34_equalizer initialized;

	if (equalizer == NULL || tap_count < V34_EQUALIZER_CENTER_TAPS ||
	    tap_count > V34_EQUALIZER_MAX_TAPS ||
	    decision_delay_symbols < V34_EQUALIZER_CENTER_TAPS / 4u ||
	    decision_delay_symbols >
		    (tap_count - V34_EQUALIZER_CENTER_TAPS / 2u) / 2u)
		return V34_EQUALIZER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.tap_count = tap_count;
	initialized.identity_tap = tap_count - 2u * decision_delay_symbols;
	initialized.in_phase_coefficient[initialized.identity_tap] = 0x1000;
	initialized.initialized = true;
	*equalizer = initialized;
	return V34_EQUALIZER_OK;
}

enum v34_equalizer_result v34_equalizer_clear_center(
	struct v34_equalizer *equalizer)
{
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	for (size_t tap = equalizer->identity_tap - V34_EQUALIZER_CENTER_TAPS / 2u;
	     tap < equalizer->identity_tap + V34_EQUALIZER_CENTER_TAPS / 2u;
	     ++tap) {
		equalizer->in_phase_coefficient[tap] = 0;
		equalizer->quadrature_coefficient[tap] = 0;
	}
	return V34_EQUALIZER_OK;
}

enum v34_equalizer_result v34_equalizer_push(
	struct v34_equalizer *equalizer, struct v34_analytic_sample sample)
{
	uint32_t index;

	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	index = equalizer->index;
	equalizer->in_phase_delay[index] = sample.in_phase;
	equalizer->quadrature_delay[index] = sample.quadrature;
	equalizer->index = index + 1u == equalizer->tap_count ? 0 : index + 1u;
	return V34_EQUALIZER_OK;
}

static void accumulate_tap(
	const struct v34_equalizer *equalizer,
	size_t sample_index, size_t coefficient_index,
	uint32_t *in_phase, uint32_t *quadrature)
{
	fir_accumulate_complex_s16(
		equalizer->in_phase_delay[sample_index],
		equalizer->quadrature_delay[sample_index],
		equalizer->in_phase_coefficient[coefficient_index],
		equalizer->quadrature_coefficient[coefficient_index],
		in_phase, quadrature);
}

enum v34_equalizer_result v34_equalizer_filter(
	const struct v34_equalizer *equalizer,
	struct v34_equalizer_accumulator *output)
{
	struct v34_equalizer_accumulator generated;
	uint32_t in_phase = 0;
	uint32_t quadrature = 0;
	size_t coefficient = 0;

	if (output == NULL || memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    output, sizeof(*output)))
		return V34_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	for (size_t sample = equalizer->index;
	     sample < equalizer->tap_count; ++sample, ++coefficient)
		accumulate_tap(equalizer, sample, coefficient,
			       &in_phase, &quadrature);
	for (size_t sample = 0; sample < equalizer->index;
	     ++sample, ++coefficient)
		accumulate_tap(equalizer, sample, coefficient,
			       &in_phase, &quadrature);
	generated.in_phase = (int32_t)in_phase;
	generated.quadrature = (int32_t)quadrature;
	*output = generated;
	return V34_EQUALIZER_OK;
}

static inline void adapt_tap(
	struct v34_equalizer *equalizer,
	const int16_t *restrict in_phase_delay, const int16_t *restrict quadrature_delay,
	size_t sample_index, size_t coefficient_index,
	struct v34_analytic_sample error, bool preserve_residue)
{
	int16_t sample_in_phase = in_phase_delay[sample_index];
	int16_t sample_quadrature = quadrature_delay[sample_index];
	uint32_t in_phase = pack_s16_u16_words(
		equalizer->in_phase_coefficient[coefficient_index],
		preserve_residue ?
			equalizer->in_phase_residue[coefficient_index] : 0);
	uint32_t quadrature = pack_s16_u16_words(
		equalizer->quadrature_coefficient[coefficient_index],
		preserve_residue ?
			equalizer->quadrature_residue[coefficient_index] : 0);

	fir_subtract_correlation_s16(sample_in_phase, sample_quadrature,
		error.in_phase, error.quadrature, &in_phase, &quadrature);
	if (!preserve_residue) {
		in_phase += UINT32_C(0x8000);
		quadrature += UINT32_C(0x8000);
	}
	equalizer->in_phase_coefficient[coefficient_index] = high_s16_word(in_phase);
	equalizer->quadrature_coefficient[coefficient_index] =
		high_s16_word(quadrature);
	if (preserve_residue) {
		equalizer->in_phase_residue[coefficient_index] = (uint16_t)in_phase;
		equalizer->quadrature_residue[coefficient_index] =
			(uint16_t)quadrature;
	}
}

static inline void adapt_history(
	struct v34_equalizer *equalizer,
	const int16_t *restrict in_phase_delay, const int16_t *restrict quadrature_delay,
	uint32_t index, struct v34_analytic_sample error, bool center_only)
{
	size_t coefficient = 0;

	if (center_only) {
		for (size_t offset = 0; offset < V34_EQUALIZER_CENTER_TAPS; ++offset) {
			coefficient = equalizer->identity_tap -
				V34_EQUALIZER_CENTER_TAPS / 2u + offset;
			size_t sample = index + coefficient;

			if (sample >= equalizer->tap_count)
				sample -= equalizer->tap_count;
			adapt_tap(equalizer, in_phase_delay, quadrature_delay,
				sample, coefficient, error, false);
		}
		return;
	}
	for (size_t sample = index;
	     sample < equalizer->tap_count; ++sample, ++coefficient)
		adapt_tap(equalizer, in_phase_delay, quadrature_delay,
			sample, coefficient, error, true);
	for (size_t sample = 0; sample < index;
	     ++sample, ++coefficient)
		adapt_tap(equalizer, in_phase_delay, quadrature_delay,
			sample, coefficient, error, true);
}

enum v34_equalizer_result v34_equalizer_adapt_history(
	struct v34_equalizer *equalizer,
	const int16_t *in_phase_delay, const int16_t *quadrature_delay,
	uint32_t index, struct v34_analytic_sample error, bool center_only)
{
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	size_t bytes = equalizer->tap_count * sizeof(*in_phase_delay);

	if (in_phase_delay == NULL || quadrature_delay == NULL ||
	    index >= equalizer->tap_count ||
	    memory_regions_overlap(equalizer, sizeof(*equalizer), in_phase_delay, bytes) ||
	    memory_regions_overlap(equalizer, sizeof(*equalizer), quadrature_delay, bytes))
		return V34_EQUALIZER_INVALID_ARGUMENT;
	adapt_history(equalizer, in_phase_delay, quadrature_delay,
		index, error, center_only);
	return V34_EQUALIZER_OK;
}

enum v34_equalizer_result v34_equalizer_adapt(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error)
{
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	adapt_history(equalizer, equalizer->in_phase_delay, equalizer->quadrature_delay,
		equalizer->index, error, false);
	return V34_EQUALIZER_OK;
}

enum v34_equalizer_result v34_equalizer_adapt_center(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error)
{
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	adapt_history(equalizer, equalizer->in_phase_delay, equalizer->quadrature_delay,
		equalizer->index, error, true);
	return V34_EQUALIZER_OK;
}
