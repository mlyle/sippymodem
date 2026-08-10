// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SELF_SYNC_SCRAMBLER_H
#define SIPPYMODEM_SELF_SYNC_SCRAMBLER_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_SELF_SYNC_MAX_HISTORY_BITS = 32,
	SIPPYMODEM_SELF_SYNC_MAX_GROUP_BITS = 8,
};

/* History bit zero is the most recent wire bit; tap delays start at one. */
struct sippymodem_self_sync_scrambler_config {
	uint8_t history_bits;
	uint8_t tap_a_delay;
	uint8_t tap_b_delay;
};

struct sippymodem_self_sync_scrambler {
	uint32_t history;
};

enum sippymodem_self_sync_scrambler_result {
	SIPPYMODEM_SELF_SYNC_SCRAMBLER_OK,
	SIPPYMODEM_SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT,
};

bool sippymodem_self_sync_scrambler_config_is_valid(
	const struct sippymodem_self_sync_scrambler_config *config);
bool sippymodem_self_sync_scrambler_is_valid(
	const struct sippymodem_self_sync_scrambler *scrambler,
	const struct sippymodem_self_sync_scrambler_config *config);

enum sippymodem_self_sync_scrambler_result
sippymodem_self_sync_scrambler_init(
	struct sippymodem_self_sync_scrambler *scrambler,
	const struct sippymodem_self_sync_scrambler_config *config);

/*
 * Process between one and eight bits, most-significant bit first.  Scrambling
 * remembers output bits and descrambling remembers input bits.  Rejected calls
 * leave both the state and output untouched.
 */
enum sippymodem_self_sync_scrambler_result
sippymodem_self_sync_scramble_group(
	struct sippymodem_self_sync_scrambler *scrambler,
	const struct sippymodem_self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output);
enum sippymodem_self_sync_scrambler_result
sippymodem_self_sync_descramble_group(
	struct sippymodem_self_sync_scrambler *descrambler,
	const struct sippymodem_self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output);

const char *sippymodem_self_sync_scrambler_result_name(
	enum sippymodem_self_sync_scrambler_result result);

#endif
