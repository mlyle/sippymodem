// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_PP_RECEIVER_H
#define V34_PHASE3_PP_RECEIVER_H

#include "v34_equalizer_handoff.h"
#include "v34_phase3_pp_sequence.h"

#include <stdbool.h>

enum v34_phase3_pp_receiver_result {
	V34_PHASE3_PP_RECEIVER_OK,
	V34_PHASE3_PP_RECEIVER_COMPLETE,
	V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_PP_RECEIVER_INVALID_STATE,
	V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE,
};

/* Train a symbol-spaced equalizer from the finite PP sequence. The two
 * generated sequences track received targets and delayed adaptation targets;
 * no retained PP table is needed. After PP, the receiver accepts exactly the
 * equalizer delay of TRN lookahead required to adapt the final PP decisions. */
struct v34_phase3_pp_receiver {
	struct qam_equalizer equalizer;
	struct v34_phase3_pp_sequence received_targets;
	struct v34_phase3_pp_sequence adaptation_targets;
	double adaptation;
	bool seeded;
};

enum v34_phase3_pp_receiver_result v34_phase3_pp_receiver_init(
	struct v34_phase3_pp_receiver *receiver, double adaptation);

bool v34_phase3_pp_receiver_is_valid(
	const struct v34_phase3_pp_receiver *receiver);
bool v34_phase3_pp_receiver_is_complete(
	const struct v34_phase3_pp_receiver *receiver);

/* Accept one finite, carrier- and timing-recovered QAM point. PP targets are
 * generated internally. Once all PP decisions have adapted, COMPLETE calls
 * are non-consuming and leave the receiver unchanged. */
enum v34_phase3_pp_receiver_result v34_phase3_pp_receive_point(
	struct v34_phase3_pp_receiver *receiver,
	const struct qam_point *received);

/* Export the trained equalizer after the final delayed PP decision. Its input
 * history already contains the first V34_EQUALIZER_DECISION_DELAY TRN points,
 * so the next push produces the first TRN decision without replaying input. */
enum v34_phase3_pp_receiver_result v34_phase3_pp_receiver_handoff(
	const struct v34_phase3_pp_receiver *receiver,
	struct v34_equalizer_handoff *handoff);

#endif /* V34_PHASE3_PP_RECEIVER_H */
