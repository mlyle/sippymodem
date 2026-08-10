// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_trellis.h"

#include <stddef.h>

static uint8_t selection_state_bits(
	enum sippymodem_v34_trellis_selection selection)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return 4;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return 5;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return 6;
	}
	return 0;
}

bool sippymodem_v34_trellis_selection_is_valid(
	enum sippymodem_v34_trellis_selection selection)
{
	return selection_state_bits(selection) != 0;
}

bool sippymodem_v34_convolutional_input_is_valid(
	const struct sippymodem_v34_convolutional_input *input)
{
	return input != NULL && input->y1 <= 1u && input->y2 <= 1u &&
	       input->y3 <= 1u && input->y4 <= 1u;
}

enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_input_from_subset_labels(
	uint8_t first_subset_label, uint8_t second_subset_label,
	struct sippymodem_v34_convolutional_input *input)
{
	struct sippymodem_v34_convolutional_input generated;
	uint8_t first_low;
	uint8_t second_low;

	if (first_subset_label >= SIPPYMODEM_V34_SUBSET_LABEL_COUNT ||
	    second_subset_label >= SIPPYMODEM_V34_SUBSET_LABEL_COUNT ||
	    input == NULL)
		return SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT;
	first_low = first_subset_label & 1u;
	second_low = second_subset_label & 1u;
	/* Boolean reduction of the eight rows and columns in Table 13. */
	generated = (struct sippymodem_v34_convolutional_input){
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
	return SIPPYMODEM_V34_TRELLIS_OK;
}

bool sippymodem_v34_convolutional_encoder_is_valid(
	const struct sippymodem_v34_convolutional_encoder *encoder)
{
	uint8_t state_bits;

	if (encoder == NULL)
		return false;
	state_bits = selection_state_bits(encoder->selection);
	return state_bits != 0 && encoder->state < (1u << state_bits);
}

enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_init(
	struct sippymodem_v34_convolutional_encoder *encoder,
	enum sippymodem_v34_trellis_selection selection)
{
	if (encoder == NULL ||
	    !sippymodem_v34_trellis_selection_is_valid(selection))
		return SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT;
	*encoder = (struct sippymodem_v34_convolutional_encoder){
		.selection = selection,
	};
	return SIPPYMODEM_V34_TRELLIS_OK;
}

enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_output(
	const struct sippymodem_v34_convolutional_encoder *encoder,
	uint8_t *y0)
{
	if (!sippymodem_v34_convolutional_encoder_is_valid(encoder) ||
	    y0 == NULL)
		return SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT;
	*y0 = encoder->state & 1u;
	return SIPPYMODEM_V34_TRELLIS_OK;
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
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
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
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
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
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
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

enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_advance(
	struct sippymodem_v34_convolutional_encoder *encoder,
	const struct sippymodem_v34_convolutional_input *input)
{
	struct sippymodem_v34_convolutional_encoder next;

	if (!sippymodem_v34_convolutional_encoder_is_valid(encoder) ||
	    !sippymodem_v34_convolutional_input_is_valid(input))
		return SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT;
	next = *encoder;
	switch (next.selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		next.state = advance_16_state(next.state, input);
		break;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		next.state = advance_32_state(next.state, input);
		break;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		next.state = advance_64_state(next.state, input);
		break;
	}
	if (!sippymodem_v34_convolutional_encoder_is_valid(&next))
		return SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT;
	*encoder = next;
	return SIPPYMODEM_V34_TRELLIS_OK;
}

const char *sippymodem_v34_trellis_result_name(
	enum sippymodem_v34_trellis_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_TRELLIS_OK:
		return "ok";
	case SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
