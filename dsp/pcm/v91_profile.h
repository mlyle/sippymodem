// SPDX-License-Identifier: BSD-3-Clause
#ifndef V91_PROFILE_H
#define V91_PROFILE_H

#include "pcm_octet_probe.h"
#include "v90_dil.h"
#include "v90_pcm_mapping.h"

/* Cover every octet and both signs through a complete 24-sample robbed-bit
 * cycle. The descriptor and generator are shared with ordinary PCM DIL. */
bool v91_profile_dil(struct v90_dil_parameters *parameters);

/* Infer the highest supported frame rate from the measured receive sets.
 * maximum_power bounds mean-square transmit power in linear PCM units;
 * UINT32_MAX means unspecified. Reduce the highest levels until the exact
 * shared mapper power bound fits. Rejected calls leave outputs unchanged. */
bool v91_profile_learn(const struct pcm_octet_probe *probe, enum g711_law law,
	uint32_t maximum_power, struct v90_pcm_mapping *mapping,
	struct pcm_octet_channel *channel);

#endif /* V91_PROFILE_H */
