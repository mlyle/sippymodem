// SPDX-License-Identifier: BSD-3-Clause

#ifndef CRC16_CCITT_FALSE_H
#define CRC16_CCITT_FALSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	CRC16_CCITT_FALSE_INITIAL_REMAINDER = 0xffff,
};

enum crc16_ccitt_false_result {
	CRC16_CCITT_FALSE_OK,
	CRC16_CCITT_FALSE_INVALID_ARGUMENT,
};

static inline uint16_t crc16_ccitt_false_step(
	uint16_t remainder, bool bit)
{
	uint16_t next = (uint16_t)(remainder << 1);

	if ((((remainder >> 15) ^ (uint16_t)bit) & 1u) != 0)
		next ^= 0x1021;
	return next;
}

/* Bits are consumed in transmission order, most-significant bit first within
 * each octet. A zero-length input does not require a backing octet. */
enum crc16_ccitt_false_result
crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder);

const char *crc16_ccitt_false_result_name(
	enum crc16_ccitt_false_result result);

#endif
