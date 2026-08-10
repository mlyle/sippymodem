// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_trellis.h>

#include "test.h"

#include <string.h>

/* Compact normative oracle; the production converter stores no such table. */
static const uint8_t TABLE13[SIPPYMODEM_V34_SUBSET_LABEL_COUNT]
			    [SIPPYMODEM_V34_SUBSET_LABEL_COUNT] = {
	{ 0x0, 0x0, 0x1, 0x1, 0x8, 0x8, 0x9, 0x9 },
	{ 0x3, 0x2, 0x2, 0x3, 0xb, 0xa, 0xa, 0xb },
	{ 0x5, 0x5, 0x4, 0x4, 0xd, 0xd, 0xc, 0xc },
	{ 0x6, 0x7, 0x7, 0x6, 0xe, 0xf, 0xf, 0xe },
	{ 0x8, 0x8, 0x9, 0x9, 0x0, 0x0, 0x1, 0x1 },
	{ 0xb, 0xa, 0xa, 0xb, 0x3, 0x2, 0x2, 0x3 },
	{ 0xd, 0xd, 0xc, 0xc, 0x5, 0x5, 0x4, 0x4 },
	{ 0xe, 0xf, 0xf, 0xe, 0x6, 0x7, 0x7, 0x6 },
};

/* Figure 9 in printed row/column order; production derives these labels. */
static const uint8_t FIGURE9_LABELS[4][4] = {
	{ 1, 6, 5, 2 },
	{ 4, 3, 0, 7 },
	{ 5, 2, 1, 6 },
	{ 0, 7, 4, 3 },
};

static bool test_grid_subset_labels(void)
{
	static const int16_t in_phase_coordinates[] = { -3, -1, 1, 3 };
	static const int16_t quadrature_coordinates[] = { 3, 1, -1, -3 };
	static const struct {
		struct sippymodem_v34_grid_point point;
		uint8_t expected;
	} extreme_cases[] = {
		{ { INT16_MIN + 1, INT16_MAX }, 1 },
		{ { INT16_MAX, INT16_MIN + 1 }, 3 },
	};
	unsigned int points_checked = 0;

	for (int quadrature_tile = -4; quadrature_tile <= 4;
	     ++quadrature_tile) {
		for (int in_phase_tile = -4; in_phase_tile <= 4;
		     ++in_phase_tile) {
			for (size_t row = 0;
			     row < ARRAY_LENGTH(quadrature_coordinates); ++row) {
				for (size_t column = 0;
				     column < ARRAY_LENGTH(in_phase_coordinates);
				     ++column) {
					const struct sippymodem_v34_grid_point point = {
						.in_phase = (int16_t)(
							in_phase_coordinates[column] +
							8 * in_phase_tile),
						.quadrature = (int16_t)(
							quadrature_coordinates[row] +
							8 * quadrature_tile),
					};
					uint8_t label = UINT8_MAX;

					CHECK(sippymodem_v34_grid_subset_label(
						      &point, &label) ==
						      SIPPYMODEM_V34_TRELLIS_OK &&
					      label == FIGURE9_LABELS[row][column],
					      "grid tile %d/%d point %zu/%zu produced %u",
					      in_phase_tile, quadrature_tile, column,
					      row, label);
					points_checked++;
				}
			}
		}
	}
	for (size_t index = 0; index < ARRAY_LENGTH(extreme_cases); ++index) {
		uint8_t label = UINT8_MAX;

		CHECK(sippymodem_v34_grid_subset_label(
			      &extreme_cases[index].point, &label) ==
			      SIPPYMODEM_V34_TRELLIS_OK &&
		      label == extreme_cases[index].expected,
		      "extreme grid point %zu produced %u", index, label);
		points_checked++;
	}
	CHECK(points_checked == 9u * 9u * 16u +
				       ARRAY_LENGTH(extreme_cases),
	      "grid-label coverage changed to %u points", points_checked);
	return true;
}

static uint8_t model_modulo_encoder_output(
	const struct sippymodem_v34_grid_point *first,
	const struct sippymodem_v34_grid_point *second)
{
	int half_coordinate_sum = first->in_phase / 2 + first->quadrature / 2 +
		second->in_phase / 2 + second->quadrature / 2;
	int modulo_two = half_coordinate_sum % 2;

	return (uint8_t)(modulo_two < 0 ? modulo_two + 2 : modulo_two);
}

static bool test_modulo_encoder(void)
{
	static const int16_t coordinates[] = {
		-8, -6, -4, -2, 0, 2, 4, 6, 8,
	};
	static const struct {
		struct sippymodem_v34_grid_point first;
		struct sippymodem_v34_grid_point second;
	} extreme_cases[] = {
		{ { INT16_MIN, INT16_MIN }, { INT16_MIN, INT16_MIN } },
		{ { INT16_MIN, INT16_MAX - 1 },
		  { INT16_MAX - 1, INT16_MIN } },
		{ { INT16_MAX - 1, INT16_MAX - 1 },
		  { INT16_MAX - 1, INT16_MAX - 1 } },
	};
	unsigned int cases_checked = 0;

	for (size_t first_i = 0; first_i < ARRAY_LENGTH(coordinates);
	     ++first_i) {
		for (size_t first_q = 0; first_q < ARRAY_LENGTH(coordinates);
		     ++first_q) {
			for (size_t second_i = 0;
			     second_i < ARRAY_LENGTH(coordinates); ++second_i) {
				for (size_t second_q = 0;
				     second_q < ARRAY_LENGTH(coordinates);
				     ++second_q) {
					const struct sippymodem_v34_grid_point first = {
						coordinates[first_i],
						coordinates[first_q],
					};
					const struct sippymodem_v34_grid_point second = {
						coordinates[second_i],
						coordinates[second_q],
					};
					uint8_t c0 = UINT8_MAX;

					CHECK(sippymodem_v34_modulo_encoder_output(
						      &first, &second, &c0) ==
						      SIPPYMODEM_V34_TRELLIS_OK &&
					      c0 == model_modulo_encoder_output(
							    &first, &second),
					      "modulo case %zu/%zu/%zu/%zu produced %u",
					      first_i, first_q, second_i, second_q,
					      c0);
					cases_checked++;
				}
			}
		}
	}
	for (size_t index = 0; index < ARRAY_LENGTH(extreme_cases); ++index) {
		uint8_t c0 = UINT8_MAX;

		CHECK(sippymodem_v34_modulo_encoder_output(
			      &extreme_cases[index].first,
			      &extreme_cases[index].second, &c0) ==
			      SIPPYMODEM_V34_TRELLIS_OK &&
		      c0 == model_modulo_encoder_output(
				    &extreme_cases[index].first,
				    &extreme_cases[index].second),
		      "extreme modulo case %zu produced %u", index, c0);
		cases_checked++;
	}
	CHECK(cases_checked == 9u * 9u * 9u * 9u +
				       ARRAY_LENGTH(extreme_cases),
	      "modulo coverage changed to %u cases", cases_checked);
	return true;
}

static uint8_t packed_input(
	const struct sippymodem_v34_convolutional_input *input)
{
	return (uint8_t)(input->y1 | (input->y2 << 1) |
			 (input->y3 << 2) | (input->y4 << 3));
}

static struct sippymodem_v34_convolutional_input unpacked_input(
	uint8_t packed)
{
	return (struct sippymodem_v34_convolutional_input){
		.y1 = packed & 1u,
		.y2 = (packed >> 1) & 1u,
		.y3 = (packed >> 2) & 1u,
		.y4 = (packed >> 3) & 1u,
	};
}

static uint8_t bit(uint8_t value, uint8_t index)
{
	return (uint8_t)((value >> index) & 1u);
}

static uint8_t placed_bit(uint8_t value, uint8_t index)
{
	return (uint8_t)((value & 1u) << index);
}

static uint8_t model_16_state(
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
{
	const uint8_t delay[4] = {
		bit(state, 3), bit(state, 2), bit(state, 1), bit(state, 0),
	};
	const uint8_t next[4] = {
		delay[3],
		delay[0] ^ input->y2 ^ delay[3],
		delay[1] ^ input->y2,
		delay[2] ^ input->y1,
	};

	return (uint8_t)(placed_bit(next[0], 3) |
			 placed_bit(next[1], 2) |
			 placed_bit(next[2], 1) |
			 placed_bit(next[3], 0));
}

static uint8_t model_32_state(
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
{
	const uint8_t delay[5] = {
		bit(state, 4), bit(state, 3), bit(state, 2),
		bit(state, 1), bit(state, 0),
	};
	const uint8_t next[5] = {
		delay[4],
		delay[0] ^ input->y2,
		delay[1] ^ input->y1,
		delay[2] ^ input->y4,
		delay[3] ^ input->y2,
	};

	return (uint8_t)(placed_bit(next[0], 4) |
			 placed_bit(next[1], 3) |
			 placed_bit(next[2], 2) |
			 placed_bit(next[3], 1) |
			 placed_bit(next[4], 0));
}

static uint8_t model_64_state(
	uint8_t state, const struct sippymodem_v34_convolutional_input *input)
{
	uint8_t q0 = bit(state, 0);
	uint8_t q1 = bit(state, 1);
	uint8_t q2 = bit(state, 2);
	uint8_t q3 = bit(state, 3);
	uint8_t q4 = bit(state, 4);
	uint8_t q5 = bit(state, 5);
	uint8_t feedback = input->y1 ^ q4;
	uint8_t tail = q4 ^ q5;

	return (uint8_t)(
		placed_bit(input->y2 ^ q1 ^ q3, 0) |
		placed_bit(q0, 1) |
		placed_bit(q3, 2) |
		placed_bit(feedback ^ q3, 3) |
		placed_bit(tail ^ q2 ^ input->y3 ^ (input->y2 & q3), 4) |
		placed_bit(input->y4 ^ tail ^ (feedback & q3), 5));
}

static uint8_t model_next_state(
	enum sippymodem_v34_trellis_selection selection, uint8_t state,
	const struct sippymodem_v34_convolutional_input *input)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return model_16_state(state, input);
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return model_32_state(state, input);
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return model_64_state(state, input);
	}
	return UINT8_MAX;
}

static uint8_t state_count(
	enum sippymodem_v34_trellis_selection selection)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return 16;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return 32;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return 64;
	}
	return 0;
}

static bool test_symbol_to_bit_converter(void)
{
	for (uint8_t first = 0;
	     first < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++first) {
		for (uint8_t second = 0;
		     second < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++second) {
			struct sippymodem_v34_convolutional_input input;

			CHECK(sippymodem_v34_convolutional_input_from_subset_labels(
				      first, second, &input) ==
				      SIPPYMODEM_V34_TRELLIS_OK &&
			      sippymodem_v34_convolutional_input_is_valid(&input),
			      "Table 13 labels %u/%u were rejected", first,
			      second);
			CHECK(packed_input(&input) == TABLE13[first][second],
			      "Table 13 labels %u/%u produced %#x instead of %#x",
			      first, second, packed_input(&input),
			      TABLE13[first][second]);
		}
	}
	return true;
}

static bool test_all_state_transitions(void)
{
	static const enum sippymodem_v34_trellis_selection selections[] = {
		SIPPYMODEM_V34_TRELLIS_16_STATE,
		SIPPYMODEM_V34_TRELLIS_32_STATE,
		SIPPYMODEM_V34_TRELLIS_64_STATE,
	};
	unsigned int transitions = 0;

	for (size_t mode = 0; mode < ARRAY_LENGTH(selections); ++mode) {
		uint8_t count = state_count(selections[mode]);

		for (uint8_t state = 0; state < count; ++state) {
			for (uint8_t packed = 0; packed < 16u; ++packed) {
				struct sippymodem_v34_convolutional_input input =
					unpacked_input(packed);
				struct sippymodem_v34_convolutional_encoder encoder = {
					.state = state,
					.selection = selections[mode],
				};
				uint8_t expected = model_next_state(
					selections[mode], state, &input);
				uint8_t y0 = UINT8_MAX;

				CHECK(sippymodem_v34_convolutional_encoder_is_valid(
					      &encoder) &&
				      sippymodem_v34_convolutional_encoder_output(
					      &encoder, &y0) ==
					      SIPPYMODEM_V34_TRELLIS_OK &&
				      y0 == (state & 1u),
				      "mode %zu state %u exposed the wrong delayed Y0",
				      mode, state);
				CHECK(sippymodem_v34_convolutional_encoder_advance(
					      &encoder, &input) ==
					      SIPPYMODEM_V34_TRELLIS_OK &&
				      encoder.state == expected,
				      "mode %zu state %u input %#x advanced to %#x, expected %#x",
				      mode, state, packed, encoder.state, expected);
				transitions++;
			}
		}
	}
	CHECK(transitions == (16u + 32u + 64u) * 16u,
	      "transition coverage changed to %u", transitions);
	return true;
}

static bool test_known_sequences(void)
{
	static const uint8_t subset_pairs[] = {
		0, 2, 8, 15, 31, 47, 63, 5, 22, 41,
		60, 9, 35, 51, 27, 58, 7, 44, 18, 55,
	};
	static const uint8_t expected_16_state[] = {
		0x0, 0x1, 0xb, 0xe, 0x1, 0xb, 0xf, 0xb, 0x9, 0xe,
		0x1, 0xa, 0x4, 0x2, 0x7, 0x8, 0x5, 0x9, 0x8, 0x4,
	};
	static const uint8_t expected_32_state[] = {
		0x0, 0x4, 0xf, 0x18, 0x7, 0x1e, 0x6, 0x1, 0x12, 0x2,
		0x8, 0xd, 0x10, 0xa, 0xc, 0x9, 0x12, 0x4, 0x2, 0x1,
	};
	static const uint8_t expected_64_y0[] = {
		0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
		0, 1, 0, 1, 0, 0, 1, 0, 0, 0,
	};
	static const enum sippymodem_v34_trellis_selection selections[] = {
		SIPPYMODEM_V34_TRELLIS_16_STATE,
		SIPPYMODEM_V34_TRELLIS_32_STATE,
		SIPPYMODEM_V34_TRELLIS_64_STATE,
	};

	for (size_t mode = 0; mode < ARRAY_LENGTH(selections); ++mode) {
		struct sippymodem_v34_convolutional_encoder encoder;

		CHECK(sippymodem_v34_convolutional_encoder_init(
			      &encoder, selections[mode]) ==
			      SIPPYMODEM_V34_TRELLIS_OK,
		      "known sequence mode %zu did not initialize", mode);
		for (size_t index = 0; index < ARRAY_LENGTH(subset_pairs);
		     ++index) {
			struct sippymodem_v34_convolutional_input input;
			uint8_t pair = subset_pairs[index];
			uint8_t y0 = UINT8_MAX;

			CHECK(sippymodem_v34_convolutional_input_from_subset_labels(
				      pair / SIPPYMODEM_V34_SUBSET_LABEL_COUNT,
				      pair % SIPPYMODEM_V34_SUBSET_LABEL_COUNT,
				      &input) ==
				      SIPPYMODEM_V34_TRELLIS_OK &&
			      sippymodem_v34_convolutional_encoder_advance(
				      &encoder, &input) ==
				      SIPPYMODEM_V34_TRELLIS_OK &&
			      sippymodem_v34_convolutional_encoder_output(
				      &encoder, &y0) ==
				      SIPPYMODEM_V34_TRELLIS_OK,
			      "known sequence mode %zu step %zu failed", mode,
			      index);
			if (mode == 0)
				CHECK(encoder.state == expected_16_state[index],
				      "16-state sequence step %zu changed", index);
			else if (mode == 1)
				CHECK(encoder.state == expected_32_state[index],
				      "32-state sequence step %zu changed", index);
			else
				CHECK(y0 == expected_64_y0[index],
				      "64-state sequence step %zu changed", index);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_convolutional_input input = { 0 };
	struct sippymodem_v34_convolutional_input saved_input;
	struct sippymodem_v34_convolutional_encoder encoder = {
		.state = 7,
		.selection = SIPPYMODEM_V34_TRELLIS_16_STATE,
	};
	struct sippymodem_v34_convolutional_encoder saved_encoder;
	struct sippymodem_v34_grid_point grid_point = { 1, 1 };
	struct sippymodem_v34_grid_point first_offset = { 2, 4 };
	struct sippymodem_v34_grid_point second_offset = { -6, 8 };
	uint8_t subset_label = UINT8_MAX;
	uint8_t c0 = UINT8_MAX;
	uint8_t y0 = UINT8_MAX;

	CHECK(sippymodem_v34_trellis_selection_is_valid(
		      SIPPYMODEM_V34_TRELLIS_16_STATE) &&
	      sippymodem_v34_trellis_selection_is_valid(
		      SIPPYMODEM_V34_TRELLIS_32_STATE) &&
	      sippymodem_v34_trellis_selection_is_valid(
		      SIPPYMODEM_V34_TRELLIS_64_STATE) &&
	      !sippymodem_v34_trellis_selection_is_valid(
		      (enum sippymodem_v34_trellis_selection)3),
	      "trellis selection validation changed");

	memset(&input, 0x5a, sizeof(input));
	saved_input = input;
	CHECK(sippymodem_v34_convolutional_input_from_subset_labels(
		      8, 0, &input) == SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_input_from_subset_labels(
		      0, 8, &input) == SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_input_from_subset_labels(
		      0, 0, NULL) == SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      memcmp(&input, &saved_input, sizeof(input)) == 0,
	      "invalid subset conversion changed its destination");
	CHECK(!sippymodem_v34_convolutional_input_is_valid(NULL),
	      "missing convolutional input was accepted");
	grid_point.in_phase = 0;
	CHECK(sippymodem_v34_grid_subset_label(
		      &grid_point, &subset_label) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      subset_label == UINT8_MAX,
	      "even in-phase coordinate changed its label");
	grid_point = (struct sippymodem_v34_grid_point){ 1, 0 };
	CHECK(sippymodem_v34_grid_subset_label(
		      &grid_point, &subset_label) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_grid_subset_label(NULL, &subset_label) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_grid_subset_label(&grid_point, NULL) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      subset_label == UINT8_MAX,
	      "invalid grid labeling changed its destination");
	first_offset.in_phase = 1;
	CHECK(sippymodem_v34_modulo_encoder_output(
		      &first_offset, &second_offset, &c0) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      c0 == UINT8_MAX,
	      "odd first modulo offset changed its destination");
	first_offset = (struct sippymodem_v34_grid_point){ 2, 4 };
	second_offset.quadrature = 7;
	CHECK(sippymodem_v34_modulo_encoder_output(
		      &first_offset, &second_offset, &c0) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_modulo_encoder_output(
		      NULL, &second_offset, &c0) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_modulo_encoder_output(
		      &first_offset, NULL, &c0) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_modulo_encoder_output(
		      &first_offset, &second_offset, NULL) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      c0 == UINT8_MAX,
	      "invalid modulo input changed its destination");
	for (uint8_t field = 0; field < 4u; ++field) {
		input = (struct sippymodem_v34_convolutional_input){ 0 };
		switch (field) {
		case 0:
			input.y1 = 2;
			break;
		case 1:
			input.y2 = 2;
			break;
		case 2:
			input.y3 = 2;
			break;
		case 3:
			input.y4 = 2;
			break;
		}
		CHECK(!sippymodem_v34_convolutional_input_is_valid(&input),
		      "convolutional input field %u accepted a non-bit", field);
	}

	saved_encoder = encoder;
	CHECK(sippymodem_v34_convolutional_encoder_init(NULL,
		      SIPPYMODEM_V34_TRELLIS_16_STATE) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_encoder_init(
		      &encoder, (enum sippymodem_v34_trellis_selection)3) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0,
	      "invalid encoder initialization changed its destination");
	input = (struct sippymodem_v34_convolutional_input){ 0 };
	CHECK(sippymodem_v34_convolutional_encoder_output(NULL, &y0) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_encoder_output(&encoder, NULL) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_encoder_advance(NULL, &input) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      sippymodem_v34_convolutional_encoder_advance(&encoder, NULL) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      y0 == UINT8_MAX &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0,
	      "invalid encoder operation changed state or output");
	encoder.state = 16;
	CHECK(!sippymodem_v34_convolutional_encoder_is_valid(&encoder) &&
	      sippymodem_v34_convolutional_encoder_advance(&encoder, &input) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT,
	      "out-of-range encoder state was accepted");
	encoder = saved_encoder;
	input.y4 = 2;
	CHECK(sippymodem_v34_convolutional_encoder_advance(&encoder, &input) ==
		      SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0,
	      "invalid convolutional input changed encoder state");
	CHECK(strcmp(sippymodem_v34_trellis_result_name(
		     SIPPYMODEM_V34_TRELLIS_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_trellis_result_name(
		     SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_trellis_result_name(
		     (enum sippymodem_v34_trellis_result)2),
		     "unknown") == 0,
	      "trellis result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "channel grid labels", test_grid_subset_labels },
		{ "modulo encoder", test_modulo_encoder },
		{ "symbol-to-bit converter", test_symbol_to_bit_converter },
		{ "all state transitions", test_all_state_transitions },
		{ "known sequences", test_known_sequences },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
