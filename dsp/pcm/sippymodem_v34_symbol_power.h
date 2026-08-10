// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_SYMBOL_POWER_H
#define SIPPYMODEM_V34_SYMBOL_POWER_H

#include "sippymodem_v34_precoder.h"

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

const char *sippymodem_v34_symbol_power_result_name(
	enum sippymodem_v34_symbol_power_result result);

#endif
