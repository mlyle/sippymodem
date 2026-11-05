// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_B1_SYNC_H
#define V92_B1_SYNC_H

#include "v92_upstream_data_rx.h"

enum {
	V92_B1_SYNC_SHIFT = 12 * V92_PCM_MAPPING_FRAME_SYMBOLS,
	V92_B1_SYNC_WORDS = 5,
};

/* One pre-data alignment retry, not a second payload decoder. Retain actual
 * inputs until acquisition succeeds or fails. A failed early decoder may
 * select a better known-word boundary, then ordinary independent B1u gates
 * still own readiness. No state or pointers survive the receive epoch. */
struct v92_b1_sync {
	struct v92_upstream_rx handoff;
	struct v92_upstream_seed seed;
	struct v92_upstream_input input[V92_UPSTREAM_INPUT_QUEUE];
	uint8_t digits[V92_B1_SYNC_WORDS][V92_PCM_MAPPING_FRAME_SYMBOLS];
	double first_score, best_score;
	uint16_t head, count, candidate, shift;
	uint8_t stage;
};

bool v92_b1_sync_init(struct v92_b1_sync *sync,
	const struct v92_upstream_rx *handoff,
	const struct v92_upstream_data_rx *receiver);
/* The enclosing owner separately validates the referenced receiver. */
bool v92_b1_sync_is_valid(const struct v92_b1_sync *sync,
	const struct v92_upstream_data_rx *receiver);

/* Exclusively owned, block-validated call path. At most one alignment score
 * (one bounded restart per epoch) or two retained-input submissions per input;
 * never two outputs.
 * Missing media abandons recovery. No deadlines or quality gates change. */
enum v92_upstream_data_rx_result v92_b1_sync_octet_owned(
	struct v92_b1_sync *sync, struct v92_upstream_data_rx *receiver,
	uint8_t code, int16_t sample, int16_t host, bool available,
	struct v92_upstream_data_frame *frame);

#endif
