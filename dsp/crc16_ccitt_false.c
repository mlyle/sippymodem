// SPDX-License-Identifier: BSD-3-Clause

#include "crc16_ccitt_false.h"

#include "packed_bits.h"

enum crc16_ccitt_false_result
crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder)
{
	uint16_t calculated =
		CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	if ((packed == NULL && bit_count != 0) || remainder == NULL)
		return CRC16_CCITT_FALSE_INVALID_ARGUMENT;
	for (size_t index = 0; index < bit_count; ++index) {
		bool bit = packed_msb_bit(packed, index) != 0;

		calculated = crc16_ccitt_false_step(
			calculated, bit);
	}
	*remainder = calculated;
	return CRC16_CCITT_FALSE_OK;
}

const char *crc16_ccitt_false_result_name(
	enum crc16_ccitt_false_result result)
{
	switch (result) {
	case CRC16_CCITT_FALSE_OK:
		return "ok";
	case CRC16_CCITT_FALSE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
