// SPDX-License-Identifier: BSD-3-Clause

#include "v92_control_rx.h"

#include "packed_bits.h"

#include <string.h>

enum {
	SUVD_SYNC_BITS = 17,
	SUVD_START_BIT = 17,
	SUVD_MARKER_BIT = 18,
};

static bool stage_is_valid(enum v92_control_rx_stage stage)
{
	return stage == V92_CONTROL_RX_SEARCH_SYNC ||
	       stage == V92_CONTROL_RX_RECEIVE_SUVD;
}

static bool prefix_is_valid(const uint8_t *frame)
{
	for (size_t bit = 0u; bit < SUVD_SYNC_BITS; ++bit) {
		if (packed_msb_bit(frame, bit) == 0u)
			return false;
	}
	return packed_msb_bit(frame, SUVD_START_BIT) == 0u;
}

static uint8_t trailing_one_count(
	const uint8_t *frame, size_t bit_count)
{
	uint8_t count = 0u;

	while (bit_count != 0u && count < SUVD_SYNC_BITS) {
		if (packed_msb_bit(frame, bit_count - 1u) == 0u)
			break;
		count++;
		bit_count--;
	}
	return count;
}

bool v92_control_rx_is_valid(
	const struct v92_control_rx *receiver)
{
	size_t expected;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    receiver->sync_one_count > SUVD_SYNC_BITS)
		return false;
	expected = v92_suvd_frame_bits(receiver->training_frame_bits);
	if (expected == 0u || receiver->frame_bit_count != expected ||
	    receiver->bit_index > expected ||
	    !packed_msb_tail_is_zero(
		    receiver->frame, receiver->bit_index,
		    sizeof(receiver->frame)))
		return false;
	if (receiver->stage == V92_CONTROL_RX_SEARCH_SYNC)
		return receiver->bit_index == 0u;
	return receiver->bit_index >= SUVD_MARKER_BIT &&
	       receiver->bit_index < receiver->frame_bit_count &&
	       prefix_is_valid(receiver->frame) &&
	       (receiver->bit_index == SUVD_MARKER_BIT ||
		packed_msb_bit(receiver->frame, SUVD_MARKER_BIT) != 0u) &&
	       receiver->sync_one_count == trailing_one_count(
		       receiver->frame, receiver->bit_index);
}

enum v92_control_rx_result v92_control_rx_init(
	struct v92_control_rx *receiver, uint8_t training_frame_bits)
{
	struct v92_control_rx initialized = {
		.frame_bit_count = v92_suvd_frame_bits(training_frame_bits),
		.training_frame_bits = training_frame_bits,
		.stage = V92_CONTROL_RX_SEARCH_SYNC,
	};

	if (receiver == NULL)
		return V92_CONTROL_RX_INVALID_ARGUMENT;
	if (initialized.frame_bit_count == 0u)
		return V92_CONTROL_RX_INVALID_PARAMETERS;
	if (!v92_control_rx_is_valid(&initialized))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_CONTROL_RX_OK;
}

static void clear_candidate(struct v92_control_rx *receiver)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->bit_index = 0u;
	receiver->sync_one_count = 0u;
	receiver->stage = V92_CONTROL_RX_SEARCH_SYNC;
}

static void begin_candidate(struct v92_control_rx *receiver)
{
	clear_candidate(receiver);
	for (size_t bit = 0u; bit < SUVD_SYNC_BITS; ++bit)
		packed_msb_set_bit(receiver->frame, bit, true);
	receiver->bit_index = SUVD_MARKER_BIT;
	receiver->stage = V92_CONTROL_RX_RECEIVE_SUVD;
}

static bool advance_sync_run(
	struct v92_control_rx *receiver, uint8_t bit)
{
	if (bit != 0u) {
		if (receiver->sync_one_count < SUVD_SYNC_BITS)
			receiver->sync_one_count++;
		return false;
	}
	if (receiver->sync_one_count == SUVD_SYNC_BITS) {
		receiver->sync_one_count = 0u;
		return true;
	}
	receiver->sync_one_count = 0u;
	return false;
}

static void reject_candidate(
	struct v92_control_rx *receiver,
	struct v92_control_event *event,
	enum v92_suvd_result result)
{
	event->type = V92_CONTROL_EVENT_REJECTED;
	event->frame_result = result;
	clear_candidate(receiver);
}

static void finish_candidate(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	struct v92_suvd_parameters parameters;
	enum v92_suvd_result result = v92_suvd_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count, receiver->training_frame_bits,
		&parameters);

	event->frame_result = result;
	if (result == V92_SUVD_OK) {
		event->type = V92_CONTROL_EVENT_SUVD;
		event->parameters = parameters;
	} else {
		event->type = V92_CONTROL_EVENT_REJECTED;
	}
	clear_candidate(receiver);
}

static void receive_valid_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	if (advance_sync_run(receiver, bit)) {
		begin_candidate(receiver);
		return;
	}
	if (receiver->stage == V92_CONTROL_RX_SEARCH_SYNC)
		return;
	packed_msb_set_bit(receiver->frame, receiver->bit_index, bit != 0u);
	receiver->bit_index++;
	if (receiver->bit_index == SUVD_MARKER_BIT + 1u && bit == 0u) {
		reject_candidate(receiver, event, V92_SUVD_INVALID_FRAME);
		return;
	}
	if (receiver->bit_index == receiver->frame_bit_count)
		finish_candidate(receiver, event);
}

enum v92_control_rx_result v92_control_rx_receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	struct v92_control_rx next;
	struct v92_control_event emitted = { 0 };

	if (receiver == NULL || event == NULL || bit > 1u)
		return V92_CONTROL_RX_INVALID_ARGUMENT;
	if (!v92_control_rx_is_valid(receiver))
		return V92_CONTROL_RX_INVALID_STATE;
	next = *receiver;
	receive_valid_bit(&next, bit, &emitted);
	if (!v92_control_rx_is_valid(&next))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	*receiver = next;
	*event = emitted;
	return V92_CONTROL_RX_OK;
}

const char *v92_control_rx_result_name(
	enum v92_control_rx_result result)
{
	switch (result) {
	case V92_CONTROL_RX_OK:
		return "ok";
	case V92_CONTROL_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_CONTROL_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_CONTROL_RX_INVALID_STATE:
		return "invalid state";
	case V92_CONTROL_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
