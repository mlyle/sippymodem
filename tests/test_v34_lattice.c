// SPDX-License-Identifier: BSD-3-Clause

#include <v34_lattice.h>
#include <v34_trellis.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool doubles_are_close(double left, double right)
{
	double scale = fmax(1.0, fmax(fabs(left), fabs(right)));

	return fabs(left - right) <= 16.0 * DBL_EPSILON * scale;
}

static bool candidate_matches(
	const struct v34_lattice_candidate *candidate,
	const struct v34_grid_point *point, uint8_t subset_label,
	double squared_error)
{
	return candidate->channel_output.in_phase == point->in_phase &&
	       candidate->channel_output.quadrature == point->quadrature &&
	       candidate->subset_label == subset_label &&
	       doubles_are_close(candidate->squared_error, squared_error);
}

static bool test_known_slices(void)
{
	static const struct v34_grid_point expected[] = {
		{ 1, 1 }, { 1, 3 }, { 3, 3 }, { 3, 1 },
	};
	static const uint8_t expected_label[] = { 0, 5, 2, 7 };
	static const double expected_error[] = { 2.93, 0.13, 3.33, 6.13 };
	static const struct v34_grid_point expected_tie[] = {
		{ 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 },
	};
	static const struct v34_grid_point expected_boundary[] = {
		{ 253, -255 }, { 253, -253 }, { 255, -253 }, { 255, -255 },
	};
	const struct sippymodem_qam_point estimate = { 1.2, 2.7 };
	const struct sippymodem_qam_point tie = { -1.0, 1.0 };
	const struct sippymodem_qam_point boundary = { 1000000.0, -1000000.0 };
	struct v34_lattice_slice slice;

	CHECK(v34_slice_odd_lattice(&estimate, &slice) ==
		      V34_LATTICE_OK,
	      "known odd-lattice slice failed");
	for (size_t path = 0; path < ARRAY_LENGTH(expected); ++path)
		CHECK(candidate_matches(
			      &slice.candidate[path], &expected[path],
			      expected_label[path], expected_error[path]),
		      "known candidate %zu changed", path);

	CHECK(v34_slice_odd_lattice(&tie, &slice) ==
		      V34_LATTICE_OK,
	      "same-class tie slice failed");
	for (size_t path = 0; path < ARRAY_LENGTH(expected_tie); ++path)
		CHECK(slice.candidate[path].channel_output.in_phase ==
			      expected_tie[path].in_phase &&
		      slice.candidate[path].channel_output.quadrature ==
			      expected_tie[path].quadrature,
		      "tie candidate %zu selected the wrong magnitude", path);

	CHECK(v34_slice_odd_lattice(&boundary, &slice) ==
		      V34_LATTICE_OK,
	      "bounded odd-lattice slice failed");
	for (size_t path = 0; path < ARRAY_LENGTH(expected_boundary); ++path)
		CHECK(slice.candidate[path].channel_output.in_phase ==
			      expected_boundary[path].in_phase &&
		      slice.candidate[path].channel_output.quadrature ==
			      expected_boundary[path].quadrature,
		      "boundary candidate %zu escaped the channel grid", path);
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

static int16_t exhaustive_axis_candidate(double estimate, uint8_t residue)
{
	bool found = false;
	double best_error = DBL_MAX;
	int best = 0;

	for (int coordinate = -V34_CHANNEL_COORDINATE_LIMIT;
	     coordinate <= V34_CHANNEL_COORDINATE_LIMIT;
	     coordinate += 2) {
		double error;

		if (((uint16_t)coordinate & 3u) != residue)
			continue;
		error = fabs(estimate - coordinate);
		if (!found || error < best_error ||
		    (error == best_error && abs(coordinate) < abs(best))) {
			found = true;
			best_error = error;
			best = coordinate;
		}
	}
	return (int16_t)best;
}

static struct v34_grid_point expected_path_point(
	const int16_t in_phase[2], const int16_t quadrature[2], uint8_t path)
{
	uint8_t in_phase_index = path >> 1;
	uint8_t quadrature_index = (uint8_t)(in_phase_index ^ (path & 1u));

	return (struct v34_grid_point){
		in_phase[in_phase_index], quadrature[quadrature_index],
	};
}

static uint8_t figure9_label(
	int16_t in_phase, int16_t quadrature)
{
	static const uint8_t labels[4][4] = {
		{ 0, 7, 4, 3 },
		{ 5, 2, 1, 6 },
		{ 4, 3, 0, 7 },
		{ 1, 6, 5, 2 },
	};
	uint8_t in_phase_index = (uint8_t)(((uint16_t)in_phase & 7u) >> 1);
	uint8_t quadrature_index =
		(uint8_t)(((uint16_t)quadrature & 7u) >> 1);

	return labels[quadrature_index][in_phase_index];
}

static int32_t oracle_magnitude_squared(
	const struct v34_grid_point *point)
{
	return (int32_t)point->in_phase * point->in_phase +
	       (int32_t)point->quadrature * point->quadrature;
}

static bool oracle_precedes(
	double error, const struct v34_grid_point *point,
	double current_error, const struct v34_grid_point *current)
{
	int32_t magnitude;
	int32_t current_magnitude;

	if (error != current_error)
		return error < current_error;
	magnitude = oracle_magnitude_squared(point);
	current_magnitude = oracle_magnitude_squared(current);
	if (magnitude != current_magnitude)
		return magnitude < current_magnitude;
	if (point->in_phase != current->in_phase)
		return point->in_phase < current->in_phase;
	return point->quadrature < current->quadrature;
}

static void exhaustive_subset_oracle(
	const struct sippymodem_qam_point *estimate,
	struct v34_subset_slice *expected)
{
	bool found[V34_SUBSET_LABEL_COUNT] = { false };

	memset(expected, 0, sizeof(*expected));
	for (int in_phase = -V34_CHANNEL_COORDINATE_LIMIT;
	     in_phase <= V34_CHANNEL_COORDINATE_LIMIT;
	     in_phase += 2) {
		for (int quadrature = -V34_CHANNEL_COORDINATE_LIMIT;
		     quadrature <= V34_CHANNEL_COORDINATE_LIMIT;
		     quadrature += 2) {
			struct v34_grid_point point = {
				(int16_t)in_phase, (int16_t)quadrature,
			};
			uint8_t label = figure9_label(
				point.in_phase, point.quadrature);
			double error_in_phase =
				estimate->in_phase - point.in_phase;
			double error_quadrature =
				estimate->quadrature - point.quadrature;
			double error = error_in_phase * error_in_phase +
				       error_quadrature * error_quadrature;
			struct v34_lattice_candidate *current =
				&expected->subset[label];

			if (!found[label] || oracle_precedes(
						     error, &point,
						     current->squared_error,
						     &current->channel_output)) {
				*current = (struct v34_lattice_candidate){
					.channel_output = point,
					.subset_label = label,
					.squared_error = error,
				};
				found[label] = true;
			}
		}
	}
}

static bool subset_slices_match(
	const struct v34_subset_slice *actual,
	const struct v34_subset_slice *expected, size_t index)
{
	for (uint8_t label = 0; label < V34_SUBSET_LABEL_COUNT;
	     ++label) {
		const struct v34_lattice_candidate *candidate =
			&actual->subset[label];

		CHECK(candidate_matches(
			      candidate, &expected->subset[label].channel_output,
			      label, expected->subset[label].squared_error),
		      "subset slice %zu label %u changed", index, label);
	}
	return true;
}

static bool test_all_grid_subsets(void)
{
	const struct sippymodem_qam_point magnitude_tie = { 3.0, 3.0 };
	const struct sippymodem_qam_point coordinate_tie = { -1.0, -1.0 };
	struct v34_subset_slice slice;
	uint32_t random = UINT32_C(0x8a11c0de);

	CHECK(v34_slice_grid_subsets(&magnitude_tie, &slice) ==
		      V34_LATTICE_OK &&
	      slice.subset[0].channel_output.in_phase == 1 &&
	      slice.subset[0].channel_output.quadrature == 1,
	      "subset magnitude tie selected the wrong point");
	CHECK(v34_slice_grid_subsets(&coordinate_tie, &slice) ==
		      V34_LATTICE_OK &&
	      slice.subset[4].channel_output.in_phase == -3 &&
	      slice.subset[4].channel_output.quadrature == 1,
	      "subset coordinate tie selected the wrong point");
	for (size_t index = 0; index < 64u; ++index) {
		const struct sippymodem_qam_point estimate = {
			generated_estimate(&random),
			generated_estimate(&random),
		};
		struct v34_subset_slice expected;

		exhaustive_subset_oracle(&estimate, &expected);
		CHECK(v34_slice_grid_subsets(&estimate, &slice) ==
			      V34_LATTICE_OK &&
		      subset_slices_match(&slice, &expected, index),
		      "generated subset slice %zu failed", index);
	}
	return true;
}

static bool test_generated_sweep(void)
{
	uint32_t random = UINT32_C(0x34a11ce5);
	bool lower_bound_seen = false;
	bool upper_bound_seen = false;

	for (size_t index = 0; index < 2048u; ++index) {
		const struct sippymodem_qam_point estimate = {
			generated_estimate(&random),
			generated_estimate(&random),
		};
		const int16_t in_phase[] = {
			exhaustive_axis_candidate(estimate.in_phase, 1),
			exhaustive_axis_candidate(estimate.in_phase, 3),
		};
		const int16_t quadrature[] = {
			exhaustive_axis_candidate(estimate.quadrature, 1),
			exhaustive_axis_candidate(estimate.quadrature, 3),
		};
		struct v34_lattice_slice slice;
		bool label_seen[V34_SUBSET_LABEL_COUNT] = { false };

		CHECK(v34_slice_odd_lattice(&estimate, &slice) ==
			      V34_LATTICE_OK,
		      "generated slice %zu failed", index);
		for (uint8_t path = 0;
		     path < V34_LATTICE_CANDIDATE_COUNT; ++path) {
			struct v34_grid_point expected =
				expected_path_point(
					in_phase, quadrature, path);
			const struct v34_lattice_candidate *candidate =
				&slice.candidate[path];
			double error_in_phase =
				estimate.in_phase - expected.in_phase;
			double error_quadrature =
				estimate.quadrature - expected.quadrature;
			double expected_error =
				error_in_phase * error_in_phase +
				error_quadrature * error_quadrature;
			uint8_t expected_label;

			CHECK(v34_grid_subset_label(
				      &expected, &expected_label) ==
				      V34_TRELLIS_OK &&
			      candidate_matches(
				      candidate, &expected, expected_label,
				      expected_error) &&
			      !label_seen[expected_label],
			      "generated candidate %zu/%u changed", index, path);
			label_seen[expected_label] = true;
			lower_bound_seen |=
				candidate->channel_output.in_phase ==
					-V34_CHANNEL_COORDINATE_LIMIT ||
				candidate->channel_output.quadrature ==
					-V34_CHANNEL_COORDINATE_LIMIT;
			upper_bound_seen |=
				candidate->channel_output.in_phase ==
					V34_CHANNEL_COORDINATE_LIMIT ||
				candidate->channel_output.quadrature ==
					V34_CHANNEL_COORDINATE_LIMIT;
		}
	}
	CHECK(lower_bound_seen && upper_bound_seen,
	      "generated sweep did not exercise both channel bounds");
	return true;
}

static bool test_transactional_validation(void)
{
	const struct sippymodem_qam_point valid = { 1.0, 1.0 };
	const struct sippymodem_qam_point not_finite[] = {
		{ NAN, 0.0 }, { 0.0, INFINITY },
	};
	const struct sippymodem_qam_point overflow = { DBL_MAX, 0.0 };
	struct v34_lattice_slice slice;
	struct v34_lattice_slice saved;
	struct v34_subset_slice subsets;
	struct v34_subset_slice saved_subsets;

	memset(&slice, 0xa5, sizeof(slice));
	saved = slice;
	CHECK(v34_slice_odd_lattice(NULL, &slice) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_odd_lattice(&valid, NULL) ==
		      V34_LATTICE_INVALID_ARGUMENT,
	      "missing lattice argument was accepted");
	for (size_t index = 0; index < ARRAY_LENGTH(not_finite); ++index)
		CHECK(v34_slice_odd_lattice(
			      &not_finite[index], &slice) ==
			      V34_LATTICE_INVALID_ARGUMENT,
		      "non-finite estimate %zu was accepted", index);
	CHECK(v34_slice_odd_lattice(&overflow, &slice) ==
		      V34_LATTICE_NUMERIC_FAILURE &&
	      memcmp(&slice, &saved, sizeof(slice)) == 0,
	      "rejected estimate changed the lattice slice");
	memset(&subsets, 0x5a, sizeof(subsets));
	saved_subsets = subsets;
	CHECK(v34_slice_grid_subsets(NULL, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&valid, NULL) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&not_finite[0], &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&overflow, &subsets) ==
		      V34_LATTICE_NUMERIC_FAILURE &&
	      memcmp(&subsets, &saved_subsets, sizeof(subsets)) == 0,
	      "rejected estimate changed the subset slice");
	CHECK(strcmp(v34_lattice_result_name(
			     V34_LATTICE_OK),
		     "ok") == 0 &&
	      strcmp(v34_lattice_result_name(
			     V34_LATTICE_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_lattice_result_name(
			     V34_LATTICE_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_lattice_result_name(
			     (enum v34_lattice_result)99),
		     "unknown") == 0,
	      "lattice result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "known slices", test_known_slices },
		{ "generated exhaustive sweep", test_generated_sweep },
		{ "all grid subsets", test_all_grid_subsets },
		{ "transactional validation", test_transactional_validation },
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
