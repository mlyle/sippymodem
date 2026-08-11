// SPDX-License-Identifier: BSD-3-Clause

#include "v34_framing.h"

#include "self_sync_scrambler.h"

#include <stddef.h>

enum {
	V34_GPC_TAP_DELAY = 18,
	V34_GPA_TAP_DELAY = 5,
	V34_COMMON_TAP_DELAY = 23,
};

static const struct sippymodem_self_sync_scrambler_config
	v34_scrambler_configs[] = {
		[V34_POLYNOMIAL_GPC] = {
			.history_bits = V34_SCRAMBLER_BITS,
			.tap_a_delay = V34_GPC_TAP_DELAY,
			.tap_b_delay = V34_COMMON_TAP_DELAY,
		},
		[V34_POLYNOMIAL_GPA] = {
			.history_bits = V34_SCRAMBLER_BITS,
			.tap_a_delay = V34_GPA_TAP_DELAY,
			.tap_b_delay = V34_COMMON_TAP_DELAY,
		},
	};

static bool polynomial_is_valid(
	enum v34_scrambler_polynomial polynomial)
{
	return polynomial == V34_POLYNOMIAL_GPC ||
	       polynomial == V34_POLYNOMIAL_GPA;
}

static const struct sippymodem_self_sync_scrambler_config *
scrambler_config(enum v34_scrambler_polynomial polynomial)
{
	if (!polynomial_is_valid(polynomial))
		return NULL;
	return &v34_scrambler_configs[(unsigned int)polynomial];
}

static bool transmit_polynomial(
	enum v34_station_role role,
	enum v34_scrambler_polynomial *polynomial)
{
	if (polynomial == NULL)
		return false;
	switch (role) {
	case V34_CALLING_STATION:
		*polynomial = V34_POLYNOMIAL_GPC;
		return true;
	case V34_ANSWERING_STATION:
		*polynomial = V34_POLYNOMIAL_GPA;
		return true;
	}
	return false;
}

bool v34_scrambler_is_valid(
	const struct v34_scrambler *scrambler)
{
	struct sippymodem_self_sync_scrambler shared;

	if (scrambler == NULL)
		return false;
	shared.history = scrambler->history;
	return sippymodem_self_sync_scrambler_is_valid(
		&shared, scrambler_config(scrambler->polynomial));
}

enum v34_framing_result v34_scrambler_init(
	struct v34_scrambler *scrambler,
	enum v34_scrambler_polynomial polynomial)
{
	struct v34_scrambler next = { 0 };
	struct sippymodem_self_sync_scrambler shared;

	if (scrambler == NULL ||
	    sippymodem_self_sync_scrambler_init(
		    &shared, scrambler_config(polynomial)) !=
		    SIPPYMODEM_SELF_SYNC_SCRAMBLER_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	next.history = shared.history;
	next.polynomial = polynomial;
	*scrambler = next;
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_transmit_scrambler_init(
	struct v34_scrambler *scrambler,
	enum v34_station_role role)
{
	enum v34_scrambler_polynomial polynomial;

	if (!transmit_polynomial(role, &polynomial))
		return V34_FRAMING_INVALID_ARGUMENT;
	return v34_scrambler_init(scrambler, polynomial);
}

bool v34_transmit_scrambler_is_valid(
	const struct v34_scrambler *scrambler,
	enum v34_station_role role)
{
	enum v34_scrambler_polynomial polynomial;

	return transmit_polynomial(role, &polynomial) &&
	       v34_scrambler_is_valid(scrambler) &&
	       scrambler->polynomial == polynomial;
}

static enum v34_framing_result transform_group(
	struct v34_scrambler *state, uint8_t input,
	unsigned int bit_count, uint8_t *output, bool descramble)
{
	struct v34_scrambler next;
	struct sippymodem_self_sync_scrambler shared;
	const struct sippymodem_self_sync_scrambler_config *config;
	enum sippymodem_self_sync_scrambler_result result;
	uint8_t transformed;

	if (!v34_scrambler_is_valid(state) || output == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
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
		return V34_FRAMING_INVALID_ARGUMENT;
	next.history = shared.history;
	*state = next;
	*output = transformed;
	return V34_FRAMING_OK;
}

enum v34_framing_result v34_scramble_group(
	struct v34_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(scrambler, input, bit_count, output, false);
}

enum v34_framing_result v34_descramble_group(
	struct v34_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output)
{
	return transform_group(descrambler, input, bit_count, output, true);
}

static uint8_t swap_dibit_bits(uint8_t dibit)
{
	return (uint8_t)(((dibit & 1u) << 1) | ((dibit >> 1) & 1u));
}

enum v34_framing_result
v34_scramble_mapped_dibit(
	struct v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t *mapped_dibit)
{
	struct v34_scrambler next;
	uint8_t scrambled_dibit;

	if (!v34_scrambler_is_valid(scrambler) ||
	    input_dibit > 3u || mapped_dibit == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	next = *scrambler;
	if (v34_scramble_group(
		    &next, input_dibit, 2u, &scrambled_dibit) !=
	    V34_FRAMING_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	*scrambler = next;
	*mapped_dibit = swap_dibit_bits(scrambled_dibit);
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_descramble_mapped_dibit(
	struct v34_scrambler *descrambler, uint8_t mapped_dibit,
	uint8_t *output_dibit)
{
	struct v34_scrambler next;
	uint8_t output;

	if (!v34_scrambler_is_valid(descrambler) ||
	    mapped_dibit > 3u || output_dibit == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	next = *descrambler;
	if (v34_descramble_group(
		    &next, swap_dibit_bits(mapped_dibit), 2u, &output) !=
	    V34_FRAMING_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	*descrambler = next;
	*output_dibit = output;
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_scramble_differential_dibit(
	struct v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t *clockwise_quarter_turns)
{
	struct v34_scrambler next;
	uint8_t increment;
	uint8_t rotation;

	if (!v34_scrambler_is_valid(scrambler) ||
	    input_dibit > 3u || previous_clockwise_quarter_turns > 3u ||
	    clockwise_quarter_turns == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	next = *scrambler;
	if (v34_scramble_mapped_dibit(
		    &next, input_dibit, &increment) !=
	    V34_FRAMING_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	rotation = (uint8_t)(
		(previous_clockwise_quarter_turns + increment) & 3u);
	*scrambler = next;
	*clockwise_quarter_turns = rotation;
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_descramble_differential_dibit(
	struct v34_scrambler *descrambler,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t current_clockwise_quarter_turns, uint8_t *output_dibit)
{
	uint8_t increment;

	if (!v34_scrambler_is_valid(descrambler) ||
	    previous_clockwise_quarter_turns > 3u ||
	    current_clockwise_quarter_turns > 3u || output_dibit == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	increment = (uint8_t)((current_clockwise_quarter_turns + 4u -
			       previous_clockwise_quarter_turns) &
			      3u);
	return v34_descramble_mapped_dibit(
		descrambler, increment, output_dibit);
}

enum v34_framing_result
v34_superframe_inversion_bit(
	enum v34_inversion_pattern pattern,
	unsigned int index, uint8_t *bit)
{
	uint16_t packed;
	unsigned int bit_count;

	if (bit == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	switch (pattern) {
	case V34_INVERSION_J7:
		/* V.34 Table 12: 01 11 01 11 11 11 10. */
		packed = UINT16_C(0x1fee);
		bit_count = V34_J7_INVERSION_BITS;
		break;
	case V34_INVERSION_J8:
		/* V.34 Table 12: 01 11 01 11 11 11 10 10. */
		packed = UINT16_C(0x5fee);
		bit_count = V34_J8_INVERSION_BITS;
		break;
	default:
		return V34_FRAMING_INVALID_ARGUMENT;
	}
	if (index >= bit_count)
		return V34_FRAMING_INVALID_ARGUMENT;
	*bit = (uint8_t)((packed >> index) & 1u);
	return V34_FRAMING_OK;
}

const char *v34_framing_result_name(
	enum v34_framing_result result)
{
	switch (result) {
	case V34_FRAMING_OK:
		return "ok";
	case V34_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
