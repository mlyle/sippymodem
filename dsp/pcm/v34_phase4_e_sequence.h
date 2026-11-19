// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_E_SEQUENCE_H
#define V34_PHASE4_E_SEQUENCE_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_phase4_mp.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE4_E_BITS = 20,
	V34_PHASE4_E_4_POINT_SYMBOLS =
		V34_PHASE4_E_BITS / 2,
	V34_PHASE4_E_16_POINT_SYMBOLS =
		V34_PHASE4_E_BITS / 4,
};

/* E is a run of twenty descrambled ones. MP framing contains zero start and
 * fill bits, so the same continuous decoder can search for either sequence. */
static inline uint8_t v34_phase4_e_extend_one_run(uint8_t current, uint8_t dibit)
{
	if (dibit == 3u) {
		unsigned int extended = (unsigned int)current + 2u;

		return extended < V34_PHASE4_E_BITS ?
			       (uint8_t)extended : V34_PHASE4_E_BITS;
	}
	return dibit == 1u ? 1u : 0u;
}

/* Decode one MP, CP or E decision into transmission-order bits. The
 * receiver supplies disjoint state and output within its own transaction.
 * MP and CP parsing and E detection use the same phase and descrambler. */
static inline bool v34_phase4_receive_symbol(struct v34_scrambler *descrambler,
	uint8_t *previous_rotation, uint8_t *ones, uint8_t quarter_point,
	uint8_t rotation, unsigned bit_count, uint8_t *bits)
{
	if (v34_descramble_control_symbol(descrambler, *previous_rotation,
		    rotation, quarter_point, bit_count, bits) != V34_FRAMING_OK)
		return false;
	for (; bit_count != 0u; bit_count -= 2u)
		*ones = v34_phase4_e_extend_one_run(*ones,
			(uint8_t)((*bits >> (bit_count - 2u)) & 3u));
	*previous_rotation = rotation;
	return true;
}

enum v34_phase4_e_sequence_result {
	V34_PHASE4_E_SEQUENCE_OK,
	V34_PHASE4_E_SEQUENCE_COMPLETE,
	V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT,
};

struct v34_phase4_e_sequence {
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t emitted_bits;
};

/* Start E from an arbitrary completed Phase 4 parameter record. The caller
 * supplies the continuous scrambler and differential histories at the exact
 * next-symbol boundary. */
enum v34_phase4_e_sequence_result v34_phase4_e_sequence_init(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_scrambler *scrambler,
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	uint8_t previous_clockwise_quarter_turns);

/* E follows a completed MP or MP-prime record without resetting the Phase 4
 * scrambler or differential encoder. */
enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_init_from_mp(
	struct v34_phase4_e_sequence *sequence,
	const struct v34_phase4_mp *mp);

bool v34_phase4_e_sequence_is_valid(
	const struct v34_phase4_e_sequence *sequence);
bool v34_phase4_e_sequence_is_complete(
	const struct v34_phase4_e_sequence *sequence);

/* Emit one symbol from the fixed 20-bit sequence. Owner and output regions
 * must be disjoint. Rejected calls leave state and every output unchanged. */
enum v34_phase4_e_sequence_result
v34_phase4_e_sequence_next(
	struct v34_phase4_e_sequence *sequence,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

#endif
