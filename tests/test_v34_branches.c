// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>
#include <v34_branches.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

static bool initialize_parameters(
	struct v34_mapping_parameters *parameters)
{
	struct v34_frame_geometry geometry;

	return v34_frame_geometry_init(
		       &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       parameters, &geometry, true) ==
		       V34_MAPPING_PARAMETERS_OK;
}

static bool grid_points_are_equal(
	const struct v34_grid_point *left,
	const struct v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool inputs_are_equal(
	const struct v34_convolutional_input *left,
	const struct v34_convolutional_input *right)
{
	return left->y1 == right->y1 && left->y2 == right->y2 &&
	       left->y3 == right->y3 && left->y4 == right->y4;
}

static uint8_t expected_branch_count(
	enum v34_trellis_selection selection)
{
	switch (selection) {
	case V34_TRELLIS_32_STATE:
		return 16;
	case V34_TRELLIS_64_STATE:
		return 32;
	case V34_TRELLIS_16_STATE:
		break;
	}
	return 0;
}

static uint8_t expected_subset_index(
	const struct v34_convolutional_input *input, uint8_t y0,
	enum v34_trellis_selection selection)
{
	uint8_t index = y0 | (uint8_t)(input->y1 << 1) |
			(uint8_t)(input->y2 << 2);

	if (selection == V34_TRELLIS_32_STATE)
		index |= (uint8_t)(input->y4 << 3);
	else
		index |= (uint8_t)(input->y3 << 3) |
			 (uint8_t)(input->y4 << 4);
	return index;
}

static int32_t pair_magnitude_squared(
	const struct v34_subset_slice slices[2],
	uint8_t first, uint8_t second)
{
	const struct v34_grid_point *points[] = {
		&slices[0].subset[first].channel_output,
		&slices[1].subset[second].channel_output,
	};
	int32_t magnitude = 0;

	for (size_t symbol = 0; symbol < ARRAY_LENGTH(points); ++symbol)
		magnitude += (int32_t)points[symbol]->in_phase *
				     points[symbol]->in_phase +
			     (int32_t)points[symbol]->quadrature *
				     points[symbol]->quadrature;
	return magnitude;
}

static bool selected_pair_precedes(
	const struct v34_branch *selected,
	const struct v34_subset_slice slices[2], uint8_t first,
	uint8_t second, double squared_error)
{
	uint8_t selected_first = selected->symbol[0].subset_label;
	uint8_t selected_second = selected->symbol[1].subset_label;
	int32_t selected_magnitude;
	int32_t magnitude;

	if (selected->squared_error != squared_error)
		return selected->squared_error < squared_error;
	selected_magnitude = pair_magnitude_squared(
		slices, selected_first, selected_second);
	magnitude = pair_magnitude_squared(slices, first, second);
	if (selected_magnitude != magnitude)
		return selected_magnitude < magnitude;
	if (selected_first != first)
		return selected_first < first;
	return selected_second <= second;
}

static bool verify_reduction(
	const struct v34_subset_slice slices[2],
	uint8_t inversion_bit,
	enum v34_trellis_selection selection, size_t case_index)
{
	struct v34_branch_set set;
	uint8_t count = expected_branch_count(selection);

	CHECK(v34_build_branch_set(
		      slices, inversion_bit, selection, &set) ==
		      V34_BRANCHES_OK &&
	      set.selection == selection && set.count == count &&
	      set.inversion_bit == inversion_bit &&
	      v34_branch_set_is_valid(&set),
	      "branch set %zu/%u/%u did not build", case_index, inversion_bit,
	      selection);
	for (uint8_t index = 0; index < count; ++index) {
		const struct v34_branch *branch = &set.branch[index];

		CHECK(expected_subset_index(
			      &branch->convolutional_input,
			      branch->convolutional_output, selection) == index &&
		      branch->squared_error ==
			      branch->symbol[0].squared_error +
				      branch->symbol[1].squared_error,
		      "branch set %zu/%u/%u subset %u changed", case_index,
		      inversion_bit, selection, index);
	}
	for (uint8_t first = 0; first < V34_SUBSET_LABEL_COUNT;
	     ++first) {
		for (uint8_t second = 0;
		     second < V34_SUBSET_LABEL_COUNT; ++second) {
			struct v34_convolutional_input input;
			uint8_t y0 = (uint8_t)(
				(first ^ second ^ inversion_bit) & 1u);
			double squared_error =
				slices[0].subset[first].squared_error +
				slices[1].subset[second].squared_error;
			uint8_t index;

			CHECK(v34_convolutional_input_from_subset_labels(
				      first, second, &input) ==
				      V34_TRELLIS_OK,
			      "oracle pair %u/%u was rejected", first, second);
			index = expected_subset_index(&input, y0, selection);
			CHECK(selected_pair_precedes(
				      &set.branch[index], slices, first, second,
				      squared_error),
			      "branch set %zu/%u/%u kept a non-minimal pair",
			      case_index, inversion_bit, selection);
		}
	}
	return true;
}

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double generated_estimate(uint32_t *state)
{
	uint32_t word = next_generated_word(state);
	int integral = (int)(word & UINT32_C(0x7ff)) - 1024;
	double fraction = (double)((word >> 11) & UINT32_C(0x3ff)) / 1024.0;

	return integral * 0.5 + fraction;
}

static bool test_generated_reduction(void)
{
	struct v34_subset_slice slices[2];
	uint32_t random = UINT32_C(0xb4a6c43e);

	for (size_t index = 0; index < 96u; ++index) {
		for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol) {
			const struct qam_point estimate = {
				generated_estimate(&random),
				generated_estimate(&random),
			};

			CHECK(v34_slice_grid_subsets(
				      &estimate, &slices[symbol]) ==
				      V34_LATTICE_OK,
			      "generated slice %zu/%zu failed", index, symbol);
		}
		for (uint8_t inversion = 0; inversion < 2u; ++inversion) {
			for (enum v34_trellis_selection selection =
				     V34_TRELLIS_32_STATE;
			     selection <= V34_TRELLIS_64_STATE;
			     ++selection)
				CHECK(verify_reduction(
					      slices, inversion, selection, index),
				      "generated reduction %zu/%u/%u failed",
				      index, inversion, selection);
		}
	}
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol)
		for (uint8_t label = 0;
		     label < V34_SUBSET_LABEL_COUNT; ++label)
			slices[symbol].subset[label].squared_error = 0.0;
	for (uint8_t inversion = 0; inversion < 2u; ++inversion)
		for (enum v34_trellis_selection selection =
			     V34_TRELLIS_32_STATE;
		     selection <= V34_TRELLIS_64_STATE;
		     ++selection)
			CHECK(verify_reduction(
				      slices, inversion, selection, 96u),
			      "tie reduction %u/%u failed", inversion, selection);
	return true;
}

static bool branch_matches_encoder(
	const struct v34_branch *branch,
	const struct v34_4d_encoder_output *encoded)
{
	if (branch->convolutional_output != encoded->convolutional_output ||
	    !inputs_are_equal(
		    &branch->convolutional_input,
		    &encoded->convolutional_input) ||
	    branch->squared_error != 0.0)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (branch->symbol[symbol].subset_label !=
			    encoded->subset_label[symbol] ||
		    !grid_points_are_equal(
			    &branch->symbol[symbol].channel_output,
			    &encoded->precoded[symbol].channel_output) ||
		    branch->symbol[symbol].squared_error != 0.0)
			return false;
	}
	return true;
}

static bool test_encoder_agreement(void)
{
	struct v34_mapping_parameters parameters;

	CHECK(initialize_parameters(&parameters),
	      "encoder-agreement parameters failed");
	for (enum v34_trellis_selection selection =
		     V34_TRELLIS_32_STATE;
	     selection <= V34_TRELLIS_64_STATE; ++selection) {
		struct v34_4d_encoder encoder;
		uint8_t point_limit = (uint8_t)(
			1u << parameters.point_index_bits_per_2d_symbol);

		CHECK(v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      V34_4D_ENCODER_OK,
		      "encoder-agreement mode %u did not initialize", selection);
		for (uint8_t interval = 0; interval < 96u; ++interval) {
			struct v34_4d_encoder_input input = {
				.mapping = {
					.rotation_bit = (uint8_t)((interval >> 1) & 1u),
					.differential_input = interval & 3u,
					.point_index = {
						(uint8_t)((3u * interval + selection) %
							  point_limit),
						(uint8_t)((5u * interval + 1u) %
							  point_limit),
					},
				},
				.ring_index = {
					(uint8_t)(interval % parameters.ring_count),
					(uint8_t)((7u * interval + selection) %
						  parameters.ring_count),
				},
				.inversion_bit = (uint8_t)((interval >> 2) & 1u),
			};
			struct v34_4d_encoder_output encoded;
			struct v34_subset_slice slices[2];
			struct v34_branch_set set;
			uint8_t index;

			CHECK(v34_4d_encoder_step(
				      &encoder, &input, &encoded) ==
				      V34_4D_ENCODER_OK,
			      "encoder-agreement mode %u interval %u failed",
			      selection, interval);
			for (uint8_t symbol = 0;
			     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
			     ++symbol) {
				const struct qam_point estimate = {
					encoded.precoded[symbol]
						.channel_output.in_phase,
					encoded.precoded[symbol]
						.channel_output.quadrature,
				};

				CHECK(v34_slice_grid_subsets(
					      &estimate, &slices[symbol]) ==
					      V34_LATTICE_OK,
				      "encoded slice %u/%u/%u failed", selection,
				      interval, symbol);
			}
			CHECK(v34_build_branch_set(
				      slices, input.inversion_bit, selection, &set) ==
				      V34_BRANCHES_OK,
			      "encoded branch set %u/%u failed", selection,
			      interval);
			index = expected_subset_index(
				&encoded.convolutional_input,
				encoded.convolutional_output, selection);
			CHECK(branch_matches_encoder(&set.branch[index], &encoded),
			      "encoded branch %u/%u/%u disagreed", selection,
			      interval, index);
		}
	}
	return true;
}

static bool rejection_is_transactional(
	const struct v34_subset_slice *slices,
	uint8_t inversion_bit,
	enum v34_trellis_selection selection,
	enum v34_branches_result expected)
{
	struct v34_branch_set set;
	struct v34_branch_set saved;

	memset(&set, 0xa5, sizeof(set));
	saved = set;
	return v34_build_branch_set(
		       slices, inversion_bit, selection, &set) == expected &&
	       memcmp(&set, &saved, sizeof(set)) == 0;
}

static bool test_validation(void)
{
	const struct qam_point estimates[] = {
		{ 1.25, -3.5 }, { -7.75, 9.0 },
	};
	struct v34_subset_slice slices[2];
	struct v34_subset_slice malformed[2];
	struct v34_branch_set branches;
	struct v34_branch_set malformed_branches;

	for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol)
		CHECK(v34_slice_grid_subsets(
			      &estimates[symbol], &slices[symbol]) ==
			      V34_LATTICE_OK,
		      "validation slice %zu failed", symbol);
	CHECK(v34_build_branch_set(
		      slices, 0, V34_TRELLIS_32_STATE, &branches) ==
		      V34_BRANCHES_OK &&
	      v34_branch_set_is_valid(&branches) &&
	      !v34_branch_set_is_valid(NULL),
	      "valid branch-set contract changed");
	malformed_branches = branches;
	malformed_branches.count--;
	CHECK(!v34_branch_set_is_valid(&malformed_branches),
	      "incomplete branch set was accepted");
	malformed_branches = branches;
	malformed_branches.inversion_bit ^= 1u;
	CHECK(!v34_branch_set_is_valid(&malformed_branches),
	      "inconsistent branch inversion was accepted");
	malformed_branches = branches;
	malformed_branches.branch[0].convolutional_input.y3 ^= 1u;
	CHECK(!v34_branch_set_is_valid(&malformed_branches),
	      "branch labels disagreed with their convolutional input");
	malformed_branches = branches;
	malformed_branches.branch[0].squared_error += 1.0;
	CHECK(!v34_branch_set_is_valid(&malformed_branches),
	      "inconsistent branch metric was accepted");
	CHECK(rejection_is_transactional(
		      NULL, 0, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT) &&
	      rejection_is_transactional(
		      slices, 2, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT) &&
	      rejection_is_transactional(
		      slices, 0, V34_TRELLIS_16_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT) &&
	      rejection_is_transactional(
		      slices, 0, (enum v34_trellis_selection)3,
		      V34_BRANCHES_INVALID_ARGUMENT) &&
	      v34_build_branch_set(
		      slices, 0, V34_TRELLIS_32_STATE, NULL) ==
		      V34_BRANCHES_INVALID_ARGUMENT,
	      "invalid branch-set arguments were accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].subset_label = 1;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT),
	      "mislabelled subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].channel_output.in_phase = 0;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT),
	      "off-grid subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].channel_output.in_phase = 257;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_64_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT),
	      "out-of-range subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].squared_error = NAN;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT),
	      "non-finite subset metric was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].squared_error = -1.0;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_32_STATE,
		      V34_BRANCHES_INVALID_ARGUMENT),
	      "negative subset metric was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(malformed); ++symbol)
		for (uint8_t label = 0;
		     label < V34_SUBSET_LABEL_COUNT; ++label)
			malformed[symbol].subset[label].squared_error = DBL_MAX;
	CHECK(rejection_is_transactional(
		      malformed, 0, V34_TRELLIS_64_STATE,
		      V34_BRANCHES_NUMERIC_FAILURE),
	      "overflowing branch metric was accepted");
	CHECK(strcmp(v34_branches_result_name(
			     V34_BRANCHES_OK),
		     "ok") == 0 &&
	      strcmp(v34_branches_result_name(
			     V34_BRANCHES_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_branches_result_name(
			     V34_BRANCHES_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_branches_result_name(
			     (enum v34_branches_result)99),
		     "unknown") == 0,
	      "branch result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated subset reduction", test_generated_reduction },
		{ "encoder agreement", test_encoder_agreement },
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
