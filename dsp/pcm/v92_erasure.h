// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_ERASURE_H
#define V92_ERASURE_H

#include "v92_residual.h"
#include "v92_pcm_frame.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_ERASURE_HISTORY = 16,
	V92_ERASURE_EPISODE_FRAMES = 167,
	V92_ERASURE_MAX_EPISODES = 8,
	V92_ERASURE_MAX_RESEEDS = 16,
	V92_ERASURE_MAX_HELD = 512,
};

struct v92_erasure_inverse {
	struct v92_residual source;
	double lower[V92_RESIDUAL_LAGS], upper[V92_RESIDUAL_LAGS];
	uint64_t received, missing;
	int polarity;
	bool ready, rejoined;
};

struct v92_erasure_interval {
	double lower, upper, point;
	int16_t rounded;
	bool received, unique, rejoined;
};

struct v92_erasure_slot {
	uint64_t frame;
	bool occupied, safe;
};

struct v92_erasure_window {
	struct v92_erasure_slot slot[V92_ERASURE_HISTORY];
	uint64_t call, epoch, next_frame, episode_start;
	unsigned episodes, reseeds, held;
	bool initialized, pending, dirty, active, fallback;
};

/* An exclusive call owner, outside replaceable mapping state. Reset invalidates
 * predictor and trust together, retaining the call-wide recovery limits.
 * Bounds certify the fixed native inverse, not uncertainty in its fitted
 * channel parameters. Physical quality checks and framed error control remain
 * required; raw or acquiring receivers cannot admit this recovery. */
struct v92_erasure {
	struct v92_erasure_inverse inverse;
	struct v92_erasure_window trust;
	uint64_t input, call, epoch, frames;
	bool usable[V92_PCM_MAPPING_FRAME_SYMBOLS];
	bool initialized, bounded, release, erased, disabled;
	uint8_t symbols;
	unsigned missing, uncertain, rejoins, epoch_missing;
};

struct v92_upstream_data_rx;

void v92_erasure_reset(struct v92_erasure *owner);
bool v92_erasure_is_valid(const struct v92_erasure *owner);
bool v92_erasure_is_stable(const struct v92_erasure *owner);
bool v92_erasure_bind(struct v92_upstream_data_rx *receiver,
	struct v92_erasure *owner, uint64_t call, uint64_t epoch);

/* Receiver helpers operate on staged state; the receiver commits both owners
 * only after the complete sample/frame transaction succeeds. */
bool v92_erasure_prime(struct v92_erasure *next, const struct v92_erasure *old);
bool v92_erasure_correct(struct v92_erasure *owner, struct v92_residual *residual,
	int polarity, int32_t raw, bool available, int32_t *correction, bool *usable);
bool v92_erasure_frame_begin(struct v92_erasure *owner,
	struct v92_pcm_frame_rx *decoder, bool full, bool *seed);
bool v92_erasure_frame_end(struct v92_erasure *owner,
	bool ready, bool ambiguous, bool *release);

#endif /* V92_ERASURE_H */
