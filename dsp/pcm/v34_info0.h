// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_INFO0_H
#define V34_INFO0_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_INFO0_PAYLOAD_BITS = 17,
	V34_INFO0_PAYLOAD_BYTES =
		(V34_INFO0_PAYLOAD_BITS + 7) / 8,
	V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS = 5,
};

enum v34_info0_clock_source {
	V34_INFO0_CLOCK_INTERNAL,
	V34_INFO0_CLOCK_RECEIVE_TIMING,
	V34_INFO0_CLOCK_EXTERNAL,
};

enum v34_info0_result {
	V34_INFO0_OK,
	V34_INFO0_INVALID_ARGUMENT,
	V34_INFO0_INVALID_CAPABILITIES,
	V34_INFO0_INVALID_PAYLOAD,
};

/* These fields are V.34 INFO0 bits 12 through 28. Framing owns the preceding
 * fill/sync bits and the following CRC and end marker. */
struct v34_info0 {
	bool symbol_rate_2743_supported;
	bool symbol_rate_2800_supported;
	bool symbol_rate_3429_supported;
	bool low_carrier_3000_supported;
	bool high_carrier_3000_supported;
	bool low_carrier_3200_supported;
	bool high_carrier_3200_supported;
	bool symbol_rate_3429_transmit_permitted;
	bool transmit_power_reduction_supported;
	uint8_t maximum_symbol_rate_difference_steps;
	bool cme_modem;
	bool constellation_1664_supported;
	enum v34_info0_clock_source transmit_clock_source;
	bool acknowledges_peer_info0;
};

bool v34_info0_is_valid(
	const struct v34_info0 *info0);

/* Compare validated encoded fields, including clock source and acknowledgment,
 * without inspecting padding. Invalid records never compare equal. */
bool v34_info0_equal(const struct v34_info0 *left, const struct v34_info0 *right);

/* payload_capacity is measured in octets. Encoded fields are packed in
 * transmission order, most-significant bit first in each octet. */
enum v34_info0_result v34_info0_encode(
	const struct v34_info0 *info0, uint8_t *payload,
	size_t payload_capacity);
enum v34_info0_result v34_info0_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info0 *info0);

#endif
