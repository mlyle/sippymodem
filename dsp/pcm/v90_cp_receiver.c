// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp_receiver.h"

_Static_assert((unsigned)V90_CP_MAX_FRAME_BITS == PCM_CONTROL_MAX_FRAME_BITS,
	"control receive storage must cover the complete frame");

bool v90_cp_receiver_is_valid(
	const struct v90_cp_receiver *receiver)
{
	return receiver != NULL &&
	       pcm_control_receiver_is_valid(&receiver->wire, PCM_CONTROL_CP);
}

enum v90_cp_result v90_cp_receiver_init(
	struct v90_cp_receiver *receiver)
{
	struct v90_cp_receiver initialized = {
		.wire.stage = PCM_CONTROL_SEARCH_SYNC,
	};

	if (receiver == NULL || !v90_cp_receiver_is_valid(&initialized))
		return V90_CP_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_CP_OK;
}

enum v90_cp_result v90_cp_receiver_receive_bit(
	struct v90_cp_receiver *receiver, uint8_t bit,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_receiver next;
	struct v90_cp_receive_event emitted = { 0 };

	if (!v90_cp_receiver_is_valid(receiver) || bit > 1u || event == NULL)
		return V90_CP_INVALID_ARGUMENT;
	next = *receiver;
	struct pcm_control_receive_event received =
		pcm_control_receiver_receive_bit(&next.wire, PCM_CONTROL_CP, bit);

	if (received.type != PCM_CONTROL_NO_EVENT) {
		emitted.frame_result = received.type == PCM_CONTROL_FRAME_REJECTED ?
			V90_CP_INVALID_FRAME : v90_cp_decode(
				next.wire.frame, sizeof(next.wire.frame),
				next.wire.frame_bit_count, &emitted.parameters);
		emitted.type = emitted.frame_result == V90_CP_OK ?
			V90_CP_RECEIVE_FRAME : V90_CP_RECEIVE_REJECTED;
		emitted.observed_bits = received.observed_bits;
		pcm_control_receiver_clear(&next.wire, received.sync_one_count);
	}
	if (!v90_cp_receiver_is_valid(&next))
		return V90_CP_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V90_CP_OK;
}
