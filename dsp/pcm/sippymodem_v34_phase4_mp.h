// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_MP_H
#define SIPPYMODEM_V34_PHASE4_MP_H

#include "sippymodem_qam.h"
#include "sippymodem_v34_constellation.h"
#include "sippymodem_v34_framing.h"
#include "sippymodem_v34_mp.h"
#include "sippymodem_v34_phase4_trn.h"
#include "sippymodem_v34_station.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE4_MP_TYPE0_4_POINT_SYMBOLS =
		SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS / 2,
	SIPPYMODEM_V34_PHASE4_MP_TYPE0_16_POINT_SYMBOLS =
		SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS / 4,
	SIPPYMODEM_V34_PHASE4_MP_TYPE1_4_POINT_SYMBOLS =
		SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS / 2,
	SIPPYMODEM_V34_PHASE4_MP_TYPE1_16_POINT_SYMBOLS =
		SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS / 4,
};

enum sippymodem_v34_phase4_mp_result {
	SIPPYMODEM_V34_PHASE4_MP_OK,
	SIPPYMODEM_V34_PHASE4_MP_RECORD_COMPLETE,
	SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS,
	SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS,
};

struct sippymodem_v34_phase4_mp {
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES];
	struct sippymodem_v34_scrambler scrambler;
	enum sippymodem_v34_station_role role;
	enum sippymodem_v34_constellation_size constellation;
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t previous_clockwise_quarter_turns;
};

/* MP follows TRN without resetting its scrambler or differential encoder.
 * Each typed initializer owns a canonical encoded record. */
enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_init_type0_from_trn(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct sippymodem_v34_mp_parameters *parameters);
enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_init_type1_from_trn(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct sippymodem_v34_mp_type1_parameters *parameters);
bool sippymodem_v34_phase4_mp_is_valid(
	const struct sippymodem_v34_phase4_mp *transmitter);
bool sippymodem_v34_phase4_mp_record_is_complete(
	const struct sippymodem_v34_phase4_mp *transmitter);

/* Begin another MP or MP-prime only at a completed record boundary. Scrambler
 * history and differential phase continue across the boundary. */
enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_start_next_type0_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_mp_parameters *parameters);
enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_start_next_type1_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_mp_type1_parameters *parameters);

/* Emit one 2D symbol. record_complete accompanies the final symbol. Rejected
 * calls leave state and every output unchanged. */
enum sippymodem_v34_phase4_mp_result sippymodem_v34_phase4_mp_next(
	struct sippymodem_v34_phase4_mp *transmitter,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete);

const char *sippymodem_v34_phase4_mp_result_name(
	enum sippymodem_v34_phase4_mp_result result);

#endif
