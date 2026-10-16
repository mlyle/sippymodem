// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_CHANNEL_H
#define V92_CPD_CHANNEL_H

#include "v92_upstream_channel.h"
#include "v92_upstream_plan.h"

/* Refine a complete initial direct CPd for the measured channel. Solve the
 * regularized joint p1/z2 channel inverse, quantize it to the wire formats,
 * spread dense constellation points, balance higher-rate moduli across trellis
 * groups, and normalize actual filtered B1u power. Rate and trellis remain
 * fixed. The matching LOCAL receive
 * plan carries gain and delay separately. A complete 12000-symbol measurement
 * and a positive trained LU are required. Rejected calls change nothing;
 * parameters, channel and plan storage must not overlap. */
bool v92_cpd_channel_design(struct v92_cpd_parameters *parameters,
	const struct v92_upstream_channel *channel, enum g711_law law,
	int16_t upstream_level, struct v92_upstream_plan *plan);

/* Choose the highest enabled rate at or below parameters->data_rate_number.
 * For a general inverse, use its residual and actual B1u point energy
 * to estimate a three-sigma margin to each nearest decision boundary.
 * Retain the highest rate when explicit codec modelling is available.
 * This local policy does not replace independent B1u validation. Reuse
 * one channel solve across all candidates. Rejection changes no outputs. */
bool v92_cpd_channel_select(struct v92_cpd_parameters *parameters,
	const struct v92_upstream_channel *channel, enum g711_law law,
	int16_t upstream_level, struct v92_upstream_plan *plan, uint32_t rate_mask);

#endif /* V92_CPD_CHANNEL_H */
