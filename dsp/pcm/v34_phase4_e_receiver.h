// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_E_RECEIVER_H
#define V34_PHASE4_E_RECEIVER_H

#include "v34_phase4_e_sequence.h"
#include "v34_phase4_mp_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase4_e_receiver_result {
	V34_PHASE4_E_RECEIVER_OK,
	V34_PHASE4_E_RECEIVER_COMPLETE,
	V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT,
};

struct v34_phase4_e_receiver {
	struct v34_scrambler descrambler;
	enum v34_station_role transmitting_role;
	enum v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t received_bits;
};

/* Search for E after an accepted MP-prime record while tolerating repeated
 * MP-prime records. The descrambler and differential phase remain continuous;
 * received_bits counts the current run of scrambled binary ones. */
enum v34_phase4_e_receiver_result
v34_phase4_e_receiver_search_from_mp(
	struct v34_phase4_e_receiver *receiver,
	const struct v34_phase4_mp_receiver *mp_receiver);
bool v34_phase4_e_receiver_is_valid(
	const struct v34_phase4_e_receiver *receiver);
bool v34_phase4_e_receiver_is_complete(
	const struct v34_phase4_e_receiver *receiver);

/* Consume one exact training-constellation decision. The final E symbol
 * returns COMPLETE. A rejected call leaves state unchanged. */
enum v34_phase4_e_receiver_result
v34_phase4_e_receive_decision(
	struct v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns);
#endif
