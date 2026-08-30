// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_SYMBOL_POWER_H
#define V34_SYMBOL_POWER_H

#include "v34_nonlinear.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_symbol_power_result {
	V34_SYMBOL_POWER_OK,
	V34_SYMBOL_POWER_INVALID_ARGUMENT,
	V34_SYMBOL_POWER_OUT_OF_RANGE,
	V34_SYMBOL_POWER_NUMERIC_FAILURE,
};

/* An online mean retains bounded calibration state instead of a symbol
 * vector. A zero-initialized measurement is valid and empty. */
struct v34_symbol_power_measurement {
	double mean_squared_magnitude;
	uint32_t symbol_count;
};

struct v34_power_normalizer {
	struct v34_mapping_parameters parameters;
	double precoded_mean_squared_magnitude;
	double projected_mean_squared_magnitude;
	double normalization_gain;
	double maximum_precoded_point_power;
	double maximum_projected_point_power;
	double maximum_normalized_point_power;
	uint32_t calibration_symbol_count;
	enum v34_nonlinear_parameter nonlinear_parameter;
};

bool v34_symbol_power_measurement_is_valid(
	const struct v34_symbol_power_measurement *measurement);

enum v34_symbol_power_result
v34_symbol_power_measurement_reset(
	struct v34_symbol_power_measurement *measurement);

/* Q7 precoder outputs are measured in unscaled constellation-grid units. */
enum v34_symbol_power_result
v34_symbol_power_observe_q7(
	struct v34_symbol_power_measurement *measurement,
	const struct v34_q7_point *point);

enum v34_symbol_power_result
v34_symbol_power_observe_point(
	struct v34_symbol_power_measurement *measurement,
	const struct qam_point *point);

/* Build a unit-average-power data constellation from paired measurements of
 * the same generated symbols before and after nonlinear encoding. The peak
 * bounds cover every configured mapper label and every legal precoder
 * quantization residual; they are not inferred from the calibration sample. */
enum v34_symbol_power_result
v34_power_normalizer_init(
	struct v34_power_normalizer *normalizer,
	const struct v34_mapping_parameters *parameters,
	enum v34_nonlinear_parameter nonlinear_parameter,
	const struct v34_symbol_power_measurement
		*precoded_measurement,
	const struct v34_symbol_power_measurement
		*projected_measurement);

/* Replace the initial unit-power gain with a negotiated line-profile gain.
 * Generated means and hard bounds remain owned by the same calibration. */
enum v34_symbol_power_result v34_power_normalizer_set_gain(
	struct v34_power_normalizer *normalizer, double normalization_gain);

bool v34_power_normalizer_is_valid(
	const struct v34_power_normalizer *normalizer);

/* Scale one nonlinear-encoder output. The destination is unchanged when the
 * point is invalid, exceeds the generated hard bound, or cannot be scaled. */
enum v34_symbol_power_result v34_normalize_power(
	const struct v34_power_normalizer *normalizer,
	const struct qam_point *projected_point,
	struct qam_point *normalized_point);

#endif
