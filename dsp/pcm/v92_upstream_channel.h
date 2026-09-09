// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_CHANNEL_H
#define V92_UPSTREAM_CHANNEL_H

#include "v92_pcm_phase4_trn2u.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* A p1/z2 realization with 24 precursor and 90 postcursor lags,
	 * within the 192-coefficient capability of the initial filter profile. */
	V92_UPSTREAM_CHANNEL_Z2_TAPS = 85,
	V92_UPSTREAM_CHANNEL_P1_TAPS = 90,
	V92_UPSTREAM_CHANNEL_DECISION_DELAY = 60,
	V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS =
		V92_UPSTREAM_CHANNEL_Z2_TAPS - 1 - V92_UPSTREAM_CHANNEL_DECISION_DELAY,
	V92_UPSTREAM_CHANNEL_MAX_LAG =
		V92_UPSTREAM_CHANNEL_DECISION_DELAY + V92_UPSTREAM_CHANNEL_P1_TAPS,
	V92_UPSTREAM_CHANNEL_CROSS_LAGS =
		V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + V92_UPSTREAM_CHANNEL_MAX_LAG + 1,
	V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS = 512,
	V92_UPSTREAM_CHANNEL_SYMBOLS = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
};

/* Zero initialization starts one bounded TRN2u measurement. Observations are
 * polarity-normalized raw PCM, aligned with their known training targets,
 * not the equalizer's output or nearest-point decisions. Retain the delayed
 * training tail across the physical control handoff. Freeze the completed
 * measurement so control/data cannot train a different channel accidentally.
 * Signed products fit int64_t even for full-scale input throughout the span.
 * Cross-correlation index PRECURSOR_LAGS+k accumulates observed(n)*expected(n-k)
 * for k>=0, or expected(n)*observed(n+k) for k<0, over the retained window. */
struct v92_upstream_channel {
	int64_t observed_autocorrelation[V92_UPSTREAM_CHANNEL_Z2_TAPS];
	int64_t expected_autocorrelation[V92_UPSTREAM_CHANNEL_P1_TAPS + 1];
	int64_t cross_correlation[V92_UPSTREAM_CHANNEL_CROSS_LAGS];
	int32_t observed_history[V92_UPSTREAM_CHANNEL_MAX_LAG + 1];
	int16_t expected_history[V92_UPSTREAM_CHANNEL_MAX_LAG + 1];
	uint16_t symbols;
};

bool v92_upstream_channel_is_valid(const struct v92_upstream_channel *channel);

/* Polarity reversal of -32768 can produce +32768, hence the wider observed
 * argument. Invalid calls change nothing. Completed measurements are stable. */
bool v92_upstream_channel_sample(
	struct v92_upstream_channel *channel, int16_t expected, int32_t observed);

#endif /* V92_UPSTREAM_CHANNEL_H */
