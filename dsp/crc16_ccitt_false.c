// SPDX-License-Identifier: BSD-3-Clause

#include "crc16_ccitt_false.h"

#include "packed_bits.h"

enum crc16_ccitt_false_result
crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder)
{
	if ((packed == NULL && bit_count != 0) || remainder == NULL)
		return CRC16_CCITT_FALSE_INVALID_ARGUMENT;
	*remainder = crc16_ccitt_false_packed_range(packed, 0, bit_count);
	return CRC16_CCITT_FALSE_OK;
}
