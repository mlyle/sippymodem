// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_INFO1D_H
#define V92_INFO1D_H

#include "v34_info1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_INFO1D_PAYLOAD_BITS = V34_INFO1C_PAYLOAD_BITS,
	V92_INFO1D_PAYLOAD_BYTES = V34_INFO1C_PAYLOAD_BYTES,
};

enum v92_info1d_result {
	V92_INFO1D_OK,
	V92_INFO1D_INVALID_ARGUMENT,
	V92_INFO1D_INVALID_PARAMETERS,
	V92_INFO1D_INVALID_PAYLOAD,
};

/* V.92 retains the V.34 INFO1c layout but assigns the otherwise-unused
 * 3429-symbol/s carrier-selection bit to PCM-upstream support. The embedded
 * V.34 record always keeps that carrier bit clear so the capability has one
 * typed owner. */
struct v92_info1d {
	struct v34_info1c v34;
	bool pcm_upstream_supported;
};

bool v92_info1d_is_valid(const struct v92_info1d *info1d);

enum v92_info1d_result v92_info1d_encode(
	const struct v92_info1d *info1d, uint8_t *payload,
	size_t payload_capacity);
enum v92_info1d_result v92_info1d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info1d *info1d);

#endif /* V92_INFO1D_H */
