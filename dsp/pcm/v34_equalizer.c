// SPDX-License-Identifier: BSD-3-Clause

#include "v34_equalizer.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int16_t high_word(uint32_t value)
{
	return wrap_s16_from_s32((int32_t)(value >> 16));
}

static uint32_t coefficient_word(int16_t coefficient, uint16_t residue)
{
	return ((uint32_t)(uint16_t)coefficient << 16) | residue;
}

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
	struct v34_equalizer next;

	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	for (size_t tap = next.identity_tap - V34_EQUALIZER_CENTER_TAPS / 2u;
	     tap < next.identity_tap + V34_EQUALIZER_CENTER_TAPS / 2u;
	     ++tap) {
		next.in_phase_coefficient[tap] = 0;
		next.quadrature_coefficient[tap] = 0;
	}
	*equalizer = next;
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
	int16_t sample_in_phase = equalizer->in_phase_delay[sample_index];
	int16_t sample_quadrature = equalizer->quadrature_delay[sample_index];
	int16_t coefficient_in_phase =
		equalizer->in_phase_coefficient[coefficient_index];
	int16_t coefficient_quadrature =
		equalizer->quadrature_coefficient[coefficient_index];

	*in_phase += multiply_low_s32(sample_in_phase, coefficient_in_phase);
	*in_phase -= multiply_low_s32(sample_quadrature, coefficient_quadrature);
	*quadrature += multiply_low_s32(sample_quadrature, coefficient_in_phase);
	*quadrature += multiply_low_s32(sample_in_phase, coefficient_quadrature);
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

static void adapt_tap(
	struct v34_equalizer *equalizer,
	size_t sample_index, size_t coefficient_index,
	struct v34_analytic_sample error, bool preserve_residue)
{
	int16_t sample_in_phase = equalizer->in_phase_delay[sample_index];
	int16_t sample_quadrature = equalizer->quadrature_delay[sample_index];
	uint32_t in_phase = coefficient_word(
		equalizer->in_phase_coefficient[coefficient_index],
		preserve_residue ?
			equalizer->in_phase_residue[coefficient_index] : 0);
	uint32_t quadrature = coefficient_word(
		equalizer->quadrature_coefficient[coefficient_index],
		preserve_residue ?
			equalizer->quadrature_residue[coefficient_index] : 0);

	in_phase -= multiply_low_s32(sample_in_phase, error.in_phase);
	in_phase -= multiply_low_s32(sample_quadrature, error.quadrature);
	quadrature -= multiply_low_s32(sample_in_phase, error.quadrature);
	quadrature += multiply_low_s32(sample_quadrature, error.in_phase);
	if (!preserve_residue) {
		in_phase += UINT32_C(0x8000);
		quadrature += UINT32_C(0x8000);
	}
	equalizer->in_phase_coefficient[coefficient_index] = high_word(in_phase);
	equalizer->quadrature_coefficient[coefficient_index] = high_word(quadrature);
	if (preserve_residue) {
		equalizer->in_phase_residue[coefficient_index] = (uint16_t)in_phase;
		equalizer->quadrature_residue[coefficient_index] =
			(uint16_t)quadrature;
	}
}

enum v34_equalizer_result v34_equalizer_adapt(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error)
{
	size_t coefficient = 0;

	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	for (size_t sample = equalizer->index;
	     sample < equalizer->tap_count; ++sample, ++coefficient)
		adapt_tap(equalizer, sample, coefficient, error, true);
	for (size_t sample = 0; sample < equalizer->index;
	     ++sample, ++coefficient)
		adapt_tap(equalizer, sample, coefficient, error, true);
	return V34_EQUALIZER_OK;
}

enum v34_equalizer_result v34_equalizer_adapt_center(
	struct v34_equalizer *equalizer,
	struct v34_analytic_sample error)
{
	if (!v34_equalizer_is_valid(equalizer))
		return V34_EQUALIZER_INVALID_STATE;
	for (size_t offset = 0; offset < V34_EQUALIZER_CENTER_TAPS; ++offset) {
		size_t coefficient = equalizer->identity_tap -
			V34_EQUALIZER_CENTER_TAPS / 2u + offset;
		size_t sample = equalizer->index + coefficient;

		if (sample >= equalizer->tap_count)
			sample -= equalizer->tap_count;
		adapt_tap(equalizer, sample, coefficient, error, false);
	}
	return V34_EQUALIZER_OK;
}
