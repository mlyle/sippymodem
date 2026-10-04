// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_OCTET_PROBE_H
#define PCM_OCTET_PROBE_H

#include "g711.h"
#include "pcm_constellation.h"

/* Empirical digital impairment learning. Each transmitted octet records its
 * observed receive set at each six-symbol frame position. No particular pad,
 * coding-law conversion, robbed-bit pattern, or polarity is assumed. */
struct pcm_octet_probe {
	uint8_t outputs[6][256][32];
	uint8_t observations[6][256];
};

struct pcm_octet_channel {
	uint8_t decoded[6][256];
	uint8_t known[6][32];
};

void pcm_octet_probe_init(struct pcm_octet_probe *probe);
bool pcm_octet_probe_observe(struct pcm_octet_probe *probe, unsigned interval,
	uint8_t expected, uint8_t received);
/* Select low-power codewords whose signed receive sets are disjoint. Each
 * sign needs repeated evidence. Unobserved codes are never selected. Masks
 * use the transmitter's Ucode ordering; decoded octets use its coding law.
 * The caller selects a rate from the product of the six set sizes. */
bool pcm_octet_probe_finish(const struct pcm_octet_probe *probe, enum g711_law law,
	struct pcm_constellation sets[6], struct pcm_octet_channel *channel);
bool pcm_octet_channel_normalize(const struct pcm_octet_channel *channel,
	unsigned interval, uint8_t received, uint8_t *normalized);
#endif /* PCM_OCTET_PROBE_H */
