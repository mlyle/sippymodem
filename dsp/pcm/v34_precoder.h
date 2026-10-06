// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PRECODER_H
#define V34_PRECODER_H

#include "v34_constellation.h"
#include "v34_mapping_parameters.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_PRECODER_TAP_COUNT = 3,
	V34_Q7_SCALE = 128,
	V34_Q14_SCALE = 16384,
};

/* Complex protocol values with seven fractional bits. The wider components
 * retain valid intermediate p(n) values without signed 16-bit wraparound. */
struct v34_q7_point {
	int32_t in_phase;
	int32_t quadrature;
};

/* Each component is signed Q14 in the half-open interval (-2, 2). */
struct v34_q14_coefficient {
	int16_t real;
	int16_t imaginary;
};

/* -2 is outside the specified half-open Q14 coefficient interval. */
static inline bool v34_precoder_coefficients_are_valid(
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	if (coefficients == NULL)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		if (coefficients[tap].real == INT16_MIN ||
		    coefficients[tap].imaginary == INT16_MIN)
			return false;
	return true;
}

/* Compare stored Q14 components; owners validate their range separately. */
static inline bool v34_precoder_coefficients_are_equal(
	const struct v34_q14_coefficient left[V34_PRECODER_TAP_COUNT],
	const struct v34_q14_coefficient right[V34_PRECODER_TAP_COUNT])
{
	if (left == NULL || right == NULL)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		if (left[tap].real != right[tap].real ||
		    left[tap].imaginary != right[tap].imaginary)
			return false;
	return true;
}

static inline bool v34_precoder_coefficients_are_zero(
	const struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT])
{
	static const struct v34_q14_coefficient zero[V34_PRECODER_TAP_COUNT];

	return v34_precoder_coefficients_are_equal(coefficients, zero);
}

enum v34_precoder_grid_step {
	V34_PRECODER_GRID_STEP_2 = 2,
	V34_PRECODER_GRID_STEP_4 = 4,
};

static inline enum v34_precoder_grid_step
v34_precoder_grid_step_for_mapping(
	const struct v34_mapping_parameters *parameters)
{
	return parameters->mapping_frame_bits < 56u ?
		V34_PRECODER_GRID_STEP_2 : V34_PRECODER_GRID_STEP_4;
}

enum v34_precoder_result {
	V34_PRECODER_OK,
	V34_PRECODER_INVALID_ARGUMENT,
	V34_PRECODER_OUT_OF_RANGE,
};

struct v34_precoder {
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT];
	/* x(n - 1) is first; all entries are signed Q7. */
	struct v34_q7_point
		previous_symbols[V34_PRECODER_TAP_COUNT];
	enum v34_precoder_grid_step grid_step;
};

struct v34_precoder_prediction {
	/* p(n), after exact Q7 rounding of the complex filter output. */
	struct v34_q7_point filter_output;
	/* c(n), represented in exact integer grid units. */
	struct v34_grid_point lattice_offset;
};

struct v34_precoder_symbol {
	/* p(n), after exact Q7 rounding of the complex filter output. */
	struct v34_q7_point filter_output;
	/* c(n), represented in exact integer grid units. */
	struct v34_grid_point lattice_offset;
	/* y(n) = u(n) + c(n), represented in exact integer grid units. */
	struct v34_grid_point channel_output;
	/* x(n) = y(n) - p(n), represented in signed Q7. */
	struct v34_q7_point precoded_symbol;
};

/* Initialize with zero history and derive the grid step from the high mapping
 * frame. The destination is unchanged when any input is invalid. */
enum v34_precoder_result v34_precoder_init(
	struct v34_precoder *precoder,
	const struct v34_mapping_parameters *parameters,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT]);

bool v34_precoder_is_valid(
	const struct v34_precoder *precoder);

/* Derive p(n) and c(n) from the current history without advancing it. This is
 * needed before mapping the second symbol of a four-dimensional interval. */
enum v34_precoder_result v34_precoder_predict(
	const struct v34_precoder *precoder,
	struct v34_precoder_prediction *prediction);

/* Process one odd-integer mapper point. Rounding and quantization break exact
 * half-way ties toward the value with smaller magnitude. State and output are
 * both unchanged on failure. */
enum v34_precoder_result v34_precoder_step(
	struct v34_precoder *precoder,
	const struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol);

/* Recover one complete 4-D interval of soft y(n) channel estimates from
 * equalized, nonlinear-decoded x(n) estimates. This receive history must be a
 * separately initialized precoder starting at B1; it is distinct from the
 * delayed exact history used by precoder_inverse_step(). Prior received x(n)
 * estimates are quantized to Q7 with half-way ties toward zero so the next
 * prediction follows the transmitter's feedback arithmetic, while the
 * current soft residual is preserved in each returned estimate. Input and
 * output may alias. State and both outputs are unchanged on failure. */
enum v34_precoder_result
v34_precoder_reconstruct_interval(
	struct v34_precoder *receive_history,
	const struct qam_point
		precoded_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct qam_point
		channel_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL]);

/* Invert one exact, survivor-selected odd-grid channel point y(n). This is a
 * post-trellis operation, not a noisy-sample slicer. It recovers u(n), reports
 * the same prediction and x(n) representation as the transmit step, and then
 * advances the shared precoder history. channel_output and mapper_output may
 * refer to the same object. State and both destinations are unchanged on
 * failure. */
enum v34_precoder_result v34_precoder_inverse_step(
	struct v34_precoder *precoder,
	const struct v34_grid_point *channel_output,
	struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol);

#endif
