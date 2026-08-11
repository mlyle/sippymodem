// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

static const double TEST_TOLERANCE = 1e-14;

static bool close_to(double left, double right)
{
	return fabs(left - right) < TEST_TOLERANCE;
}

static int grid_squared_magnitude(
	const struct sippymodem_v34_grid_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

static size_t independent_quarter_grid_rank(
	const struct sippymodem_v34_grid_point *point)
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
	const struct sippymodem_v34_grid_point *points, size_t point_count)
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
	struct sippymodem_v34_grid_point
		points[SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];

	CHECK(sippymodem_v34_quarter_superconstellation_generate(
		      points, ARRAY_LENGTH(points)) ==
		      SIPPYMODEM_V34_CONSTELLATION_OK,
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
	struct sippymodem_v34_grid_point
		points[SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];

	CHECK(sippymodem_v34_quarter_superconstellation_generate(
		      points, ARRAY_LENGTH(points)) ==
		      SIPPYMODEM_V34_CONSTELLATION_OK,
	      "indexed fixture generation failed");
	/* 37 is coprime to 416, so this visits every label in a deliberately
	 * non-sequential order. */
	for (size_t visit = 0; visit < ARRAY_LENGTH(points); ++visit) {
		size_t label = visit * 37u % ARRAY_LENGTH(points);
		size_t recovered_label = SIZE_MAX;
		struct sippymodem_v34_grid_point selected = { 0 };

		CHECK(sippymodem_v34_quarter_superconstellation_point(
			      label, &selected) ==
			      SIPPYMODEM_V34_CONSTELLATION_OK,
		      "indexed label %zu was rejected", label);
		CHECK(selected.in_phase == points[label].in_phase &&
		      selected.quadrature == points[label].quadrature &&
		      independent_quarter_grid_rank(&selected) == label &&
		      sippymodem_v34_quarter_superconstellation_label(
			      &selected, &recovered_label) ==
			      SIPPYMODEM_V34_CONSTELLATION_OK &&
		      recovered_label == label,
		      "indexed label %zu selected the wrong point", label);
	}
	return true;
}

static bool test_quarter_superconstellation_prefixes(void)
{
	struct sippymodem_v34_grid_point
		full[SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	struct sippymodem_v34_grid_point prefix[32];
	struct sippymodem_v34_grid_point sentinel[32];
	static const size_t prefix_lengths[] = { 1, 4, 16, 32 };

	CHECK(sippymodem_v34_quarter_superconstellation_generate(
		      full, ARRAY_LENGTH(full)) ==
		      SIPPYMODEM_V34_CONSTELLATION_OK,
	      "full prefix fixture generation failed");
	memset(sentinel, 0x5a, sizeof(sentinel));
	for (size_t fixture = 0; fixture < ARRAY_LENGTH(prefix_lengths);
	     ++fixture) {
		size_t prefix_length = prefix_lengths[fixture];

		memcpy(prefix, sentinel, sizeof(prefix));
		CHECK(sippymodem_v34_quarter_superconstellation_generate(
			      prefix, prefix_length) ==
			      SIPPYMODEM_V34_CONSTELLATION_OK,
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

static struct sippymodem_qam_point expected_rotated_grid_point(
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
	return (struct sippymodem_qam_point){
		.in_phase = in_phase * grid_unit,
		.quadrature = quadrature * grid_unit,
	};
}

static bool test_point_zero_rotations(void)
{
	for (int turns = -9; turns <= 9; ++turns) {
		struct sippymodem_qam_point point =
			sippymodem_v34_rotated_point_zero(turns);
		struct sippymodem_qam_point expected =
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

static bool test_four_point_constellation(void)
{
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		struct sippymodem_qam_point point;
		struct sippymodem_qam_point expected =
			expected_rotated_grid_point(1, 1, sqrt(0.5), rotation);
		double power;

		CHECK(sippymodem_v34_training_constellation_point(
			      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 0,
			      rotation, &point) ==
			      SIPPYMODEM_V34_CONSTELLATION_OK,
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
	struct sippymodem_qam_point points[16];
	double power_sum = 0.0;
	double maximum_power = 0.0;
	size_t count = 0;

	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		for (uint8_t quarter = 0; quarter < 4u; ++quarter) {
			struct sippymodem_qam_point expected =
				expected_rotated_grid_point(
					quarter_grid[quarter][0],
					quarter_grid[quarter][1], sqrt(0.1),
					rotation);
			double power;

			CHECK(sippymodem_v34_training_constellation_point(
				      SIPPYMODEM_V34_CONSTELLATION_16_POINT,
				      quarter, rotation, &points[count]) ==
				      SIPPYMODEM_V34_CONSTELLATION_OK,
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

static bool test_training_constellation_slicing(void)
{
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};
	static const struct sippymodem_qam_point perturbations[] = {
		{ 0.0, 0.0 },
		{ 0.12, -0.09 },
		{ -0.11, 0.13 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(constellations);
	     ++fixture) {
		enum sippymodem_v34_constellation_size constellation =
			constellations[fixture];
		uint8_t quarter_count =
			constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT ?
				4u :
				1u;

		for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
			for (uint8_t quarter = 0; quarter < quarter_count;
			     ++quarter) {
				struct sippymodem_qam_point exact;

				CHECK(sippymodem_v34_training_constellation_point(
					      constellation, quarter, rotation,
					      &exact) ==
					      SIPPYMODEM_V34_CONSTELLATION_OK,
				      "slicer fixture %zu/%u/%u failed",
				      fixture, quarter, rotation);
				for (size_t noise = 0;
				     noise < ARRAY_LENGTH(perturbations);
				     ++noise) {
					struct sippymodem_qam_point observed = {
						.in_phase = exact.in_phase +
							perturbations[noise].in_phase,
						.quadrature = exact.quadrature +
							perturbations[noise].quadrature,
					};
					uint8_t sliced_quarter = UINT8_MAX;
					uint8_t sliced_rotation = UINT8_MAX;

					CHECK(sippymodem_v34_training_constellation_slice(
						      constellation, &observed,
						      &sliced_quarter,
						      &sliced_rotation) ==
						      SIPPYMODEM_V34_CONSTELLATION_OK &&
					      sliced_quarter == quarter &&
					      sliced_rotation == rotation,
					      "slicer changed %zu/%u/%u at noise %zu",
					      fixture, quarter, rotation, noise);
				}
			}
		}
		{
			struct sippymodem_qam_point tie = { 0 };
			uint8_t quarter = UINT8_MAX;
			uint8_t rotation = UINT8_MAX;

			CHECK(sippymodem_v34_training_constellation_slice(
				      constellation, &tie, &quarter, &rotation) ==
				      SIPPYMODEM_V34_CONSTELLATION_OK &&
			      quarter == 0u && rotation == 0u,
			      "slicer tie order changed for fixture %zu", fixture);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before = point;
	struct sippymodem_v34_grid_point generated[4];
	struct sippymodem_v34_grid_point saved[4];
	struct sippymodem_v34_grid_point indexed = { 23, -29 };
	struct sippymodem_v34_grid_point saved_indexed = indexed;
	static const struct sippymodem_v34_grid_point invalid_labels[] = {
		{ 3, 1 },
		{ 49, 1 },
		{ 45, 45 },
	};
	struct sippymodem_qam_point not_finite = { NAN, 0.0 };
	struct sippymodem_qam_point too_large = { DBL_MAX, DBL_MAX };
	size_t label = 0xa5a5u;
	uint8_t quarter = 0xa5;
	uint8_t rotation = 0x5a;

	memset(generated, 0x5a, sizeof(generated));
	memcpy(saved, generated, sizeof(saved));

	CHECK(sippymodem_v34_constellation_size_is_valid(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT) &&
	      sippymodem_v34_constellation_size_is_valid(
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT) &&
	      !sippymodem_v34_constellation_size_is_valid(
		      (enum sippymodem_v34_constellation_size)8) &&
	      sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 1, 0,
		      &point) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT, 4, 0,
		      &point) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT, 0, 4,
		      &point) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_point(
		      (enum sippymodem_v34_constellation_size)8, 0, 0,
		      &point) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 0, 0, NULL) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &before, sizeof(point)) == 0,
	      "constellation validation changed its output");
	CHECK(sippymodem_v34_quarter_superconstellation_generate(NULL, 1) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_quarter_superconstellation_generate(
		      generated, 0) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_quarter_superconstellation_generate(
		      generated,
		      SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT +
			      1u) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(generated, saved, sizeof(generated)) == 0,
	      "invalid quarter-superconstellation generation changed output");
	CHECK(sippymodem_v34_quarter_superconstellation_point(
		      SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT,
		      &indexed) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_quarter_superconstellation_point(0, NULL) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&indexed, &saved_indexed, sizeof(indexed)) == 0,
	      "invalid indexed lookup changed its output");
	for (size_t invalid = 0; invalid < ARRAY_LENGTH(invalid_labels);
	     ++invalid) {
		CHECK(sippymodem_v34_quarter_superconstellation_label(
			      &invalid_labels[invalid], &label) ==
			      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
		      label == 0xa5a5u,
		      "invalid inverse label %zu changed its output", invalid);
	}
	CHECK(sippymodem_v34_quarter_superconstellation_label(NULL, &label) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_quarter_superconstellation_label(&indexed, NULL) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      label == 0xa5a5u,
	      "invalid inverse-label pointer changed its output");
	CHECK(sippymodem_v34_training_constellation_slice(
		      (enum sippymodem_v34_constellation_size)8, &point,
		      &quarter, &rotation) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_slice(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, NULL, &quarter,
		      &rotation) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_slice(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &not_finite,
		      &quarter, &rotation) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_slice(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &too_large,
		      &quarter, &rotation) ==
		      SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_slice(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &point, NULL,
		      &rotation) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      sippymodem_v34_training_constellation_slice(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &point, &quarter,
		      &quarter) == SIPPYMODEM_V34_CONSTELLATION_INVALID_ARGUMENT &&
	      quarter == 0xa5 && rotation == 0x5a,
	      "invalid training-constellation slice changed outputs");
	CHECK(strcmp(sippymodem_v34_constellation_result_name(
			     SIPPYMODEM_V34_CONSTELLATION_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_constellation_result_name(
			     (enum sippymodem_v34_constellation_result)99),
		     "unknown") == 0,
	      "constellation result names changed");
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
		{ "four-point training set", test_four_point_constellation },
		{ "sixteen-point training set", test_sixteen_point_constellation },
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
