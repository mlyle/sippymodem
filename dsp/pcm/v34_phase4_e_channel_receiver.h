// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_E_CHANNEL_RECEIVER_H
#define SIPPYMODEM_V34_PHASE4_E_CHANNEL_RECEIVER_H

#include "qam_gain.h"
#include "v34_phase4_e_receiver.h"
#include "v34_phase4_mp_channel_receiver.h"

#include <stdbool.h>

struct sippymodem_v34_phase4_e_channel_receiver {
	struct sippymodem_v34_phase4_e_receiver symbol_receiver;
	struct sippymodem_qam_gain gain;
};

/* Continue the normalized receiver and flat-channel estimate immediately
 * after an accepted MP-prime record. A later FIR equalizer can feed
 * normalized decisions directly to symbol_receiver. */
enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
	struct sippymodem_v34_phase4_e_channel_receiver *receiver,
	const struct sippymodem_v34_phase4_mp_channel_receiver *mp_receiver);
bool sippymodem_v34_phase4_e_channel_receiver_is_valid(
	const struct sippymodem_v34_phase4_e_channel_receiver *receiver);

/* Equalize, slice, and decision-direct one raw E point transactionally. The
 * final E point returns COMPLETE. */
enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_channel_receive_point(
	struct sippymodem_v34_phase4_e_channel_receiver *receiver,
	const struct sippymodem_qam_point *received);

#endif
