// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_dil_descriptor.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_CAPACITY = 40,
	CRC_INITIAL_REMAINDER = 0xffff,
	CRC_REFLECTED_POLYNOMIAL = 0x8408,
};

static uint8_t packed_bit(const uint8_t *descriptor, size_t bit)
{
	return (uint8_t)((descriptor[bit / 8u] >>
			  (7u - bit % 8u)) &
			  1u);
}

static void toggle_packed_bit(uint8_t *descriptor, size_t bit)
{
	descriptor[bit / 8u] ^=
		(uint8_t)(1u << (7u - bit % 8u));
}

static uint16_t reference_crc(
	const uint8_t *descriptor,
	const struct pcm_dil_descriptor_layout *layout)
{
	uint16_t remainder = CRC_INITIAL_REMAINDER;

	for (size_t bit = PCM_DIL_INFORMATION_FIRST_BIT;
	     bit < layout->crc_start_bit; ++bit) {
		bool start = bit >= PCM_DIL_FRAME_SYNC_BITS &&
			     (bit - PCM_DIL_FRAME_SYNC_BITS) %
				     PCM_DIL_FIELD_STRIDE_BITS == 0u;
		uint16_t next;

		if (start)
			continue;
		next = (uint16_t)(remainder >> 1);
		if (((remainder ^ packed_bit(descriptor, bit)) & 1u) != 0u)
			next ^= CRC_REFLECTED_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static uint16_t packed_crc(
	const uint8_t *descriptor,
	const struct pcm_dil_descriptor_layout *layout)
{
	uint16_t value = 0u;

	for (size_t bit = 0; bit < PCM_DIL_CRC_BITS; ++bit)
		value |= (uint16_t)packed_bit(
				 descriptor, layout->crc_start_bit + 1u + bit) << bit;
	return value;
}

static bool check_layout(
	const struct pcm_dil_descriptor_layout *layout,
	size_t expected_bytes)
{
	uint8_t descriptor[TEST_CAPACITY];
	uint8_t saved[TEST_CAPACITY];
	uint32_t value;
	uint32_t unchanged = UINT32_C(0xa5a5a5a5);

	memset(descriptor, 0xa5, sizeof(descriptor));
	CHECK(pcm_dil_descriptor_layout_is_valid(layout) &&
	      pcm_dil_descriptor_storage_bytes(layout) == expected_bytes &&
	      pcm_dil_descriptor_initialize(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_OK,
	      "DIL layout %zu/%zu did not initialize",
	      layout->crc_start_bit, layout->bit_count);
	for (size_t bit = 0; bit < layout->bit_count; ++bit)
		CHECK(packed_bit(descriptor, bit) ==
		      (bit < PCM_DIL_FRAME_SYNC_BITS),
		      "initialized DIL bit %zu differed", bit);
	for (size_t byte = expected_bytes; byte < sizeof(descriptor); ++byte)
		CHECK(descriptor[byte] == 0xa5u,
		      "DIL initialization exceeded its span at %zu", byte);

	CHECK(pcm_dil_descriptor_write_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      PCM_DIL_INFORMATION_FIRST_BIT, 8u, UINT32_C(0xa5)) &&
	      pcm_dil_descriptor_write_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      layout->crc_start_bit - 16u, 16u, UINT32_C(0xa55a)) &&
	      pcm_dil_descriptor_read_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      PCM_DIL_INFORMATION_FIRST_BIT, 8u, &value) &&
	      value == UINT32_C(0xa5) &&
	      pcm_dil_descriptor_read_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      layout->crc_start_bit - 16u, 16u, &value) &&
	      value == UINT32_C(0xa55a),
	      "DIL fields did not round trip");

	memcpy(saved, descriptor, sizeof(descriptor));
	CHECK(!pcm_dil_descriptor_write_lsb_field(
		      descriptor, sizeof(descriptor), layout, 18u, 7u, 128u) &&
	      !pcm_dil_descriptor_write_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      layout->bit_count, 1u, 0u) &&
	      !pcm_dil_descriptor_write_lsb_field(
		      descriptor, sizeof(descriptor), layout, 18u, 0u, 0u) &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0 &&
	      !pcm_dil_descriptor_read_lsb_field(
		      descriptor, sizeof(descriptor), layout,
		      layout->bit_count, 1u, &unchanged) &&
	      unchanged == UINT32_C(0xa5a5a5a5),
	      "rejected DIL field access was not transactional");

	CHECK(pcm_dil_descriptor_validate_framing(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_OK &&
	      pcm_dil_descriptor_finalize(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_OK &&
	      reference_crc(descriptor, layout) ==
		      packed_crc(descriptor, layout) &&
	      pcm_dil_descriptor_validate_crc(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_OK,
	      "DIL CRC differed from the independent recurrence");

	toggle_packed_bit(descriptor, 26u);
	CHECK(pcm_dil_descriptor_validate_framing(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_OK &&
	      pcm_dil_descriptor_validate_crc(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_CRC_MISMATCH,
	      "damaged DIL information did not fail CRC");
	toggle_packed_bit(descriptor, 26u);
	toggle_packed_bit(descriptor, PCM_DIL_FRAME_SYNC_BITS * 2u);
	memcpy(saved, descriptor, sizeof(descriptor));
	CHECK(pcm_dil_descriptor_validate_framing(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_INVALID_FRAME &&
	      pcm_dil_descriptor_finalize(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_INVALID_FRAME &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0,
	      "invalid DIL start bit was not rejected transactionally");
	toggle_packed_bit(descriptor, PCM_DIL_FRAME_SYNC_BITS * 2u);
	toggle_packed_bit(
		descriptor, layout->crc_start_bit + PCM_DIL_CRC_BITS + 1u);
	CHECK(pcm_dil_descriptor_validate_framing(
		      descriptor, sizeof(descriptor), layout) ==
		      PCM_DIL_DESCRIPTOR_INVALID_FRAME,
	      "nonzero DIL fill bit was accepted");
	return true;
}

static bool test_layouts_and_crc(void)
{
	static const struct {
		struct pcm_dil_descriptor_layout layout;
		size_t bytes;
	} cases[] = {
		{ { 221, 240 }, 30 },
		{ { 255, 276 }, 35 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_layout(&cases[index].layout, cases[index].bytes),
		      "DIL layout case %zu failed", index);
	return true;
}

static bool test_contracts(void)
{
	struct pcm_dil_descriptor_layout valid = { 221, 240 };
	struct pcm_dil_descriptor_layout maximum = {
		2669, PCM_DIL_DESCRIPTOR_MAX_BITS,
	};
	struct pcm_dil_descriptor_layout bad_cadence = { 222, 240 };
	struct pcm_dil_descriptor_layout short_frame = { 221, 238 };
	struct pcm_dil_descriptor_layout overflowing = {
		PCM_DIL_DESCRIPTOR_MAX_BITS,
		PCM_DIL_DESCRIPTOR_MAX_BITS + 1u,
	};
	uint8_t descriptor[30];
	uint8_t saved[sizeof(descriptor)];
	static const char *const names[] = {
		"ok", "invalid argument", "invalid frame", "CRC mismatch",
	};

	memset(descriptor, 0xa5, sizeof(descriptor));
	memcpy(saved, descriptor, sizeof(descriptor));
	CHECK(!pcm_dil_descriptor_layout_is_valid(NULL) &&
	      pcm_dil_descriptor_layout_is_valid(&maximum) &&
	      pcm_dil_descriptor_storage_bytes(&maximum) == 336u &&
	      !pcm_dil_descriptor_layout_is_valid(&bad_cadence) &&
	      !pcm_dil_descriptor_layout_is_valid(&short_frame) &&
	      !pcm_dil_descriptor_layout_is_valid(&overflowing) &&
	      pcm_dil_descriptor_storage_bytes(NULL) == 0u &&
	      pcm_dil_descriptor_initialize(NULL, sizeof(descriptor), &valid) ==
		      PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT &&
	      pcm_dil_descriptor_initialize(
		      descriptor, sizeof(descriptor) - 1u, &valid) ==
		      PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT &&
	      pcm_dil_descriptor_finalize(NULL, sizeof(descriptor), &valid) ==
		      PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT &&
	      pcm_dil_descriptor_validate_framing(
		      NULL, sizeof(descriptor), &valid) ==
		      PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT &&
	      pcm_dil_descriptor_validate_crc(
		      descriptor, sizeof(descriptor), NULL) ==
		      PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT &&
	      memcmp(descriptor, saved, sizeof(descriptor)) == 0,
	      "invalid DIL descriptor contracts changed output");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      pcm_dil_descriptor_result_name(
				      (enum pcm_dil_descriptor_result)index),
			      names[index]) == 0,
		      "DIL descriptor result name %zu differed", index);
	CHECK(strcmp(
		      pcm_dil_descriptor_result_name(
			      (enum pcm_dil_descriptor_result)-1),
		      "unknown") == 0,
	      "unknown DIL descriptor result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] common PCM DIL descriptor framing\n");
	if (!test_layouts_and_crc() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] common PCM DIL descriptor framing\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] common PCM DIL descriptor framing\n");
	return 0;
}
