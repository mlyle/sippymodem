// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_SURVIVOR_H
#define V92_SURVIVOR_H
#include "v92_pcm_mapping.h"
#include "v34_trellis.h"
#include <stdbool.h>
#include <stdint.h>
enum {
	/* 120 symbols / 15 ms of delayed source decisions. */
	V92_PCM_RX_DELAY_FRAMES = 10,
	/* Qualify 240T inside the 384T request, before its 24T prime tail. */
	V92_PCM_RM_QUALIFY_FRAMES = 20,
	V92_SURVIVOR_STATES = 64,
	V92_SURVIVOR_BRANCHES = 32,
	V92_SURVIVOR_MARGINS = V92_PCM_RX_DELAY_FRAMES + 3,
	V92_SURVIVOR_POINTS = 2 * V92_CPD_MAX_CONSTELLATION_POINTS
};
enum v92_pcm_frame_rm {
	V92_PCM_FRAME_RM_NONE,
	V92_PCM_FRAME_RM_SIGNAL,
	V92_PCM_FRAME_RM_PRIME,
	V92_PCM_FRAME_RM_COMPLETE
};
struct v92_survivor_word {
	uint64_t lo, hi;
};
struct v92_survivor_context {
	struct v92_survivor_word pending[V92_PCM_RX_DELAY_FRAMES], encoded;
	uint64_t margin[V92_SURVIVOR_MARGINS];
	uint32_t gpa;
	uint8_t sign, rm_frames, rm_prime_frames;
	uint16_t code_mismatches, nondata;
	int16_t eta[12];
};
/* Transition/geometry caches are frozen at initialization. Mutable coding
 * histories and delayed evidence belong exclusively to this receiver. */
struct v92_survivor {
	struct v92_survivor_context context[V92_SURVIVOR_STATES];
	uint64_t metric[V92_SURVIVOR_STATES];
	uint8_t classes[256], next[V92_SURVIVOR_STATES][V92_SURVIVOR_BRANCHES];
	uint8_t bits[12], offset[12], count[12];
	enum v34_trellis_selection selection;
	uint8_t states, branches, fill, data_bits;
	struct v92_survivor_word rm_word, prime_word;
};
struct v92_survivor_output {
	struct v92_survivor_word word;
	/* Gaps retain the supplied point-cost units; UINT64_MAX means no alternative. */
	uint64_t margin, uncertainty_margin, cost, frame_cost;
	int16_t eta[12];
	bool ready, code_mismatch;
	enum v92_pcm_frame_rm rm;
};
/* Full-capacity power-of-two mappings only. Start in physical state zero;
 * the frame owner explicitly imports alternate coding hypotheses as needed.
 * Invalid parameters preserve the destination. No pointers are retained. */
bool v92_survivor_init(struct v92_survivor *, const struct v92_pcm_mapping *);
bool v92_survivor_is_valid(const struct v92_survivor *);
/* Validated owned epoch and disjoint arguments are required. Expected is an
 * optional zero-GPA B1 coded word, used only to report evidence, never rank.
 * Failure preserves state/output; scratch is reusable caller-owned storage. */
bool v92_survivor_advance_owned(struct v92_survivor *,
				const uint64_t costs[12][V92_SURVIVOR_POINTS], bool b1u,
				const struct v92_survivor_word *expected,
				struct v92_survivor_output *, struct v92_survivor *scratch);
/* Preserve metrics/physical state and delayed payload/nondata/evidence. */
void v92_survivor_reset_control_owned(struct v92_survivor *);
#endif
