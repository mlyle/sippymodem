// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_STATUS_UPDATE_H
#define V92_STATUS_UPDATE_H

#include <stddef.h>
#include <stdint.h>

enum {
	V92_STATUS_UPDATE_PAYLOAD_BITS = 15,
	V92_STATUS_UPDATE_INFORMATION_BITS = 34,
	V92_STATUS_UPDATE_TRAILER_BITS = 18,
	V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS = 9,
	V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS = 30,
	V92_STATUS_UPDATE_DOWNSTREAM_DATA_MAX_BITS = 42,
	V92_STATUS_UPDATE_4_POINT_ALIGNMENT = 24,
	V92_STATUS_UPDATE_8_POINT_ALIGNMENT = 36,
	/* Data-mode SUVd can fill two 42-bit frames during parameter exchange. */
	V92_STATUS_UPDATE_MAX_FRAME_BITS = 84,
	V92_STATUS_UPDATE_MAX_FRAME_BYTES =
		(V92_STATUS_UPDATE_MAX_FRAME_BITS + 7) / 8,
};

enum v92_status_update_result {
	V92_STATUS_UPDATE_OK,
	V92_STATUS_UPDATE_INVALID_ARGUMENT,
	V92_STATUS_UPDATE_INVALID_ALIGNMENT,
	V92_STATUS_UPDATE_OUTPUT_TOO_SMALL,
	V92_STATUS_UPDATE_INVALID_FRAME,
	V92_STATUS_UPDATE_CRC_MISMATCH,
};

/* Return the exact wire extent for a legal six-symbol downstream PCM mapping
 * frame, or one upstream 12-symbol 4/8-point frame. */
size_t v92_status_update_frame_bits(unsigned int alignment_bits);
size_t v92_status_update_frame_bytes(unsigned int alignment_bits);

/* payload bit zero occupies wire bit 19; wire bit 18's status-update marker
 * is fixed at one. Rejected calls leave output unchanged. */
enum v92_status_update_result v92_status_update_encode(
	uint16_t payload, unsigned int alignment_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* Reserved payload fields are covered by the CRC but interpreted by the
 * directional caller. Fixed, fill, and storage-padding bits are strict.
 * Rejected calls leave payload unchanged. */
enum v92_status_update_result v92_status_update_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int alignment_bits,
	uint16_t *payload);

#endif /* V92_STATUS_UPDATE_H */
