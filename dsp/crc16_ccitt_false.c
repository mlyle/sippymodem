// SPDX-License-Identifier: BSD-3-Clause

#include "crc16_ccitt_false.h"

enum {
	CRC16_CCITT_FALSE_POLYNOMIAL = 0x1021,
};

uint16_t sippymodem_crc16_ccitt_false_step(
	uint16_t remainder, bool bit)
{
	uint16_t next = (uint16_t)(remainder << 1);

	if ((((remainder >> 15) ^ (uint16_t)bit) & 1u) != 0)
		next ^= CRC16_CCITT_FALSE_POLYNOMIAL;
	return next;
}

enum sippymodem_crc16_ccitt_false_result
sippymodem_crc16_ccitt_false_packed_bits(
	const uint8_t *packed, size_t bit_count, uint16_t *remainder)
{
	uint16_t calculated =
		SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	if ((packed == NULL && bit_count != 0) || remainder == NULL)
		return SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT;
	for (size_t index = 0; index < bit_count; ++index) {
		bool bit = ((packed[index / 8u] >>
			      (7u - index % 8u)) &
			     1u) != 0;

		calculated = sippymodem_crc16_ccitt_false_step(
			calculated, bit);
	}
	*remainder = calculated;
	return SIPPYMODEM_CRC16_CCITT_FALSE_OK;
}

const char *sippymodem_crc16_ccitt_false_result_name(
	enum sippymodem_crc16_ccitt_false_result result)
{
	switch (result) {
	case SIPPYMODEM_CRC16_CCITT_FALSE_OK:
		return "ok";
	case SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
