// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_MP_CHANNEL_RECEIVER_H
#define SIPPYMODEM_V34_PHASE4_MP_CHANNEL_RECEIVER_H

#include "sippymodem_qam_gain.h"
#include "sippymodem_v34_phase4_mp_receiver.h"

#include <stdbool.h>

/* Phase 4 MP reception after carrier recovery, represented by one flat
 * complex channel gain. A later adaptive FIR equalizer can supply normalized
 * points directly to symbol_receiver instead. */
struct sippymodem_v34_phase4_mp_channel_receiver {
	struct sippymodem_v34_phase4_mp_receiver symbol_receiver;
	struct sippymodem_qam_gain gain;
};

/* Seed the channel from the final known Phase 4 TRN point. Rejected calls
 * leave receiver unchanged. */
enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_channel_receiver_init(
	struct sippymodem_v34_phase4_mp_channel_receiver *receiver,
	enum sippymodem_v34_station_role transmitting_role,
	enum sippymodem_v34_constellation_size constellation,
	const struct sippymodem_qam_point *received_reference,
	const struct sippymodem_qam_point *transmitted_reference);
bool sippymodem_v34_phase4_mp_channel_receiver_is_valid(
	const struct sippymodem_v34_phase4_mp_channel_receiver *receiver);

/* Equalize, slice, and decision-direct the gain estimate for one received
 * point. Rejected calls leave receiver and event unchanged. */
enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_channel_receive_point(
	struct sippymodem_v34_phase4_mp_channel_receiver *receiver,
	const struct sippymodem_qam_point *received,
	struct sippymodem_v34_mp_receive_event *event);

#endif
