// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_control_frame.h"
#include "v92_cpd.h"
#include "test.h"

#include <string.h>

static bool check_payload_end(const uint8_t *frame, size_t end)
{
	uint8_t information[V92_CPD_MAX_FRAME_BYTES] = { 0 };
	uint8_t changed[V92_CPD_MAX_FRAME_BYTES];
	size_t bits = 0;
	uint16_t expected;

	/* Independently unpack complete/partial 16-bit words, not a modulo
	 * predicate over the frame. The generic CRC sees only information. */
	for (size_t word = 17u; word < end; word += 17u)
		for (size_t bit = 1u; bit <= 16u && word + bit < end; ++bit)
			packed_msb_set_bit(information, bits++,
				packed_msb_bit(frame, word + bit) != 0u);
	CHECK(crc16_ccitt_false_packed_bits(information, bits, &expected) ==
		      CRC16_CCITT_FALSE_OK &&
	      pcm_control_frame_crc(frame, end) == expected,
	      "CRC disagrees with %zu unpacked bits at end %zu", bits, end);
	memcpy(changed, frame, sizeof(changed));
	for (size_t bit = 0; bit < sizeof(changed) * 8u; ++bit)
		if (bit < 18u || bit >= end || bit % 17u == 0u)
			packed_msb_set_bit(changed, bit,
				packed_msb_bit(changed, bit) == 0u);
	CHECK(pcm_control_frame_crc(changed, end) == expected,
	      "CRC included sync, start, or suffix bits at end %zu", end);
	return true;
}

static bool test_payload_bounds(void)
{
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	const size_t long_ends[] = { 289u, 1768u, V92_CPD_MAX_PAYLOAD_BITS };

	for (unsigned int pattern = 0; pattern < 4; ++pattern) {
		for (size_t byte = 0; byte < sizeof(frame); ++byte)
			frame[byte] = pattern == 0 ? 0 : pattern == 1 ? UINT8_MAX :
				(uint8_t)(byte * 73u + pattern * 29u);
		/* All short record bounds, including both CRC-boundary conventions. */
		for (size_t end = 18u; end <= 188u; ++end)
			CHECK(check_payload_end(frame, end), "short bound failed");
		for (size_t i = 0; i < ARRAY_LENGTH(long_ends); ++i) {
			CHECK(check_payload_end(frame, long_ends[i]) &&
			      check_payload_end(frame, long_ends[i] + 1u),
			      "long bound failed");
		}
	}
	return true;
}

static bool test_known_check(void)
{
	static const uint8_t text[] = "123456789";
	uint8_t frame[12] = { 0 };
	size_t cursor = 18u;

	for (size_t bit = 0; bit < 72u; ++bit) {
		if (cursor % 17u == 0u)
			++cursor;
		packed_msb_set_bit(frame, cursor++, packed_msb_bit(text, bit) != 0u);
	}
	CHECK(pcm_control_frame_crc(frame, cursor) == UINT16_C(0x29b1),
	      "framed CRC-16/CCITT-FALSE check value changed");
	return true;
}

int main(void)
{
	return test_known_check() && test_payload_bounds() ? 0 : 1;
}
