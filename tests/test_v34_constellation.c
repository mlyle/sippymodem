// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>

#include "test.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static const double TEST_TOLERANCE = 1e-14;

static bool close_to(double left, double right)
{
	return fabs(left - right) < TEST_TOLERANCE;
}

static int grid_squared_magnitude(
	const struct v34_grid_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

static size_t independent_quarter_grid_rank(
	const struct v34_grid_point *point)
{
	size_t rank = 0;
	int selected_magnitude = grid_squared_magnitude(point);

	/* Include the first lattice coordinates outside Figure 5 so this also
	 * detects an insufficient production search boundary. */
	for (int quadrature = -47; quadrature <= 49; quadrature += 4) {
		for (int in_phase = -47; in_phase <= 49; in_phase += 4) {
			int magnitude = in_phase * in_phase +
				quadrature * quadrature;

			if (magnitude < selected_magnitude ||
			    (magnitude == selected_magnitude &&
			     quadrature > point->quadrature))
				rank++;
		}
	}
	return rank;
}

static uint64_t quarter_constellation_digest(
	const struct v34_grid_point *points, size_t point_count)
{
	uint64_t digest = UINT64_C(14695981039346656037);

	for (size_t index = 0; index < point_count; ++index) {
		const uint8_t coordinates[] = {
			(uint8_t)points[index].in_phase,
			(uint8_t)points[index].quadrature,
		};

		for (size_t coordinate = 0;
		     coordinate < ARRAY_LENGTH(coordinates); ++coordinate) {
			digest ^= coordinates[coordinate];
			digest *= UINT64_C(1099511628211);
		}
	}
	return digest;
}

static bool test_quarter_superconstellation(void)
{
	struct v34_grid_point
		points[V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];

	CHECK(v34_quarter_superconstellation_generate(
		      points, ARRAY_LENGTH(points)) ==
		      V34_CONSTELLATION_OK,
	      "quarter-superconstellation generation failed");
	for (size_t index = 0; index < ARRAY_LENGTH(points); ++index) {
		int magnitude = grid_squared_magnitude(&points[index]);

		CHECK(((uint16_t)points[index].in_phase & 3u) == 1u &&
		      ((uint16_t)points[index].quadrature & 3u) == 1u,
		      "label %zu left the quarter-grid lattice", index);
		CHECK(independent_quarter_grid_rank(&points[index]) == index,
		      "label %zu has the wrong magnitude/tie rank", index);
		if (index != 0) {
			int previous_magnitude =
				grid_squared_magnitude(&points[index - 1u]);

			CHECK(previous_magnitude < magnitude ||
			      (previous_magnitude == magnitude &&
			       points[index - 1u].quadrature >
				       points[index].quadrature),
			      "labels %zu and %zu are not strictly ordered",
			      index - 1u, index);
		}
	}
	CHECK(points[0].in_phase == 1 && points[0].quadrature == 1 &&
	      points[ARRAY_LENGTH(points) - 1u].in_phase == 45 &&
	      points[ARRAY_LENGTH(points) - 1u].quadrature == 9,
	      "quarter-superconstellation endpoints changed");
	CHECK(quarter_constellation_digest(points, ARRAY_LENGTH(points)) ==
		      UINT64_C(0x79ad213cde9d9435),
	      "quarter-superconstellation coordinate digest changed");
	return true;
}

static bool test_indexed_quarter_superconstellation(void)
{
	struct v34_grid_point
		points[V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];

	CHECK(v34_quarter_superconstellation_generate(
		      points, ARRAY_LENGTH(points)) ==
		      V34_CONSTELLATION_OK,
	      "indexed fixture generation failed");
	/* 37 is coprime to 416, so this visits every label in a deliberately
	 * non-sequential order. */
	for (size_t visit = 0; visit < ARRAY_LENGTH(points); ++visit) {
		size_t label = visit * 37u % ARRAY_LENGTH(points);
		size_t recovered_label = SIZE_MAX;
		struct v34_grid_point selected = { 0 };

		CHECK(v34_quarter_superconstellation_point(
			      label, &selected) ==
			      V34_CONSTELLATION_OK,
		      "indexed label %zu was rejected", label);
		CHECK(selected.in_phase == points[label].in_phase &&
		      selected.quadrature == points[label].quadrature &&
		      independent_quarter_grid_rank(&selected) == label &&
		      v34_quarter_superconstellation_label(
			      &selected, &recovered_label) ==
			      V34_CONSTELLATION_OK &&
		      recovered_label == label,
		      "indexed label %zu selected the wrong point", label);
	}
	return true;
}

static bool test_quarter_superconstellation_prefixes(void)
{
	struct v34_grid_point
		full[V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	struct v34_grid_point prefix[32];
	struct v34_grid_point sentinel[32];
	static const size_t prefix_lengths[] = { 1, 4, 16, 32 };

	CHECK(v34_quarter_superconstellation_generate(
		      full, ARRAY_LENGTH(full)) ==
		      V34_CONSTELLATION_OK,
	      "full prefix fixture generation failed");
	memset(sentinel, 0x5a, sizeof(sentinel));
	for (size_t fixture = 0; fixture < ARRAY_LENGTH(prefix_lengths);
	     ++fixture) {
		size_t prefix_length = prefix_lengths[fixture];

		memcpy(prefix, sentinel, sizeof(prefix));
		CHECK(v34_quarter_superconstellation_generate(
			      prefix, prefix_length) ==
			      V34_CONSTELLATION_OK,
		      "prefix length %zu was rejected", prefix_length);
		for (size_t index = 0; index < prefix_length; ++index) {
			CHECK(prefix[index].in_phase == full[index].in_phase &&
			      prefix[index].quadrature == full[index].quadrature,
			      "prefix length %zu changed label %zu",
			      prefix_length, index);
		}
		CHECK(memcmp(
			      &prefix[prefix_length], &sentinel[prefix_length],
			      (ARRAY_LENGTH(prefix) - prefix_length) *
				      sizeof(prefix[0])) == 0,
		      "prefix length %zu overwrote its suffix", prefix_length);
	}
	return true;
}

static struct qam_point expected_rotated_grid_point(
	int in_phase, int quadrature, double grid_unit,
	uint8_t clockwise_quarter_turns)
{
	switch (clockwise_quarter_turns) {
	case 0:
		break;
	case 1: {
		int saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
		break;
	}
	case 2:
		in_phase = -in_phase;
		quadrature = -quadrature;
		break;
	case 3: {
		int saved_in_phase = in_phase;

		in_phase = -quadrature;
		quadrature = saved_in_phase;
		break;
	}
	}
	return (struct qam_point){
		.in_phase = in_phase * grid_unit,
		.quadrature = quadrature * grid_unit,
	};
}

static bool test_point_zero_rotations(void)
{
	for (int turns = -9; turns <= 9; ++turns) {
		struct qam_point point =
			v34_rotated_point_zero(turns);
		struct qam_point expected =
			expected_rotated_grid_point(
				1, 1, sqrt(0.5),
				(uint8_t)((-(turns % 4) + 4) % 4));
		double power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;

		CHECK(close_to(point.in_phase, expected.in_phase) &&
			      close_to(point.quadrature, expected.quadrature) &&
			      close_to(power, 1.0),
		      "point-zero rotation changed at turn %d", turns);
	}
	return true;
}

static bool test_training_decision_validation(void)
{
	static const int sizes[] = { -1, 0, 4, 8, 16, 64, INT_MAX };

	for (size_t size = 0; size < ARRAY_LENGTH(sizes); ++size) {
		enum v34_constellation_size constellation =
			(enum v34_constellation_size)sizes[size];
		for (unsigned int quarter = 0; quarter <= UINT8_MAX; ++quarter) {
			for (unsigned int rotation = 0;
			     rotation <= UINT8_MAX; ++rotation) {
				const struct qam_point sentinel = { 7.0, -9.0 };
				const struct v34_analytic_sample fixed_sentinel =
					{ 7, -9 };
				struct qam_point point = sentinel;
				struct v34_analytic_sample fixed = fixed_sentinel;
				unsigned int quarter_count = sizes[size] == 4 ? 1u :
					sizes[size] == 16 ? 4u : 0u;
				bool valid = quarter < quarter_count && rotation < 4u;
				enum v34_constellation_result expected = valid ?
					V34_CONSTELLATION_OK :
					V34_CONSTELLATION_INVALID_ARGUMENT;

				CHECK(v34_training_decision_is_valid(constellation,
					      (uint8_t)quarter, (uint8_t)rotation) == valid &&
				      v34_training_constellation_point(constellation,
					      (uint8_t)quarter, (uint8_t)rotation, &point) ==
					      expected &&
				      v34_training_constellation_fixed_point(constellation,
					      (uint8_t)quarter, (uint8_t)rotation, &fixed) ==
					      expected,
				      "wrong label validity: size=%d quarter=%u rotation=%u",
				      sizes[size], quarter, rotation);
				CHECK(valid || (point.in_phase == sentinel.in_phase &&
				      point.quadrature == sentinel.quadrature &&
				      fixed.in_phase == fixed_sentinel.in_phase &&
				      fixed.quadrature == fixed_sentinel.quadrature),
				      "invalid training decision changed output");
			}
		}
	}
	return true;
}

static bool test_four_point_constellation(void)
{
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		struct qam_point point;
		struct qam_point expected =
			expected_rotated_grid_point(1, 1, sqrt(0.5), rotation);
		double power;

		CHECK(v34_training_constellation_point(
			      V34_CONSTELLATION_4_POINT, 0,
			      rotation, &point) ==
			      V34_CONSTELLATION_OK,
		      "four-point rotation %u was not generated", rotation);
		power = point.in_phase * point.in_phase +
			point.quadrature * point.quadrature;
		CHECK(close_to(point.in_phase, expected.in_phase) &&
			      close_to(point.quadrature, expected.quadrature) &&
			      close_to(power, 1.0),
		      "four-point rotation %u changed", rotation);
	}
	return true;
}

static bool test_sixteen_point_constellation(void)
{
	static const int quarter_grid[4][2] = {
		{ 1, 1 },
		{ -3, 1 },
		{ 1, -3 },
		{ -3, -3 },
	};
	struct qam_point points[16];
	double power_sum = 0.0;
	double maximum_power = 0.0;
	size_t count = 0;

	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		for (uint8_t quarter = 0; quarter < 4u; ++quarter) {
			struct qam_point expected =
				expected_rotated_grid_point(
					quarter_grid[quarter][0],
					quarter_grid[quarter][1], sqrt(0.1),
					rotation);
			double power;

			CHECK(v34_training_constellation_point(
				      V34_CONSTELLATION_16_POINT,
				      quarter, rotation, &points[count]) ==
				      V34_CONSTELLATION_OK,
			      "sixteen-point selection %u/%u was not generated",
			      quarter, rotation);
			CHECK(close_to(points[count].in_phase,
				       expected.in_phase) &&
				      close_to(points[count].quadrature,
				       expected.quadrature),
			      "sixteen-point selection %u/%u changed", quarter,
			      rotation);
			power = points[count].in_phase *
					points[count].in_phase +
				points[count].quadrature *
					points[count].quadrature;
			power_sum += power;
			if (power > maximum_power)
				maximum_power = power;
			for (size_t previous = 0; previous < count; ++previous)
				CHECK(!close_to(
					      points[count].in_phase,
					      points[previous].in_phase) ||
					      !close_to(
						      points[count].quadrature,
						      points[previous].quadrature),
				      "sixteen-point selections %zu and %zu coincide",
				      previous, count);
			count++;
		}
	}
	CHECK(count == ARRAY_LENGTH(points) &&
		      close_to(power_sum / (double)count, 1.0) &&
		      close_to(maximum_power, 9.0 / 5.0),
	      "sixteen-point power normalization changed");
	return true;
}

static bool test_fixed_training_constellations(void)
{
	static const struct v34_analytic_sample four_point[] = {
		{ 2289, 2289 },
		{ 2289, -2290 },
		{ -2290, -2290 },
		{ -2290, 2289 },
	};
	static const struct v34_analytic_sample sixteen_point[] = {
		{ 1024, 1024 }, { -3072, 1024 },
		{ 1024, -3072 }, { -3072, -3072 },
		{ 1024, -1024 }, { 1024, 3072 },
		{ -3072, -1024 }, { -3072, 3072 },
		{ -1024, -1024 }, { 3072, -1024 },
		{ -1024, 3072 }, { 3072, 3072 },
		{ -1024, 1024 }, { -1024, -3072 },
		{ 3072, 1024 }, { 3072, -3072 },
	};
	static const struct v34_analytic_sample perturbations[] = {
		{ 0, 0 },
		{ 211, -173 },
		{ -193, 229 },
	};
	static const struct {
		enum v34_constellation_size constellation;
		const struct v34_analytic_sample *points;
		uint8_t quarters;
	} fixtures[] = {
		{ V34_CONSTELLATION_4_POINT, four_point, 1 },
		{ V34_CONSTELLATION_16_POINT, sixteen_point, 4 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures);
	     ++fixture) {
		for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
			for (uint8_t quarter = 0;
			     quarter < fixtures[fixture].quarters; ++quarter) {
				size_t index =
					(size_t)rotation * fixtures[fixture].quarters +
					quarter;
				struct v34_analytic_sample generated = { 0 };

				CHECK(v34_training_constellation_fixed_point(
					      fixtures[fixture].constellation,
					      quarter, rotation, &generated) ==
					      V34_CONSTELLATION_OK &&
				      generated.in_phase ==
					      fixtures[fixture].points[index].in_phase &&
				      generated.quadrature ==
					      fixtures[fixture].points[index].quadrature,
				      "fixed point %zu/%u/%u changed",
				      fixture, quarter, rotation);
				for (size_t noise = 0;
				     noise < ARRAY_LENGTH(perturbations); ++noise) {
					struct v34_fixed_training_decision decision;
					struct v34_analytic_sample observed = {
						.in_phase = (int16_t)(
							generated.in_phase +
							perturbations[noise].in_phase),
						.quadrature = (int16_t)(
							generated.quadrature +
							perturbations[noise].quadrature),
					};

					CHECK(v34_training_constellation_has_signal(
						      fixtures[fixture].constellation, observed),
					      "training point %zu/%u/%u rejected at %zu",
					      fixture, quarter, rotation, noise);
					CHECK(v34_training_constellation_fixed_slice(
						      fixtures[fixture].constellation,
						      observed, &decision) ==
						      V34_CONSTELLATION_OK &&
					      decision.quarter_point == quarter &&
					      decision.clockwise_quarter_turns ==
						      rotation &&
					      decision.target.in_phase ==
						      generated.in_phase &&
					      decision.target.quadrature ==
						      generated.quadrature,
					      "fixed slice %zu/%u/%u changed at %zu",
					      fixture, quarter, rotation, noise);
				}
			}
		}
		{
			struct v34_fixed_training_decision decision;

			CHECK(v34_training_constellation_fixed_slice(
				      fixtures[fixture].constellation,
				      (struct v34_analytic_sample){ 0 },
				      &decision) == V34_CONSTELLATION_OK &&
			      decision.quarter_point == 0 &&
			      decision.clockwise_quarter_turns == 0,
			      "fixed tie order changed for fixture %zu", fixture);
		}
	}
	return true;
}

static bool test_training_signal_qualification(void)
{
	static const struct {
		enum v34_constellation_size constellation;
		int16_t half_coordinate;
	} fixtures[] = {
		{ V34_CONSTELLATION_4_POINT, 1145 },
		{ V34_CONSTELLATION_16_POINT, 512 },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(fixtures); ++i) {
		enum v34_constellation_size constellation = fixtures[i].constellation;
		int16_t edge = fixtures[i].half_coordinate;
		struct v34_analytic_sample weak = { edge - 1, edge - 1 };
		struct v34_analytic_sample threshold = { edge, edge };

		CHECK(!v34_training_constellation_has_signal(
			      constellation, (struct v34_analytic_sample){ 0 }) &&
		      !v34_training_constellation_has_signal(constellation, weak) &&
		      v34_training_constellation_has_signal(constellation, threshold),
		      "half-radius signal threshold changed for %zu", i);
		CHECK(v34_training_constellation_has_signal(
			      constellation,
			      (struct v34_analytic_sample){ INT16_MIN, INT16_MIN }) &&
		      v34_training_constellation_has_signal(
			      constellation,
			      (struct v34_analytic_sample){ INT16_MAX, INT16_MAX }),
		      "signal power overflow for %zu", i);
	}
	CHECK(!v34_training_constellation_has_signal(
		      (enum v34_constellation_size)0,
		      (struct v34_analytic_sample){ INT16_MAX, INT16_MAX }),
	      "invalid training constellation qualified as signal");
	return true;
}

static bool test_training_constellation_slicing(void)
{
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	static const struct qam_point perturbations[] = {
		{ 0.0, 0.0 },
		{ 0.12, -0.09 },
		{ -0.11, 0.13 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(constellations);
	     ++fixture) {
		enum v34_constellation_size constellation =
			constellations[fixture];
		uint8_t quarter_count =
			constellation == V34_CONSTELLATION_16_POINT ?
				4u :
				1u;

		for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
			for (uint8_t quarter = 0; quarter < quarter_count;
			     ++quarter) {
				struct qam_point exact;

				CHECK(v34_training_constellation_point(
					      constellation, quarter, rotation,
					      &exact) ==
					      V34_CONSTELLATION_OK,
				      "slicer fixture %zu/%u/%u failed",
				      fixture, quarter, rotation);
				for (size_t noise = 0;
				     noise < ARRAY_LENGTH(perturbations);
				     ++noise) {
					struct qam_point observed = {
						.in_phase = exact.in_phase +
							perturbations[noise].in_phase,
						.quadrature = exact.quadrature +
							perturbations[noise].quadrature,
					};
					uint8_t sliced_quarter = UINT8_MAX;
					uint8_t sliced_rotation = UINT8_MAX;

					CHECK(v34_training_constellation_slice(
						      constellation, &observed,
						      &sliced_quarter,
						      &sliced_rotation) ==
						      V34_CONSTELLATION_OK &&
					      sliced_quarter == quarter &&
					      sliced_rotation == rotation,
					      "slicer changed %zu/%u/%u at noise %zu",
					      fixture, quarter, rotation, noise);
				}
			}
		}
		{
			struct qam_point tie = { 0 };
			uint8_t quarter = UINT8_MAX;
			uint8_t rotation = UINT8_MAX;

			CHECK(v34_training_constellation_slice(
				      constellation, &tie, &quarter, &rotation) ==
				      V34_CONSTELLATION_OK &&
			      quarter == 0u && rotation == 0u,
			      "slicer tie order changed for fixture %zu", fixture);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct qam_point point = { 7.0, -9.0 };
	struct qam_point before = point;
	struct v34_grid_point generated[4];
	struct v34_grid_point saved[4];
	struct v34_grid_point indexed = { 23, -29 };
	struct v34_grid_point saved_indexed = indexed;
	static const struct v34_grid_point invalid_labels[] = {
		{ 3, 1 },
		{ 49, 1 },
		{ 45, 45 },
	};
	struct qam_point not_finite = { NAN, 0.0 };
	struct qam_point too_large = { DBL_MAX, DBL_MAX };
	struct v34_analytic_sample fixed_point = { 123, -456 };
	struct v34_analytic_sample saved_fixed_point = fixed_point;
	struct v34_fixed_training_decision fixed_decision = {
		.target = { 17, -29 },
		.quarter_point = 2,
		.clockwise_quarter_turns = 3,
	};
	struct v34_fixed_training_decision saved_fixed_decision =
		fixed_decision;
	size_t label = 0xa5a5u;
	uint8_t quarter = 0xa5;
	uint8_t rotation = 0x5a;

	memset(generated, 0x5a, sizeof(generated));
	memcpy(saved, generated, sizeof(saved));

	CHECK(v34_constellation_size_is_valid(
		      V34_CONSTELLATION_4_POINT) &&
	      v34_constellation_size_is_valid(
		      V34_CONSTELLATION_16_POINT) &&
	      !v34_constellation_size_is_valid(
		      (enum v34_constellation_size)8) &&
	      v34_constellation_bits_per_symbol(
		      V34_CONSTELLATION_4_POINT) == 2u &&
	      v34_constellation_bits_per_symbol(
		      V34_CONSTELLATION_16_POINT) == 4u &&
	      v34_constellation_bits_per_symbol(
		      (enum v34_constellation_size)8) == 0u &&
	      v34_training_constellation_point(
		      V34_CONSTELLATION_4_POINT, 1, 0,
		      &point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_point(
		      V34_CONSTELLATION_16_POINT, 4, 0,
		      &point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_point(
		      V34_CONSTELLATION_16_POINT, 0, 4,
		      &point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_point(
		      (enum v34_constellation_size)8, 0, 0,
		      &point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_point(
		      V34_CONSTELLATION_4_POINT, 0, 0, NULL) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &before, sizeof(point)) == 0,
	      "constellation validation changed its output");
	CHECK(v34_quarter_superconstellation_generate(NULL, 1) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_quarter_superconstellation_generate(
		      generated, 0) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_quarter_superconstellation_generate(
		      generated,
		      V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT +
			      1u) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(generated, saved, sizeof(generated)) == 0,
	      "invalid quarter-superconstellation generation changed output");
	CHECK(v34_quarter_superconstellation_point(
		      V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT,
		      &indexed) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_quarter_superconstellation_point(0, NULL) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&indexed, &saved_indexed, sizeof(indexed)) == 0,
	      "invalid indexed lookup changed its output");
	for (size_t invalid = 0; invalid < ARRAY_LENGTH(invalid_labels);
	     ++invalid) {
		CHECK(v34_quarter_superconstellation_label(
			      &invalid_labels[invalid], &label) ==
			      V34_CONSTELLATION_INVALID_ARGUMENT &&
		      label == 0xa5a5u,
		      "invalid inverse label %zu changed its output", invalid);
	}
	CHECK(v34_quarter_superconstellation_label(NULL, &label) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_quarter_superconstellation_label(&indexed, NULL) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      label == 0xa5a5u,
	      "invalid inverse-label pointer changed its output");
	CHECK(v34_training_constellation_slice(
		      (enum v34_constellation_size)8, &point,
		      &quarter, &rotation) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_slice(
		      V34_CONSTELLATION_4_POINT, NULL, &quarter,
		      &rotation) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_slice(
		      V34_CONSTELLATION_4_POINT, &not_finite,
		      &quarter, &rotation) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_slice(
		      V34_CONSTELLATION_4_POINT, &too_large,
		      &quarter, &rotation) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_slice(
		      V34_CONSTELLATION_4_POINT, &point, NULL,
		      &rotation) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_slice(
		      V34_CONSTELLATION_4_POINT, &point, &quarter,
		      &quarter) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      quarter == 0xa5 && rotation == 0x5a,
	      "invalid training-constellation slice changed outputs");
	CHECK(v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 1, 0,
		      &fixed_point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_16_POINT, 4, 0,
		      &fixed_point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_16_POINT, 0, 4,
		      &fixed_point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_fixed_point(
		      (enum v34_constellation_size)8, 0, 0,
		      &fixed_point) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 0, 0, NULL) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&fixed_point, &saved_fixed_point,
		     sizeof(fixed_point)) == 0,
	      "invalid fixed-point generation changed its output");
	CHECK(v34_training_constellation_fixed_slice(
		      (enum v34_constellation_size)8,
		      (struct v34_analytic_sample){ 0 },
		      &fixed_decision) == V34_CONSTELLATION_INVALID_ARGUMENT &&
	      v34_training_constellation_fixed_slice(
		      V34_CONSTELLATION_4_POINT,
		      (struct v34_analytic_sample){ 0 }, NULL) ==
		      V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&fixed_decision, &saved_fixed_decision,
		     sizeof(fixed_decision)) == 0,
	      "invalid fixed slicing changed its output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "quarter superconstellation", test_quarter_superconstellation },
		{ "indexed quarter superconstellation",
		  test_indexed_quarter_superconstellation },
		{ "quarter-superconstellation prefixes",
		  test_quarter_superconstellation_prefixes },
		{ "point-zero rotations", test_point_zero_rotations },
		{ "training decision validation", test_training_decision_validation },
		{ "four-point training set", test_four_point_constellation },
		{ "sixteen-point training set", test_sixteen_point_constellation },
		{ "fixed training sets", test_fixed_training_constellations },
		{ "training signal qualification", test_training_signal_qualification },
		{ "training-constellation slicing",
		  test_training_constellation_slicing },
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
