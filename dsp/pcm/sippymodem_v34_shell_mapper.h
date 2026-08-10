// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_SHELL_MAPPER_H
#define SIPPYMODEM_V34_SHELL_MAPPER_H

#include "sippymodem_v34_mapping_parameters.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_RING_INDICES_PER_4D_INTERVAL = 2,
};

enum sippymodem_v34_shell_mapper_result {
	SIPPYMODEM_V34_SHELL_MAPPER_OK,
	SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT,
};

struct sippymodem_v34_ring_indices {
	uint8_t index[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME]
		     [SIPPYMODEM_V34_RING_INDICES_PER_4D_INTERVAL];
};

/* Map R0 to the eight ring indices m(0)..m(7) from clause 9.4.  Coefficients
 * are generated from the selected ring count; no normative table is stored. */
enum sippymodem_v34_shell_mapper_result sippymodem_v34_shell_map(
	const struct sippymodem_v34_mapping_parameters *parameters,
	uint32_t shell_label, struct sippymodem_v34_ring_indices *indices);

bool sippymodem_v34_ring_indices_are_valid(
	const struct sippymodem_v34_ring_indices *indices,
	const struct sippymodem_v34_mapping_parameters *parameters);

const char *sippymodem_v34_shell_mapper_result_name(
	enum sippymodem_v34_shell_mapper_result result);

#endif
