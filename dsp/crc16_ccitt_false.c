// SPDX-License-Identifier: BSD-3-Clause

#include "crc16_ccitt_false.h"

enum {
	CRC16_CCITT_FALSE_POLYNOMIAL = 0x1021,
};

uint16_t crc16_ccitt_false_step(
	uint16_t remainder, bool bit)
{
	uint16_t next = (uint16_t)(remainder << 1);

	if ((((remainder >> 15) ^ (uint16_t)bit) & 1u) != 0)
		next ^= CRC16_CCITT_FALSE_POLYNOMIAL;
	return next;
}

enum crc16_ccitt_false_result
crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder)
{
	uint16_t calculated =
		CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	if ((packed == NULL && bit_count != 0) || remainder == NULL)
		return CRC16_CCITT_FALSE_INVALID_ARGUMENT;
	for (size_t index = 0; index < bit_count; ++index) {
		bool bit = ((packed[index / 8u] >>
			      (7u - index % 8u)) &
			     1u) != 0;

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
