// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE3_SYNC_H
#define V92_PHASE3_SYNC_H

#include "pcm_binary_sign.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PHASE3_PREAMBLE_BITS = 24,
	V92_PHASE3_FRAME_SYNC_BITS = 17,
	V92_PHASE3_MARKER_BITS =
		V92_PHASE3_PREAMBLE_BITS + V92_PHASE3_FRAME_SYNC_BITS + 1,
	V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS = V34_SCRAMBLER_BITS,
	V92_PHASE3_SYNC_MAX_PREAMBLE_ERRORS = 3,
};

enum v92_phase3_sync_stage {
	V92_PHASE3_SYNC_TRAINING,
	V92_PHASE3_SYNC_SEARCHING,
	V92_PHASE3_SYNC_COMPLETE,
};

enum v92_phase3_sync_result {
	V92_PHASE3_SYNC_OK,
	V92_PHASE3_SYNC_SYNCHRONIZED,
	V92_PHASE3_SYNC_ALREADY_COMPLETE,
	V92_PHASE3_SYNC_INVALID_ARGUMENT,
	V92_PHASE3_SYNC_INVALID_STATE,
};

/* Search polarity-normalized hard signs for the 24-one preamble shared by
 * Ja and CPt and the following 17-one frame sync plus zero start bit. Search
 * starts while TRN1u is still present. Accepted preamble errors are repaired,
 * so the handoff owns the exact coding state at the first payload bit. */
struct v92_phase3_sync {
	struct pcm_binary_sign training;
	struct pcm_binary_sign window_start;
	uint64_t trained_symbols;
	uint8_t marker_signs[V92_PHASE3_MARKER_BITS];
	enum v92_phase3_sync_stage stage;
	uint8_t marker_index;
	uint8_t marker_count;
};

enum v92_phase3_sync_result v92_phase3_sync_init(
	struct v92_phase3_sync *sync);
bool v92_phase3_sync_is_valid(const struct v92_phase3_sync *sync);
bool v92_phase3_sync_is_complete(const struct v92_phase3_sync *sync);

/* Train consumes scrambled but non-differential TRN1u signs. Begin search
 * only after the channel receiver is locked; receive then consumes one hard
 * sign at a time without requiring the caller to know the sequence boundary. */
enum v92_phase3_sync_result v92_phase3_sync_train(
	struct v92_phase3_sync *sync, uint8_t sign);
enum v92_phase3_sync_result v92_phase3_sync_begin(
	struct v92_phase3_sync *sync);
enum v92_phase3_sync_result v92_phase3_sync_receive(
	struct v92_phase3_sync *sync, uint8_t sign);

/* Copy the corrected coding state at the first unconsumed payload bit.
 * Rejected calls leave the destination unchanged. */
bool v92_phase3_sync_handoff(
	const struct v92_phase3_sync *sync,
	struct pcm_binary_sign *coding);

const char *v92_phase3_sync_result_name(enum v92_phase3_sync_result result);

#endif /* V92_PHASE3_SYNC_H */
