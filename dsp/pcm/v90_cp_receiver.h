// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_CP_RECEIVER_H
#define V90_CP_RECEIVER_H

#include "v90_cp.h"
#include "pcm_control_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_cp_receive_event_type {
	V90_CP_RECEIVE_NO_EVENT,
	V90_CP_RECEIVE_FRAME,
	V90_CP_RECEIVE_REJECTED,
};

struct v90_cp_receive_event {
	enum v90_cp_receive_event_type type;
	/* Accepted frames report OK. Rejected frames retain the codec result. */
	enum v90_cp_result frame_result;
	/* Number of candidate bits consumed before an accepted or rejected event. */
	size_t observed_bits;
	struct v90_cp_parameters parameters;
};

struct v90_cp_receiver {
	struct pcm_control_receiver wire;
};

/* Search a continuous descrambled bit stream for the 17-one prefix, derive
 * the variable record length from the six constellation indices and codec
 * flag, then emit only a validated typed frame. Accept a peer starting the
 * next sync after one or two fill zeros; the CRC and first fill zero remain
 * mandatory. observed_bits then includes that retained first sync bit.
 * A rejected call changes neither receiver state nor the caller's event. */
enum v90_cp_result v90_cp_receiver_init(
	struct v90_cp_receiver *receiver);
bool v90_cp_receiver_is_valid(
	const struct v90_cp_receiver *receiver);
enum v90_cp_result v90_cp_receiver_receive_bit(
	struct v90_cp_receiver *receiver, uint8_t bit,
	struct v90_cp_receive_event *event);

#endif /* V90_CP_RECEIVER_H */
