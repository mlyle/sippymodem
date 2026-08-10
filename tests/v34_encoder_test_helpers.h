// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_ENCODER_TEST_HELPERS_H
#define SIPPYMODEM_V34_ENCODER_TEST_HELPERS_H

#include <sippymodem_v34_mapping_frame_encoder.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool v34_test_grid_points_are_equal(
	const struct sippymodem_v34_grid_point *left,
	const struct sippymodem_v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool v34_test_q7_points_are_equal(
	const struct sippymodem_v34_q7_point *left,
	const struct sippymodem_v34_q7_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool v34_test_mapping_parameters_are_equal(
	const struct sippymodem_v34_mapping_parameters *left,
	const struct sippymodem_v34_mapping_parameters *right)
{
	return left->constellation_points == right->constellation_points &&
	       left->mapping_frame_bits == right->mapping_frame_bits &&
	       left->shell_mapper_bits == right->shell_mapper_bits &&
	       left->point_index_bits_per_2d_symbol ==
		       right->point_index_bits_per_2d_symbol &&
	       left->ring_count == right->ring_count &&
	       left->expanded_shaping == right->expanded_shaping;
}

static bool v34_test_interval_outputs_are_equal(
	const struct sippymodem_v34_4d_encoder_output *left,
	const struct sippymodem_v34_4d_encoder_output *right)
{
	if (left->differential_output != right->differential_output ||
	    left->modulo_output != right->modulo_output ||
	    left->convolutional_output != right->convolutional_output ||
	    left->trellis_output != right->trellis_output ||
	    left->convolutional_input.y1 != right->convolutional_input.y1 ||
	    left->convolutional_input.y2 != right->convolutional_input.y2 ||
	    left->convolutional_input.y3 != right->convolutional_input.y3 ||
	    left->convolutional_input.y4 != right->convolutional_input.y4)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct sippymodem_v34_precoder_symbol *left_precoded =
			&left->precoded[symbol];
		const struct sippymodem_v34_precoder_symbol *right_precoded =
			&right->precoded[symbol];

		if (left->subset_label[symbol] != right->subset_label[symbol] ||
		    left->mapped[symbol].constellation_label !=
			    right->mapped[symbol].constellation_label ||
		    !v34_test_grid_points_are_equal(
			    &left->mapped[symbol].mapper_output,
			    &right->mapped[symbol].mapper_output) ||
		    !v34_test_q7_points_are_equal(
			    &left_precoded->filter_output,
			    &right_precoded->filter_output) ||
		    !v34_test_grid_points_are_equal(
			    &left_precoded->lattice_offset,
			    &right_precoded->lattice_offset) ||
		    !v34_test_grid_points_are_equal(
			    &left_precoded->channel_output,
			    &right_precoded->channel_output) ||
		    !v34_test_q7_points_are_equal(
			    &left_precoded->precoded_symbol,
			    &right_precoded->precoded_symbol))
			return false;
	}
	return true;
}

static bool v34_test_encoded_frames_are_equal(
	const struct sippymodem_v34_encoded_mapping_frame *left,
	const struct sippymodem_v34_encoded_mapping_frame *right)
{
	if (left->parsed.shell_label != right->parsed.shell_label)
		return false;
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const struct sippymodem_v34_4d_mapping_input *left_mapping =
			&left->parsed.interval[interval];
		const struct sippymodem_v34_4d_mapping_input *right_mapping =
			&right->parsed.interval[interval];

		if (left_mapping->rotation_bit != right_mapping->rotation_bit ||
		    left_mapping->differential_input !=
			    right_mapping->differential_input ||
		    !v34_test_interval_outputs_are_equal(
			    &left->interval[interval],
			    &right->interval[interval]))
			return false;
		for (uint8_t symbol = 0;
		     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol) {
			if (left_mapping->point_index[symbol] !=
				    right_mapping->point_index[symbol] ||
			    left->ring_indices.index[interval][symbol] !=
				    right->ring_indices.index[interval][symbol])
				return false;
		}
	}
	return true;
}

static bool v34_test_encoders_are_equal(
	const struct sippymodem_v34_4d_encoder *left,
	const struct sippymodem_v34_4d_encoder *right)
{
	if (!v34_test_mapping_parameters_are_equal(
		    &left->parameters, &right->parameters) ||
	    left->precoder.grid_step != right->precoder.grid_step ||
	    left->convolutional_encoder.state !=
		    right->convolutional_encoder.state ||
	    left->convolutional_encoder.selection !=
		    right->convolutional_encoder.selection ||
	    left->previous_differential_output !=
		    right->previous_differential_output)
		return false;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (left->precoder.coefficients[tap].real !=
			    right->precoder.coefficients[tap].real ||
		    left->precoder.coefficients[tap].imaginary !=
			    right->precoder.coefficients[tap].imaginary ||
		    !v34_test_q7_points_are_equal(
			    &left->precoder.previous_symbols[tap],
			    &right->precoder.previous_symbols[tap]))
			return false;
	}
	return true;
}

#endif
