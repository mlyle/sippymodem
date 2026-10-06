// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_MEDIA_H
#define DATAPUMP_MEDIA_H

#include "modem.h"
#include "g711.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* An invalid callback may still have a bounded writable output fragment.
 * Silence it so a failed datapump cannot transmit stale samples. */
static inline bool datapump_linear_fragment_is_ready(
	const void *owner, const int16_t *receive, int16_t *transmit,
	int count)
{
	bool bounded = count > 0 && count <= MODEM_FRAG;

	if (owner != NULL && receive != NULL && transmit != NULL && bounded)
		return true;
	if (transmit != NULL && bounded)
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
	return false;
}

/* Canonical codec silence and its matching linear samples. Invalid
 * callbacks may still own bounded writable output; preserve other storage. */
static inline void datapump_mute_g711(enum g711_law law,
	uint8_t *octets, int16_t *samples, int count)
{
	if (count <= 0 || count > MODEM_FRAG)
		return;
	uint8_t zero = g711_encode(law, 0);
	int16_t linear = g711_decode(law, zero);

	if (octets != NULL)
		memset(octets, zero, (size_t)count);
	if (samples != NULL)
		for (int i = 0; i < count; ++i)
			samples[i] = linear;
}

#endif
