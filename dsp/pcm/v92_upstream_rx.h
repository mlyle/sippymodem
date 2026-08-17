// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_RX_H
#define V92_UPSTREAM_RX_H

#include "v92_pcm_phase4_trn2u_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_upstream_rx_result {
	V92_UPSTREAM_RX_OK,
	V92_UPSTREAM_RX_FAILED,
	V92_UPSTREAM_RX_INVALID_ARGUMENT,
	V92_UPSTREAM_RX_INVALID_STATE,
	V92_UPSTREAM_RX_NUMERIC_FAILURE,
};

struct v92_upstream_decision {
	double equalized_level;
	int16_t nearest_level;
	uint8_t group;
};

/* Own the continuous, trained PCM-upstream equalizer and demodulator shared
 * by Phase 4 control, E2u, B1u, and data. The short inherited training tail
 * is adapted against its known targets; later decisions adapt against the
 * nearest selected constellation level. */
struct v92_upstream_rx {
	struct v92_pcm_phase4_trn2u_line_handoff line;
	uint8_t training_tail_remaining;
	bool failed;
};

enum v92_upstream_rx_result v92_upstream_rx_init(
	struct v92_upstream_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff);
bool v92_upstream_rx_is_valid(const struct v92_upstream_rx *receiver);
bool v92_upstream_rx_has_failed(const struct v92_upstream_rx *receiver);

/* Consume one signed 8 kHz sample and return one earliest-bit-first group.
 * Invalid and terminal calls leave receiver and decision unchanged. A new
 * DSP failure consumes no sample, latches failure, and leaves decision
 * unchanged. */
enum v92_upstream_rx_result v92_upstream_rx_sample(
	struct v92_upstream_rx *receiver, int16_t sample,
	struct v92_upstream_decision *decision);

const char *v92_upstream_rx_result_name(enum v92_upstream_rx_result result);

#endif /* V92_UPSTREAM_RX_H */
