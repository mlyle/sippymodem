// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PRECODER_H
#define SIPPYMODEM_V34_PRECODER_H

#include "sippymodem_v34_constellation.h"
#include "sippymodem_v34_mapping_parameters.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PRECODER_TAP_COUNT = 3,
	SIPPYMODEM_V34_Q7_SCALE = 128,
	SIPPYMODEM_V34_Q14_SCALE = 16384,
	SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT = 255,
};

/* Complex protocol values with seven fractional bits. The wider components
 * retain valid intermediate p(n) values without signed 16-bit wraparound. */
struct sippymodem_v34_q7_point {
	int32_t in_phase;
	int32_t quadrature;
};

/* Each component is signed Q14 in the half-open interval (-2, 2). */
struct sippymodem_v34_q14_coefficient {
	int16_t real;
	int16_t imaginary;
};

enum sippymodem_v34_precoder_grid_step {
	SIPPYMODEM_V34_PRECODER_GRID_STEP_2 = 2,
	SIPPYMODEM_V34_PRECODER_GRID_STEP_4 = 4,
};

enum sippymodem_v34_precoder_result {
	SIPPYMODEM_V34_PRECODER_OK,
	SIPPYMODEM_V34_PRECODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE,
};

struct sippymodem_v34_precoder {
	struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT];
	/* x(n - 1) is first; all entries are signed Q7. */
	struct sippymodem_v34_q7_point
		previous_symbols[SIPPYMODEM_V34_PRECODER_TAP_COUNT];
	enum sippymodem_v34_precoder_grid_step grid_step;
};

struct sippymodem_v34_precoder_prediction {
	/* p(n), after exact Q7 rounding of the complex filter output. */
	struct sippymodem_v34_q7_point filter_output;
	/* c(n), represented in exact integer grid units. */
	struct sippymodem_v34_grid_point lattice_offset;
};

struct sippymodem_v34_precoder_symbol {
	/* p(n), after exact Q7 rounding of the complex filter output. */
	struct sippymodem_v34_q7_point filter_output;
	/* c(n), represented in exact integer grid units. */
	struct sippymodem_v34_grid_point lattice_offset;
	/* y(n) = u(n) + c(n), represented in exact integer grid units. */
	struct sippymodem_v34_grid_point channel_output;
	/* x(n) = y(n) - p(n), represented in signed Q7. */
	struct sippymodem_v34_q7_point precoded_symbol;
};

/* Initialize zero history and derive the grid step from the high mapping
 * frame. The destination is unchanged when any input is invalid. */
enum sippymodem_v34_precoder_result sippymodem_v34_precoder_init(
	struct sippymodem_v34_precoder *precoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT]);

bool sippymodem_v34_precoder_is_valid(
	const struct sippymodem_v34_precoder *precoder);

/* Derive p(n) and c(n) from the current history without advancing it. This is
 * needed before mapping the second symbol of a four-dimensional interval. */
enum sippymodem_v34_precoder_result sippymodem_v34_precoder_predict(
	const struct sippymodem_v34_precoder *precoder,
	struct sippymodem_v34_precoder_prediction *prediction);

/* Process one odd-integer mapper point. Rounding and quantization break exact
 * half-way ties toward the value with smaller magnitude. State and output are
 * both unchanged on failure. */
enum sippymodem_v34_precoder_result sippymodem_v34_precoder_step(
	struct sippymodem_v34_precoder *precoder,
	const struct sippymodem_v34_grid_point *mapper_output,
	struct sippymodem_v34_precoder_symbol *symbol);

const char *sippymodem_v34_precoder_result_name(
	enum sippymodem_v34_precoder_result result);

#endif
