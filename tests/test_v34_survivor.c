// SPDX-License-Identifier: BSD-3-Clause

#include <v34_4d_encoder.h>
#include <v34_lattice.h>
#include <v34_survivor.h>

#include "test.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

enum {
	ROUND_TRIP_INTERVALS = 192,
	FAULT_INTERVAL = 24,
	FAULT_TEST_INTERVALS = 80,
};

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

static struct v34_4d_encoder_input fixture_input(
	const struct v34_mapping_parameters *parameters,
	enum v34_trellis_selection selection, size_t interval)
{
	uint8_t point_count = (uint8_t)(
		1u << parameters->point_index_bits_per_2d_symbol);

	return (struct v34_4d_encoder_input){
		.mapping = {
			.rotation_bit = (uint8_t)((interval >> 1) & 1u),
			.differential_input = (uint8_t)((3u * interval + 1u) & 3u),
			.point_index = {
				(uint8_t)((5u * interval + selection) % point_count),
				(uint8_t)((7u * interval + 1u) % point_count),
			},
		},
		.ring_index = {
			(uint8_t)((interval + selection) % parameters->ring_count),
			(uint8_t)((3u * interval + 2u) % parameters->ring_count),
		},
		.inversion_bit = (uint8_t)((interval / 5u) & 1u),
	};
}

static uint32_t next_generated_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double generated_noise(uint32_t *state)
{
	double unit = (double)(next_generated_word(state) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.75;
}

static struct v34_survivor_decision encoded_decision(
	const struct v34_4d_encoder_output *encoded,
	uint8_t inversion_bit)
{
	struct v34_survivor_decision decision = {
		.convolutional_output = encoded->convolutional_output,
		.inversion_bit = inversion_bit,
	};

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		decision.channel_output[symbol] =
			encoded->precoded[symbol].channel_output;
	return decision;
}

static struct v34_survivor_decision branch_decision(
	const struct v34_branch *branch, uint8_t inversion_bit)
{
	struct v34_survivor_decision decision = {
		.convolutional_output = branch->convolutional_output,
		.inversion_bit = inversion_bit,
	};

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		decision.channel_output[symbol] =
			branch->symbol[symbol].channel_output;
	return decision;
}

static bool decisions_are_equal(
	const struct v34_survivor_decision *left,
	const struct v34_survivor_decision *right)
{
	if (left->convolutional_output != right->convolutional_output)
		return false;
	if (left->inversion_bit != right->inversion_bit)
		return false;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (left->channel_output[symbol].in_phase !=
			    right->channel_output[symbol].in_phase ||
		    left->channel_output[symbol].quadrature !=
			    right->channel_output[symbol].quadrature)
			return false;
	}
	return true;
}

static bool build_branches(
	const struct v34_4d_encoder_output *encoded,
	uint8_t inversion_bit, uint32_t *random, bool add_noise,
	enum v34_trellis_selection selection,
	struct v34_branch_set *branches)
{
	struct v34_subset_slice slices[2];

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		struct qam_point estimate = {
			encoded->precoded[symbol].channel_output.in_phase,
			encoded->precoded[symbol].channel_output.quadrature,
		};

		if (add_noise) {
			estimate.in_phase += generated_noise(random);
			estimate.quadrature += generated_noise(random);
		}
		if (v34_slice_grid_subsets(&estimate, &slices[symbol]) !=
		    V34_LATTICE_OK)
			return false;
	}
	return v34_build_branch_set(
		       slices, inversion_bit, selection, branches) ==
		       V34_BRANCHES_OK;
}

static uint8_t encoded_branch_index(
	const struct v34_branch_set *branches,
	const struct v34_survivor_decision *expected)
{
	for (uint8_t branch = 0; branch < branches->count; ++branch) {
		struct v34_survivor_decision candidate =
			branch_decision(
				&branches->branch[branch], branches->inversion_bit);

		if (decisions_are_equal(&candidate, expected))
			return branch;
	}
	return UINT8_MAX;
}

static bool test_generated_round_trip(void)
{
	struct v34_mapping_parameters parameters;

	CHECK(initialize_parameters(&parameters),
	      "survivor parameters did not initialize");
	for (enum v34_trellis_selection selection =
		     V34_TRELLIS_16_STATE;
	     selection <= V34_TRELLIS_64_STATE; ++selection) {
		struct v34_survivor_decision
			expected[ROUND_TRIP_INTERVALS];
		struct v34_4d_encoder encoder;
		struct v34_survivor survivor;
		uint32_t random = UINT32_C(0x8421c637) + selection;

		CHECK(v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      V34_4D_ENCODER_OK &&
		      v34_survivor_init(&survivor, selection) ==
			      V34_SURVIVOR_OK,
		      "survivor mode %u did not initialize", selection);
		for (size_t interval = 0; interval < ROUND_TRIP_INTERVALS;
		     ++interval) {
			struct v34_4d_encoder_input input =
				fixture_input(&parameters, selection, interval);
			struct v34_4d_encoder_output encoded;
			struct v34_branch_set branches;
			struct v34_survivor_decision current;
			struct v34_survivor_decision traceback;
			struct v34_survivor_decision saved_traceback;
			bool ready = true;

			memset(&traceback, 0xa5, sizeof(traceback));
			saved_traceback = traceback;
			CHECK(v34_4d_encoder_step(
				      &encoder, &input, &encoded) ==
				      V34_4D_ENCODER_OK &&
			      build_branches(
				      &encoded, input.inversion_bit, &random, true,
				      selection, &branches),
			      "survivor fixture %u/%zu failed", selection,
			      interval);
			expected[interval] = encoded_decision(
				&encoded, input.inversion_bit);
			CHECK(v34_survivor_advance(
				      &survivor, &branches, &current, &traceback,
				      &ready) == V34_SURVIVOR_OK &&
			      decisions_are_equal(&current, &expected[interval]) &&
			      ready ==
				      (interval + 1u >=
				       V34_SURVIVOR_TRACEBACK_INTERVALS),
			      "survivor mode %u decision %zu changed", selection,
			      interval);
			if (ready) {
				size_t decoded =
					interval + 1u -
					V34_SURVIVOR_TRACEBACK_INTERVALS;

				CHECK(decisions_are_equal(
					      &traceback, &expected[decoded]),
				      "survivor mode %u traceback %zu changed",
				      selection, decoded);
			} else {
				CHECK(memcmp(
					      &traceback, &saved_traceback,
					      sizeof(traceback)) == 0,
				      "survivor mode %u emitted before traceback",
				      selection);
			}
		}
		CHECK(v34_survivor_is_valid(&survivor),
		      "survivor mode %u ended invalid", selection);
		for (uint8_t state = 0; state < (uint8_t)(16u << selection);
		     ++state)
			CHECK(survivor.path_metric[state] != DBL_MAX,
			      "survivor mode %u never reached state %u",
			      selection, state);
	}
	return true;
}

static void set_branch_metric(
	struct v34_branch *branch, double metric)
{
	branch->symbol[0].squared_error = metric;
	branch->symbol[1].squared_error = 0.0;
	branch->squared_error = metric;
}

static uint8_t corrective_fault_branch(
	const struct v34_branch_set *branches, uint8_t origin_state,
	uint8_t correct_branch, uint8_t correct_next_state)
{
	for (uint8_t branch = 0; branch < branches->count; ++branch) {
		struct v34_convolutional_encoder candidate = {
			.state = origin_state,
			.selection = branches->selection,
		};

		if (branch == correct_branch ||
		    branches->branch[branch].convolutional_output !=
			    (origin_state & 1u))
			continue;
		if (v34_convolutional_encoder_advance(
			    &candidate,
			    &branches->branch[branch].convolutional_input) ==
			    V34_TRELLIS_OK &&
		    (candidate.state & 1u) != (correct_next_state & 1u))
			return branch;
	}
	return UINT8_MAX;
}

static bool test_path_correction(void)
{
	struct v34_mapping_parameters parameters;

	CHECK(initialize_parameters(&parameters),
	      "correction parameters did not initialize");
	for (enum v34_trellis_selection selection =
		     V34_TRELLIS_16_STATE;
	     selection <= V34_TRELLIS_64_STATE; ++selection) {
		struct v34_survivor_decision
			expected[FAULT_TEST_INTERVALS];
		struct v34_4d_encoder encoder;
		struct v34_survivor survivor;
		bool local_error_seen = false;

		CHECK(v34_4d_encoder_init(
			      &encoder, &parameters, COEFFICIENTS, selection) ==
			      V34_4D_ENCODER_OK &&
		      v34_survivor_init(&survivor, selection) ==
			      V34_SURVIVOR_OK,
		      "correction mode %u did not initialize", selection);
		for (size_t interval = 0; interval < FAULT_TEST_INTERVALS;
		     ++interval) {
			struct v34_4d_encoder_input input =
				fixture_input(&parameters, selection, interval);
			struct v34_4d_encoder_output encoded;
			struct v34_branch_set branches;
			struct v34_survivor_decision current;
			struct v34_survivor_decision traceback;
			uint8_t origin_state = encoder.convolutional_encoder.state;
			uint8_t correct_branch;
			uint8_t wrong_branch = UINT8_MAX;
			bool ready;

			CHECK(v34_4d_encoder_step(
				      &encoder, &input, &encoded) ==
				      V34_4D_ENCODER_OK &&
			      build_branches(
				      &encoded, input.inversion_bit, NULL, false,
				      selection, &branches),
			      "correction fixture %u/%zu failed", selection,
			      interval);
			expected[interval] = encoded_decision(
				&encoded, input.inversion_bit);
			correct_branch = encoded_branch_index(
				&branches, &expected[interval]);
			CHECK(correct_branch < branches.count,
			      "correction branch %u/%zu was not found", selection,
			      interval);
			for (uint8_t branch = 0; branch < branches.count; ++branch)
				set_branch_metric(&branches.branch[branch], 100.0);
			set_branch_metric(&branches.branch[correct_branch], 0.0);
			if (interval == FAULT_INTERVAL) {
				wrong_branch = corrective_fault_branch(
					&branches, origin_state, correct_branch,
					encoder.convolutional_encoder.state);
				CHECK(wrong_branch < branches.count,
				      "correction mode %u lacked a divergent branch",
				      selection);
				set_branch_metric(&branches.branch[correct_branch], 2.0);
				set_branch_metric(&branches.branch[wrong_branch], 0.0);
			}
			CHECK(v34_branch_set_is_valid(&branches) &&
			      v34_survivor_advance(
				      &survivor, &branches, &current, &traceback,
				      &ready) == V34_SURVIVOR_OK,
			      "correction survivor %u/%zu failed", selection,
			      interval);
			if (interval == FAULT_INTERVAL) {
				struct v34_survivor_decision wrong =
					branch_decision(
						&branches.branch[wrong_branch],
						branches.inversion_bit);

				local_error_seen =
					decisions_are_equal(&current, &wrong) &&
					!decisions_are_equal(
						&current, &expected[interval]);
			}
			if (ready) {
				size_t decoded =
					interval + 1u -
					V34_SURVIVOR_TRACEBACK_INTERVALS;

				CHECK(decisions_are_equal(
					      &traceback, &expected[decoded]),
				      "correction mode %u did not repair interval %zu",
				      selection, decoded);
			}
		}
		CHECK(local_error_seen,
		      "correction mode %u did not exercise traceback",
		      selection);
	}
	return true;
}

static bool rejected_advance_is_transactional(
	struct v34_survivor *survivor,
	const struct v34_branch_set *branches,
	enum v34_survivor_result expected)
{
	struct v34_survivor saved = { 0 };
	struct v34_survivor_decision current;
	struct v34_survivor_decision traceback;
	struct v34_survivor_decision saved_current;
	struct v34_survivor_decision saved_traceback;
	bool ready = true;

	if (survivor != NULL)
		saved = *survivor;
	memset(&current, 0x5a, sizeof(current));
	memset(&traceback, 0xa5, sizeof(traceback));
	saved_current = current;
	saved_traceback = traceback;
	return v34_survivor_advance(
		       survivor, branches, &current, &traceback, &ready) ==
		       expected &&
	       (survivor == NULL ||
		memcmp(survivor, &saved, sizeof(saved)) == 0) &&
	       memcmp(&current, &saved_current, sizeof(current)) == 0 &&
	       memcmp(&traceback, &saved_traceback, sizeof(traceback)) == 0 &&
	       ready;
}

static bool test_validation(void)
{
	struct v34_mapping_parameters parameters;
	struct v34_4d_encoder encoder;
	struct v34_4d_encoder_output encoded;
	struct v34_4d_encoder_input input;
	struct v34_branch_set branches;
	struct v34_branch_set malformed;
	struct v34_survivor survivor;
	struct v34_survivor saved;
	struct v34_survivor corrupted;

	memset(&survivor, 0xa5, sizeof(survivor));
	saved = survivor;
	CHECK(v34_survivor_init(NULL,
		      V34_TRELLIS_16_STATE) ==
		      V34_SURVIVOR_INVALID_ARGUMENT &&
	      v34_survivor_init(
		      &survivor, (enum v34_trellis_selection)3) ==
		      V34_SURVIVOR_INVALID_ARGUMENT &&
	      memcmp(&survivor, &saved, sizeof(saved)) == 0,
	      "invalid survivor initialization changed its destination");
	CHECK(initialize_parameters(&parameters) &&
	      v34_4d_encoder_init(
		      &encoder, &parameters, COEFFICIENTS,
		      V34_TRELLIS_16_STATE) ==
		      V34_4D_ENCODER_OK &&
	      v34_survivor_init(
		      &survivor, V34_TRELLIS_16_STATE) ==
		      V34_SURVIVOR_OK,
	      "validation survivor did not initialize");
	for (uint8_t state = 0; state < V34_MAX_TRELLIS_STATES; ++state)
		CHECK(survivor.path_metric[state] == 0.0,
		      "survivor did not start state %u with equal likelihood",
		      state);
	input = fixture_input(
		&parameters, V34_TRELLIS_16_STATE, 0);
	CHECK(v34_4d_encoder_step(&encoder, &input, &encoded) ==
		      V34_4D_ENCODER_OK &&
	      build_branches(
		      &encoded, input.inversion_bit, NULL, false,
		      V34_TRELLIS_16_STATE, &branches),
	      "validation branches did not build");
	CHECK(rejected_advance_is_transactional(
		      NULL, &branches, V34_SURVIVOR_INVALID_ARGUMENT) &&
	      rejected_advance_is_transactional(
		      &survivor, NULL, V34_SURVIVOR_INVALID_ARGUMENT),
	      "null survivor arguments were accepted");
	{
		struct v34_survivor_decision current;
		struct v34_survivor_decision traceback;
		struct v34_survivor_decision saved_current;
		struct v34_survivor_decision saved_traceback;
		struct v34_survivor before = survivor;
		bool ready = true;

		memset(&current, 0x5a, sizeof(current));
		memset(&traceback, 0xa5, sizeof(traceback));
		saved_current = current;
		saved_traceback = traceback;
		CHECK(v34_survivor_advance(
			      &survivor, &branches, NULL, &traceback, &ready) ==
			      V34_SURVIVOR_INVALID_ARGUMENT &&
		      v34_survivor_advance(
			      &survivor, &branches, &current, NULL, &ready) ==
			      V34_SURVIVOR_INVALID_ARGUMENT &&
		      v34_survivor_advance(
			      &survivor, &branches, &current, &traceback, NULL) ==
			      V34_SURVIVOR_INVALID_ARGUMENT &&
		      memcmp(&survivor, &before, sizeof(before)) == 0 &&
		      memcmp(&current, &saved_current, sizeof(current)) == 0 &&
		      memcmp(
			      &traceback, &saved_traceback,
			      sizeof(traceback)) == 0 &&
		      ready,
		      "null survivor outputs changed state");
	}
	malformed = branches;
	malformed.selection = V34_TRELLIS_32_STATE;
	CHECK(!v34_branch_set_is_valid(&malformed) &&
	      rejected_advance_is_transactional(
		      &survivor, &malformed,
		      V34_SURVIVOR_INVALID_ARGUMENT),
	      "mismatched branch selection was accepted");
	malformed = branches;
	for (uint8_t branch = 0; branch < malformed.count; ++branch)
		set_branch_metric(&malformed.branch[branch], DBL_MAX);
	CHECK(v34_branch_set_is_valid(&malformed) &&
	      rejected_advance_is_transactional(
		      &survivor, &malformed,
		      V34_SURVIVOR_NUMERIC_FAILURE),
	      "unusable finite metrics did not fail transactionally");

	CHECK(v34_survivor_is_valid(&survivor) &&
	      !v34_survivor_is_valid(NULL),
	      "fresh survivor validity changed");
	corrupted = survivor;
	for (uint8_t state = 0; state < 16u; ++state)
		corrupted.path_metric[state] = 1.0;
	CHECK(!v34_survivor_is_valid(&corrupted),
	      "unnormalized survivor metrics were accepted");
	corrupted = survivor;
	corrupted.path_index = 1;
	CHECK(!v34_survivor_is_valid(&corrupted),
	      "inconsistent survivor cursor was accepted");
	{
		struct v34_survivor_decision current;
		struct v34_survivor_decision traceback;
		bool ready;

		CHECK(v34_survivor_advance(
			      &survivor, &branches, &current, &traceback, &ready) ==
			      V34_SURVIVOR_OK &&
		      !ready,
		      "validation survivor did not populate history");
	}
	corrupted = survivor;
	corrupted.path[0].predecessor[0] = 16;
	CHECK(!v34_survivor_is_valid(&corrupted),
	      "out-of-range survivor predecessor was accepted");
	corrupted = survivor;
	corrupted.path[0].branch[0].channel_output[0].in_phase = 0;
	CHECK(!v34_survivor_is_valid(&corrupted),
	      "off-grid survivor history was accepted");
	return true;
}

static bool test_traceback_index_bounds(void)
{
	struct v34_mapping_parameters parameters;

	CHECK(initialize_parameters(&parameters), "index-bound parameters failed");
	for (enum v34_trellis_selection selection = V34_TRELLIS_16_STATE;
	     selection <= V34_TRELLIS_64_STATE; ++selection) {
		struct v34_4d_encoder encoder;
		struct v34_4d_encoder_output encoded;
		struct v34_4d_encoder_input input = fixture_input(&parameters, selection, 0);
		struct v34_branch_set branches;
		struct v34_survivor survivor;
		struct v34_survivor_decision current, traceback;
		bool ready;

		CHECK(v34_4d_encoder_init(&encoder, &parameters, COEFFICIENTS, selection) ==
			V34_4D_ENCODER_OK &&
		      v34_4d_encoder_step(&encoder, &input, &encoded) == V34_4D_ENCODER_OK &&
		      build_branches(&encoded, input.inversion_bit, NULL, false, selection, &branches) &&
		      v34_survivor_init(&survivor, selection) == V34_SURVIVOR_OK &&
		      v34_survivor_advance(&survivor, &branches, &current, &traceback, &ready) ==
			V34_SURVIVOR_OK && !ready && v34_survivor_is_valid(&survivor),
		      "index-bound fixture failed for trellis %u", selection);
		uint8_t *indices[] = { survivor.path[0].predecessor, survivor.path[0].branch_index };
		const uint8_t states = v34_trellis_state_count(selection);
		const uint8_t limits[] = { states, v34_trellis_branch_count(selection) };

		for (size_t field = 0; field < ARRAY_LENGTH(indices); ++field) {
			const uint8_t values[] = { 0, limits[field] - 1u, limits[field], UINT8_MAX };

			for (unsigned state = 0; state < V34_MAX_TRELLIS_STATES; ++state) {
				uint8_t saved = indices[field][state];

				for (size_t value = 0; value < ARRAY_LENGTH(values); ++value) {
					indices[field][state] = values[value];
					CHECK(v34_survivor_is_valid(&survivor) ==
						(state >= states || values[value] < limits[field]),
					      "trellis %u field %zu state %u value %u changed bounds",
					      selection, field, state, values[value]);
				}
				indices[field][state] = saved;
			}
		}
	}
	return true;
}

static bool test_decision_parity(void)
{
	/* All signed modulo-eight residues, the legal rails, off-grid values,
	 * and out-of-range odd/even values in every coordinate position. */
	static const int16_t coordinates[] = {
		INT16_MIN, -257, -256, -255, -7, -5, -3, -1,
		0, 1, 3, 5, 7, 255, 256, 257, INT16_MAX,
	};
	const size_t count = ARRAY_LENGTH(coordinates);

	CHECK(!v34_survivor_decision_is_valid(NULL), "null decision accepted");
	for (size_t index = 0; index < count * count * count * count; ++index) {
		struct v34_survivor_decision decision = { 0 };
		uint8_t labels[2] = { 0 };
		size_t digits = index;
		bool valid = true;

		for (unsigned symbol = 0; symbol < 2; ++symbol) {
			struct v34_grid_point *point = &decision.channel_output[symbol];

			point->in_phase = coordinates[digits % count];
			digits /= count;
			point->quadrature = coordinates[digits % count];
			digits /= count;
			valid &= v34_grid_point_is_bounded(point) &&
				v34_grid_subset_label(point, &labels[symbol]) == V34_TRELLIS_OK;
		}
		for (uint8_t inversion = 0; inversion <= 2; ++inversion)
			for (uint8_t output = 0; output <= 2; ++output) {
				decision.inversion_bit = inversion;
				decision.convolutional_output = output;
				bool expected = valid && inversion <= 1 && output <= 1 &&
					output == ((labels[0] ^ labels[1] ^ inversion) & 1u);

				CHECK(v34_survivor_decision_is_valid(&decision) == expected,
				      "decision parity changed for coordinates %zu inversion %u output %u",
				      index, inversion, output);
			}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bounded decision parity", test_decision_parity },
		{ "generated round trip", test_generated_round_trip },
		{ "path correction", test_path_correction },
		{ "transactional validation", test_validation },
		{ "traceback index bounds", test_traceback_index_bounds },
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
