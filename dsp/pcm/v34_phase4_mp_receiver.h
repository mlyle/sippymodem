// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_MP_RECEIVER_H
#define SIPPYMODEM_V34_PHASE4_MP_RECEIVER_H

#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_mp.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase4_mp_receiver_result {
	SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK,
	SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase4_mp_receiver {
	struct sippymodem_v34_scrambler descrambler;
	struct sippymodem_v34_mp_receiver records;
	enum sippymodem_v34_station_role transmitting_role;
	enum sippymodem_v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	bool has_previous_symbol;
};

/* Initialize while the peer is transmitting Phase 4 TRN. The self-
 * synchronizing descrambler does not require the peer's scrambler history. */
enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receiver_init(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	enum sippymodem_v34_station_role transmitting_role,
	enum sippymodem_v34_constellation_size constellation);
bool sippymodem_v34_phase4_mp_receiver_is_valid(
	const struct sippymodem_v34_phase4_mp_receiver *receiver);

/* Consume one exact training-constellation decision. The first decision seeds
 * differential phase; later decisions continuously feed the MP record parser.
 * Rejected calls leave state and event unchanged. */
enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receive_decision(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct sippymodem_v34_mp_receive_event *event);

/* Slice one finite point that has already been equalized to the normalized
 * training constellation, then receive its labels transactionally. */
enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receive_point(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	const struct sippymodem_qam_point *point,
	struct sippymodem_v34_mp_receive_event *event);

const char *sippymodem_v34_phase4_mp_receiver_result_name(
	enum sippymodem_v34_phase4_mp_receiver_result result);

#endif
