// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_SYMBOL_POWER_H
#define SIPPYMODEM_V34_SYMBOL_POWER_H

#include "sippymodem_v34_nonlinear_encoder.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_symbol_power_result {
	SIPPYMODEM_V34_SYMBOL_POWER_OK,
	SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE,
	SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE,
};

/* An online mean retains bounded calibration state instead of a symbol
 * vector. A zero-initialized measurement is valid and empty. */
struct sippymodem_v34_symbol_power_measurement {
	double mean_squared_magnitude;
	uint32_t symbol_count;
};

struct sippymodem_v34_power_normalizer {
	struct sippymodem_v34_mapping_parameters parameters;
	double precoded_mean_squared_magnitude;
	double projected_mean_squared_magnitude;
	double normalization_gain;
	double maximum_precoded_point_power;
	double maximum_projected_point_power;
	double maximum_normalized_point_power;
	uint32_t calibration_symbol_count;
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter;
};

bool sippymodem_v34_symbol_power_measurement_is_valid(
	const struct sippymodem_v34_symbol_power_measurement *measurement);

enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_measurement_reset(
	struct sippymodem_v34_symbol_power_measurement *measurement);

/* Q7 precoder outputs are measured in unscaled constellation-grid units. */
enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_observe_q7(
	struct sippymodem_v34_symbol_power_measurement *measurement,
	const struct sippymodem_v34_q7_point *point);

enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_observe_point(
	struct sippymodem_v34_symbol_power_measurement *measurement,
	const struct sippymodem_qam_point *point);

/* Build a unit-average-power data constellation from paired measurements of
 * the same generated symbols before and after nonlinear encoding. The peak
 * bounds cover every configured mapper label and every legal precoder
 * quantization residual; they are not inferred from the calibration sample. */
enum sippymodem_v34_symbol_power_result
sippymodem_v34_power_normalizer_init(
	struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_mapping_parameters *parameters,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	const struct sippymodem_v34_symbol_power_measurement
		*precoded_measurement,
	const struct sippymodem_v34_symbol_power_measurement
		*projected_measurement);

bool sippymodem_v34_power_normalizer_is_valid(
	const struct sippymodem_v34_power_normalizer *normalizer);

/* Scale one nonlinear-encoder output. The destination is unchanged when the
 * point is invalid, exceeds the generated hard bound, or cannot be scaled. */
enum sippymodem_v34_symbol_power_result sippymodem_v34_normalize_power(
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_qam_point *projected_point,
	struct sippymodem_qam_point *normalized_point);

const char *sippymodem_v34_symbol_power_result_name(
	enum sippymodem_v34_symbol_power_result result);

#endif
