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

/* Own the continuous, trained PCM-upstream equalizer and control demodulator
 * through Phase 4 control and E2u. The short inherited training tail
 * is adapted against its known targets; later decisions adapt against the
 * nearest selected control-constellation level. B1u/data inherits the line
 * through v92_upstream_data_rx and uses its negotiated CPd constellation. */
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

/* Predict the equalizer-coordinate level for a known next input group without
 * advancing either coding history. This is the reference used by E2u. */
bool v92_upstream_rx_expected_level(
	const struct v92_upstream_rx *receiver, uint8_t input_group,
	int16_t *level);

/* Consume one signed 8 kHz sample and return one earliest-bit-first group.
 * Invalid and terminal calls leave receiver and decision unchanged. A new
 * DSP failure consumes no sample, latches failure, and leaves decision
 * unchanged. */
enum v92_upstream_rx_result v92_upstream_rx_sample(
	struct v92_upstream_rx *receiver, int16_t sample,
	struct v92_upstream_decision *decision);

/* Consume one sample while adapting a post-training decision against a known
 * input group rather than a possibly damaged nearest decision. */
enum v92_upstream_rx_result v92_upstream_rx_sample_known(
	struct v92_upstream_rx *receiver, int16_t sample,
	uint8_t expected_group, struct v92_upstream_decision *decision);

const char *v92_upstream_rx_result_name(enum v92_upstream_rx_result result);

#endif /* V92_UPSTREAM_RX_H */
