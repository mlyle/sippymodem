// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_E_RECEIVER_H
#define SIPPYMODEM_V34_PHASE4_E_RECEIVER_H

#include "sippymodem_qam.h"
#include "sippymodem_v34_phase4_e_sequence.h"
#include "sippymodem_v34_phase4_mp_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase4_e_receiver_result {
	SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK,
	SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE,
	SIPPYMODEM_V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH,
	SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase4_e_receiver {
	struct sippymodem_v34_scrambler descrambler;
	enum sippymodem_v34_station_role transmitting_role;
	enum sippymodem_v34_constellation_size constellation;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t received_bits;
};

/* E continues the peer's scrambler and differential phase immediately after
 * an accepted MP or MP-prime record. The startup controller owns that record
 * acceptance decision. */
enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receiver_init_from_mp(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	const struct sippymodem_v34_phase4_mp_receiver *mp_receiver);
bool sippymodem_v34_phase4_e_receiver_is_valid(
	const struct sippymodem_v34_phase4_e_receiver *receiver);
bool sippymodem_v34_phase4_e_receiver_is_complete(
	const struct sippymodem_v34_phase4_e_receiver *receiver);

/* Consume one exact training-constellation decision. The final E symbol
 * returns COMPLETE. A mismatch or rejected call leaves state unchanged. */
enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receive_decision(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns);

/* Slice one finite point already equalized to the normalized training
 * constellation, then receive its labels transactionally. */
enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receive_point(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	const struct sippymodem_qam_point *point);

const char *sippymodem_v34_phase4_e_receiver_result_name(
	enum sippymodem_v34_phase4_e_receiver_result result);

#endif
