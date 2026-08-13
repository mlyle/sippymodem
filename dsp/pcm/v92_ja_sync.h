// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_JA_SYNC_H
#define V92_JA_SYNC_H

#include "pcm_binary_sign.h"
#include "v92_pcm_phase3_ja.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_JA_FRAME_SYNC_BITS = 17,
	V92_JA_MARKER_BITS =
		V92_PCM_PHASE3_JA_PREAMBLE_BITS + V92_JA_FRAME_SYNC_BITS + 1,
	V92_JA_SYNC_MIN_TRAINING_SYMBOLS = V34_SCRAMBLER_BITS,
	V92_JA_SYNC_MAX_PREAMBLE_ERRORS = 3,
};

enum v92_ja_sync_stage {
	V92_JA_SYNC_TRAINING,
	V92_JA_SYNC_SEARCHING,
	V92_JA_SYNC_COMPLETE,
};

enum v92_ja_sync_result {
	V92_JA_SYNC_OK,
	V92_JA_SYNC_SYNCHRONIZED,
	V92_JA_SYNC_ALREADY_COMPLETE,
	V92_JA_SYNC_INVALID_ARGUMENT,
	V92_JA_SYNC_INVALID_STATE,
};

/* Search polarity-normalized hard signs for the Ja preamble and the first
 * descriptor boundary. Search starts while TRN1u is still present. Accepted
 * preamble errors are repaired, so the handoff owns the exact coding state
 * following the descriptor's 17 one bits and initial zero bit. */
struct v92_ja_sync {
	struct pcm_binary_sign training;
	struct pcm_binary_sign window_start;
	uint64_t trained_symbols;
	uint8_t marker_signs[V92_JA_MARKER_BITS];
	enum v92_ja_sync_stage stage;
	uint8_t marker_index;
	uint8_t marker_count;
};

enum v92_ja_sync_result v92_ja_sync_init(
	struct v92_ja_sync *sync);
bool v92_ja_sync_is_valid(const struct v92_ja_sync *sync);
bool v92_ja_sync_is_complete(const struct v92_ja_sync *sync);

/* Train consumes scrambled but non-differential TRN1u signs. Begin search
 * only after the channel receiver is locked; receive then consumes one hard
 * sign at a time without requiring the caller to know the Ja boundary. */
enum v92_ja_sync_result v92_ja_sync_train(
	struct v92_ja_sync *sync, uint8_t sign);
enum v92_ja_sync_result v92_ja_sync_begin(
	struct v92_ja_sync *sync);
enum v92_ja_sync_result v92_ja_sync_receive(
	struct v92_ja_sync *sync, uint8_t sign);

/* Copy the corrected coding state at the first unconsumed descriptor bit.
 * Rejected calls leave the destination unchanged. */
bool v92_ja_sync_handoff(
	const struct v92_ja_sync *sync,
	struct pcm_binary_sign *coding);

const char *v92_ja_sync_result_name(enum v92_ja_sync_result result);

#endif /* V92_JA_SYNC_H */
