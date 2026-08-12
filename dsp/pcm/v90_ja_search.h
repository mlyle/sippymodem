// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_JA_SEARCH_H
#define V90_JA_SEARCH_H

#include "v34_framing.h"
#include "v90_dil.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_JA_SEARCH_WARMUP_BITS = V34_SCRAMBLER_BITS,
};

enum v90_ja_search_result {
	V90_JA_SEARCH_OK,
	V90_JA_SEARCH_DETECTED,
	V90_JA_SEARCH_REJECTED,
	V90_JA_SEARCH_UNSUPPORTED,
	V90_JA_SEARCH_COMPLETE,
	V90_JA_SEARCH_INVALID_ARGUMENT,
};

/* Search hard four-point phase decisions for a repeated Ja descriptor. The
 * GPA descrambler is deliberately initialized without the remote state. TRN
 * is not differentially encoded, so one Ja repetition may be consumed while
 * differential decoding begins and the first 23 Ja bits self-synchronize. */
struct v90_ja_search {
	struct v34_scrambler descrambler;
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint16_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t warmup_bits;
	uint8_t consecutive_ones;
	bool has_previous_rotation;
	bool collecting;
	bool complete;
};

enum v90_ja_search_result v90_ja_search_init(
	struct v90_ja_search *search);
bool v90_ja_search_is_valid(const struct v90_ja_search *search);
bool v90_ja_search_is_complete(const struct v90_ja_search *search);

/* Consume one symbol's absolute clockwise quarter-turn decision. Rejected
 * descriptors are discarded while the synchronized descrambler continues,
 * allowing a later repetition to recover without reacquiring TRN. */
enum v90_ja_search_result v90_ja_search_receive(
	struct v90_ja_search *search,
	uint8_t clockwise_quarter_turns);

const char *v90_ja_search_result_name(
	enum v90_ja_search_result result);

#endif /* V90_JA_SEARCH_H */
