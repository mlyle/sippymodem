// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_PHASE34_STATE_FIXTURE_H
#define V34_PHASE34_STATE_FIXTURE_H

#include <v34_phase34_receiver.h>
#include "v34_state_fixture.h"

static inline bool v34_phase3_handoffs_equal(
	const struct v34_phase3_decision_handoff *left,
	const struct v34_phase3_decision_handoff *right)
{
	return left->s.clockwise_quarter_turn_offset ==
			right->s.clockwise_quarter_turn_offset &&
	       left->s.s_errors == right->s.s_errors &&
	       left->s.s_bar_errors == right->s.s_bar_errors &&
	       v34_phase3_j_sequence_is_equal(&left->j.sequence, &right->j.sequence) &&
	       left->j.preceding_symbols == right->j.preceding_symbols &&
	       left->j.errors == right->j.errors;
}

static inline bool v34_phase34_receivers_equal(
	const struct v34_phase34_receiver *left,
	const struct v34_phase34_receiver *right)
{
	struct v34_phase34_receiver snapshots[2];

	if (left->stage != right->stage ||
	    !v34_phase3_handoffs_equal(&left->phase3_handoff, &right->phase3_handoff))
		return false;
	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	for (size_t index = 0; index < 2; ++index) {
		/* Handoff fields were compared above; discard only its copy here. */
		memset(&snapshots[index].phase3_handoff, 0,
		       sizeof(snapshots[index].phase3_handoff));
		if (snapshots[index].stage == V34_PHASE34_RECEIVER_DATA)
			v34_snapshot_data_decisions(&snapshots[index].semantic.data);
	}
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

#endif
