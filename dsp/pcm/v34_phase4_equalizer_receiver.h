// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_EQUALIZER_RECEIVER_H
#define V34_PHASE4_EQUALIZER_RECEIVER_H

#include "qam_equalizer.h"
#include "v34_phase4_e_receiver.h"

#include <stdbool.h>

enum v34_phase4_equalizer_receiver_stage {
	V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP,
	V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E,
	V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE,
};

enum v34_phase4_equalizer_receiver_result {
	V34_PHASE4_EQUALIZER_RECEIVER_OK,
	/* The equalizer accepted a point but has not produced a delayed output. */
	V34_PHASE4_EQUALIZER_RECEIVER_WARMING,
	V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL,
	/* A complete MP record is waiting for a continue-or-E decision. */
	V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY,
	V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE,
	V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE,
	V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE,
	V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE,
	V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH,
};

/* Own the symbol-spaced equalizer and Phase 4 control parser after carrier
 * and timing recovery. The equalizer's decision delay naturally retains the
 * first QAM_EQUALIZER_DELAY symbols following E; those are the first B1
 * inputs and must carry into the data receiver with this equalizer. */
struct v34_phase4_equalizer_receiver {
	struct qam_equalizer equalizer;
	struct v34_phase4_mp_receiver mp_receiver;
	struct v34_phase4_e_receiver e_receiver;
	double adaptation;
	enum v34_phase4_equalizer_receiver_stage stage;
	bool record_pending;
	bool pending_record_is_valid;
};

/* Begin while the peer is still sending Phase 4 TRN. The trained equalizer
 * is copied and any immediate, unowned adaptation opportunity is closed. */
enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_init(
	struct v34_phase4_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation, double adaptation);

bool v34_phase4_equalizer_receiver_is_valid(
	const struct v34_phase4_equalizer_receiver *receiver);

/* A record event pauses input so the startup controller cannot accidentally
 * parse the first symbols of E as another MP record. Continue after an MP or
 * rejected record, or enter E after a validated final MP-prime record. */
enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_continue_mp(
	struct v34_phase4_equalizer_receiver *receiver);
enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_start_e(
	struct v34_phase4_equalizer_receiver *receiver);

/* Accept one finite, carrier- and timing-recovered QAM point. `event` changes
 * only when RECORD_READY is returned. Rejected calls and sequence mismatches
 * leave receiver and event unchanged so a point-domain caller may retry. */
enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receive_point(
	struct v34_phase4_equalizer_receiver *receiver,
	const struct qam_point *received,
	struct v34_mp_receive_event *event);

const char *v34_phase4_equalizer_receiver_result_name(
	enum v34_phase4_equalizer_receiver_result result);

#endif /* V34_PHASE4_EQUALIZER_RECEIVER_H */
