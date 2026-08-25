// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_MP_RECEIVER_H
#define V34_PHASE4_MP_RECEIVER_H

#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_mp.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase4_mp_receiver_result {
	V34_PHASE4_MP_RECEIVER_OK,
	V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT,
};

struct v34_phase4_mp_receiver {
	struct v34_scrambler descrambler;
	struct v34_mp_receiver records;
	enum v34_station_role transmitting_role;
	enum v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	bool has_previous_symbol;
};

/* Initialize while the peer is transmitting Phase 4 TRN. The self-
 * synchronizing descrambler does not require the peer's scrambler history. */
enum v34_phase4_mp_receiver_result
v34_phase4_mp_receiver_init(
	struct v34_phase4_mp_receiver *receiver,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation);
bool v34_phase4_mp_receiver_is_valid(
	const struct v34_phase4_mp_receiver *receiver);

/* Consume one exact training-constellation decision. The first decision seeds
 * differential phase; later decisions continuously feed the MP record parser.
 * Rejected calls leave state and event unchanged. */
enum v34_phase4_mp_receiver_result
v34_phase4_mp_receive_decision(
	struct v34_phase4_mp_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v34_mp_receive_event *event);

/* Slice one finite point that has already been equalized to the normalized
 * training constellation, then receive its labels transactionally. */
enum v34_phase4_mp_receiver_result
v34_phase4_mp_receive_point(
	struct v34_phase4_mp_receiver *receiver,
	const struct qam_point *point,
	struct v34_mp_receive_event *event);

#endif
