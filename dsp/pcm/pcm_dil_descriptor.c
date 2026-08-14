// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_dil_descriptor.h"

#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	FRAME_START_FIRST_BIT = 17,
	CRC_INITIAL_REMAINDER = 0xffff,
	CRC_REFLECTED_POLYNOMIAL = 0x8408,
};

bool pcm_dil_descriptor_layout_is_valid(
	const struct pcm_dil_descriptor_layout *layout)
{
	return layout != NULL &&
	       layout->crc_start_bit >= PCM_DIL_INFORMATION_FIRST_BIT &&
	       (layout->crc_start_bit - FRAME_START_FIRST_BIT) %
		       PCM_DIL_FIELD_STRIDE_BITS == 0u &&
	       layout->bit_count <= PCM_DIL_DESCRIPTOR_MAX_BITS &&
	       layout->crc_start_bit < layout->bit_count &&
	       layout->bit_count > layout->crc_start_bit +
		       PCM_DIL_CRC_BITS + 1u;
}

size_t pcm_dil_descriptor_storage_bytes(
	const struct pcm_dil_descriptor_layout *layout)
{
	return pcm_dil_descriptor_layout_is_valid(layout) ?
		       (layout->bit_count + 7u) / 8u :
		       0u;
}

static bool span_is_valid(
	const void *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	size_t bytes = pcm_dil_descriptor_storage_bytes(layout);

	return descriptor != NULL && bytes != 0u && capacity >= bytes;
}

static bool field_is_valid(
	const void *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits)
{
	return span_is_valid(descriptor, capacity, layout) &&
	       field_bits > 0u && field_bits <= 32u &&
	       first_bit <= layout->bit_count &&
	       (size_t)field_bits <= layout->bit_count - first_bit;
}

bool pcm_dil_descriptor_read_lsb_field(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t *value)
{
	if (value == NULL ||
	    !field_is_valid(
		    descriptor, capacity, layout, first_bit, field_bits))
		return false;
	*value = packed_msb_read_lsb_field(
		descriptor, first_bit, field_bits);
	return true;
}

bool pcm_dil_descriptor_write_lsb_field(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t value)
{
	if (!field_is_valid(
		    descriptor, capacity, layout, first_bit, field_bits) ||
	    (field_bits < 32u && (value >> field_bits) != 0u))
		return false;
	packed_msb_write_lsb_field(
		descriptor, first_bit, field_bits, value);
	return true;
}

static bool is_start_bit(size_t index)
{
	return index >= FRAME_START_FIRST_BIT &&
	       (index - FRAME_START_FIRST_BIT) %
		       PCM_DIL_FIELD_STRIDE_BITS == 0u;
}

enum pcm_dil_descriptor_result pcm_dil_descriptor_validate_framing(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	if (!span_is_valid(descriptor, capacity, layout))
		return PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT;
	for (size_t bit = 0; bit < PCM_DIL_FRAME_SYNC_BITS; ++bit) {
		if (packed_msb_bit(descriptor, bit) == 0u)
			return PCM_DIL_DESCRIPTOR_INVALID_FRAME;
	}
	for (size_t bit = FRAME_START_FIRST_BIT;
	     bit <= layout->crc_start_bit;
	     bit += PCM_DIL_FIELD_STRIDE_BITS) {
		if (packed_msb_bit(descriptor, bit) != 0u)
			return PCM_DIL_DESCRIPTOR_INVALID_FRAME;
	}
	for (size_t bit = layout->crc_start_bit + PCM_DIL_CRC_BITS + 1u;
	     bit < layout->bit_count; ++bit) {
		if (packed_msb_bit(descriptor, bit) != 0u)
			return PCM_DIL_DESCRIPTOR_INVALID_FRAME;
	}
	return PCM_DIL_DESCRIPTOR_OK;
}

static uint16_t crc_step(uint16_t remainder, bool bit)
{
	uint16_t next = (uint16_t)(remainder >> 1);

	if (((remainder ^ (uint16_t)bit) & 1u) != 0u)
		next ^= CRC_REFLECTED_POLYNOMIAL;
	return next;
}

static uint16_t descriptor_crc(
	const uint8_t *descriptor,
	const struct pcm_dil_descriptor_layout *layout)
{
	uint16_t remainder = CRC_INITIAL_REMAINDER;

	for (size_t bit = PCM_DIL_INFORMATION_FIRST_BIT;
	     bit < layout->crc_start_bit; ++bit) {
		if (!is_start_bit(bit))
			remainder = crc_step(
				remainder, packed_msb_bit(descriptor, bit) != 0u);
	}
	return remainder;
}

static uint16_t encoded_crc(
	const uint8_t *descriptor,
	const struct pcm_dil_descriptor_layout *layout)
{
	return (uint16_t)packed_msb_read_lsb_field(
		descriptor, layout->crc_start_bit + 1u,
		PCM_DIL_CRC_BITS);
}

enum pcm_dil_descriptor_result pcm_dil_descriptor_validate_crc(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	enum pcm_dil_descriptor_result framing =
		pcm_dil_descriptor_validate_framing(
			descriptor, capacity, layout);

	if (framing != PCM_DIL_DESCRIPTOR_OK)
		return framing;
	return descriptor_crc(descriptor, layout) ==
		       encoded_crc(descriptor, layout) ?
		       PCM_DIL_DESCRIPTOR_OK :
		       PCM_DIL_DESCRIPTOR_CRC_MISMATCH;
}

enum pcm_dil_descriptor_result pcm_dil_descriptor_initialize(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	size_t bytes;

	if (!span_is_valid(descriptor, capacity, layout))
		return PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT;
	bytes = pcm_dil_descriptor_storage_bytes(layout);
	memset(descriptor, 0, bytes);
	for (size_t bit = 0; bit < PCM_DIL_FRAME_SYNC_BITS; ++bit)
		packed_msb_set_bit(descriptor, bit, true);
	return PCM_DIL_DESCRIPTOR_OK;
}

enum pcm_dil_descriptor_result pcm_dil_descriptor_finalize(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	enum pcm_dil_descriptor_result framing =
		pcm_dil_descriptor_validate_framing(
			descriptor, capacity, layout);
	uint16_t remainder;

	if (framing != PCM_DIL_DESCRIPTOR_OK)
		return framing;
	remainder = descriptor_crc(descriptor, layout);
	packed_msb_write_lsb_field(
		descriptor, layout->crc_start_bit + 1u,
		PCM_DIL_CRC_BITS, remainder);
	return PCM_DIL_DESCRIPTOR_OK;
}

const char *pcm_dil_descriptor_result_name(
	enum pcm_dil_descriptor_result result)
{
	switch (result) {
	case PCM_DIL_DESCRIPTOR_OK:
		return "ok";
	case PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_DIL_DESCRIPTOR_INVALID_FRAME:
		return "invalid frame";
	case PCM_DIL_DESCRIPTOR_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
