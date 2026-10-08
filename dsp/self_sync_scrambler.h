// SPDX-License-Identifier: BSD-3-Clause

#ifndef SELF_SYNC_SCRAMBLER_H
#define SELF_SYNC_SCRAMBLER_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SELF_SYNC_MAX_HISTORY_BITS = 32,
	SELF_SYNC_MAX_GROUP_BITS = 8,
};

/* History bit zero is the most recent wire bit; tap delays start at one. */
struct self_sync_scrambler_config {
	uint8_t history_bits;
	uint8_t tap_a_delay;
	uint8_t tap_b_delay;
};

/* V.32 clause 4 and V.34 clause 7 share these 23-bit polynomials. */
extern const struct self_sync_scrambler_config self_sync_scrambler_gpc;
extern const struct self_sync_scrambler_config self_sync_scrambler_gpa;

struct self_sync_scrambler {
	uint32_t history;
};

enum self_sync_scrambler_result {
	SELF_SYNC_SCRAMBLER_OK,
	SELF_SYNC_SCRAMBLER_INVALID_ARGUMENT,
};

bool self_sync_scrambler_config_is_valid(
	const struct self_sync_scrambler_config *config);
bool self_sync_scrambler_is_valid(
	const struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config);

enum self_sync_scrambler_result
self_sync_scrambler_init(
	struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config);

/*
 * Process between one and eight bits, most-significant bit first.  Scrambling
 * remembers output bits and descrambling remembers input bits.  Rejected calls
 * leave both the state and output untouched.
 */
enum self_sync_scrambler_result
self_sync_scramble_group(
	struct self_sync_scrambler *scrambler,
	const struct self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output);
enum self_sync_scrambler_result
self_sync_descramble_group(
	struct self_sync_scrambler *descrambler,
	const struct self_sync_scrambler_config *config,
	uint8_t input, unsigned int bit_count, uint8_t *output);


#endif
