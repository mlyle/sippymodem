// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>
#include <v34_branches.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

static bool initialize_parameters(
	struct sippymodem_v34_mapping_parameters *parameters)
{
	struct sippymodem_v34_frame_geometry geometry;

	return sippymodem_v34_frame_geometry_init(
		       &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8, false) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       sippymodem_v34_mapping_parameters_init(
		       parameters, &geometry, true) ==
		       SIPPYMODEM_V34_MAPPING_PARAMETERS_OK;
}

static bool grid_points_are_equal(
	const struct sippymodem_v34_grid_point *left,
	const struct sippymodem_v34_grid_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool inputs_are_equal(
	const struct sippymodem_v34_convolutional_input *left,
	const struct sippymodem_v34_convolutional_input *right)
{
	return left->y1 == right->y1 && left->y2 == right->y2 &&
	       left->y3 == right->y3 && left->y4 == right->y4;
}

static uint8_t expected_branch_count(
	enum sippymodem_v34_trellis_selection selection)
{
	switch (selection) {
	case SIPPYMODEM_V34_TRELLIS_16_STATE:
		return 8;
	case SIPPYMODEM_V34_TRELLIS_32_STATE:
		return 16;
	case SIPPYMODEM_V34_TRELLIS_64_STATE:
		return 32;
	}
	return 0;
}

static uint8_t expected_subset_index(
	const struct sippymodem_v34_convolutional_input *input, uint8_t y0,
	enum sippymodem_v34_trellis_selection selection)
{
	uint8_t index = y0 | (uint8_t)(input->y1 << 1) |
			(uint8_t)(input->y2 << 2);

	if (selection == SIPPYMODEM_V34_TRELLIS_32_STATE)
		index |= (uint8_t)(input->y4 << 3);
	else if (selection == SIPPYMODEM_V34_TRELLIS_64_STATE)
		index |= (uint8_t)(input->y3 << 3) |
			 (uint8_t)(input->y4 << 4);
	return index;
}

static int32_t pair_magnitude_squared(
	const struct sippymodem_v34_subset_slice slices[2],
	uint8_t first, uint8_t second)
{
	const struct sippymodem_v34_grid_point *points[] = {
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
	const struct sippymodem_v34_branch *selected,
	const struct sippymodem_v34_subset_slice slices[2], uint8_t first,
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
	const struct sippymodem_v34_subset_slice slices[2],
	uint8_t inversion_bit,
	enum sippymodem_v34_trellis_selection selection, size_t case_index)
{
	struct sippymodem_v34_branch_set set;
	uint8_t count = expected_branch_count(selection);

	CHECK(sippymodem_v34_build_branch_set(
		      slices, inversion_bit, selection, &set) ==
		      SIPPYMODEM_V34_BRANCHES_OK &&
	      set.selection == selection && set.count == count &&
	      set.inversion_bit == inversion_bit &&
	      sippymodem_v34_branch_set_is_valid(&set),
	      "branch set %zu/%u/%u did not build", case_index, inversion_bit,
	      selection);
	for (uint8_t index = 0; index < count; ++index) {
		const struct sippymodem_v34_branch *branch = &set.branch[index];

		CHECK(expected_subset_index(
			      &branch->convolutional_input,
			      branch->convolutional_output, selection) == index &&
		      branch->squared_error ==
			      branch->symbol[0].squared_error +
				      branch->symbol[1].squared_error,
		      "branch set %zu/%u/%u subset %u changed", case_index,
		      inversion_bit, selection, index);
	}
	for (uint8_t first = 0; first < SIPPYMODEM_V34_SUBSET_LABEL_COUNT;
	     ++first) {
		for (uint8_t second = 0;
		     second < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++second) {
			struct sippymodem_v34_convolutional_input input;
			uint8_t y0 = (uint8_t)(
				(first ^ second ^ inversion_bit) & 1u);
			double squared_error =
				slices[0].subset[first].squared_error +
				slices[1].subset[second].squared_error;
			uint8_t index;

			CHECK(sippymodem_v34_convolutional_input_from_subset_labels(
				      first, second, &input) ==
				      SIPPYMODEM_V34_TRELLIS_OK,
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
	struct sippymodem_v34_subset_slice slices[2];
	uint32_t random = UINT32_C(0xb4a6c43e);

	for (size_t index = 0; index < 96u; ++index) {
		for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol) {
			const struct sippymodem_qam_point estimate = {
				generated_estimate(&random),
				generated_estimate(&random),
			};

			CHECK(sippymodem_v34_slice_grid_subsets(
				      &estimate, &slices[symbol]) ==
				      SIPPYMODEM_V34_LATTICE_OK,
			      "generated slice %zu/%zu failed", index, symbol);
		}
		for (uint8_t inversion = 0; inversion < 2u; ++inversion) {
			for (enum sippymodem_v34_trellis_selection selection =
				     SIPPYMODEM_V34_TRELLIS_16_STATE;
			     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE;
			     ++selection)
				CHECK(verify_reduction(
					      slices, inversion, selection, index),
				      "generated reduction %zu/%u/%u failed",
				      index, inversion, selection);
		}
	}
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol)
		for (uint8_t label = 0;
		     label < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++label)
			slices[symbol].subset[label].squared_error = 0.0;
	for (uint8_t inversion = 0; inversion < 2u; ++inversion)
		for (enum sippymodem_v34_trellis_selection selection =
			     SIPPYMODEM_V34_TRELLIS_16_STATE;
		     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE;
		     ++selection)
			CHECK(verify_reduction(
				      slices, inversion, selection, 96u),
			      "tie reduction %u/%u failed", inversion, selection);
	return true;
}

static bool branch_matches_encoder(
	const struct sippymodem_v34_branch *branch,
	const struct sippymodem_v34_4d_encoder_output *encoded)
{
	if (branch->convolutional_output != encoded->convolutional_output ||
	    !inputs_are_equal(
		    &branch->convolutional_input,
		    &encoded->convolutional_input) ||
	    branch->squared_error != 0.0)
		return false;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
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
	struct sippymodem_v34_mapping_parameters parameters;

	CHECK(initialize_parameters(&parameters),
	      "encoder-agreement parameters failed");
	for (enum sippymodem_v34_trellis_selection selection =
		     SIPPYMODEM_V34_TRELLIS_16_STATE;
	     selection <= SIPPYMODEM_V34_TRELLIS_64_STATE; ++selection) {
		struct sippymodem_v34_4d_encoder encoder;
		uint8_t point_limit = (uint8_t)(
			1u << parameters.point_index_bits_per_2d_symbol);

		CHECK(sippymodem_v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      SIPPYMODEM_V34_4D_ENCODER_OK,
		      "encoder-agreement mode %u did not initialize", selection);
		for (uint8_t interval = 0; interval < 96u; ++interval) {
			struct sippymodem_v34_4d_encoder_input input = {
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
			struct sippymodem_v34_4d_encoder_output encoded;
			struct sippymodem_v34_subset_slice slices[2];
			struct sippymodem_v34_branch_set set;
			uint8_t index;

			CHECK(sippymodem_v34_4d_encoder_step(
				      &encoder, &input, &encoded) ==
				      SIPPYMODEM_V34_4D_ENCODER_OK,
			      "encoder-agreement mode %u interval %u failed",
			      selection, interval);
			for (uint8_t symbol = 0;
			     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL;
			     ++symbol) {
				const struct sippymodem_qam_point estimate = {
					encoded.precoded[symbol]
						.channel_output.in_phase,
					encoded.precoded[symbol]
						.channel_output.quadrature,
				};

				CHECK(sippymodem_v34_slice_grid_subsets(
					      &estimate, &slices[symbol]) ==
					      SIPPYMODEM_V34_LATTICE_OK,
				      "encoded slice %u/%u/%u failed", selection,
				      interval, symbol);
			}
			CHECK(sippymodem_v34_build_branch_set(
				      slices, input.inversion_bit, selection, &set) ==
				      SIPPYMODEM_V34_BRANCHES_OK,
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
	const struct sippymodem_v34_subset_slice *slices,
	uint8_t inversion_bit,
	enum sippymodem_v34_trellis_selection selection,
	enum sippymodem_v34_branches_result expected)
{
	struct sippymodem_v34_branch_set set;
	struct sippymodem_v34_branch_set saved;

	memset(&set, 0xa5, sizeof(set));
	saved = set;
	return sippymodem_v34_build_branch_set(
		       slices, inversion_bit, selection, &set) == expected &&
	       memcmp(&set, &saved, sizeof(set)) == 0;
}

static bool test_validation(void)
{
	const struct sippymodem_qam_point estimates[] = {
		{ 1.25, -3.5 }, { -7.75, 9.0 },
	};
	struct sippymodem_v34_subset_slice slices[2];
	struct sippymodem_v34_subset_slice malformed[2];
	struct sippymodem_v34_branch_set branches;
	struct sippymodem_v34_branch_set malformed_branches;

	for (size_t symbol = 0; symbol < ARRAY_LENGTH(slices); ++symbol)
		CHECK(sippymodem_v34_slice_grid_subsets(
			      &estimates[symbol], &slices[symbol]) ==
			      SIPPYMODEM_V34_LATTICE_OK,
		      "validation slice %zu failed", symbol);
	CHECK(sippymodem_v34_build_branch_set(
		      slices, 0, SIPPYMODEM_V34_TRELLIS_16_STATE, &branches) ==
		      SIPPYMODEM_V34_BRANCHES_OK &&
	      sippymodem_v34_branch_set_is_valid(&branches) &&
	      !sippymodem_v34_branch_set_is_valid(NULL),
	      "valid branch-set contract changed");
	malformed_branches = branches;
	malformed_branches.count--;
	CHECK(!sippymodem_v34_branch_set_is_valid(&malformed_branches),
	      "incomplete branch set was accepted");
	malformed_branches = branches;
	malformed_branches.inversion_bit ^= 1u;
	CHECK(!sippymodem_v34_branch_set_is_valid(&malformed_branches),
	      "inconsistent branch inversion was accepted");
	malformed_branches = branches;
	malformed_branches.branch[0].convolutional_input.y3 ^= 1u;
	CHECK(!sippymodem_v34_branch_set_is_valid(&malformed_branches),
	      "branch labels disagreed with their convolutional input");
	malformed_branches = branches;
	malformed_branches.branch[0].squared_error += 1.0;
	CHECK(!sippymodem_v34_branch_set_is_valid(&malformed_branches),
	      "inconsistent branch metric was accepted");
	CHECK(rejection_is_transactional(
		      NULL, 0, SIPPYMODEM_V34_TRELLIS_16_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT) &&
	      rejection_is_transactional(
		      slices, 2, SIPPYMODEM_V34_TRELLIS_16_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT) &&
	      rejection_is_transactional(
		      slices, 0, (enum sippymodem_v34_trellis_selection)3,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT) &&
	      sippymodem_v34_build_branch_set(
		      slices, 0, SIPPYMODEM_V34_TRELLIS_16_STATE, NULL) ==
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT,
	      "invalid branch-set arguments were accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].subset_label = 1;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_16_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
	      "mislabelled subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].channel_output.in_phase = 0;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_32_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
	      "off-grid subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].channel_output.in_phase = 257;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_64_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
	      "out-of-range subset was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].squared_error = NAN;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_16_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
	      "non-finite subset metric was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	malformed[0].subset[0].squared_error = -1.0;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_16_STATE,
		      SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
	      "negative subset metric was accepted");
	memcpy(malformed, slices, sizeof(malformed));
	for (size_t symbol = 0; symbol < ARRAY_LENGTH(malformed); ++symbol)
		for (uint8_t label = 0;
		     label < SIPPYMODEM_V34_SUBSET_LABEL_COUNT; ++label)
			malformed[symbol].subset[label].squared_error = DBL_MAX;
	CHECK(rejection_is_transactional(
		      malformed, 0, SIPPYMODEM_V34_TRELLIS_64_STATE,
		      SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE),
	      "overflowing branch metric was accepted");
	CHECK(strcmp(sippymodem_v34_branches_result_name(
			     SIPPYMODEM_V34_BRANCHES_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_branches_result_name(
			     SIPPYMODEM_V34_BRANCHES_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_branches_result_name(
			     SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_branches_result_name(
			     (enum sippymodem_v34_branches_result)99),
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
