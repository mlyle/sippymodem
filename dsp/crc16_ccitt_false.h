// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_CRC16_CCITT_FALSE_H
#define SIPPYMODEM_CRC16_CCITT_FALSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER = 0xffff,
};

enum sippymodem_crc16_ccitt_false_result {
	SIPPYMODEM_CRC16_CCITT_FALSE_OK,
	SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT,
};

uint16_t sippymodem_crc16_ccitt_false_step(
	uint16_t remainder, bool bit);

/* Bits are consumed in transmission order, most-significant bit first within
 * each octet. A zero-length input does not require a backing octet. */
enum sippymodem_crc16_ccitt_false_result
sippymodem_crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder);

const char *sippymodem_crc16_ccitt_false_result_name(
	enum sippymodem_crc16_ccitt_false_result result);

#endif
