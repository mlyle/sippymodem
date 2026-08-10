// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_framing.h"

#include "sippymodem_self_sync_scrambler.h"

#include <stddef.h>

enum {
	V34_GPC_TAP_DELAY = 18,
	V34_GPA_TAP_DELAY = 5,
	V34_COMMON_TAP_DELAY = 23,
};

static const struct sippymodem_self_sync_scrambler_config
	v34_scrambler_configs[] = {
		[SIPPYMODEM_V34_POLYNOMIAL_GPC] = {
			.history_bits = SIPPYMODEM_V34_SCRAMBLER_BITS,
			.tap_a_delay = V34_GPC_TAP_DELAY,
			.tap_b_delay = V34_COMMON_TAP_DELAY,
		},
		[SIPPYMODEM_V34_POLYNOMIAL_GPA] = {
			.history_bits = SIPPYMODEM_V34_SCRAMBLER_BITS,
			.tap_a_delay = V34_GPA_TAP_DELAY,
			.tap_b_delay = V34_COMMON_TAP_DELAY,
		},
	};

static bool polynomial_is_valid(
	enum sippymodem_v34_scrambler_polynomial polynomial)
{
	return polynomial == SIPPYMODEM_V34_POLYNOMIAL_GPC ||
	       polynomial == SIPPYMODEM_V34_POLYNOMIAL_GPA;
}

static const struct sippymodem_self_sync_scrambler_config *
scrambler_config(enum sippymodem_v34_scrambler_polynomial polynomial)
{
	if (!polynomial_is_valid(polynomial))
		return NULL;
	return &v34_scrambler_configs[(unsigned int)polynomial];
}

static bool transmit_polynomial(
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_scrambler_polynomial *polynomial)
{
	if (polynomial == NULL)
		return false;
	switch (role) {
	case SIPPYMODEM_V34_CALLING_STATION:
		*polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPC;
		return true;
	case SIPPYMODEM_V34_ANSWERING_STATION:
		*polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPA;
		return true;
	}
	return false;
}

bool sippymodem_v34_scrambler_is_valid(
	const struct sippymodem_v34_scrambler *scrambler)
{
	struct sippymodem_self_sync_scrambler shared;

	if (scrambler == NULL)
		return false;
	shared.history = scrambler->history;
	return sippymodem_self_sync_scrambler_is_valid(
		&shared, scrambler_config(scrambler->polynomial));
}

enum sippymodem_v34_framing_result sippymodem_v34_scrambler_init(
	struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_scrambler_polynomial polynomial)
{
	struct sippymodem_v34_scrambler next = { 0 };
	struct sippymodem_self_sync_scrambler shared;

	if (scrambler == NULL ||
	    sippymodem_self_sync_scrambler_init(
		    &shared, scrambler_config(polynomial)) !=
		    SIPPYMODEM_SELF_SYNC_SCRAMBLER_OK)
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	next.history = shared.history;
	next.polynomial = polynomial;
	*scrambler = next;
	return SIPPYMODEM_V34_FRAMING_OK;
}

enum sippymodem_v34_framing_result
sippymodem_v34_transmit_scrambler_init(
	struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_station_role role)
{
	enum sippymodem_v34_scrambler_polynomial polynomial;

	if (!transmit_polynomial(role, &polynomial))
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	return sippymodem_v34_scrambler_init(scrambler, polynomial);
}

bool sippymodem_v34_transmit_scrambler_is_valid(
	const struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_station_role role)
{
	enum sippymodem_v34_scrambler_polynomial polynomial;

	return transmit_polynomial(role, &polynomial) &&
	       sippymodem_v34_scrambler_is_valid(scrambler) &&
	       scrambler->polynomial == polynomial;
}

static enum sippymodem_v34_framing_result transform_group(
	struct sippymodem_v34_scrambler *state, uint8_t input,
	unsigned int bit_count, uint8_t *output, bool descramble)
{
	struct sippymodem_v34_scrambler next;
	struct sippymodem_self_sync_scrambler shared;
	const struct sippymodem_self_sync_scrambler_config *config;
	enum sippymodem_self_sync_scrambler_result result;
	uint8_t transformed;

	if (!sippymodem_v34_scrambler_is_valid(state) || output == NULL)
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	next = *state;
	shared.history = next.history;
	config = scrambler_config(next.polynomial);
	if (descramble)
		result = sippymodem_self_sync_descramble_group(
			&shared, config, input, bit_count, &transformed);
	else
		result = sippymodem_self_sync_scramble_group(
			&shared, config, input, bit_count, &transformed);
	if (result != SIPPYMODEM_SELF_SYNC_SCRAMBLER_OK)
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	next.history = shared.history;
	*state = next;
	*output = transformed;
	return SIPPYMODEM_V34_FRAMING_OK;
}

enum sippymodem_v34_framing_result sippymodem_v34_scramble_group(
	struct sippymodem_v34_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(scrambler, input, bit_count, output, false);
}

enum sippymodem_v34_framing_result sippymodem_v34_descramble_group(
	struct sippymodem_v34_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(descrambler, input, bit_count, output, true);
}

enum sippymodem_v34_framing_result
sippymodem_v34_superframe_inversion_bit(
	enum sippymodem_v34_inversion_pattern pattern,
	unsigned int index, uint8_t *bit)
{
	uint16_t packed;
	unsigned int bit_count;

	if (bit == NULL)
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	switch (pattern) {
	case SIPPYMODEM_V34_INVERSION_J7:
		/* V.34 Table 12: 01 11 01 11 11 11 10. */
		packed = UINT16_C(0x1fee);
		bit_count = SIPPYMODEM_V34_J7_INVERSION_BITS;
		break;
	case SIPPYMODEM_V34_INVERSION_J8:
		/* V.34 Table 12: 01 11 01 11 11 11 10 10. */
		packed = UINT16_C(0x5fee);
		bit_count = SIPPYMODEM_V34_J8_INVERSION_BITS;
		break;
	default:
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	}
	if (index >= bit_count)
		return SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT;
	*bit = (uint8_t)((packed >> index) & 1u);
	return SIPPYMODEM_V34_FRAMING_OK;
}

const char *sippymodem_v34_framing_result_name(
	enum sippymodem_v34_framing_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_FRAMING_OK:
		return "ok";
	case SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
