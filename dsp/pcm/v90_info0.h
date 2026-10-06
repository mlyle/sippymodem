// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_INFO0_H
#define V90_INFO0_H

#include "v34_info0.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_INFO0A_PAYLOAD_BITS = 17,
	V90_INFO0D_PAYLOAD_BITS = 30,
	V90_INFO0A_PAYLOAD_BYTES =
		(V90_INFO0A_PAYLOAD_BITS + 7) / 8,
	V90_INFO0D_PAYLOAD_BYTES =
		(V90_INFO0D_PAYLOAD_BITS + 7) / 8,
	V90_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS =
		V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS,
};

enum v90_pcm_law {
	V90_PCM_MU_LAW,
	V90_PCM_A_LAW,
};

enum v90_info0_result {
	V90_INFO0_OK,
	V90_INFO0_INVALID_ARGUMENT,
	V90_INFO0_INVALID_CAPABILITIES,
	V90_INFO0_INVALID_PAYLOAD,
};

/* INFO0a and INFO0d carry the V.34 capabilities in wire bits 12:28.
 * V.90 ignores transmit_clock_source: bits 26:27 are emitted as zero and
 * ignored on reception. V.92 interprets those bits in its own record codec. */

struct v90_info0d {
	struct v34_info0 common;
	/* Codes defined by V.90 Table 7, not absolute dBm0 values. */
	uint8_t nominal_transmit_power;
	uint8_t maximum_transmit_power;
	bool measure_power_at_codec_output;
	enum v90_pcm_law pcm_law;
	bool upstream_symbol_rate_3429_supported;
};

bool v90_info0_is_valid(const struct v34_info0 *info0);
bool v90_info0d_is_valid(const struct v90_info0d *info0d);

/* Compare validated encoded fields, including acknowledgment. Clock source is
 * reserved in V.90 and ignored, along with padding. Invalid records differ. */
bool v90_info0a_equal(const struct v34_info0 *left, const struct v34_info0 *right);
bool v90_info0d_equal(const struct v90_info0d *left, const struct v90_info0d *right);

enum v90_info0_result v90_info0a_encode(
	const struct v34_info0 *info0a, uint8_t *payload,
	size_t payload_capacity);
enum v90_info0_result v90_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info0 *info0a);
enum v90_info0_result v90_info0d_encode(
	const struct v90_info0d *info0d, uint8_t *payload,
	size_t payload_capacity);
enum v90_info0_result v90_info0d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info0d *info0d);

const char *v90_info0_result_name(enum v90_info0_result result);

#endif
