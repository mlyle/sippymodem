// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapper.h>

#include "test.h"

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

static struct v34_grid_point rotate_clockwise(
	struct v34_grid_point point, uint8_t quarter_turns)
{
	int in_phase = point.in_phase;
	int quadrature = point.quadrature;

	for (uint8_t turn = 0; turn < quarter_turns; ++turn) {
		int saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	return (struct v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
}

static bool test_all_supported_mappings(void)
{
	struct v34_grid_point
		quarter[V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT];
	bool point_width_seen[6] = { false };
	size_t configurations = 0;
	size_t mappings = 0;
	uint16_t largest_label = 0;

	CHECK(v34_quarter_superconstellation_generate(
		      quarter, ARRAY_LENGTH(quarter)) ==
		      V34_CONSTELLATION_OK,
	      "mapper oracle generation failed");
	for (size_t symbol_rate = 0;
	     symbol_rate < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_rate) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_rate];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct v34_frame_geometry geometry;

				CHECK(v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      V34_FRAME_SCHEDULE_OK,
				      "geometry %zu/%u/%u failed", symbol_rate,
				      rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct v34_mapping_parameters
						parameters;
					uint8_t point_limit;

					CHECK(v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      V34_MAPPING_PARAMETERS_OK,
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
									v34_2d_mapping_input
									input = {
										.ring_index = ring,
										.point_index = point,
										.clockwise_quarter_turns =
											rotation,
									};
								struct v34_2d_mapping_output
									output;
								struct v34_2d_mapping_input
									recovered;
								struct v34_grid_point expected =
									rotate_clockwise(
										quarter[label], rotation);

								CHECK(v34_2d_mapping_input_is_valid(
									      &input, &parameters) &&
								      v34_map_2d_symbol(
									      &parameters, &input, &output) ==
									      V34_MAPPER_OK,
								      "mapping %zu/%u/%u/%u/%u/%u/%u failed",
								      symbol_rate, rate, auxiliary, expanded,
								      ring, point, rotation);
								CHECK(output.constellation_label == label &&
								      output.mapper_output.in_phase ==
									      expected.in_phase &&
								      output.mapper_output.quadrature ==
									      expected.quadrature &&
								      v34_unmap_2d_symbol(
									      &parameters,
									      &output.mapper_output,
									      &recovered) ==
									      V34_MAPPER_OK &&
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
		      V34_QUARTER_SUPERCONSTELLATION_POINT_COUNT - 1u,
	      "mapper coverage changed to %zu/%zu/%u", configurations,
	      mappings, largest_label);
	for (size_t width = 0; width < ARRAY_LENGTH(point_width_seen); ++width)
		CHECK(point_width_seen[width],
		      "point-index width %zu was not covered", width);
	return true;
}

static bool test_validation(void)
{
	struct v34_frame_geometry geometry;
	struct v34_frame_geometry low_geometry;
	struct v34_mapping_parameters parameters;
	struct v34_mapping_parameters low_parameters;
	struct v34_2d_mapping_input input = { 0 };
	struct v34_2d_mapping_output output;
	struct v34_2d_mapping_output saved;
	struct v34_2d_mapping_input recovered;
	struct v34_2d_mapping_input recovered_saved;
	struct v34_grid_point invalid_point = { 3, 1 };
	struct v34_grid_point off_grid_point = { 2, 1 };
	struct v34_grid_point outside_point;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 14, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &parameters, &geometry, true) ==
		      V34_MAPPING_PARAMETERS_OK,
	      "validation parameters did not initialize");
	CHECK(v34_frame_geometry_init(
		      &low_geometry, V34_SYMBOL_RATE_2400, 1, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &low_parameters, &low_geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      low_parameters.constellation_points == 4u &&
	      v34_quarter_superconstellation_point(
		      1, &outside_point) == V34_CONSTELLATION_OK,
	      "low-constellation validation fixture did not initialize");
	memset(&output, 0x5a, sizeof(output));
	saved = output;
	memset(&recovered, 0xa5, sizeof(recovered));
	recovered_saved = recovered;
	input.ring_index = parameters.ring_count;
	CHECK(!v34_2d_mapping_input_is_valid(&input, &parameters) &&
	      v34_map_2d_symbol(&parameters, &input, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "invalid ring changed the mapping output");
	input = (struct v34_2d_mapping_input){
		.point_index = (uint8_t)(
			1u << parameters.point_index_bits_per_2d_symbol),
	};
	CHECK(v34_map_2d_symbol(&parameters, &input, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT,
	      "invalid point index was accepted");
	input = (struct v34_2d_mapping_input){
		.clockwise_quarter_turns = 4,
	};
	CHECK(v34_map_2d_symbol(&parameters, &input, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_map_2d_symbol(NULL, &input, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_map_2d_symbol(&parameters, NULL, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_map_2d_symbol(&parameters, &input, NULL) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      !v34_2d_mapping_input_is_valid(NULL, &parameters) &&
	      !v34_2d_mapping_input_is_valid(&input, NULL) &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "invalid mapper argument changed the output");
	input = (struct v34_2d_mapping_input){ 0 };
	parameters.mapping_frame_bits++;
	CHECK(!v34_2d_mapping_input_is_valid(&input, &parameters) &&
	      v34_map_2d_symbol(&parameters, &input, &output) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "inconsistent parameters changed the output");
	CHECK(v34_unmap_2d_symbol(
		      &low_parameters, &invalid_point, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_symbol(
		      &low_parameters, &off_grid_point, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_symbol(
		      &low_parameters, &outside_point, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_symbol(
		      NULL, &invalid_point, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_symbol(
		      &low_parameters, NULL, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_symbol(
		      &low_parameters, &invalid_point, NULL) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&recovered, &recovered_saved, sizeof(recovered)) == 0,
	      "invalid inverse mapper changed its output");
	return true;
}

static bool test_hard_decision_folding(void)
{
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_grid_point outside;
	struct v34_2d_mapping_input recovered;
	struct v34_2d_mapping_input saved;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_2400, 1, false) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_mapping_parameters_init(
		      &parameters, &geometry, false) ==
		      V34_MAPPING_PARAMETERS_OK &&
	      parameters.constellation_points == 4u &&
	      v34_quarter_superconstellation_point(
		      1, &outside) == V34_CONSTELLATION_OK,
	      "hard-decision fixture did not initialize");
	for (uint8_t rotation = 0; rotation < 4u; ++rotation) {
		struct v34_grid_point decided =
			rotate_clockwise(outside, rotation);

		CHECK(v34_unmap_2d_symbol(
			      &parameters, &decided, &recovered) ==
			      V34_MAPPER_INVALID_ARGUMENT &&
		      v34_unmap_2d_decision(
			      &parameters, &decided, &recovered) ==
			      V34_MAPPER_OK &&
		      recovered.ring_index == 0u &&
		      recovered.point_index == 0u &&
		      recovered.clockwise_quarter_turns == rotation,
		      "hard decision rotation %u did not fold", rotation);
	}
	recovered = (struct v34_2d_mapping_input){
		.ring_index = 0xa5,
		.point_index = 0x5a,
		.clockwise_quarter_turns = 0xa5,
	};
	CHECK(v34_unmap_2d_decision(
		      &parameters,
		      &(const struct v34_grid_point){ 2, 1 },
		      &recovered) == V34_MAPPER_INVALID_ARGUMENT,
	      "off-grid hard decision was accepted");
	CHECK(v34_unmap_2d_decision(
		      &parameters,
		      &(const struct v34_grid_point){ 47, 1 },
		      &recovered) == V34_MAPPER_OK &&
	      recovered.ring_index == 0u &&
	      recovered.point_index == 0u &&
	      recovered.clockwise_quarter_turns == 3u,
	      "bounded hard decision did not choose a nearest label");
	recovered = (struct v34_2d_mapping_input){
		.ring_index = 0xa5,
		.point_index = 0x5a,
		.clockwise_quarter_turns = 0xa5,
	};
	saved = recovered;
	CHECK(v34_unmap_2d_decision(
		      &parameters,
		      &(const struct v34_grid_point){ 257, 1 },
		      &recovered) == V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_decision(NULL, &outside, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_decision(&parameters, NULL, &recovered) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      v34_unmap_2d_decision(&parameters, &outside, NULL) ==
		      V34_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&recovered, &saved, sizeof(recovered)) == 0,
	      "invalid hard decision changed its output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all supported mappings", test_all_supported_mappings },
		{ "hard decision folding", test_hard_decision_folding },
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
