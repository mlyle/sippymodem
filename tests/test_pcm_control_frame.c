// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_control_frame.h"
#include "v92_cpd.h"
#include "test.h"

#include <string.h>

static bool test_sync_prefix(void)
{
	uint8_t frame[3];

	/* Exhaust all eighteen prefix bits; the following six bits are ignored. */
	for (uint32_t prefix = 0; prefix < (1u << 18); ++prefix) {
		frame[0] = (uint8_t)(prefix >> 10);
		frame[1] = (uint8_t)(prefix >> 2);
		frame[2] = (uint8_t)((prefix << 6) | 0x3fu);
		CHECK(pcm_control_frame_prefix_is_valid(frame) ==
			      (prefix == 0x3fffeu),
		      "sync prefix 0x%x was misclassified", prefix);
	}
	return true;
}

static bool test_write_prefix(void)
{
	uint8_t frame[16], expected[sizeof(frame)];

	for (unsigned int pattern = 0; pattern <= UINT8_MAX; ++pattern) {
		for (size_t offset = 0; offset < 8; ++offset) {
			memset(frame, pattern, sizeof(frame));
			memcpy(expected, frame, sizeof(expected));
			for (size_t bit = 0; bit < 18u; ++bit) {
				uint8_t mask = (uint8_t)(0x80u >> (bit % 8u));

				if (bit < 17u)
					expected[offset + bit / 8u] |= mask;
				else
					expected[offset + bit / 8u] &= (uint8_t)~mask;
			}
			pcm_control_frame_write_prefix(frame + offset);
			CHECK(memcmp(frame, expected, sizeof(frame)) == 0,
			      "prefix write damaged payload/guard at %zu/%u", offset, pattern);
		}
	}
	return true;
}

static bool test_streaming_sync(void)
{
	/* Every eighteen-bit input from every retained suffix. A sliding wire
	 * window is independent of the receiver's saturated run counter. */
	for (unsigned initial = 0; initial <= 17u; ++initial) {
		for (uint32_t input = 0; input < (1u << 18); ++input) {
			uint32_t window = (1u << initial) - 1u;
			uint8_t ones = (uint8_t)initial;

			for (unsigned remaining = 18u; remaining != 0u; --remaining) {
				bool bit = ((input >> (remaining - 1u)) & 1u) != 0u;
				unsigned suffix = 0;

				window = ((window << 1u) | bit) & 0x3ffffu;
				while (suffix < 17u && (window & (1u << suffix)) != 0u)
					++suffix;
				CHECK(pcm_control_frame_sync_bit(&ones, bit) ==
					      (window == 0x3fffeu) && ones == suffix,
				      "stream sync changed at suffix %u/input %x/bit %u",
				      initial, input, remaining);
			}
		}
	}
	return true;
}

static bool test_sync_suffix(void)
{
	uint8_t frame[32];

	for (size_t end = 0; end <= sizeof(frame) * 8u; ++end) {
		for (size_t run = 0; run <= 32u; ++run) {
			size_t expected = run < end ? run : end;

			if (expected > 17u)
				expected = 17u;
			/* Set the suffix and the unreceived tail, leaving a zero before
			 * the suffix. Nonzero tail bits must not affect the count. */
			memset(frame, 0, sizeof(frame));
			for (size_t bit = 0; bit < sizeof(frame) * 8u; ++bit)
				packed_msb_set_bit(frame, bit, bit + run >= end);
			CHECK(pcm_control_frame_trailing_ones(frame, end) == expected,
			      "sync suffix changed for end %zu/run %zu", end, run);
		}
	}
	return true;
}

static bool check_start_range(size_t first, size_t end)
{
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];

	memset(frame, 0xff, sizeof(frame));
	/* Independent per-bit predicate: everything except in-range start bits
	 * stays nonzero, including the sync, information and unreceived tail. */
	for (size_t bit = 0; bit < sizeof(frame) * 8u; ++bit)
		if (bit >= first && bit < end && bit % 17u == 0u)
			frame[bit / 8u] &= (uint8_t)~(0x80u >> (bit % 8u));
	CHECK(pcm_control_frame_starts_are_zero(frame, first, end),
	      "valid start range rejected: %zu/%zu", first, end);
	for (size_t bit = 0; bit < end + 17u; ++bit) {
		uint8_t mask = (uint8_t)(0x80u >> (bit % 8u));
		bool required = bit >= first && bit < end && bit % 17u == 0u;

		frame[bit / 8u] ^= mask;
		CHECK(pcm_control_frame_starts_are_zero(frame, first, end) == !required,
		      "start range %zu/%zu misclassified bit %zu", first, end, bit);
		frame[bit / 8u] ^= mask;
	}
	return true;
}

static bool test_start_ranges(void)
{
	const size_t first_bits[] = { 17u, 34u, 136u };
	const size_t long_ends[] = { 289u, 1768u, V92_CPD_MAX_PAYLOAD_BITS };

	for (size_t i = 0; i < ARRAY_LENGTH(first_bits); ++i) {
		for (size_t length = 0; length <= 51u; ++length)
			CHECK(check_start_range(first_bits[i], first_bits[i] + length),
			      "empty or partial word range failed");
		for (size_t j = 0; j < ARRAY_LENGTH(long_ends); ++j)
			CHECK(check_start_range(first_bits[i], long_ends[j]) &&
			      check_start_range(first_bits[i], long_ends[j] + 1u),
			      "payload/CRC boundary failed");
	}
	return true;
}

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

static bool test_constellation_rows(void)
{
	struct pcm_constellation rows[6], decoded[7], expected_rows[7];
	uint8_t frame[256], expected[256];

	/* Both planes, all row extents (including empty), each Ucode, and the
	 * empty/full masks. Independently place bits by advancing a wire cursor
	 * through eight framed words, and retain non-mask bits as sentinels. */
	for (size_t first = 0; first <= 12u; ++first) {
		for (size_t count = 0; count <= 6u && first + count <= 12u; ++count) {
			for (unsigned int pattern = 0; pattern < 130u; ++pattern) {
				size_t cursor = 136u + first * 136u;

				memset(rows, 0, sizeof(rows));
				memset(frame, 0xa5, sizeof(frame));
				memcpy(expected, frame, sizeof(frame));
				for (size_t row = 0; row < count; ++row) {
					if (pattern < 128u) {
						unsigned int ucode = (pattern + row * 7u) % 128u;

						rows[row].ucode_mask[ucode / 8u] =
							(uint8_t)(1u << (ucode % 8u));
					} else if (pattern == 129u) {
						memset(&rows[row], 0xff, sizeof(rows[row]));
					}
					for (unsigned int ucode = 0; ucode < 128u; ++ucode) {
						if (ucode % 16u == 0u)
							++cursor;
						uint8_t mask = (uint8_t)(0x80u >> (cursor % 8u));

						if (rows[row].ucode_mask[ucode / 8u] &
						    (1u << (ucode % 8u)))
							expected[cursor / 8u] |= mask;
						else
							expected[cursor / 8u] &= (uint8_t)~mask;
						++cursor;
					}
				}
				pcm_control_frame_write_constellations(frame, first, rows, count);
				CHECK(memcmp(frame, expected, sizeof(frame)) == 0,
				      "wire mask or sentinel changed: %zu/%zu/%u",
				      first, count, pattern);
				memset(decoded, 0x5a, sizeof(decoded));
				memcpy(expected_rows, decoded, sizeof(decoded));
				memcpy(expected_rows, rows, count * sizeof(rows[0]));
				pcm_control_frame_read_constellations(
					expected, first, decoded, count);
				CHECK(memcmp(decoded, expected_rows, sizeof(decoded)) == 0,
				      "decoded mask or sentinel changed: %zu/%zu/%u",
				      first, count, pattern);
			}
		}
	}
	return true;
}

static bool check_constellation_map(const uint8_t map[6], unsigned pattern)
{
	uint8_t frame[18], expected[sizeof(frame)], exact_header[16], decoded[8];
	size_t cursor = 103u;
	uint8_t count = 1u;
	bool valid = true;

	memset(frame, pattern, sizeof(frame));
	memcpy(expected, frame, sizeof(frame));
	for (size_t interval = 0; interval < 6u; ++interval) {
		valid = valid && map[interval] < 6u;
		if (map[interval] >= count)
			count = map[interval] + 1u;
		for (unsigned bit = 0; bit < 4u; ++bit) {
			if (cursor % 17u == 0u)
				++cursor;
			uint8_t mask = (uint8_t)(0x80u >> (cursor % 8u));

			if ((map[interval] & (1u << bit)) != 0u)
				expected[1u + cursor / 8u] |= mask;
			else
				expected[1u + cursor / 8u] &= (uint8_t)~mask;
			++cursor;
		}
	}
	pcm_control_frame_write_map(frame + 1u, map);
	CHECK(memcmp(frame, expected, sizeof(frame)) == 0,
	      "constellation map changed wire bits or adjacent fields");
	memcpy(exact_header, expected + 1u, sizeof(exact_header));
	memset(decoded, 0xa5, sizeof(decoded));
	CHECK(pcm_control_frame_read_map(exact_header, decoded + 1u) ==
		      (valid ? count : 0u) &&
	      pcm_control_frame_read_map(exact_header, NULL) ==
		      (valid ? count : 0u) &&
	      decoded[0] == 0xa5 && decoded[7] == 0xa5,
	      "constellation map count, bounds or shape-only probe changed");
	CHECK(!valid || memcmp(decoded + 1u, map, 6u) == 0,
	      "constellation map decoded different indices");
	return true;
}

static bool test_constellation_map(void)
{
	uint8_t map[6];

	/* Every six-row map, including unused intermediate rows. The record
	 * owner decides whether holes are legal for its message kind. */
	for (unsigned combination = 0; combination < 46656u; ++combination) {
		unsigned digits = combination;

		for (size_t interval = 0; interval < 6u; ++interval) {
			map[interval] = (uint8_t)(digits % 6u);
			digits /= 6u;
		}
		CHECK(check_constellation_map(map, combination & 1u ? 0xffu : 0u),
		      "map %u failed", combination);
	}
	for (size_t interval = 0; interval < 6u; ++interval) {
		for (unsigned index = 6u; index < 16u; ++index) {
			memset(map, 0, sizeof(map));
			map[interval] = (uint8_t)index;
			CHECK(check_constellation_map(map, 0xa5u),
			      "invalid map index %u at %zu passed", index, interval);
		}
	}
	return true;
}

int main(void)
{
	return test_sync_prefix() && test_write_prefix() && test_streaming_sync() &&
	       test_sync_suffix() && test_start_ranges() &&
	       test_known_check() && test_payload_bounds() &&
	       test_constellation_rows() && test_constellation_map() ? 0 : 1;
}
