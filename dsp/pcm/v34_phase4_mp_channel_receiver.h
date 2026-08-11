// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_MP_CHANNEL_RECEIVER_H
#define V34_PHASE4_MP_CHANNEL_RECEIVER_H

#include "qam_gain.h"
#include "v34_phase4_mp_receiver.h"

#include <stdbool.h>

/* Phase 4 MP reception after carrier recovery, represented by one flat
 * complex channel gain. A later adaptive FIR equalizer can supply normalized
 * points directly to symbol_receiver instead. */
struct v34_phase4_mp_channel_receiver {
	struct v34_phase4_mp_receiver symbol_receiver;
	struct qam_gain gain;
};

/* Seed the channel from the final known Phase 4 TRN point. Rejected calls
 * leave receiver unchanged. */
enum v34_phase4_mp_receiver_result
v34_phase4_mp_channel_receiver_init(
	struct v34_phase4_mp_channel_receiver *receiver,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference);
bool v34_phase4_mp_channel_receiver_is_valid(
	const struct v34_phase4_mp_channel_receiver *receiver);

/* Equalize, slice, and decision-direct the gain estimate for one received
 * point. Rejected calls leave receiver and event unchanged. */
enum v34_phase4_mp_receiver_result
v34_phase4_mp_channel_receive_point(
	struct v34_phase4_mp_channel_receiver *receiver,
	const struct qam_point *received,
	struct v34_mp_receive_event *event);

#endif
