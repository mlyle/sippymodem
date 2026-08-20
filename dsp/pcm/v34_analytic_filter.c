// SPDX-License-Identifier: BSD-3-Clause

#include "v34_analytic_filter.h"

#include "memory_region.h"

#include <limits.h>
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

static int16_t real_coefficient(size_t tap)
{
	size_t position;

	if ((tap & 1u) != 0 || tap > 60u)
		return 0;
	position = tap / 2u;
	if (position > 15u)
		position = 30u - position;
	return REAL_HALF_PROFILE[position];
}

static int16_t quadrature_coefficient(size_t tap)
{
	size_t position;

	if ((tap & 1u) == 0 || tap > 59u)
		return 0;
	position = tap / 2u;
	if (position < 15u)
		return QUADRATURE_HALF_PROFILE[position];
	return (int16_t)-QUADRATURE_HALF_PROFILE[29u - position];
}

static int32_t arithmetic_shift_right(uint32_t value, unsigned int shift)
{
	uint32_t shifted;

	if (shift == 0)
		shifted = value;
	else if ((value & UINT32_C(0x80000000)) == 0)
		shifted = value >> shift;
	else
		shifted = (value >> shift) | ~(UINT32_MAX >> shift);
	return shifted <= INT32_MAX ?
		       (int32_t)shifted :
		       (int32_t)((int64_t)shifted - (INT64_C(1) << 32));
}

static int16_t rounded_q14(uint32_t accumulator)
{
	uint16_t word = (uint16_t)(uint32_t)arithmetic_shift_right(
		accumulator + UINT32_C(0x2000), 14);

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

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
	int16_t previous = 0;

	if (!v34_analytic_filter_is_valid(filter))
		return V34_ANALYTIC_FILTER_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			filter, sizeof(*filter), output, sizeof(*output)))
		return V34_ANALYTIC_FILTER_INVALID_ARGUMENT;
	next = *filter;
	next.history[0] = sample;
	for (size_t tap = 0; tap < V34_ANALYTIC_FILTER_TAPS; ++tap) {
		int16_t current = next.history[tap];

		next.history[tap] = previous;
		in_phase += (uint32_t)((int32_t)current *
					 real_coefficient(tap));
		quadrature += (uint32_t)((int32_t)current *
					   quadrature_coefficient(tap));
		previous = current;
	}
	generated = (struct v34_analytic_sample){
		.in_phase = rounded_q14(in_phase),
		.quadrature = rounded_q14(quadrature),
	};
	*filter = next;
	*output = generated;
	return V34_ANALYTIC_FILTER_OK;
}
