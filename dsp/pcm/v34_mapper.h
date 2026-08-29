// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MAPPER_H
#define V34_MAPPER_H

#include "v34_constellation.h"
#include "v34_mapping_parameters.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_mapper_result {
	V34_MAPPER_OK,
	V34_MAPPER_INVALID_ARGUMENT,
};

struct v34_2d_mapping_input {
	uint8_t ring_index;
	/* Earliest Q subgroup bit is bit zero. */
	uint8_t point_index;
	uint8_t clockwise_quarter_turns;
};

/* A hard receive decision retains the complete Figure 5 grid prefix.  Unlike
 * a mapping input, that prefix is not limited by the negotiated ring count. */
struct v34_2d_mapping_decision {
	uint16_t ring_prefix;
	uint8_t point_index;
	uint8_t clockwise_quarter_turns;
};

struct v34_2d_mapping_output {
	uint16_t constellation_label;
	/* Exact odd-integer u(n) supplied to the precoder. */
	struct v34_grid_point mapper_output;
};

bool v34_2d_mapping_input_is_valid(
	const struct v34_2d_mapping_input *input,
	const struct v34_mapping_parameters *parameters);

/* Compute Q(n), select the generated quarter-superconstellation label, and
 * rotate it clockwise. The destination is unchanged on failure. */
enum v34_mapper_result v34_map_2d_symbol(
	const struct v34_mapping_parameters *parameters,
	const struct v34_2d_mapping_input *input,
	struct v34_2d_mapping_output *output);

/* Invert one exact mapper output, including the rotation encoded by its
 * odd-grid coordinate residues. This is the receive-side boundary after
 * trellis and precoder decisions; noisy channel symbols must be decided
 * before calling it. Rejection leaves the destination unchanged. */
enum v34_mapper_result v34_unmap_2d_symbol(
	const struct v34_mapping_parameters *parameters,
	const struct v34_grid_point *mapper_output,
	struct v34_2d_mapping_input *input);

/* Recover a hard receiver decision. A noisy trellis path can produce a valid
 * odd-lattice point outside either the negotiated prefix or the complete
 * Figure 5 superconstellation. Preserve an in-superconstellation prefix;
 * otherwise select the nearest complete-superconstellation point. Off-grid
 * and unbounded points remain errors. */
enum v34_mapper_result v34_unmap_2d_decision(
	const struct v34_mapping_parameters *parameters,
	const struct v34_grid_point *mapper_output,
	struct v34_2d_mapping_decision *decision);

#endif
