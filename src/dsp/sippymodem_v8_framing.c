// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v8_framing.h"

#include <string.h>

static size_t control_bit_count(
	const struct sippymodem_v8_control_generator *generator)
{
	return SIPPYMODEM_V8_PREAMBLE_BITS +
	       generator->information_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
}

static bool control_generator_is_valid(
	const struct sippymodem_v8_control_generator *generator)
{
	return generator != NULL && generator->information_count != 0 &&
	       generator->information_count <=
		       SIPPYMODEM_V8_MAX_INFORMATION_OCTETS &&
	       generator->bit_index < control_bit_count(generator);
}

enum sippymodem_v8_framing_result sippymodem_v8_control_generator_init(
	struct sippymodem_v8_control_generator *generator,
	const uint8_t *information, size_t information_count)
{
	struct sippymodem_v8_control_generator initialized = {
		.information_count = information_count,
	};

	if (generator == NULL || information == NULL ||
	    information_count == 0 ||
	    information_count > SIPPYMODEM_V8_MAX_INFORMATION_OCTETS)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	memcpy(initialized.information, information, information_count);
	if (!control_generator_is_valid(&initialized))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*generator = initialized;
	return SIPPYMODEM_V8_FRAMING_OK;
}

static uint8_t framed_octet_bit(uint8_t octet, size_t bit_index)
{
	if (bit_index == 0)
		return 0;
	if (bit_index == SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u)
		return 1;
	return (uint8_t)((octet >> (bit_index - 1u)) & 1u);
}

static uint8_t control_bit(
	const struct sippymodem_v8_control_generator *generator)
{
	size_t bit_index = generator->bit_index;

	if (bit_index < SIPPYMODEM_V8_SYNC_MARK_BITS)
		return 1;
	if (bit_index < SIPPYMODEM_V8_PREAMBLE_BITS)
		return bit_index >=
		       SIPPYMODEM_V8_PREAMBLE_BITS - 4u;
	bit_index -= SIPPYMODEM_V8_PREAMBLE_BITS;
	return framed_octet_bit(
		generator->information[
			bit_index / SIPPYMODEM_V8_FRAMED_OCTET_BITS],
		bit_index % SIPPYMODEM_V8_FRAMED_OCTET_BITS);
}

enum sippymodem_v8_framing_result sippymodem_v8_control_next_bit(
	struct sippymodem_v8_control_generator *generator, uint8_t *bit,
	bool *sequence_complete)
{
	struct sippymodem_v8_control_generator next;
	bool complete;
	uint8_t generated;

	if (!control_generator_is_valid(generator) || bit == NULL ||
	    sequence_complete == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	next = *generator;
	generated = control_bit(&next);
	complete = ++next.bit_index == control_bit_count(&next);
	if (complete)
		next.bit_index = 0;
	*generator = next;
	*bit = generated;
	*sequence_complete = complete;
	return SIPPYMODEM_V8_FRAMING_OK;
}

enum sippymodem_v8_framing_result sippymodem_v8_cj_bit(
	size_t bit_index, uint8_t *bit)
{
	if (bit_index >= SIPPYMODEM_V8_CJ_BITS || bit == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*bit = bit_index % SIPPYMODEM_V8_FRAMED_OCTET_BITS ==
	       SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u;
	return SIPPYMODEM_V8_FRAMING_OK;
}

const char *sippymodem_v8_framing_result_name(
	enum sippymodem_v8_framing_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_FRAMING_OK:
		return "ok";
	case SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
