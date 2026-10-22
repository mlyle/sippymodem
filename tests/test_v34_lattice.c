// SPDX-License-Identifier: BSD-3-Clause

#include <v34_lattice.h>
#include <v34_trellis.h>

#include "test.h"

#include <float.h>
#include <math.h>
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
	const struct qam_point *estimate,
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
	const struct qam_point magnitude_tie = { 3.0, 3.0 };
	const struct qam_point coordinate_tie = { -1.0, -1.0 };
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
		const struct qam_point estimate = {
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

static bool initialize_nonlinear_normalizer(
	struct v34_power_normalizer *normalizer)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	const struct v34_symbol_power_measurement precoded = {
		.mean_squared_magnitude = 25.0,
		.symbol_count = 256,
	};
	const struct v34_symbol_power_measurement projected = {
		.mean_squared_magnitude = 30.0,
		.symbol_count = 256,
	};

	return v34_frame_geometry_init(
		       &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &geometry, false) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_power_normalizer_init(
		       normalizer, &parameters,
		       V34_NONLINEAR_THETA_5_OVER_16, &precoded,
		       &projected) == V34_SYMBOL_POWER_OK;
}

static bool test_nonlinear_signal_plane_metric(void)
{
	/* This observation lies on opposite sides of a subset decision boundary
	 * before and after the radial nonlinear encoder. */
	const struct qam_point projected_observation = {
		-0.8867705582103556,
		-3.2059170928206857,
	};
	struct v34_power_normalizer normalizer;
	struct qam_point channel_estimate;
	struct qam_point normalized_observation;
	struct v34_subset_slice linear;
	struct v34_subset_slice projected;

	CHECK(initialize_nonlinear_normalizer(&normalizer) &&
	      v34_nonlinear_unproject(
		      normalizer.nonlinear_parameter,
		      normalizer.precoded_mean_squared_magnitude,
		      &projected_observation, &channel_estimate) ==
		      V34_NONLINEAR_OK,
	      "nonlinear metric fixture did not initialize");
	normalized_observation = (struct qam_point){
		.in_phase = projected_observation.in_phase *
			normalizer.normalization_gain,
		.quadrature = projected_observation.quadrature *
			normalizer.normalization_gain,
	};
	CHECK(v34_slice_grid_subsets(&channel_estimate, &linear) ==
		      V34_LATTICE_OK &&
	      v34_slice_grid_subsets_projected(
		      &channel_estimate, &normalized_observation, &normalizer,
		      &projected) == V34_LATTICE_OK,
	      "nonlinear signal-plane slicing failed");
	CHECK(linear.subset[1].channel_output.in_phase == -3 &&
	      linear.subset[1].channel_output.quadrature == -5 &&
	      projected.subset[1].channel_output.in_phase == 1 &&
	      projected.subset[1].channel_output.quadrature == -1,
	      "nonlinear metric did not select the signal-plane neighbor");
	return true;
}

static bool test_linear_projection_scaling(void)
{
	const struct qam_point estimates[] = {
		{ 0.0, 0.0 }, { 3.0, 3.0 }, { -1.0, -1.0 },
		{ -3.5, 6.25 }, { 255.25, -255.25 }, { 512.0, -512.0 },
	};
	const double gains[] = { 0.25, 1.0, 4.0 };
	struct v34_power_normalizer normalizer;

	CHECK(initialize_nonlinear_normalizer(&normalizer),
	      "linear projection fixture did not initialize");
	for (size_t gain = 0; gain < ARRAY_LENGTH(gains); ++gain) {
		CHECK(v34_power_normalizer_init_linear(&normalizer,
			      &normalizer.parameters, gains[gain]) == V34_SYMBOL_POWER_OK,
		      "linear projection gain did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(estimates); ++index) {
			struct qam_point observation = {
				.in_phase = estimates[index].in_phase * gains[gain],
				.quadrature = estimates[index].quadrature * gains[gain],
			};
			struct v34_subset_slice linear, projected;

			CHECK(v34_slice_grid_subsets(&estimates[index], &linear) ==
				      V34_LATTICE_OK &&
			      v34_slice_grid_subsets_projected(&estimates[index],
				      &observation, &normalizer, &projected) == V34_LATTICE_OK,
			      "linear projection failed at gain %zu estimate %zu", gain, index);
			for (uint8_t label = 0; label < V34_SUBSET_LABEL_COUNT; ++label) {
				const struct v34_lattice_candidate *a = &linear.subset[label];
				const struct v34_lattice_candidate *b = &projected.subset[label];

				/* Binary gains and quarter-grid observations keep this
				 * identity exact, including ties and clipped grid edges. */
				CHECK(a->channel_output.in_phase == b->channel_output.in_phase &&
				      a->channel_output.quadrature == b->channel_output.quadrature &&
				      a->subset_label == b->subset_label &&
				      a->squared_error * gains[gain] * gains[gain] ==
					      b->squared_error,
				      "linear projection changed subset %u at gain %zu estimate %zu",
				      label, gain, index);
			}
		}
	}
	return true;
}

static bool test_transactional_validation(void)
{
	const struct qam_point valid = { 1.0, 1.0 };
	const struct qam_point not_finite[] = {
		{ NAN, 0.0 }, { 0.0, INFINITY },
	};
	const struct qam_point overflow = { DBL_MAX, 0.0 };
	struct v34_power_normalizer normalizer;
	struct v34_power_normalizer invalid_normalizer = { 0 };
	struct v34_subset_slice subsets;
	struct v34_subset_slice saved_subsets;

	memset(&subsets, 0x5a, sizeof(subsets));
	saved_subsets = subsets;
	CHECK(initialize_nonlinear_normalizer(&normalizer),
	      "transaction fixture did not initialize");
	CHECK(v34_slice_grid_subsets(NULL, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&valid, NULL) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&not_finite[0], &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets(&overflow, &subsets) ==
		      V34_LATTICE_NUMERIC_FAILURE &&
	      v34_slice_grid_subsets_projected(
		      &valid, NULL, &normalizer, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets_projected(
		      &valid, &valid, NULL, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets_projected(
		      &valid, &not_finite[1], &normalizer, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets_projected(
		      &valid, &valid, &invalid_normalizer, &subsets) ==
		      V34_LATTICE_INVALID_ARGUMENT &&
	      v34_slice_grid_subsets_projected(
		      &valid, &overflow, &normalizer, &subsets) ==
		      V34_LATTICE_NUMERIC_FAILURE &&
	      memcmp(&subsets, &saved_subsets, sizeof(subsets)) == 0,
	      "rejected estimate changed the subset slice");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all grid subsets", test_all_grid_subsets },
		{ "nonlinear signal-plane metric",
		  test_nonlinear_signal_plane_metric },
		{ "linear projection scaling", test_linear_projection_scaling },
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
