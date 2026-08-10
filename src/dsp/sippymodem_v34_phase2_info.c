// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase2_info.h"

enum {
	V34_PHASE2_INFO_SYNC_WORD = 0xff72,
	V34_PHASE2_INFO_CRC_INITIAL = 0xffff,
	V34_PHASE2_INFO_CRC_POLYNOMIAL = 0x1021,
};

static bool payload_arguments_are_valid(
	const uint8_t *payload, size_t payload_bit_count)
{
	return payload_bit_count <=
		       SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS &&
	       (payload != NULL || payload_bit_count == 0);
}

static uint8_t packed_bit(const uint8_t *packed, size_t bit_index)
{
	return (uint8_t)((packed[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void set_packed_bit(uint8_t *packed, size_t bit_index, uint8_t bit)
{
	packed[bit_index / 8u] |=
		(uint8_t)(bit << (7u - bit_index % 8u));
}

static uint16_t crc_step(uint16_t crc, uint8_t bit)
{
	uint16_t next = (uint16_t)(crc << 1);

	if ((((crc >> 15) ^ bit) & 1u) != 0)
		next ^= V34_PHASE2_INFO_CRC_POLYNOMIAL;
	return next;
}

static uint16_t payload_crc(
	const uint8_t *payload, size_t payload_bit_count)
{
	uint16_t crc = V34_PHASE2_INFO_CRC_INITIAL;

	for (size_t index = 0; index < payload_bit_count; ++index)
		crc = crc_step(crc, packed_bit(payload, index));
	return crc;
}

size_t sippymodem_v34_phase2_info_frame_bit_count(
	size_t payload_bit_count)
{
	if (payload_bit_count >
	    SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS)
		return 0;
	return SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS + payload_bit_count +
	       SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS +
	       SIPPYMODEM_V34_PHASE2_INFO_END_BITS;
}

enum sippymodem_v34_phase2_info_result sippymodem_v34_phase2_info_crc(
	const uint8_t *payload, size_t payload_bit_count, uint16_t *crc)
{
	if (!payload_arguments_are_valid(payload, payload_bit_count) ||
	    crc == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	*crc = payload_crc(payload, payload_bit_count);
	return SIPPYMODEM_V34_PHASE2_INFO_OK;
}

static bool payload_padding_is_zero(
	const struct sippymodem_v34_phase2_info_generator *generator)
{
	size_t used_bytes = (generator->payload_bit_count + 7u) / 8u;

	if (generator->payload_bit_count % 8u != 0) {
		uint8_t padding_mask = (uint8_t)(
			(1u << (8u - generator->payload_bit_count % 8u)) - 1u);

		if ((generator->payload[used_bytes - 1u] & padding_mask) != 0)
			return false;
	}
	for (size_t index = used_bytes;
	     index < SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BYTES; ++index) {
		if (generator->payload[index] != 0)
			return false;
	}
	return true;
}

bool sippymodem_v34_phase2_info_generator_is_valid(
	const struct sippymodem_v34_phase2_info_generator *generator)
{
	size_t frame_bit_count;

	if (generator == NULL ||
	    generator->payload_bit_count >
		    SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS)
		return false;
	frame_bit_count = sippymodem_v34_phase2_info_frame_bit_count(
		generator->payload_bit_count);
	return generator->bit_index <= frame_bit_count &&
	       payload_padding_is_zero(generator) &&
	       generator->crc == payload_crc(
				 generator->payload,
				 generator->payload_bit_count);
}

enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_generator_init(
	struct sippymodem_v34_phase2_info_generator *generator,
	const uint8_t *payload, size_t payload_bit_count)
{
	struct sippymodem_v34_phase2_info_generator initialized = {
		.payload_bit_count = payload_bit_count,
	};

	if (generator == NULL ||
	    !payload_arguments_are_valid(payload, payload_bit_count))
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	for (size_t index = 0; index < payload_bit_count; ++index)
		set_packed_bit(initialized.payload, index,
			       packed_bit(payload, index));
	initialized.crc = payload_crc(initialized.payload, payload_bit_count);
	if (!sippymodem_v34_phase2_info_generator_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	*generator = initialized;
	return SIPPYMODEM_V34_PHASE2_INFO_OK;
}

enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_generator_reset(
	struct sippymodem_v34_phase2_info_generator *generator)
{
	struct sippymodem_v34_phase2_info_generator reset;

	if (!sippymodem_v34_phase2_info_generator_is_valid(generator))
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	reset = *generator;
	reset.bit_index = 0;
	*generator = reset;
	return SIPPYMODEM_V34_PHASE2_INFO_OK;
}

static uint8_t info_frame_bit(
	const struct sippymodem_v34_phase2_info_generator *generator,
	size_t bit_index)
{
	if (bit_index < SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS)
		return (uint8_t)(
			(V34_PHASE2_INFO_SYNC_WORD >>
			 (SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS - bit_index - 1u)) &
			1u);
	bit_index -= SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS;
	if (bit_index < generator->payload_bit_count)
		return packed_bit(generator->payload, bit_index);
	bit_index -= generator->payload_bit_count;
	if (bit_index < SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS)
		return (uint8_t)(
			(generator->crc >>
			 (SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS - bit_index - 1u)) &
			1u);
	return 1;
}

enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_next_bit(
	struct sippymodem_v34_phase2_info_generator *generator,
	uint8_t *bit, bool *frame_complete)
{
	struct sippymodem_v34_phase2_info_generator next;
	size_t frame_bit_count;
	uint8_t generated;
	bool complete;

	if (!sippymodem_v34_phase2_info_generator_is_valid(generator) ||
	    bit == NULL || frame_complete == NULL)
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	frame_bit_count = sippymodem_v34_phase2_info_frame_bit_count(
		generator->payload_bit_count);
	if (generator->bit_index == frame_bit_count)
		return SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT;
	next = *generator;
	generated = info_frame_bit(&next, next.bit_index);
	complete = ++next.bit_index == frame_bit_count;
	*generator = next;
	*bit = generated;
	*frame_complete = complete;
	return SIPPYMODEM_V34_PHASE2_INFO_OK;
}

const char *sippymodem_v34_phase2_info_result_name(
	enum sippymodem_v34_phase2_info_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_INFO_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
