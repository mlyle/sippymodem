// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONSTELLATION_H
#define PCM_CONSTELLATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PCM_CONSTELLATION_UCODES = 128,
	PCM_CONSTELLATION_MASK_BYTES = PCM_CONSTELLATION_UCODES / 8,
};

/* Bit n represents companded codeword n. Keeping the set packed avoids a
 * 128-entry level table while preserving the normative Ucode ordering. */
struct pcm_constellation {
	uint8_t ucode_mask[PCM_CONSTELLATION_MASK_BYTES];
};

static inline bool pcm_constellation_contains(
	const struct pcm_constellation *constellation, uint8_t ucode)
{
	return constellation != NULL && ucode < PCM_CONSTELLATION_UCODES &&
	       (constellation->ucode_mask[ucode / 8u] &
		(uint8_t)(1u << (ucode % 8u))) != 0;
}

#endif /* PCM_CONSTELLATION_H */
