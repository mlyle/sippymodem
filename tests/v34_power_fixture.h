// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_POWER_FIXTURE_H
#define V34_POWER_FIXTURE_H

#include <v34_symbol_power.h>
#include <string.h>

/* Compare every owned field exactly, including floating-point representation.
 * Initialization need not define struct padding. */
static inline bool v34_normalizers_equal(
	const struct v34_power_normalizer *left,
	const struct v34_power_normalizer *right)
{
#define SAME(field) (memcmp(&left->field, &right->field, sizeof(left->field)) == 0)
	return v34_mapping_parameters_are_equal(
		       &left->parameters, &right->parameters) &&
	       SAME(precoded_mean_squared_magnitude) &&
	       SAME(projected_mean_squared_magnitude) &&
	       SAME(normalization_gain) &&
	       SAME(maximum_precoded_point_power) &&
	       SAME(maximum_projected_point_power) &&
	       SAME(maximum_normalized_point_power) &&
	       left->calibration_symbol_count ==
		       right->calibration_symbol_count &&
	       left->nonlinear_parameter == right->nonlinear_parameter;
#undef SAME
}

#endif
