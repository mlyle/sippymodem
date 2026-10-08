// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_dil_descriptor.h"

#include "packed_bits.h"
#include "pcm_control_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	FRAME_START_FIRST_BIT = 17,
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

enum pcm_dil_descriptor_result pcm_dil_descriptor_validate_framing(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout)
{
	if (!span_is_valid(descriptor, capacity, layout))
		return PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT;
	if (!pcm_control_frame_prefix_is_valid(descriptor) ||
	    !pcm_control_frame_starts_are_zero(
		    descriptor, FRAME_START_FIRST_BIT,
		    layout->crc_start_bit + 1u))
		return PCM_DIL_DESCRIPTOR_INVALID_FRAME;
	if (!packed_msb_bits_are_zero(
		    descriptor,
		    layout->crc_start_bit + PCM_DIL_CRC_BITS + 1u,
		    layout->bit_count - layout->crc_start_bit -
			    PCM_DIL_CRC_BITS - 1u))
		return PCM_DIL_DESCRIPTOR_INVALID_FRAME;
	return PCM_DIL_DESCRIPTOR_OK;
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
	return pcm_control_frame_crc(descriptor, layout->crc_start_bit) ==
		       packed_msb_read_msb_field(
			       descriptor, layout->crc_start_bit + 1u,
			       PCM_DIL_CRC_BITS) ?
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
	pcm_control_frame_write_prefix(descriptor);
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
	remainder = pcm_control_frame_crc(descriptor, layout->crc_start_bit);
	packed_msb_write_msb_field(
		descriptor, layout->crc_start_bit + 1u,
		PCM_DIL_CRC_BITS, remainder);
	return PCM_DIL_DESCRIPTOR_OK;
}
