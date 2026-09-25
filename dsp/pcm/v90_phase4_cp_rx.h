// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_CP_RX_H
#define V90_PHASE4_CP_RX_H

#include "qam.h"
#include "v34_constellation.h"
#include "v34_framing.h"
#include "v34_station.h"
#include "v90_cp_receiver.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase4_cp_rx {
	struct v34_scrambler descrambler;
	struct v90_cp_receiver records;
	enum v34_station_role transmitting_role;
	enum v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
};

/* Initialize immediately before the peer's first CPt. That sequence starts
 * with zero scrambler history and zero differential phase, so the first
 * decision carries data and is not discarded as a phase seed. */
enum v90_cp_result v90_phase4_cp_rx_init(
	struct v90_phase4_cp_rx *rx,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation);
bool v90_phase4_cp_rx_is_valid(
	const struct v90_phase4_cp_rx *rx);
/* Compare logical receiver state, not the padding in copied C structs. */
bool v90_phase4_cp_rx_equal(
	const struct v90_phase4_cp_rx *left,
	const struct v90_phase4_cp_rx *right);

/* Consume one exact training-constellation decision and emit at most one
 * alignment-free CP parser event. Rejected calls leave state and event
 * unchanged; accepted calls preserve modulation history across records. */
enum v90_cp_result v90_phase4_cp_rx_receive_decision(
	struct v90_phase4_cp_rx *rx,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v90_cp_receive_event *event);

/* Slice one finite, equalized training-constellation point and receive its
 * labels transactionally. */
enum v90_cp_result v90_phase4_cp_rx_receive_point(
	struct v90_phase4_cp_rx *rx,
	const struct qam_point *point,
	struct v90_cp_receive_event *event);

#endif /* V90_PHASE4_CP_RX_H */
