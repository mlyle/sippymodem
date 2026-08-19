// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_PROFILE_H
#define V32_TRELLIS_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_TRELLIS_PROFILE_CANDIDATES = 8,
	V32_TRELLIS_PROFILE_REGION_BLOCK_LABELS = 32,
};

enum v32_trellis_profile_result {
	V32_TRELLIS_PROFILE_OK,
	V32_TRELLIS_PROFILE_INVALID_ARGUMENT,
};

/*
 * Candidate labels are stored directly.  Region entries name aligned blocks
 * of 32 labels, keeping the decoder profile compact while preserving its
 * exact region geometry.
 */
struct v32_trellis_profile {
	uint8_t data_bits_per_symbol;
	uint8_t region_window;
	const uint8_t *candidate_labels;
	size_t candidate_label_count;
	const uint8_t *region_blocks;
	size_t region_block_count;
};

const struct v32_trellis_profile *
v32_trellis_profile_for_data_bits(uint8_t data_bits_per_symbol);
bool v32_trellis_profile_is_valid(
	const struct v32_trellis_profile *profile);

enum v32_trellis_profile_result v32_trellis_profile_candidates(
	const struct v32_trellis_profile *profile, int16_t in_phase,
	int16_t quadrature,
	uint8_t candidates[V32_TRELLIS_PROFILE_CANDIDATES]);

#endif
