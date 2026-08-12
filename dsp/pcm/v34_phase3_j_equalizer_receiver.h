// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_J_EQUALIZER_RECEIVER_H
#define V34_PHASE3_J_EQUALIZER_RECEIVER_H

#include "v34_equalizer_handoff.h"
#include "v34_phase3_j_receiver.h"

#include <stdbool.h>

enum v34_phase3_j_equalizer_receiver_result {
	V34_PHASE3_J_EQUALIZER_RECEIVER_OK,
	V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED,
	V34_PHASE3_J_EQUALIZER_RECEIVER_COMPLETE,
	V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_STATE,
	V34_PHASE3_J_EQUALIZER_RECEIVER_OUT_OF_RANGE,
	V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE,
};

/* Atomic point-domain handoff after J detection. The detector state is at the
 * end of its qualified J window. The equalizer has additionally retained the
 * following QAM_EQUALIZER_DELAY raw inputs, which have not produced decisions
 * yet; the next push therefore produces the first unprocessed J decision. */
struct v34_phase3_j_handoff {
	struct v34_equalizer_handoff equalizer;
	struct v34_phase3_j_detection detection;
};

bool v34_phase3_j_handoff_is_valid(
	const struct v34_phase3_j_handoff *handoff);

/* Continue from the PP receiver's delayed handoff. Each accepted point trains
 * the equalizer decision-directed on the four-point TRN/J constellation and
 * advances the generated J correlator by exactly one equalized decision. */
struct v34_phase3_j_equalizer_receiver {
	struct qam_equalizer equalizer;
	struct v34_phase3_j_receiver detector;
	double adaptation;
};

enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receiver_init(
	struct v34_phase3_j_equalizer_receiver *receiver,
	const struct v34_equalizer_handoff *pp_handoff,
	enum v34_station_role transmitting_role, double adaptation);

bool v34_phase3_j_equalizer_receiver_is_valid(
	const struct v34_phase3_j_equalizer_receiver *receiver);
bool v34_phase3_j_equalizer_receiver_is_complete(
	const struct v34_phase3_j_equalizer_receiver *receiver);

/* Accept one finite carrier- and timing-recovered QAM point. Rejected,
 * complete, and saturated calls consume nothing. */
enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receive_point(
	struct v34_phase3_j_equalizer_receiver *receiver,
	const struct qam_point *received);

enum v34_phase3_j_equalizer_receiver_result
v34_phase3_j_equalizer_receiver_handoff(
	const struct v34_phase3_j_equalizer_receiver *receiver,
	struct v34_phase3_j_handoff *handoff);

const char *v34_phase3_j_equalizer_receiver_result_name(
	enum v34_phase3_j_equalizer_receiver_result result);

#endif /* V34_PHASE3_J_EQUALIZER_RECEIVER_H */
