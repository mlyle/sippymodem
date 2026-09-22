// SPDX-License-Identifier: BSD-3-Clause

#include "v34_analytic_filter.h"

#include "integer_math.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The even real response is symmetric around tap 30. */
static const int16_t REAL_HALF_PROFILE[] = {
	-21, -53, -97, -154, -223, -304, -394, -490,
	-589, -686, -778, -861, -929, -981, -1013, 15360,
};

/* The odd quadrature response is antisymmetric around taps 29 and 31. */
static const int16_t QUADRATURE_HALF_PROFILE[] = {
	116, 137, 151, 154, 140, 106, 43, -53,
	-194, -392, -674, -1093, -1791, -3295, -10369,
};

_Static_assert(
	sizeof(REAL_HALF_PROFILE) / sizeof(REAL_HALF_PROFILE[0]) == 16,
	"V.34 analytic real profile size changed");
_Static_assert(
	sizeof(QUADRATURE_HALF_PROFILE) /
			sizeof(QUADRATURE_HALF_PROFILE[0]) == 15,
	"V.34 analytic quadrature profile size changed");

bool v34_analytic_filter_is_valid(
	const struct v34_analytic_filter *filter)
{
	return filter != NULL && filter->initialized;
}

enum v34_analytic_filter_result v34_analytic_filter_init(
	struct v34_analytic_filter *filter)
{
	struct v34_analytic_filter initialized;

	if (filter == NULL)
		return V34_ANALYTIC_FILTER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.initialized = true;
	*filter = initialized;
	return V34_ANALYTIC_FILTER_OK;
}

enum v34_analytic_filter_result v34_analytic_filter_process(
	struct v34_analytic_filter *filter, int16_t sample,
	struct v34_analytic_sample *output)
{
	struct v34_analytic_filter next;
	struct v34_analytic_sample generated;
	uint32_t in_phase = 0;
	uint32_t quadrature = 0;

	if (!v34_analytic_filter_is_valid(filter))
		return V34_ANALYTIC_FILTER_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			filter, sizeof(*filter), output, sizeof(*output)))
		return V34_ANALYTIC_FILTER_INVALID_ARGUMENT;
	next = *filter;
	next.history[0] = sample;
	/* Pair symmetric real taps and antisymmetric quadrature taps. */
	for (size_t pair = 0; pair < 15u; ++pair) {
		int32_t even_sum =
			(int32_t)next.history[2u * pair] +
			next.history[60u - 2u * pair];
		int32_t odd_difference =
			(int32_t)next.history[2u * pair + 1u] -
			next.history[59u - 2u * pair];

		in_phase += (uint32_t)(even_sum * REAL_HALF_PROFILE[pair]);
		quadrature += (uint32_t)(
			odd_difference * QUADRATURE_HALF_PROFILE[pair]);
	}
	in_phase += (uint32_t)(
		(int32_t)next.history[30] * REAL_HALF_PROFILE[15]);
	memmove(&next.history[2], &next.history[1],
		(V34_ANALYTIC_FILTER_TAPS - 2u) * sizeof(next.history[0]));
	next.history[1] = sample;
	next.history[0] = 0;
	generated = (struct v34_analytic_sample){
		.in_phase = round_shift_s32_word_to_s16(in_phase, 14),
		.quadrature = round_shift_s32_word_to_s16(quadrature, 14),
	};
	*filter = next;
	*output = generated;
	return V34_ANALYTIC_FILTER_OK;
}
