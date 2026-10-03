// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_FRAME_SOURCE_H
#define PCM_FRAME_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Supply one complete frame as one byte per bit, earliest bit first. Fill all
 * requested bits with zero or one; false rejects the frame. NULL emits idle
 * ones. The source owns external read side effects and must not modify or
 * reenter the transmitter while its frame is being fetched. */
typedef bool (*pcm_frame_source)(void *context, uint8_t *bits, size_t bit_count);

/* The caller provides bit_count bytes. Prefill missing source bits with an
 * invalid value so the frame encoder rejects incomplete input. */
static inline bool pcm_frame_source_read(
	pcm_frame_source source, void *context, uint8_t *bits, size_t bit_count)
{
	memset(bits, source == NULL ? 1 : 2, bit_count);
	return source == NULL || source(context, bits, bit_count);
}

#endif /* PCM_FRAME_SOURCE_H */
