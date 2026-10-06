// SPDX-License-Identifier: BSD-3-Clause

#include "v34_framing.h"
#include "packed_bits.h"

#include "self_sync_scrambler.h"

#include <stddef.h>

static const struct self_sync_scrambler_config *
scrambler_config(enum v34_scrambler_polynomial polynomial)
{
	switch (polynomial) {
	case V34_POLYNOMIAL_GPC: return &self_sync_scrambler_gpc;
	case V34_POLYNOMIAL_GPA: return &self_sync_scrambler_gpa;
	}
	return NULL;
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
	struct self_sync_scrambler shared;

	if (scrambler == NULL)
		return false;
	shared.history = scrambler->history;
	return self_sync_scrambler_is_valid(
		&shared, scrambler_config(scrambler->polynomial));
}

enum v34_framing_result v34_scrambler_init(
	struct v34_scrambler *scrambler,
	enum v34_scrambler_polynomial polynomial)
{
	struct v34_scrambler next = { 0 };
	struct self_sync_scrambler shared;

	if (scrambler == NULL ||
	    self_sync_scrambler_init(
		    &shared, scrambler_config(polynomial)) !=
		    SELF_SYNC_SCRAMBLER_OK)
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
	struct self_sync_scrambler shared;
	const struct self_sync_scrambler_config *config;
	enum self_sync_scrambler_result result;
	uint8_t transformed;

	if (state == NULL || output == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	shared.history = state->history;
	config = scrambler_config(state->polynomial);
	if (descramble)
		result = self_sync_descramble_group(
			&shared, config, input, bit_count, &transformed);
	else
		result = self_sync_scramble_group(
			&shared, config, input, bit_count, &transformed);
	if (result != SELF_SYNC_SCRAMBLER_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	state->history = shared.history;
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

static enum v34_framing_result transform_unpacked_bits(
	struct v34_scrambler *state, const uint8_t *input,
	size_t bit_count, uint8_t *output, bool descramble)
{
	struct v34_scrambler next;

	if (state == NULL || input == NULL || output == NULL || bit_count == 0u ||
	    !v34_scrambler_is_valid(state) ||
	    !unpacked_bits_are_binary(input, bit_count) ||
	    memory_regions_overlap(state, sizeof(*state), input, bit_count) ||
	    memory_regions_overlap(state, sizeof(*state), output, bit_count) ||
	    (input != output &&
	     memory_regions_overlap(input, bit_count, output, bit_count)))
		return V34_FRAMING_INVALID_ARGUMENT;
	next = *state;
	for (size_t offset = 0; offset < bit_count;) {
		unsigned int count = (unsigned int)(
			bit_count - offset < SELF_SYNC_MAX_GROUP_BITS ?
				bit_count - offset : SELF_SYNC_MAX_GROUP_BITS);
		uint8_t group = 0;

		for (unsigned int bit = 0; bit < count; ++bit)
			group = (uint8_t)((group << 1) | input[offset + bit]);
		if (transform_group(&next, group, count, &group, descramble) !=
		    V34_FRAMING_OK)
			return V34_FRAMING_INVALID_ARGUMENT;
		for (unsigned int bit = 0; bit < count; ++bit)
			output[offset + bit] = (uint8_t)(
				(group >> (count - bit - 1u)) & 1u);
		offset += count;
	}
	*state = next;
	return V34_FRAMING_OK;
}

enum v34_framing_result v34_scramble_unpacked_bits(
	struct v34_scrambler *scrambler, const uint8_t *input,
	size_t bit_count, uint8_t *output)
{
	return transform_unpacked_bits(
		scrambler, input, bit_count, output, false);
}

enum v34_framing_result v34_descramble_unpacked_bits(
	struct v34_scrambler *descrambler, const uint8_t *input,
	size_t bit_count, uint8_t *output)
{
	return transform_unpacked_bits(
		descrambler, input, bit_count, output, true);
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
	uint8_t scrambled_dibit;

	if (mapped_dibit == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	if (v34_scramble_group(
		    scrambler, input_dibit, 2u, &scrambled_dibit) !=
	    V34_FRAMING_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	*mapped_dibit = swap_dibit_bits(scrambled_dibit);
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_descramble_mapped_dibit(
	struct v34_scrambler *descrambler, uint8_t mapped_dibit,
	uint8_t *output_dibit)
{
	if (mapped_dibit > 3u)
		return V34_FRAMING_INVALID_ARGUMENT;
	return v34_descramble_group(
		descrambler, swap_dibit_bits(mapped_dibit), 2u, output_dibit);
}

enum v34_framing_result
v34_scramble_differential_dibit(
	struct v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t *clockwise_quarter_turns)
{
	uint8_t increment;
	uint8_t rotation;

	if (previous_clockwise_quarter_turns > 3u ||
	    clockwise_quarter_turns == NULL)
		return V34_FRAMING_INVALID_ARGUMENT;
	if (v34_scramble_mapped_dibit(
		    scrambler, input_dibit, &increment) !=
	    V34_FRAMING_OK)
		return V34_FRAMING_INVALID_ARGUMENT;
	rotation = (uint8_t)(
		(previous_clockwise_quarter_turns + increment) & 3u);
	*clockwise_quarter_turns = rotation;
	return V34_FRAMING_OK;
}

enum v34_framing_result
v34_descramble_differential_dibit(
	struct v34_scrambler *descrambler,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t current_clockwise_quarter_turns, uint8_t *output_dibit)
{
	return v34_descramble_control_symbol(
		descrambler, previous_clockwise_quarter_turns,
		current_clockwise_quarter_turns, 0, 2u, output_dibit);
}

enum v34_framing_result v34_descramble_control_symbol(
	struct v34_scrambler *descrambler,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t current_clockwise_quarter_turns, uint8_t quarter_point,
	unsigned int bit_count, uint8_t *output)
{
	uint8_t wire;

	if ((bit_count != 2u && bit_count != 4u) ||
	    previous_clockwise_quarter_turns > 3u ||
	    current_clockwise_quarter_turns > 3u ||
	    quarter_point > (bit_count == 2u ? 0u : 3u))
		return V34_FRAMING_INVALID_ARGUMENT;
	wire = swap_dibit_bits((uint8_t)(
		(current_clockwise_quarter_turns + 4u -
		 previous_clockwise_quarter_turns) & 3u));
	if (bit_count == 4u)
		wire = (uint8_t)((wire << 2u) | swap_dibit_bits(quarter_point));
	return v34_descramble_group(descrambler, wire, bit_count, output);
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
