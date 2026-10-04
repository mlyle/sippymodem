// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_INFO0_H
#define V92_INFO0_H

#include "v90_info0.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_info0_result {
	V92_INFO0_OK,
	V92_INFO0_INVALID_ARGUMENT,
	V92_INFO0_INVALID_CAPABILITIES,
	V92_INFO0_INVALID_PAYLOAD,
};

/* V.92 retains the V.90 INFO0 records and assigns their two reserved bits to
 * V.92 capability and the short-Phase-2 request. The role reverses those two
 * bit positions. A decoded record may deliberately report no V.92 capability
 * so the session controller can fall back to V.90. */
struct v92_info0a {
	struct v34_info0 v90;
	bool v92_capable;
	bool short_phase2_requested;
};

struct v92_info0d {
	struct v90_info0d v90;
	bool v92_capable;
	bool short_phase2_requested;
};

bool v92_info0a_is_valid(const struct v92_info0a *info0a);
bool v92_info0d_is_valid(const struct v92_info0d *info0d);

enum v92_info0_result v92_info0a_encode(
	const struct v92_info0a *info0a, uint8_t *payload,
	size_t payload_capacity);
enum v92_info0_result v92_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info0a *info0a);
enum v92_info0_result v92_info0d_encode(
	const struct v92_info0d *info0d, uint8_t *payload,
	size_t payload_capacity);
enum v92_info0_result v92_info0d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info0d *info0d);

const char *v92_info0_result_name(enum v92_info0_result result);

#endif /* V92_INFO0_H */
