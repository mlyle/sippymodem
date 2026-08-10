// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_crc16_ccitt_false.h>

#include "test.h"

#include <string.h>

static bool test_known_checks(void)
{
	static const uint8_t check_text[] = "123456789";
	static const uint8_t partial_octets[] = {
		0xa5, 0x5a, 0xc3, 0x3c, 0xf0,
		0x0f, 0x96, 0x69, 0x80, 0x00,
	};
	static const uint8_t one_bit[] = { 0x80 };
	uint16_t remainder = 0;

	CHECK(sippymodem_crc16_ccitt_false_packed_bits(
		      check_text, 9u * 8u, &remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      remainder == UINT16_C(0x29b1),
	      "CRC-16/CCITT-FALSE check value changed");
	CHECK(sippymodem_crc16_ccitt_false_packed_bits(
		      one_bit, 1, &remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      remainder == UINT16_C(0xfffe) &&
	      sippymodem_crc16_ccitt_false_packed_bits(
		      partial_octets, 77, &remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      remainder == UINT16_C(0x93d5),
	      "partial-octet CRC changed");
	return true;
}

static bool test_bit_traversal(void)
{
	static const uint8_t check_text[] = "123456789";
	uint16_t incremental =
		SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER;
	uint16_t packed = 0;

	for (size_t index = 0; index < 9u * 8u; ++index) {
		bool bit = ((check_text[index / 8u] >>
			      (7u - index % 8u)) &
			     1u) != 0;

		incremental = sippymodem_crc16_ccitt_false_step(
			incremental, bit);
	}
	CHECK(sippymodem_crc16_ccitt_false_packed_bits(
		      check_text, 9u * 8u, &packed) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      incremental == packed && packed == UINT16_C(0x29b1),
	      "incremental and packed traversal diverged");
	return true;
}

static bool test_exact_bit_bound(void)
{
	static const uint8_t left[] = { 0xa5, 0xa8 };
	static const uint8_t different_tail[] = { 0xa5, 0xaf };
	static const uint8_t different_payload[] = { 0xa5, 0xb0 };
	uint16_t left_remainder;
	uint16_t tail_remainder;
	uint16_t payload_remainder;

	CHECK(sippymodem_crc16_ccitt_false_packed_bits(
		      left, 13, &left_remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      sippymodem_crc16_ccitt_false_packed_bits(
		      different_tail, 13, &tail_remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      sippymodem_crc16_ccitt_false_packed_bits(
		      different_payload, 13, &payload_remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      left_remainder == tail_remainder &&
	      left_remainder != payload_remainder,
	      "CRC included padding or omitted the final information bit");
	return true;
}

static bool test_validation(void)
{
	static const uint8_t byte = 0;
	uint16_t remainder = UINT16_C(0x5a5a);

	CHECK(sippymodem_crc16_ccitt_false_packed_bits(
		      NULL, 0, &remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_OK &&
	      remainder ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER &&
	      sippymodem_crc16_ccitt_false_packed_bits(
		      NULL, 1, &remainder) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT &&
	      remainder ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_INITIAL_REMAINDER &&
	      sippymodem_crc16_ccitt_false_packed_bits(
		      &byte, 8, NULL) ==
		      SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT &&
	      strcmp(sippymodem_crc16_ccitt_false_result_name(
		     SIPPYMODEM_CRC16_CCITT_FALSE_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_crc16_ccitt_false_result_name(
		     (enum sippymodem_crc16_ccitt_false_result)2),
		     "unknown") == 0,
	      "CRC accepted invalid arguments or mutated output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "known checks", test_known_checks },
		{ "bit traversal", test_bit_traversal },
		{ "exact bit bound", test_exact_bit_bound },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
