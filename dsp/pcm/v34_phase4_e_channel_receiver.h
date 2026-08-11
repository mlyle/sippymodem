// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_E_CHANNEL_RECEIVER_H
#define V34_PHASE4_E_CHANNEL_RECEIVER_H

#include "qam_gain.h"
#include "v34_phase4_e_receiver.h"
#include "v34_phase4_mp_channel_receiver.h"

#include <stdbool.h>

struct v34_phase4_e_channel_receiver {
	struct v34_phase4_e_receiver symbol_receiver;
	struct qam_gain gain;
};

/* Continue the normalized receiver and flat-channel estimate immediately
 * after an accepted MP-prime record. A later FIR equalizer can feed
 * normalized decisions directly to symbol_receiver. */
enum v34_phase4_e_receiver_result
v34_phase4_e_channel_receiver_init_from_mp(
	struct v34_phase4_e_channel_receiver *receiver,
	const struct v34_phase4_mp_channel_receiver *mp_receiver);
bool v34_phase4_e_channel_receiver_is_valid(
	const struct v34_phase4_e_channel_receiver *receiver);

/* Equalize, slice, and decision-direct one raw E point transactionally. The
 * final E point returns COMPLETE. */
enum v34_phase4_e_receiver_result
v34_phase4_e_channel_receive_point(
	struct v34_phase4_e_channel_receiver *receiver,
	const struct qam_point *received);

#endif
