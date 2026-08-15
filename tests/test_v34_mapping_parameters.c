// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_parameters.h>

#include "test.h"

#include <math.h>
#include <string.h>

struct symbol_rate_fixture {
	enum v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ V34_SYMBOL_RATE_2400, 1, 9 },
	{ V34_SYMBOL_RATE_2743, 2, 11 },
	{ V34_SYMBOL_RATE_2800, 2, 11 },
	{ V34_SYMBOL_RATE_3000, 2, 12 },
	{ V34_SYMBOL_RATE_3200, 2, 13 },
	{ V34_SYMBOL_RATE_3429, 2, 14 },
};

static bool test_normative_parameters(void)
{
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
		uint8_t mapping_bits;
		uint8_t shell_bits;
		uint8_t point_bits;
		uint8_t minimum_rings;
		uint8_t expanded_rings;
		uint16_t minimum_points;
		uint16_t expanded_points;
	} cases[] = {
		{ V34_SYMBOL_RATE_2400, 1, 8, 0, 0,
		  1, 1, 4, 4 },
		{ V34_SYMBOL_RATE_2400, 3, 24, 12, 0,
		  3, 4, 12, 16 },
		{ V34_SYMBOL_RATE_2400, 9, 72, 28, 4,
		  12, 14, 768, 896 },
		{ V34_SYMBOL_RATE_2743, 6, 42, 30, 0,
		  14, 17, 56, 68 },
		{ V34_SYMBOL_RATE_2800, 11, 76, 24, 5,
		  8, 10, 1024, 1280 },
		{ V34_SYMBOL_RATE_3000, 12, 77, 25, 5,
		  9, 11, 1152, 1408 },
		{ V34_SYMBOL_RATE_3200, 13, 78, 26, 5,
		  10, 12, 1280, 1536 },
		{ V34_SYMBOL_RATE_3429, 14, 79, 27, 5,
		  11, 13, 1408, 1664 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_frame_geometry geometry;

		CHECK(v34_frame_geometry_init(
			      &geometry, cases[index].symbol_rate,
			      cases[index].primary_rate, false) ==
			      V34_FRAME_SCHEDULE_OK,
		      "normative mapping fixture %zu did not initialize", index);
		for (unsigned int expanded = 0; expanded < 2; ++expanded) {
			struct v34_mapping_parameters parameters;
			uint8_t expected_rings = expanded != 0 ?
				cases[index].expanded_rings :
				cases[index].minimum_rings;
			uint16_t expected_points = expanded != 0 ?
				cases[index].expanded_points :
				cases[index].minimum_points;

			CHECK(v34_mapping_parameters_init(
				      &parameters, &geometry, expanded != 0) ==
				      V34_MAPPING_PARAMETERS_OK,
			      "normative mapping fixture %zu/%u did not run",
			      index, expanded);
			CHECK(parameters.mapping_frame_bits ==
				      cases[index].mapping_bits &&
			      parameters.shell_mapper_bits ==
				      cases[index].shell_bits &&
			      parameters.point_index_bits_per_2d_symbol ==
				      cases[index].point_bits &&
			      parameters.ring_count == expected_rings &&
			      parameters.constellation_points == expected_points &&
			      parameters.expanded_shaping == (expanded != 0),
			      "normative mapping fixture %zu/%u changed", index,
			      expanded);
		}
	}
	return true;
}

static void expected_dimensions(
	uint8_t mapping_bits, bool expanded, uint8_t *shell_bits,
	uint8_t *point_bits, uint8_t *rings, uint16_t *points)
{
	unsigned int k = mapping_bits > 12u ? mapping_bits - 12u : 0u;
	unsigned int q = 0;
	double ideal_rings;
	unsigned int minimum_rings;
	unsigned int selected_rings;

	while (k >= 32u) {
		k -= 8u;
		++q;
	}
	ideal_rings = exp2((double)k / 8.0);
	minimum_rings = (unsigned int)ceil(ideal_rings);
	selected_rings = minimum_rings;
	if (expanded) {
		unsigned int rounded =
			(unsigned int)floor(1.25 * ideal_rings + 0.5);

		if (rounded > selected_rings)
			selected_rings = rounded;
	}
	*shell_bits = (uint8_t)k;
	*point_bits = (uint8_t)q;
	*rings = (uint8_t)selected_rings;
	*points = (uint16_t)(4u * selected_rings * (1u << q));
}

static bool test_all_supported_geometries(void)
{
	for (size_t symbol_index = 0;
	     symbol_index < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct v34_frame_geometry geometry;

				CHECK(v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      V34_FRAME_SCHEDULE_OK,
				      "mapping geometry %zu/%u/%u was rejected",
				      symbol_index, rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct v34_mapping_parameters
						parameters;
					uint16_t expected_points;
					uint8_t expected_shell_bits;
					uint8_t expected_point_bits;
					uint8_t expected_rings;

					expected_dimensions(
						geometry.high_mapping_frame_bits,
						expanded != 0, &expected_shell_bits,
						&expected_point_bits, &expected_rings,
						&expected_points);
					CHECK(v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      V34_MAPPING_PARAMETERS_OK &&
					      v34_mapping_parameters_is_valid(
						      &parameters),
					      "mapping parameters %zu/%u/%u/%u failed",
					      symbol_index, rate, auxiliary, expanded);
					CHECK(parameters.mapping_frame_bits ==
						      geometry.high_mapping_frame_bits &&
					      parameters.shell_mapper_bits ==
						      expected_shell_bits &&
					      parameters
						      .point_index_bits_per_2d_symbol ==
						      expected_point_bits &&
					      parameters.ring_count == expected_rings &&
					      parameters.constellation_points ==
						      expected_points &&
					      parameters.shell_mapper_bits < 32u &&
					      (parameters.mapping_frame_bits <= 12u ||
					       parameters.mapping_frame_bits ==
						       12u +
						       8u * parameters
							      .point_index_bits_per_2d_symbol +
						       parameters.shell_mapper_bits),
					      "mapping dimensions %zu/%u/%u/%u changed",
					      symbol_index, rate, auxiliary, expanded);
				}
			}
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_mapping_parameters saved;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &parameters, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK,
	      "validation mapping parameters did not initialize");
	saved = parameters;
	CHECK(v34_mapping_parameters_init(
		      NULL, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_INVALID_ARGUMENT &&
	      v34_mapping_parameters_init(
		      &parameters, NULL, false) ==
		      V34_MAPPING_PARAMETERS_INVALID_ARGUMENT &&
	      v34_mapping_parameters_are_equal(&parameters, &saved),
	      "invalid mapping initialization changed its destination");

	geometry.high_mapping_frame_bits++;
	CHECK(v34_mapping_parameters_init(
		      &parameters, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_INVALID_ARGUMENT &&
	      v34_mapping_parameters_are_equal(&parameters, &saved),
	      "corrupt geometry changed mapping parameters");

	parameters = saved;
	parameters.constellation_points++;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted a changed constellation size");
	parameters = saved;
	parameters.mapping_frame_bits = 7;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted too few frame bits");
	parameters = saved;
	parameters.mapping_frame_bits = 80;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted too many frame bits");
	parameters = saved;
	parameters.shell_mapper_bits++;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted a changed shell width");
	parameters = saved;
	parameters.point_index_bits_per_2d_symbol++;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted a changed point width");
	parameters = saved;
	parameters.ring_count++;
	CHECK(!v34_mapping_parameters_is_valid(&parameters),
	      "mapping parameters accepted a changed ring count");
	parameters = saved;
	parameters.expanded_shaping = true;
	CHECK(!v34_mapping_parameters_is_valid(&parameters) &&
	      !v34_mapping_parameters_is_valid(NULL),
	      "mapping parameters accepted inconsistent shaping");

	CHECK(strcmp(v34_mapping_parameters_result_name(
		     V34_MAPPING_PARAMETERS_OK),
		     "ok") == 0 &&
	      strcmp(v34_mapping_parameters_result_name(
		     V34_MAPPING_PARAMETERS_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_mapping_parameters_result_name(
		     (enum v34_mapping_parameters_result)2),
		     "unknown") == 0,
	      "mapping parameter result names changed");
	return true;
}

static bool test_equality(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, true) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &parameters, &geometry, true) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      v34_mapping_parameters_are_equal(&parameters, &parameters) &&
	      !v34_mapping_parameters_are_equal(NULL, &parameters) &&
	      !v34_mapping_parameters_are_equal(&parameters, NULL),
	      "mapping-parameter equality rejected its base contract");
	for (unsigned int field = 0; field < 6; ++field) {
		struct v34_mapping_parameters changed = parameters;

		switch (field) {
		case 0:
			changed.constellation_points++;
			break;
		case 1:
			changed.mapping_frame_bits++;
			break;
		case 2:
			changed.shell_mapper_bits++;
			break;
		case 3:
			changed.point_index_bits_per_2d_symbol++;
			break;
		case 4:
			changed.ring_count++;
			break;
		case 5:
			changed.expanded_shaping = !changed.expanded_shaping;
			break;
		}
		CHECK(!v34_mapping_parameters_are_equal(&parameters, &changed) &&
		      !v34_mapping_parameters_are_equal(&changed, &parameters),
		      "mapping-parameter field %u did not affect equality", field);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative parameters", test_normative_parameters },
		{ "all supported geometries", test_all_supported_geometries },
		{ "validation", test_validation },
		{ "equality", test_equality },
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
