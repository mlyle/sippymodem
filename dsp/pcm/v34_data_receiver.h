// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_RECEIVER_H
#define V34_DATA_RECEIVER_H

#include "v34_mapping_frame_receiver.h"

#include <stdbool.h>

enum v34_data_receiver_result {
	V34_DATA_RECEIVER_OK,
	V34_DATA_RECEIVER_B1_IN_PROGRESS,
	V34_DATA_RECEIVER_B1_COMPLETE,
	V34_DATA_RECEIVER_INVALID_ARGUMENT,
};

struct v34_data_receiver {
	struct v34_frame_schedule schedule;
	struct v34_scrambler descrambler;
	struct v34_4d_decoder mapping_decoder;
	enum v34_station_role transmitting_role;
	/* Retained after B1 for call recording and receiver diagnostics. */
	uint16_t b1_received_bits;
	uint16_t b1_error_bits;
	bool b1_active;
};

/* Begin at the first mapping frame of the peer's B1 data frame. The data-mode
 * scrambler and mapping decoder start with fresh histories; they do not inherit
 * Phase 4 E state. Rejected calls leave the destination unchanged. */
enum v34_data_receiver_result v34_data_receiver_init(
	struct v34_data_receiver *receiver,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	enum v34_station_role transmitting_role);

bool v34_data_receiver_is_valid(
	const struct v34_data_receiver *receiver);
bool v34_data_receiver_is_receiving_b1(
	const struct v34_data_receiver *receiver);

/* Receive one exact decided mapping frame. B1 results identify and expose its
 * known all-one payload while counting non-one decisions without stalling a
 * noisy live stream; OK exposes ordinary data. A completed ordinary data frame
 * advances automatically, including superframe wraparound. State and
 * destination are unchanged on rejection. */
enum v34_data_receiver_result
v34_data_receiver_receive_mapping_frame(
	struct v34_data_receiver *receiver,
	const struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME],
	struct v34_received_mapping_frame *received);

#endif /* V34_DATA_RECEIVER_H */
