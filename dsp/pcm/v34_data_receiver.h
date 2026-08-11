// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_RECEIVER_H
#define SIPPYMODEM_V34_DATA_RECEIVER_H

#include "v34_mapping_frame_receiver.h"

#include <stdbool.h>

enum sippymodem_v34_data_receiver_result {
	SIPPYMODEM_V34_DATA_RECEIVER_OK,
	SIPPYMODEM_V34_DATA_RECEIVER_B1_IN_PROGRESS,
	SIPPYMODEM_V34_DATA_RECEIVER_B1_COMPLETE,
	SIPPYMODEM_V34_DATA_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v34_data_receiver {
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler descrambler;
	struct sippymodem_v34_4d_decoder mapping_decoder;
	enum sippymodem_v34_station_role transmitting_role;
	/* Retained after B1 for call recording and receiver diagnostics. */
	uint16_t b1_received_bits;
	uint16_t b1_error_bits;
	bool b1_active;
};

/* Begin at the first mapping frame of the peer's B1 data frame. The data-mode
 * scrambler and mapping decoder start with fresh histories; they do not inherit
 * Phase 4 E state. Rejected calls leave the destination unchanged. */
enum sippymodem_v34_data_receiver_result sippymodem_v34_data_receiver_init(
	struct sippymodem_v34_data_receiver *receiver,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	enum sippymodem_v34_station_role transmitting_role);

bool sippymodem_v34_data_receiver_is_valid(
	const struct sippymodem_v34_data_receiver *receiver);
bool sippymodem_v34_data_receiver_is_receiving_b1(
	const struct sippymodem_v34_data_receiver *receiver);

/* Receive one exact decided mapping frame. B1 results identify and expose its
 * known all-one payload while counting non-one decisions without stalling a
 * noisy live stream; OK exposes ordinary data. A completed ordinary data frame
 * advances automatically, including superframe wraparound. State and
 * destination are unchanged on rejection. */
enum sippymodem_v34_data_receiver_result
sippymodem_v34_data_receiver_receive_mapping_frame(
	struct sippymodem_v34_data_receiver *receiver,
	const struct sippymodem_v34_decided_4d_interval
		decided[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct sippymodem_v34_received_mapping_frame *received);

const char *sippymodem_v34_data_receiver_result_name(
	enum sippymodem_v34_data_receiver_result result);

#endif /* SIPPYMODEM_V34_DATA_RECEIVER_H */
