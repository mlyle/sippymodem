// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_rate_signal.h"

#include <stddef.h>

enum {
	/* ITU-T V.32 Tables 6 and 7, with B0 stored in bit 15. */
	V32_RATE_FIXED_BITS = 0x0111,
	V32_END_FIXED_BITS = 0xf111,
	V32_B4_2400 = 0x0800,
	V32_B5_4800 = 0x0400,
	V32_B6_9600 = 0x0200,
	V32_B8_TRELLIS = 0x0080,
	V32_RATE_FIELD_MASK = V32_B4_2400 | V32_B5_4800 | V32_B6_9600,
	V32_PAYLOAD_MASK = V32_RATE_FIELD_MASK | V32_B8_TRELLIS,
	V32_RATE_CAPABILITY_MASK = SIPPYMODEM_V32_RATE_CAPABILITY_2400 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600,
};

static bool signal_type_is_valid(enum sippymodem_v32_rate_signal_type type)
{
	return type == SIPPYMODEM_V32_RATE_SIGNAL ||
	       type == SIPPYMODEM_V32_END_SIGNAL;
}

static bool has_one_rate(uint8_t rates)
{
	return rates != 0 && (rates & (uint8_t)(rates - 1u)) == 0;
}

static bool offer_is_valid(
	const struct sippymodem_v32_rate_offer *offer,
	enum sippymodem_v32_rate_signal_type type)
{
	if (offer == NULL ||
	    (offer->rates & (uint8_t)~V32_RATE_CAPABILITY_MASK) != 0 ||
	    offer->rates == 0)
		return false;
	return type != SIPPYMODEM_V32_END_SIGNAL || has_one_rate(offer->rates);
}

static uint16_t encode_rates(uint8_t rates)
{
	uint16_t word = 0;

	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_2400) != 0)
		word |= V32_B4_2400;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0)
		word |= V32_B5_4800;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_9600) != 0)
		word |= V32_B6_9600;
	return word;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_encode(
	enum sippymodem_v32_rate_signal_type type,
	const struct sippymodem_v32_rate_offer *offer, uint16_t *word)
{
	uint16_t encoded;

	if (!signal_type_is_valid(type) || word == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	if (!offer_is_valid(offer, type))
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER;
	encoded = type == SIPPYMODEM_V32_RATE_SIGNAL ? V32_RATE_FIXED_BITS :
						      V32_END_FIXED_BITS;
	encoded |= encode_rates(offer->rates);
	if (offer->trellis_at_highest_rate)
		encoded |= V32_B8_TRELLIS;
	*word = encoded;
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

static uint8_t decode_rates(uint16_t word)
{
	uint8_t rates = 0;

	if ((word & V32_B4_2400) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_2400;
	if ((word & V32_B5_4800) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_4800;
	if ((word & V32_B6_9600) != 0)
		rates |= SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	return rates;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_decode(
	uint16_t word, enum sippymodem_v32_rate_signal_type *type,
	struct sippymodem_v32_rate_offer *offer)
{
	struct sippymodem_v32_rate_offer decoded;
	enum sippymodem_v32_rate_signal_type decoded_type;
	uint16_t fixed = word & (uint16_t)~V32_PAYLOAD_MASK;

	if (type == NULL || offer == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	if (fixed == V32_RATE_FIXED_BITS) {
		decoded_type = SIPPYMODEM_V32_RATE_SIGNAL;
	} else if (fixed == V32_END_FIXED_BITS) {
		decoded_type = SIPPYMODEM_V32_END_SIGNAL;
	} else {
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD;
	}
	decoded.rates = decode_rates(word);
	decoded.trellis_at_highest_rate = (word & V32_B8_TRELLIS) != 0;
	if (decoded.rates == 0)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD;
	*type = decoded_type;
	*offer = decoded;
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_dibit(
	uint16_t word, unsigned int index, uint8_t *dibit)
{
	if (index >= SIPPYMODEM_V32_RATE_SIGNAL_DIBITS || dibit == NULL)
		return SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT;
	*dibit = (uint8_t)((word >> (14u - index * 2u)) & 3u);
	return SIPPYMODEM_V32_RATE_SIGNAL_OK;
}

const char *sippymodem_v32_rate_signal_result_name(
	enum sippymodem_v32_rate_signal_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_RATE_SIGNAL_OK:
		return "ok";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER:
		return "invalid offer";
	case SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD:
		return "invalid word";
	}
	return "unknown";
}
