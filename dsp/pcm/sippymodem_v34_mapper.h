// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MAPPER_H
#define SIPPYMODEM_V34_MAPPER_H

#include "sippymodem_v34_constellation.h"
#include "sippymodem_v34_mapping_parameters.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_mapper_result {
	SIPPYMODEM_V34_MAPPER_OK,
	SIPPYMODEM_V34_MAPPER_INVALID_ARGUMENT,
};

struct sippymodem_v34_2d_mapping_input {
	uint8_t ring_index;
	/* Earliest Q subgroup bit is bit zero. */
	uint8_t point_index;
	uint8_t clockwise_quarter_turns;
};

struct sippymodem_v34_2d_mapping_output {
	uint16_t constellation_label;
	/* Exact odd-integer u(n) supplied to the precoder. */
	struct sippymodem_v34_grid_point mapper_output;
};

bool sippymodem_v34_2d_mapping_input_is_valid(
	const struct sippymodem_v34_2d_mapping_input *input,
	const struct sippymodem_v34_mapping_parameters *parameters);

/* Compute Q(n), select the generated quarter-superconstellation label, and
 * rotate it clockwise. The destination is unchanged on failure. */
enum sippymodem_v34_mapper_result sippymodem_v34_map_2d_symbol(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_2d_mapping_input *input,
	struct sippymodem_v34_2d_mapping_output *output);

/* Invert one exact mapper output, including the rotation encoded by its
 * odd-grid coordinate residues. This is the receive-side boundary after
 * trellis and precoder decisions; noisy channel symbols must be decided
 * before calling it. Rejection leaves the destination unchanged. */
enum sippymodem_v34_mapper_result sippymodem_v34_unmap_2d_symbol(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_grid_point *mapper_output,
	struct sippymodem_v34_2d_mapping_input *input);

const char *sippymodem_v34_mapper_result_name(
	enum sippymodem_v34_mapper_result result);

#endif
