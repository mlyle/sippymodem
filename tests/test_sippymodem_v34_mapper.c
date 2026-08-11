// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_mapper.h>

#include "test.h"

#include <string.h>

struct symbol_rate_fixture {
	enum sippymodem_v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, 9 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2743, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3000, 2, 12 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3200, 2, 13 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, 14 },
};

static struct sippymodem_v34_grid_point rotate_clockwise(
	struct sippymodem_v34_grid_point point, uint8_t quarter_turns)
{
	int in_phase = point.in_phase;
	int quadrature = point.quadrature;

	for (uint8_t turn = 0; turn < quarter_turns; ++turn) {
		int saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	return (struct sippymodem_v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
}

static bool test_all_supported_mappings(void)
{
	struct sippymodem_v34_grid_point
		quarter[SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	bool point_width_seen[6] = { false };
	size_t configurations = 0;
	size_t mappings = 0;
	uint16_t largest_label = 0;

	CHECK(sippymodem_v34_quarter_superconstellation_generate(
		      quarter, ARRAY_LENGTH(quarter)) ==
		      SIPPYMODEM_V34_CONSTELLATION_OK,
	      "mapper oracle generation failed");
	for (size_t symbol_rate = 0;
	     symbol_rate < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_rate) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_rate];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct sippymodem_v34_frame_geometry geometry;

				CHECK(sippymodem_v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
				      "geometry %zu/%u/%u failed", symbol_rate,
				      rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct sippymodem_v34_mapping_parameters
						parameters;
					uint8_t point_limit;

					CHECK(sippymodem_v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
					      "parameters %zu/%u/%u/%u failed",
					      symbol_rate, rate, auxiliary, expanded);
					point_limit = (uint8_t)(
						1u << parameters
							.point_index_bits_per_2d_symbol);
					point_width_seen[parameters
							.point_index_bits_per_2d_symbol] = true;
					for (uint8_t ring = 0;
					     ring < parameters.ring_count; ++ring) {
						for (uint8_t point = 0;
						     point < point_limit; ++point) {
							uint16_t label = (uint16_t)(
								((uint16_t)ring << parameters
									 .point_index_bits_per_2d_symbol) |
								point);

							for (uint8_t rotation = 0;
							     rotation < 4u; ++rotation) {
								const struct
									sippymodem_v34_2d_mapping_input
									input = {
										.ring_index = ring,
										.point_index = point,
										.clockwise_quarter_turns =
											rotation,
									};
								struct sippymodem_v34_2d_mapping_output
									output;
								struct sippymodem_v34_2d_mapping_input
									recovered;
								struct sippymodem_v34_grid_point expected =
									rotate_clockwise(
										quarter[label], rotation);

								CHECK(sippymodem_v34_2d_mapping_input_is_valid(
									      &input, &parameters) &&
								      sippymodem_v34_map_2d_symbol(
									      &parameters, &input, &output) ==
									      SIPPYMODEM_V34_MAPPER_OK,
								      "mapping %zu/%u/%u/%u/%u/%u/%u failed",
								      symbol_rate, rate, auxiliary, expanded,
								      ring, point, rotation);
								CHECK(output.constellation_label == label &&
								      output.mapper_output.in_phase ==
									      expected.in_phase &&
								      output.mapper_output.quadrature ==
									      expected.quadrature &&
								      sippymodem_v34_unmap_2d_symbol(
									      &parameters,
									      &output.mapper_output,
									      &recovered) ==
									      SIPPYMODEM_V34_MAPPER_OK &&
								      recovered.ring_index == ring &&
								      recovered.point_index == point &&
								      recovered.clockwise_quarter_turns ==
									      rotation,
								      "mapping label %u rotation %u changed",
								      label, rotation);
								mappings++;
							}
							if (label > largest_label)
								largest_label = label;
						}
					}
					configurations++;
				}
			}
		}
	}
	CHECK(configurations == 260u && mappings == 72140u &&
	      largest_label ==
		      SIPPYMODEM_V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT - 1u,
	      "mapper coverage changed to %zu/%zu/%u", configurations,
	      mappings, largest_label);
	for (size_t width = 0; width < ARRAY_LENGTH(point_width_seen); ++width)
		CHECK(point_width_seen[width],
		      "point-index width %zu was not covered", width);
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_frame_geometry low_geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_mapping_parameters low_parameters;
	struct sippymodem_v34_2d_mapping_input input = { 0 };
	struct sippymodem_v34_2d_mapping_output output;
	struct sippymodem_v34_2d_mapping_output saved;
	struct sippymodem_v34_2d_mapping_input recovered;
	struct sippymodem_v34_2d_mapping_input recovered_saved;
	struct sippymodem_v34_grid_point invalid_point = { 3, 1 };
	struct sippymodem_v34_grid_point off_grid_point = { 2, 1 };
	struct sippymodem_v34_grid_point outside_point;

	CHECK(sippymodem_v34_frame_geometry_init(
		      &geometry, SIPPYMODEM_V34_SYMBOL_RATE_3429, 14, false) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &parameters, &geometry, true) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
	      "validation parameters did not initialize");
	CHECK(sippymodem_v34_frame_geometry_init(
		      &low_geometry, SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, false) ==
		      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &low_parameters, &low_geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
	      low_parameters.constellation_points == 4u &&
	      sippymodem_v34_quarter_superconstellation_point(
		      1, &outside_point) == SIPPYMODEM_V34_CONSTELLATION_OK,
	      "low-constellation validation fixture did not initialize");
	memset(&output, 0x5a, sizeof(output));
	saved = output;
	memset(&recovered, 0xa5, sizeof(recovered));
	recovered_saved = recovered;
	input.ring_index = parameters.ring_count;
	CHECK(!sippymodem_v34_2d_mapping_input_is_valid(&input, &parameters) &&
	      sippymodem_v34_map_2d_symbol(&parameters, &input, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "invalid ring changed the mapping output");
	input = (struct sippymodem_v34_2d_mapping_input){
		.point_index = (uint8_t)(
			1u << parameters.point_index_bits_per_2d_symbol),
	};
	CHECK(sippymodem_v34_map_2d_symbol(&parameters, &input, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT,
	      "invalid point index was accepted");
	input = (struct sippymodem_v34_2d_mapping_input){
		.clockwise_quarter_turns = 4,
	};
	CHECK(sippymodem_v34_map_2d_symbol(&parameters, &input, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_map_2d_symbol(NULL, &input, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_map_2d_symbol(&parameters, NULL, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_map_2d_symbol(&parameters, &input, NULL) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      !sippymodem_v34_2d_mapping_input_is_valid(NULL, &parameters) &&
	      !sippymodem_v34_2d_mapping_input_is_valid(&input, NULL) &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "invalid mapper argument changed the output");
	input = (struct sippymodem_v34_2d_mapping_input){ 0 };
	parameters.mapping_frame_bits++;
	CHECK(!sippymodem_v34_2d_mapping_input_is_valid(&input, &parameters) &&
	      sippymodem_v34_map_2d_symbol(&parameters, &input, &output) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "inconsistent parameters changed the output");
	CHECK(strcmp(sippymodem_v34_mapper_result_name(
		     SIPPYMODEM_V34_MAPPER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_mapper_result_name(
		     SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_mapper_result_name(
		     (enum sippymodem_v34_mapper_result)2),
		     "unknown") == 0,
	      "mapper result names changed");
	CHECK(sippymodem_v34_unmap_2d_symbol(
		      &low_parameters, &invalid_point, &recovered) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_unmap_2d_symbol(
		      &low_parameters, &off_grid_point, &recovered) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_unmap_2d_symbol(
		      &low_parameters, &outside_point, &recovered) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_unmap_2d_symbol(
		      NULL, &invalid_point, &recovered) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_unmap_2d_symbol(
		      &low_parameters, NULL, &recovered) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_unmap_2d_symbol(
		      &low_parameters, &invalid_point, NULL) ==
		      SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&recovered, &recovered_saved, sizeof(recovered)) == 0,
	      "invalid inverse mapper changed its output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all supported mappings", test_all_supported_mappings },
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
