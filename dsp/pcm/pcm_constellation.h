// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONSTELLATION_H
#define PCM_CONSTELLATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PCM_CONSTELLATION_UCODES = 128,
	PCM_CONSTELLATION_MASK_BYTES = PCM_CONSTELLATION_UCODES / 8,
	PCM_CONSTELLATION_FRAME_INTERVALS = 6,
	PCM_CONSTELLATION_MAX_ROWS = 6,
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

/* Count packed Ucodes without a data-dependent bit loop or a lookup table.
 * Sum adjacent bit pairs, then nibbles, then each byte; the total is at most
 * 128 and therefore fits the mapping's one-byte modulus. */
static inline uint8_t pcm_constellation_count(
	const struct pcm_constellation *constellation)
{
	unsigned int count = 0;

	if (constellation == NULL)
		return 0;
	for (size_t index = 0; index < PCM_CONSTELLATION_MASK_BYTES; ++index) {
		uint8_t bits = constellation->ucode_mask[index];

		bits = (uint8_t)((bits & 0x55u) + ((bits >> 1) & 0x55u));
		bits = (uint8_t)((bits & 0x33u) + ((bits >> 2) & 0x33u));
		count += (bits + (bits >> 4)) & 0x0fu;
	}
	return (uint8_t)count;
}

static inline bool pcm_constellation_is_empty(
	const struct pcm_constellation *constellation)
{
	uint8_t used = 0;

	if (constellation == NULL)
		return false;
	for (size_t index = 0; index < PCM_CONSTELLATION_MASK_BYTES; ++index)
		used |= constellation->ucode_mask[index];
	return used == 0;
}

/* V.90 CP and V.92 CPt/CPu use the same six-interval layout. Every declared
 * row must be referenced and nonempty; unused rows, including the whole
 * optional codec plane when absent, must be empty. */
static inline bool pcm_constellations_are_canonical(
	const struct pcm_constellation transmit[PCM_CONSTELLATION_MAX_ROWS],
	const struct pcm_constellation codec[PCM_CONSTELLATION_MAX_ROWS],
	const uint8_t frame_constellation[PCM_CONSTELLATION_FRAME_INTERVALS],
	uint8_t count, bool codec_differs)
{
	uint8_t used = 0;

	if (transmit == NULL || codec == NULL || frame_constellation == NULL ||
	    count == 0 || count > PCM_CONSTELLATION_MAX_ROWS)
		return false;
	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		uint8_t index = frame_constellation[interval];

		if (index >= count)
			return false;
		used |= (uint8_t)(1u << index);
	}
	if (used != (uint8_t)((1u << count) - 1u))
		return false;
	for (size_t index = 0; index < PCM_CONSTELLATION_MAX_ROWS; ++index) {
		bool active = index < count;

		if (pcm_constellation_is_empty(&transmit[index]) == active ||
		    pcm_constellation_is_empty(&codec[index]) ==
			    (active && codec_differs))
			return false;
	}
	return true;
}

#endif /* PCM_CONSTELLATION_H */
