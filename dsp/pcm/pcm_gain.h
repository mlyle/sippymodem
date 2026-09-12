// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_GAIN_H
#define PCM_GAIN_H

#include <stdbool.h>
#include <stdint.h>

/* Per-stream outer bounds for one positive host gain followed by rounding
 * and signed-16 clipping. These describe corresponding decoded wire levels
 * and host samples, not the unknown analogue gain before the wire codec.
 * Include half-way ties conservatively: containment alone is not evidence
 * that this model reproduces every sample or that a channel is usable. */
struct pcm_gain {
	double lower, upper;
	double correlation, energy;
};

void pcm_gain_init(struct pcm_gain *gain);
bool pcm_gain_is_valid(const struct pcm_gain *gain);

/* Pass actually received, aligned pairs before any polarity correction or
 * equalization. Skip erasures, which carry no measured codec level. A zero
 * wire level is valid only with a zero host sample and adds no information.
 * Rejected input leaves the accumulated state unchanged. */
bool pcm_gain_observe(struct pcm_gain *gain, int16_t wire, int16_t sample);

/* Clamp the uncensored least-squares estimate to the surviving interval.
 * No finite upper bound or no nonzero correlation leaves output unchanged.
 * The result is a point in the interval, not proof of an identified gain.
 * Output must not overlap the accumulated state; estimation never changes
 * that state. The owner decides when training ends and freezes the result. */
bool pcm_gain_estimate(const struct pcm_gain *gain, double *estimate);

#endif /* PCM_GAIN_H */
