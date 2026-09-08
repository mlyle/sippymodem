// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SUVD_H
#define V92_SUVD_H

#include "v92_status_update.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_SUVD_INFORMATION_BITS = V92_STATUS_UPDATE_INFORMATION_BITS,
	V92_SUVD_TRAILER_BITS = V92_STATUS_UPDATE_TRAILER_BITS,
	V92_SUVD_SYMBOLS_PER_FRAME = 6,
	V92_SUVD_MAX_FRAME_BITS = V92_STATUS_UPDATE_MAX_FRAME_BITS,
	V92_SUVD_MAX_FRAME_BYTES = V92_STATUS_UPDATE_MAX_FRAME_BYTES,
};

enum v92_suvd_result {
	V92_SUVD_OK,
	V92_SUVD_INVALID_ARGUMENT,
	V92_SUVD_OUTPUT_TOO_SMALL,
	V92_SUVD_INVALID_FRAME,
	V92_SUVD_CRC_MISMATCH,
};

struct v92_suvd_parameters {
	bool request_silence;
	bool acknowledges_cpu;
};

bool v92_suvd_parameters_are_valid(
	const struct v92_suvd_parameters *parameters);

/* Downstream records end on a complete six-symbol PCM mapping frame. Use the
 * negotiated bit width: D during training, K during fast parameter exchange,
 * not the number of carrier symbols. */
size_t v92_suvd_frame_bits(uint8_t mapping_frame_bits);
size_t v92_suvd_frame_bytes(uint8_t mapping_frame_bits);

/* Pack the Table 31 record in transmission order. Rejected calls leave every
 * output unchanged. */
enum v92_suvd_result v92_suvd_encode(
	const struct v92_suvd_parameters *parameters,
	uint8_t mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* Reserved information bits are CRC-covered but otherwise ignored. Fixed,
 * fill, and storage-padding bits are strict. Rejected calls leave parameters
 * unchanged. */
enum v92_suvd_result v92_suvd_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, uint8_t mapping_frame_bits,
	struct v92_suvd_parameters *parameters);

const char *v92_suvd_result_name(enum v92_suvd_result result);

#endif /* V92_SUVD_H */
