// SPDX-License-Identifier: BSD-3-Clause

#include "v34_trellis.h"

#include <stddef.h>

static uint8_t selection_state_bits(
	enum v34_trellis_selection selection)
{
	switch (selection) {
	case V34_TRELLIS_16_STATE:
		return 4;
	case V34_TRELLIS_32_STATE:
		return 5;
	case V34_TRELLIS_64_STATE:
		return 6;
	}
	return 0;
}

static uint8_t grid_residue_index(int16_t coordinate)
{
	return (uint8_t)((((uint16_t)coordinate - 1u) & 7u) >> 1);
}

enum v34_trellis_result v34_grid_subset_label(
	const struct v34_grid_point *point, uint8_t *subset_label)
{
	uint8_t in_phase;
	uint8_t quadrature;
	uint8_t in_phase_low;
	uint8_t quadrature_low;
	uint8_t high;
	uint8_t generated;

	if (point == NULL || subset_label == NULL ||
	    ((uint16_t)point->in_phase & 1u) == 0u ||
	    ((uint16_t)point->quadrature & 1u) == 0u)
		return V34_TRELLIS_INVALID_ARGUMENT;
	in_phase = grid_residue_index(point->in_phase);
	quadrature = grid_residue_index(point->quadrature);
	in_phase_low = in_phase & 1u;
	quadrature_low = quadrature & 1u;
	/* Boolean reduction of Figure 9's repeating four-by-four labels. */
	high = (uint8_t)(
		(in_phase >> 1) ^ in_phase_low ^ (quadrature >> 1) ^
		quadrature_low);
	generated = (uint8_t)((high << 2) | (in_phase_low << 1) |
			      (in_phase_low ^ quadrature_low));
	*subset_label = generated;
	return V34_TRELLIS_OK;
}

static bool grid_point_coordinates_are_even(
	const struct v34_grid_point *point)
{
	return point != NULL && ((uint16_t)point->in_phase & 1u) == 0u &&
	       ((uint16_t)point->quadrature & 1u) == 0u;
}

static uint8_t half_coordinate_sum_parity(
	const struct v34_grid_point *point)
{
	int sum = point->in_phase / 2 + point->quadrature / 2;

	return (uint8_t)((unsigned int)sum & 1u);
}

enum v34_trellis_result v34_modulo_encoder_output(
	const struct v34_grid_point *first_offset,
	const struct v34_grid_point *second_offset, uint8_t *c0)
{
	if (!grid_point_coordinates_are_even(first_offset) ||
	    !grid_point_coordinates_are_even(second_offset) || c0 == NULL)
		return V34_TRELLIS_INVALID_ARGUMENT;
	*c0 = half_coordinate_sum_parity(first_offset) ^
	      half_coordinate_sum_parity(second_offset);
	return V34_TRELLIS_OK;
}

bool v34_convolutional_input_is_valid(
	const struct v34_convolutional_input *input)
{
	return input != NULL && input->y1 <= 1u && input->y2 <= 1u &&
	       input->y3 <= 1u && input->y4 <= 1u;
}

enum v34_trellis_result
v34_convolutional_input_from_subset_labels(
	uint8_t first_subset_label, uint8_t second_subset_label,
	struct v34_convolutional_input *input)
{
	struct v34_convolutional_input generated;
	uint8_t first_low;
	uint8_t second_low;

	if (first_subset_label >= V34_SUBSET_LABEL_COUNT ||
	    second_subset_label >= V34_SUBSET_LABEL_COUNT ||
	    input == NULL)
		return V34_TRELLIS_INVALID_ARGUMENT;
	first_low = first_subset_label & 1u;
	second_low = second_subset_label & 1u;
	/* Boolean reduction of the eight rows and columns in Table 13. */
	generated = (struct v34_convolutional_input){
		.y1 = (uint8_t)(((first_subset_label >> 1) ^
				 (second_subset_label >> 1) ^ first_low ^
				 (first_low & second_low)) &
				1u),
		.y2 = first_low,
		.y3 = (uint8_t)((first_subset_label >> 1) & 1u),
		.y4 = (uint8_t)(((first_subset_label >> 2) ^
				 (second_subset_label >> 2)) &
				1u),
	};
	*input = generated;
	return V34_TRELLIS_OK;
}

bool v34_convolutional_encoder_is_valid(
	const struct v34_convolutional_encoder *encoder)
{
	uint8_t state_bits;

	if (encoder == NULL)
		return false;
	state_bits = selection_state_bits(encoder->selection);
	return state_bits != 0 && encoder->state < (1u << state_bits);
}

enum v34_trellis_result
v34_convolutional_encoder_init(
	struct v34_convolutional_encoder *encoder,
	enum v34_trellis_selection selection)
{
	if (encoder == NULL ||
	    !v34_trellis_selection_is_valid(selection))
		return V34_TRELLIS_INVALID_ARGUMENT;
	*encoder = (struct v34_convolutional_encoder){
		.selection = selection,
	};
	return V34_TRELLIS_OK;
}

enum v34_trellis_result
v34_convolutional_encoder_output(
	const struct v34_convolutional_encoder *encoder,
	uint8_t *y0)
{
	if (!v34_convolutional_encoder_is_valid(encoder) ||
	    y0 == NULL)
		return V34_TRELLIS_INVALID_ARGUMENT;
	*y0 = encoder->state & 1u;
	return V34_TRELLIS_OK;
}

static uint8_t state_bit(uint8_t state, uint8_t index)
{
	return (uint8_t)((state >> index) & 1u);
}

static uint8_t pack_state_bit(uint8_t bit, uint8_t index)
{
	return (uint8_t)((bit & 1u) << index);
}

static uint8_t advance_16_state(
	uint8_t state, const struct v34_convolutional_input *input)
{
	uint8_t y0 = state_bit(state, 0);

	return (uint8_t)(
		pack_state_bit(state_bit(state, 1) ^ input->y1, 0) |
		pack_state_bit(state_bit(state, 2) ^ input->y2, 1) |
		pack_state_bit(
			state_bit(state, 3) ^ input->y2 ^ y0, 2) |
		pack_state_bit(y0, 3));
}

static uint8_t advance_32_state(
	uint8_t state, const struct v34_convolutional_input *input)
{
	uint8_t y0 = state_bit(state, 0);

	return (uint8_t)(
		pack_state_bit(state_bit(state, 1) ^ input->y2, 0) |
		pack_state_bit(state_bit(state, 2) ^ input->y4, 1) |
		pack_state_bit(state_bit(state, 3) ^ input->y1, 2) |
		pack_state_bit(state_bit(state, 4) ^ input->y2, 3) |
		pack_state_bit(y0, 4));
}

static uint8_t advance_64_state(
	uint8_t state, const struct v34_convolutional_input *input)
{
	/* delay[0] is Figure 12's current Y0; the other entries follow the
	 * encoder's six individual delay outputs. */
	uint8_t delay[6];
	uint8_t next[6];
	uint8_t feedback;
	uint8_t tail_mix;
	uint8_t packed = 0;

	for (uint8_t index = 0; index < 6; ++index)
		delay[index] = state_bit(state, index);
	feedback = input->y1 ^ delay[4];
	tail_mix = delay[4] ^ delay[5];
	next[0] = input->y2 ^ delay[1] ^ delay[3];
	next[1] = delay[0];
	next[2] = delay[3];
	next[3] = feedback ^ delay[3];
	next[4] = tail_mix ^ delay[2] ^ input->y3 ^
		(input->y2 & delay[3]);
	next[5] = input->y4 ^ tail_mix ^ (feedback & delay[3]);
	for (uint8_t index = 0; index < 6; ++index)
		packed |= pack_state_bit(next[index], index);
	return packed;
}

enum v34_trellis_result
v34_convolutional_encoder_advance(
	struct v34_convolutional_encoder *encoder,
	const struct v34_convolutional_input *input)
{
	struct v34_convolutional_encoder next;

	if (!v34_convolutional_encoder_is_valid(encoder) ||
	    !v34_convolutional_input_is_valid(input))
		return V34_TRELLIS_INVALID_ARGUMENT;
	next = *encoder;
	switch (next.selection) {
	case V34_TRELLIS_16_STATE:
		next.state = advance_16_state(next.state, input);
		break;
	case V34_TRELLIS_32_STATE:
		next.state = advance_32_state(next.state, input);
		break;
	case V34_TRELLIS_64_STATE:
		next.state = advance_64_state(next.state, input);
		break;
	}
	if (!v34_convolutional_encoder_is_valid(&next))
		return V34_TRELLIS_INVALID_ARGUMENT;
	*encoder = next;
	return V34_TRELLIS_OK;
}
