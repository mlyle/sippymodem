// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_J_FRAME_H
#define PCM_J_FRAME_H

#include <stddef.h>
#include <stdint.h>

enum {
	PCM_J_FRAME_SYNC_BITS = 17,
	PCM_J_FRAME_PAYLOAD_BITS = 32,
	PCM_J_FRAME_BITS = 72,
	PCM_J_FRAME_BYTES = PCM_J_FRAME_BITS / 8,
};

enum pcm_j_frame_result {
	PCM_J_FRAME_OK,
	PCM_J_FRAME_INVALID_ARGUMENT,
	PCM_J_FRAME_INVALID_FRAME,
	PCM_J_FRAME_CRC_MISMATCH,
};

/* V.90 Jd and V.92 Jd/Jp share this wire envelope. Payload bit zero is the
 * first information bit transmitted. The envelope inserts start bits after
 * each 16 payload bits, sends the CRC most-significant bit first, and packs
 * the resulting wire bits most-significant bit first in each octet. */
enum pcm_j_frame_result pcm_j_frame_encode(
	uint32_t payload, uint8_t *frame, size_t frame_capacity);

/* Decode only the common envelope. Protocol-specific reserved bits and field
 * meanings remain the caller's responsibility. Rejected frames leave the
 * destination unchanged. */
enum pcm_j_frame_result pcm_j_frame_decode(
	const uint8_t *frame, size_t frame_capacity, uint32_t *payload);

#endif /* PCM_J_FRAME_H */
