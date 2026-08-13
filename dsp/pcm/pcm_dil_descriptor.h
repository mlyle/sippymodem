// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DIL_DESCRIPTOR_H
#define PCM_DIL_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PCM_DIL_FRAME_SYNC_BITS = 17,
	PCM_DIL_INFORMATION_FIRST_BIT = 18,
	PCM_DIL_FIELD_STRIDE_BITS = 17,
	PCM_DIL_CRC_BITS = 16,
	/* N=255 and both 128-bit learning patterns reach this V.92 bound. */
	PCM_DIL_DESCRIPTOR_MAX_BITS = 2688,
};

struct pcm_dil_descriptor_layout {
	size_t crc_start_bit;
	size_t bit_count;
};

enum pcm_dil_descriptor_result {
	PCM_DIL_DESCRIPTOR_OK,
	PCM_DIL_DESCRIPTOR_INVALID_ARGUMENT,
	PCM_DIL_DESCRIPTOR_INVALID_FRAME,
	PCM_DIL_DESCRIPTOR_CRC_MISMATCH,
};

bool pcm_dil_descriptor_layout_is_valid(
	const struct pcm_dil_descriptor_layout *layout);
size_t pcm_dil_descriptor_storage_bytes(
	const struct pcm_dil_descriptor_layout *layout);

/* Initialize a zero-valued descriptor with the common 17-one frame sync.
 * Finalize verifies framing, then writes the reflected CRC transactionally. */
enum pcm_dil_descriptor_result pcm_dil_descriptor_initialize(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout);
enum pcm_dil_descriptor_result pcm_dil_descriptor_finalize(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout);

enum pcm_dil_descriptor_result pcm_dil_descriptor_validate_framing(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout);
enum pcm_dil_descriptor_result pcm_dil_descriptor_validate_crc(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout);

/* Descriptor fields are transmitted least-significant bit first while the
 * containing octets are packed in transmission order, most-significant bit
 * first. Rejected calls leave state and output unchanged. */
bool pcm_dil_descriptor_read_lsb_field(
	const uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t *value);
bool pcm_dil_descriptor_write_lsb_field(
	uint8_t *descriptor, size_t capacity,
	const struct pcm_dil_descriptor_layout *layout,
	size_t first_bit, unsigned int field_bits, uint32_t value);

const char *pcm_dil_descriptor_result_name(
	enum pcm_dil_descriptor_result result);

#endif /* PCM_DIL_DESCRIPTOR_H */
