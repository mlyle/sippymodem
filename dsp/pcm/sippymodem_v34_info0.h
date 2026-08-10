// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_INFO0_H
#define SIPPYMODEM_V34_INFO0_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_INFO0_PAYLOAD_BITS = 17,
	SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES =
		(SIPPYMODEM_V34_INFO0_PAYLOAD_BITS + 7) / 8,
	SIPPYMODEM_V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS = 5,
};

enum sippymodem_v34_info0_clock_source {
	SIPPYMODEM_V34_INFO0_CLOCK_INTERNAL,
	SIPPYMODEM_V34_INFO0_CLOCK_RECEIVE_TIMING,
	SIPPYMODEM_V34_INFO0_CLOCK_EXTERNAL,
};

enum sippymodem_v34_info0_result {
	SIPPYMODEM_V34_INFO0_OK,
	SIPPYMODEM_V34_INFO0_INVALID_ARGUMENT,
	SIPPYMODEM_V34_INFO0_INVALID_CAPABILITIES,
	SIPPYMODEM_V34_INFO0_INVALID_PAYLOAD,
};

/* These fields are V.34 INFO0 bits 12 through 28. Framing owns the preceding
 * fill/sync bits and the following CRC and end marker. */
struct sippymodem_v34_info0 {
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
	enum sippymodem_v34_info0_clock_source transmit_clock_source;
	bool acknowledges_peer_info0;
};

bool sippymodem_v34_info0_is_valid(
	const struct sippymodem_v34_info0 *info0);

/* payload_capacity is measured in octets. Encoded fields are packed in
 * transmission order, most-significant bit first in each octet. */
enum sippymodem_v34_info0_result sippymodem_v34_info0_encode(
	const struct sippymodem_v34_info0 *info0, uint8_t *payload,
	size_t payload_capacity);
enum sippymodem_v34_info0_result sippymodem_v34_info0_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct sippymodem_v34_info0 *info0);

const char *sippymodem_v34_info0_result_name(
	enum sippymodem_v34_info0_result result);

#endif
