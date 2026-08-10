// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_trellis.h"

#include "sippymodem_v32_constellation.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const uint8_t decoded_high_dibit[4][4] = {
	{ 0, 1, 2, 3 },
	{ 1, 0, 3, 2 },
	{ 3, 2, 0, 1 },
	{ 2, 3, 1, 0 },
};

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool data_bits_are_valid(uint8_t data_bits_per_symbol)
{
	return data_bits_per_symbol >= SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS &&
	       data_bits_per_symbol <= SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS;
}

static bool path_step_is_valid(
	const struct sippymodem_v32_trellis_path_step *step,
	uint8_t data_bits_per_symbol)
{
	for (size_t state = 0; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state) {
		if (step->predecessor[state] >=
			    SIPPYMODEM_V32_TRELLIS_STATE_COUNT ||
		    step->differential_symbol[state] >=
			    (UINT8_C(1) << data_bits_per_symbol))
			return false;
	}
	return true;
}

bool sippymodem_v32_trellis_decoder_is_valid(
	const struct sippymodem_v32_trellis_decoder *decoder)
{
	bool has_normalized_path = false;

	if (decoder == NULL ||
	    !data_bits_are_valid(decoder->data_bits_per_symbol) ||
	    decoder->path_index >= SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS ||
	    decoder->path_symbols > SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS ||
	    (decoder->path_symbols < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS &&
	     decoder->path_index != decoder->path_symbols) ||
	    decoder->previous_differential_quadrant >=
		    SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT ||
	    decoder->carrier_phase_quadrant >=
		    SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT)
		return false;
	for (size_t state = 0; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state) {
		if (!isfinite(decoder->path_metric[state]) ||
		    decoder->path_metric[state] < 0.0)
			return false;
		if (decoder->path_metric[state] == 0.0)
			has_normalized_path = true;
	}
	if (!has_normalized_path)
		return false;
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS; ++index) {
		if (!path_step_is_valid(
			    &decoder->path[index], decoder->data_bits_per_symbol))
			return false;
	}
	return true;
}

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_decoder_init(
	struct sippymodem_v32_trellis_decoder *decoder,
	uint8_t data_bits_per_symbol, uint8_t initial_carrier_phase_quadrant)
{
	struct sippymodem_v32_trellis_decoder initialized;

	if (decoder == NULL || !data_bits_are_valid(data_bits_per_symbol) ||
	    initial_carrier_phase_quadrant >=
			       SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT)
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	for (size_t state = 1; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state)
		initialized.path_metric[state] = DBL_MAX;
	initialized.carrier_phase_quadrant = initial_carrier_phase_quadrant;
	initialized.data_bits_per_symbol = data_bits_per_symbol;
	*decoder = initialized;
	return SIPPYMODEM_V32_TRELLIS_OK;
}

static double squared_error(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

static bool measure_branches(
	const struct sippymodem_qam_point *point, uint8_t data_bits_per_symbol,
	double error[SIPPYMODEM_V32_14400_SIGNAL_POINTS])
{
	uint8_t signal_points = (uint8_t)(
		UINT8_C(1) << (data_bits_per_symbol + 1u));

	for (uint8_t symbol = 0; symbol < signal_points; ++symbol) {
		struct sippymodem_qam_point candidate;

		if (sippymodem_v32_trellis_constellation_point(
			    data_bits_per_symbol, symbol, &candidate) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
			return false;
		error[symbol] = squared_error(point, &candidate);
		if (!isfinite(error[symbol]))
			return false;
	}
	return true;
}

static bool select_paths(
	struct sippymodem_v32_trellis_decoder *decoder,
	const double branch_error[SIPPYMODEM_V32_14400_SIGNAL_POINTS],
	struct sippymodem_v32_trellis_path_step *step)
{
	double next_metric[SIPPYMODEM_V32_TRELLIS_STATE_COUNT];
	double minimum = DBL_MAX;
	uint8_t low_bit_count =
		(uint8_t)(decoder->data_bits_per_symbol - 2u);
	uint8_t low_value_count = (uint8_t)(UINT8_C(1) << low_bit_count);

	for (size_t state = 0; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state)
		next_metric[state] = DBL_MAX;
	memset(step, 0, sizeof(*step));

	for (uint8_t predecessor = 0;
	     predecessor < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++predecessor) {
		if (decoder->path_metric[predecessor] == DBL_MAX)
			continue;
		for (uint8_t next_quadrant = 0;
		     next_quadrant < SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT;
		     ++next_quadrant) {
			for (uint8_t low_value = 0;
			     low_value < low_value_count; ++low_value) {
				/* With differential state zero, the high input
				 * selects the candidate next quadrant directly. */
				struct sippymodem_v32_trellis_encoder branch = {
					.trellis_state = predecessor,
					.carrier_phase_quadrant =
						decoder->carrier_phase_quadrant,
					.data_bits_per_symbol =
						decoder->data_bits_per_symbol,
				};
				uint8_t differential_symbol =
					(uint8_t)((next_quadrant << low_bit_count) |
						  low_value);
				uint8_t signal_symbol;
				double metric;

				if (sippymodem_v32_trellis_encode(
					    &branch, differential_symbol,
					    &signal_symbol) !=
				    SIPPYMODEM_V32_TRELLIS_OK)
					return false;
				metric = decoder->path_metric[predecessor] +
					 branch_error[signal_symbol];
				if (!isfinite(metric))
					continue;
				if (metric < next_metric[branch.trellis_state]) {
					next_metric[branch.trellis_state] = metric;
					step->predecessor[branch.trellis_state] =
						predecessor;
					step->differential_symbol
						[branch.trellis_state] =
						differential_symbol;
				}
			}
		}
	}
	for (size_t state = 0; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state) {
		if (next_metric[state] < minimum)
			minimum = next_metric[state];
	}
	if (minimum == DBL_MAX)
		return false;
	for (size_t state = 0; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state) {
		decoder->path_metric[state] = next_metric[state] == DBL_MAX ?
						      DBL_MAX :
						      next_metric[state] - minimum;
	}
	decoder->carrier_phase_quadrant =
		(uint8_t)((decoder->carrier_phase_quadrant + 3u) & 3u);
	return true;
}

static uint8_t best_path_state(
	const struct sippymodem_v32_trellis_decoder *decoder)
{
	uint8_t best = 0;

	for (uint8_t state = 1; state < SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	     ++state) {
		if (decoder->path_metric[state] < decoder->path_metric[best])
			best = state;
	}
	return best;
}

static uint8_t traceback_symbol(
	const struct sippymodem_v32_trellis_decoder *decoder,
	uint8_t newest_path_index)
{
	uint8_t state = best_path_state(decoder);
	uint8_t index = newest_path_index;

	for (size_t distance = 1;
	     distance < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
	     ++distance) {
		state = decoder->path[index].predecessor[state];
		index = index == 0 ?
				SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS - 1u :
				(uint8_t)(index - 1u);
	}
	return decoder->path[index].differential_symbol[state];
}

enum sippymodem_v32_trellis_result sippymodem_v32_trellis_decode(
	struct sippymodem_v32_trellis_decoder *decoder,
	const struct sippymodem_qam_point *point, uint8_t *scrambled_data,
	bool *ready)
{
	struct sippymodem_v32_trellis_decoder next;
	double branch_error[SIPPYMODEM_V32_14400_SIGNAL_POINTS];
	uint8_t newest_path_index;
	uint8_t differential_symbol = 0;
	bool symbol_ready;

	if (!sippymodem_v32_trellis_decoder_is_valid(decoder) ||
	    !point_is_finite(point) || scrambled_data == NULL || ready == NULL ||
	    !measure_branches(
		    point, decoder->data_bits_per_symbol, branch_error))
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;
	next = *decoder;
	newest_path_index = next.path_index;
	if (!select_paths(
		    &next, branch_error, &next.path[newest_path_index]))
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;
	next.path_index =
		(uint8_t)((next.path_index + 1u) %
			  SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS);
	if (next.path_symbols < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS)
		next.path_symbols++;
	symbol_ready =
		next.path_symbols == SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
	if (symbol_ready) {
		uint8_t next_quadrant;
		uint8_t high_dibit;
		uint8_t low_bit_count =
			(uint8_t)(next.data_bits_per_symbol - 2u);
		uint8_t low_mask =
			(uint8_t)((UINT8_C(1) << low_bit_count) - 1u);

		differential_symbol = traceback_symbol(&next, newest_path_index);
		next_quadrant =
			(uint8_t)(differential_symbol >> low_bit_count);
		high_dibit = decoded_high_dibit
			[next.previous_differential_quadrant][next_quadrant];
		next.previous_differential_quadrant = next_quadrant;
		differential_symbol =
			(uint8_t)((high_dibit << low_bit_count) |
				  (differential_symbol & low_mask));
	}
	if (!sippymodem_v32_trellis_decoder_is_valid(&next))
		return SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT;
	*decoder = next;
	*ready = symbol_ready;
	if (symbol_ready)
		*scrambled_data = differential_symbol;
	return SIPPYMODEM_V32_TRELLIS_OK;
}
