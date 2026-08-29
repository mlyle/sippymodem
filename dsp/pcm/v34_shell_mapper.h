// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_SHELL_MAPPER_H
#define V34_SHELL_MAPPER_H

#include "v34_mapping_parameters.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_RING_INDICES_PER_4D_INTERVAL = 2,
};

enum v34_shell_mapper_result {
	V34_SHELL_MAPPER_OK,
	V34_SHELL_MAPPER_INVALID_ARGUMENT,
};

struct v34_ring_indices {
	uint8_t index[V34_4D_INTERVALS_PER_MAPPING_FRAME]
		     [V34_RING_INDICES_PER_4D_INTERVAL];
};

/* Receive decisions may lie outside the negotiated shaping rings. */
struct v34_ring_prefixes {
	uint16_t prefix[V34_4D_INTERVALS_PER_MAPPING_FRAME]
		       [V34_RING_INDICES_PER_4D_INTERVAL];
};

/* Map R0 to the eight ring indices m(0)..m(7) from clause 9.4.  Coefficients
 * are generated from the selected ring count; no normative table is stored. */
enum v34_shell_mapper_result v34_shell_map(
	const struct v34_mapping_parameters *parameters,
	uint32_t shell_label, struct v34_ring_indices *indices);

/* Recover R0 from eight exact decided ring indices. The same generated
 * coefficients define both directions; unused expanded-shaping sequences
 * are rejected and the destination is unchanged on failure. */
enum v34_shell_mapper_result v34_shell_unmap(
	const struct v34_mapping_parameters *parameters,
	const struct v34_ring_indices *indices,
	uint32_t *shell_label);

/* Recover the low `shell_bits` data bits from received Figure 5 prefixes.
 * Prefixes outside the negotiated shaping rings follow the bounded receive
 * extension before the recovered shell word is truncated to K bits. */
enum v34_shell_mapper_result v34_shell_unmap_decision(
	const struct v34_mapping_parameters *parameters,
	const struct v34_ring_prefixes *prefixes,
	uint8_t shell_bits, uint32_t *shell_label);

bool v34_ring_indices_are_valid(
	const struct v34_ring_indices *indices,
	const struct v34_mapping_parameters *parameters);

#endif
