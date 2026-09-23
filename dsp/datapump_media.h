// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_MEDIA_H
#define DATAPUMP_MEDIA_H

#include "modem.h"

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

#endif
