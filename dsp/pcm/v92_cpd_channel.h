// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_CHANNEL_H
#define V92_CPD_CHANNEL_H

#include "v92_upstream_channel.h"
#include "v92_upstream_plan.h"

/* Replace only filters and gain in a complete initial CPd. Solve the
 * regularized joint p1/z2 channel inverse, quantize it to the wire formats,
 * and normalize actual filtered B1u power. The matching LOCAL receive plan
 * carries gain and delay separately. A complete 12000-symbol measurement
 * and a positive trained LU are required. Rejected calls change nothing;
 * parameters, channel and plan storage must not overlap. */
bool v92_cpd_channel_design(struct v92_cpd_parameters *parameters,
	const struct v92_upstream_channel *channel, enum g711_law law,
	int16_t upstream_level, struct v92_upstream_plan *plan);

#endif /* V92_CPD_CHANNEL_H */
