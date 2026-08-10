// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_constellation.h>

#include "test.h"

#include <math.h>
#include <string.h>

static const double TEST_TOLERANCE = 1e-14;

static bool close_to(double left, double right)
{
	return fabs(left - right) < TEST_TOLERANCE;
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

static bool test_validation(void)
{
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	struct sippymodem_qam_point before = point;

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
		{ "point-zero rotations", test_point_zero_rotations },
		{ "four-point training set", test_four_point_constellation },
		{ "sixteen-point training set", test_sixteen_point_constellation },
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
