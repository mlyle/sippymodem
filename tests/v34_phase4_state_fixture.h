// SPDX-License-Identifier: BSD-3-Clause
#ifndef V34_PHASE4_STATE_FIXTURE_H
#define V34_PHASE4_STATE_FIXTURE_H

#include <v34_phase4_exchange.h>
#include "v34_phase34_state_fixture.h"
#include "v34_startup_state_fixture.h"

static inline bool v34_training_transmitters_equal(
	const struct v34_training_transmitter *left,
	const struct v34_training_transmitter *right)
{
	struct v34_training_transmitter snapshots[2];

	if (!v34_training_lines_equal(&left->line, &right->line))
		return false;
	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	memset(&snapshots[0].line, 0, sizeof(snapshots[0].line));
	memset(&snapshots[1].line, 0, sizeof(snapshots[1].line));
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

static inline bool v34_phase4_exchanges_equal(
	const struct v34_phase4_exchange *left, const struct v34_phase4_exchange *right)
{
	struct v34_phase4_exchange snapshots[2];

	if (left->transmit_data_active != right->transmit_data_active ||
	    !v34_phase34_receivers_equal(&left->receive, &right->receive) ||
	    !v34_data_startups_equal(&left->data_startup, &right->data_startup))
		return false;
	if (left->transmit_data_active ?
	    !v34_data_streams_equal(&left->transmit.data, &right->transmit.data) :
	    !v34_training_transmitters_equal(&left->transmit.training, &right->transmit.training))
		return false;
	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	for (size_t index = 0; index < 2; ++index) {
		/* Each active child was compared above; retain all exchange controls. */
		memset(&snapshots[index].transmit, 0, sizeof(snapshots[index].transmit));
		memset(&snapshots[index].receive, 0, sizeof(snapshots[index].receive));
		memset(&snapshots[index].data_startup, 0, sizeof(snapshots[index].data_startup));
	}
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

#endif
