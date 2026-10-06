// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpt_receiver.h"

_Static_assert((unsigned)V92_CPT_MAX_FRAME_BITS == PCM_CONTROL_MAX_FRAME_BITS,
	"control receive storage must cover the complete frame");

bool v92_cpt_receiver_is_valid(
	const struct v92_cpt_receiver *receiver)
{
	return receiver != NULL &&
	       pcm_control_receiver_is_valid(&receiver->wire, PCM_CONTROL_CPT);
}

enum v92_cpt_result v92_cpt_receiver_init(
	struct v92_cpt_receiver *receiver)
{
	struct v92_cpt_receiver initialized = {
		.wire.stage = PCM_CONTROL_SEARCH_SYNC,
	};

	if (receiver == NULL || !v92_cpt_receiver_is_valid(&initialized))
		return V92_CPT_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_CPT_OK;
}

enum v92_cpt_result v92_cpt_receiver_begin_after_sync(
	struct v92_cpt_receiver *receiver)
{
	struct v92_cpt_receiver next;

	if (!v92_cpt_receiver_is_valid(receiver))
		return V92_CPT_INVALID_ARGUMENT;
	if (receiver->wire.stage != PCM_CONTROL_SEARCH_SYNC)
		return V92_CPT_INVALID_FRAME;
	next = *receiver;
	pcm_control_receiver_begin(&next.wire);
	if (!v92_cpt_receiver_is_valid(&next))
		return V92_CPT_INVALID_FRAME;
	*receiver = next;
	return V92_CPT_OK;
}

enum v92_cpt_result v92_cpt_receiver_receive_bit(
	struct v92_cpt_receiver *receiver, uint8_t bit,
	struct v92_cpt_receive_event *event)
{
	struct v92_cpt_receiver next;
	struct v92_cpt_receive_event emitted = { 0 };

	if (!v92_cpt_receiver_is_valid(receiver) || bit > 1u || event == NULL)
		return V92_CPT_INVALID_ARGUMENT;
	next = *receiver;
	struct pcm_control_receive_event received =
		pcm_control_receiver_receive_bit(&next.wire, PCM_CONTROL_CPT, bit);

	if (received.type != PCM_CONTROL_NO_EVENT) {
		emitted.frame_result = received.type == PCM_CONTROL_FRAME_REJECTED ?
			V92_CPT_INVALID_FRAME : v92_cpt_decode(
				next.wire.frame, sizeof(next.wire.frame),
				next.wire.frame_bit_count, &emitted.parameters);
		emitted.type = emitted.frame_result == V92_CPT_OK ?
			V92_CPT_RECEIVE_FRAME : V92_CPT_RECEIVE_REJECTED;
		emitted.observed_bits = received.observed_bits;
		pcm_control_receiver_clear(&next.wire, received.sync_one_count);
	}
	if (!v92_cpt_receiver_is_valid(&next))
		return V92_CPT_INVALID_FRAME;
	*receiver = next;
	*event = emitted;
	return V92_CPT_OK;
}
