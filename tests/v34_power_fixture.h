// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_POWER_FIXTURE_H
#define V34_POWER_FIXTURE_H

#include <v34_power_calibration.h>
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

/* Complete synthetic setup before advancing the peer media clock. */
static inline enum v34_power_calibration_result v34_calibrate_mapping_fixture(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	enum v34_nonlinear_parameter nonlinear_parameter,
	struct v34_power_normalizer *normalizer)
{
	struct v34_mapping_power_calibration calibration;
	enum v34_power_calibration_result result =
		v34_mapping_power_calibration_begin(&calibration, initial_schedule,
		initial_scrambler, initial_encoder, nonlinear_parameter);
	return result == V34_POWER_CALIBRATION_OK ?
		v34_mapping_power_calibration_step(&calibration, UINT32_MAX, normalizer) : result;
}

static inline enum v34_power_calibration_result v34_calibrate_line_fixture(
	const struct v34_frame_schedule *initial_schedule,
	const struct v34_scrambler *initial_scrambler,
	const struct v34_4d_encoder *initial_encoder,
	const struct v34_power_normalizer *normalizer,
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate, struct v34_line_power_measurement *measurement)
{
	struct v34_line_power_calibration calibration;
	enum v34_power_calibration_result result =
		v34_line_power_calibration_begin(&calibration, initial_schedule,
		initial_scrambler, initial_encoder, normalizer, profile, sample_rate);
	return result == V34_POWER_CALIBRATION_OK ?
		v34_line_power_calibration_step(&calibration, UINT32_MAX, measurement) : result;
}

#endif
