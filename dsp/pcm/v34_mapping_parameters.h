// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPING_PARAMETERS_H
#define V34_MAPPING_PARAMETERS_H

#include "v34_frame_schedule.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_mapping_parameters_result {
	V34_MAPPING_PARAMETERS_OK,
	V34_MAPPING_PARAMETERS_INVALID_ARGUMENT,
};

enum {
	V34_MAX_MAPPING_FRAME_BITS = 79,
	V34_MAX_SHELL_MAPPER_BITS = 31,
	V34_MAX_RING_COUNT = 18,
};

/* Clause 9.2 dimensions for one high mapping frame. */
struct v34_mapping_parameters {
	uint16_t constellation_points;
	uint8_t mapping_frame_bits;
	uint8_t shell_mapper_bits;
	uint8_t point_index_bits_per_2d_symbol;
	uint8_t ring_count;
	bool expanded_shaping;
};

/* Generate K, q, M, and L from the selected frame geometry. The result uses
 * exact integer comparisons instead of floating point or stored ring tables. */
enum v34_mapping_parameters_result
v34_mapping_parameters_init(
	struct v34_mapping_parameters *parameters,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping);

bool v34_mapping_parameters_is_valid(
	const struct v34_mapping_parameters *parameters);
/* Compare every derived parameter field; NULL never compares equal. */
bool v34_mapping_parameters_are_equal(
	const struct v34_mapping_parameters *left,
	const struct v34_mapping_parameters *right);

const char *v34_mapping_parameters_result_name(
	enum v34_mapping_parameters_result result);

#endif
