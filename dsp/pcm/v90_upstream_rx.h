// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_RX_H
#define V90_UPSTREAM_RX_H

#include "v90_phase4_e_line_rx.h"
#include "v90_upstream_data.h"

/* Start conventional upstream B1/data reception after the analogue modem's
 * E sequence. MP is the record sent by this digital modem. */
enum v90_upstream_data_result v90_upstream_rx_start_after_e(
	struct v34_data_line_receiver *receiver,
	const struct v90_phase4_e_line_rx *completed_e,
	const struct v90_mp_record *mp);

#endif /* V90_UPSTREAM_RX_H */
