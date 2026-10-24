// SPDX-License-Identifier: BSD-3-Clause

#include "self_sync_scrambler.h"
#include "packed_bits.h"

#include <stddef.h>

const struct self_sync_scrambler_config self_sync_scrambler_gpc = {
	.history_bits = 23, .tap_a_delay = 18, .tap_b_delay = 23,
};
const struct self_sync_scrambler_config self_sync_scrambler_gpa = {
	.history_bits = 23, .tap_a_delay = 5, .tap_b_delay = 23,
};

bool self_sync_scrambler_config_is_valid(
	const struct self_sync_scrambler_config *config)
{
	return config != NULL && config->history_bits > 0 &&
	       config->history_bits <= SELF_SYNC_MAX_HISTORY_BITS &&
	       config->tap_a_delay > 0 &&
	       config->tap_a_delay <= config->history_bits &&
	       config->tap_b_delay > 0 &&
	       config->tap_b_delay <= config->history_bits &&
	       config->tap_a_delay != config->tap_b_delay;
}

static uint32_t history_mask(
	const struct self_sync_scrambler_config *config)
{
	if (config->history_bits == SELF_SYNC_MAX_HISTORY_BITS)
		return UINT32_MAX;
	return (UINT32_C(1) << config->history_bits) - 1u;
}

bool self_sync_scrambler_is_valid(
	const struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config)
{
	return scrambler != NULL &&
	       self_sync_scrambler_config_is_valid(config) &&
	       (scrambler->history & ~history_mask(config)) == 0;
}

enum self_sync_scrambler_result
self_sync_scrambler_init(
	struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config)
{
	if (scrambler == NULL ||
	    !self_sync_scrambler_config_is_valid(config))
		return SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT;
	scrambler->history = 0;
	return SELF_SYNC_SCRAMBLER_OK;
}

static uint8_t transform_bit(uint32_t *history, uint32_t mask,
	unsigned int tap_a, unsigned int tap_b, uint8_t input, bool descramble)
{
	uint8_t output = input ^ (uint8_t)(
		((*history >> tap_a) ^ (*history >> tap_b)) & 1u);

	*history = ((*history << 1) | (descramble ? input : output)) & mask;
	return output;
}

static enum self_sync_scrambler_result transform_group(
	struct self_sync_scrambler *state,
	const struct self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output,
	bool descramble)
{
	struct self_sync_scrambler next;
	uint32_t mask;
	uint8_t transformed = 0;

	if (!self_sync_scrambler_is_valid(state, config) ||
	    bit_count == 0 ||
	    bit_count > SELF_SYNC_MAX_GROUP_BITS ||
	    ((unsigned int)input >> bit_count) != 0 || output == NULL)
		return SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT;
	next = *state;
	mask = history_mask(config);
	for (unsigned int bit = bit_count; bit > 0; --bit) {
		uint8_t input_bit = (uint8_t)((input >> (bit - 1u)) & 1u);
		uint8_t output_bit = transform_bit(&next.history, mask,
			config->tap_a_delay - 1u, config->tap_b_delay - 1u,
			input_bit, descramble);
		transformed = (uint8_t)((transformed << 1) | output_bit);
	}
	*state = next;
	*output = transformed;
	return SELF_SYNC_SCRAMBLER_OK;
}

enum self_sync_scrambler_result
self_sync_scramble_group(
	struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output)
{
	return transform_group(
		scrambler, config, input, bit_count, output, false);
}

enum self_sync_scrambler_result
self_sync_descramble_group(
	struct self_sync_scrambler *descrambler,
	const struct self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output)
{
	return transform_group(
		descrambler, config, input, bit_count, output, true);
}

static enum self_sync_scrambler_result transform_unpacked_bits(
	struct self_sync_scrambler *state,
	const struct self_sync_scrambler_config *config,
	const uint8_t *input, size_t bit_count, uint8_t *output, bool descramble)
{
	if (input == NULL || output == NULL || bit_count == 0 ||
	    !self_sync_scrambler_is_valid(state, config) ||
	    memory_regions_overlap(state, sizeof(*state), input, bit_count) ||
	    memory_regions_overlap(state, sizeof(*state), output, bit_count) ||
	    memory_regions_overlap(state, sizeof(*state), config, sizeof(*config)) ||
	    memory_regions_overlap(config, sizeof(*config), output, bit_count) ||
	    (input != output &&
	     memory_regions_overlap(input, bit_count, output, bit_count)) ||
	    !unpacked_bits_are_binary(input, bit_count))
		return SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT;

	uint32_t history = state->history;
	uint32_t mask = history_mask(config);
	unsigned int tap_a = config->tap_a_delay - 1u;
	unsigned int tap_b = config->tap_b_delay - 1u;

	for (size_t bit = 0; bit < bit_count; ++bit)
		output[bit] = transform_bit(&history, mask,
			tap_a, tap_b, input[bit], descramble);
	state->history = history;
	return SELF_SYNC_SCRAMBLER_OK;
}

enum self_sync_scrambler_result
self_sync_scramble_unpacked_bits(
	struct self_sync_scrambler *state,
	const struct self_sync_scrambler_config *config,
	const uint8_t *input, size_t bit_count, uint8_t *output)
{
	return transform_unpacked_bits(state, config,
		input, bit_count, output, false);
}

enum self_sync_scrambler_result
self_sync_descramble_unpacked_bits(
	struct self_sync_scrambler *state,
	const struct self_sync_scrambler_config *config,
	const uint8_t *input, size_t bit_count, uint8_t *output)
{
	return transform_unpacked_bits(state, config,
		input, bit_count, output, true);
}
