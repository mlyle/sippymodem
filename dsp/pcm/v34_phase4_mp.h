// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_MP_H
#define V34_PHASE4_MP_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_mp.h"
#include "v34_phase4_trn.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_PHASE4_MP_TYPE0_4_POINT_SYMBOLS =
		V34_MP_TYPE0_FRAME_BITS / 2,
	V34_PHASE4_MP_TYPE0_16_POINT_SYMBOLS =
		V34_MP_TYPE0_FRAME_BITS / 4,
	V34_PHASE4_MP_TYPE1_4_POINT_SYMBOLS =
		V34_MP_TYPE1_FRAME_BITS / 2,
	V34_PHASE4_MP_TYPE1_16_POINT_SYMBOLS =
		V34_MP_TYPE1_FRAME_BITS / 4,
};

enum v34_phase4_mp_result {
	V34_PHASE4_MP_OK,
	V34_PHASE4_MP_RECORD_COMPLETE,
	V34_PHASE4_MP_RECORD_IN_PROGRESS,
	V34_PHASE4_MP_INVALID_ARGUMENT,
	V34_PHASE4_MP_INVALID_PARAMETERS,
};

struct v34_phase4_mp {
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES];
	struct v34_scrambler scrambler;
	enum v34_station_role role;
	enum v34_constellation_size constellation;
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
};

/* MP follows TRN without resetting its scrambler or differential encoder.
 * Each typed initializer owns a canonical encoded record. */
enum v34_phase4_mp_result
v34_phase4_mp_init_type0_from_trn(
	struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn,
	const struct v34_mp_parameters *parameters);
enum v34_phase4_mp_result
v34_phase4_mp_init_type1_from_trn(
	struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn,
	const struct v34_mp_type1_parameters *parameters);
bool v34_phase4_mp_is_valid(
	const struct v34_phase4_mp *transmitter);
bool v34_phase4_mp_record_is_complete(
	const struct v34_phase4_mp *transmitter);

/* Begin another MP or MP-prime only at a completed record boundary. Scrambler
 * history and differential phase continue across the boundary. */
enum v34_phase4_mp_result
v34_phase4_mp_start_next_type0_record(
	struct v34_phase4_mp *transmitter,
	const struct v34_mp_parameters *parameters);
enum v34_phase4_mp_result
v34_phase4_mp_start_next_type1_record(
	struct v34_phase4_mp *transmitter,
	const struct v34_mp_type1_parameters *parameters);

/* Emit one 2D symbol. record_complete accompanies the final symbol. Rejected
 * calls leave state and every output unchanged. */
enum v34_phase4_mp_result v34_phase4_mp_next(
	struct v34_phase4_mp *transmitter,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete);

#endif
