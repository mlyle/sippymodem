// SPDX-License-Identifier: BSD-3-Clause

#include "v8_framing.h"

#include <string.h>

enum {
	V8_SYNC_WINDOW_MASK =
		(1u << SIPPYMODEM_V8_PREAMBLE_BITS) - 1u,
	V8_SYNC_PATTERN =
		(((1u << SIPPYMODEM_V8_SYNC_MARK_BITS) - 1u) <<
		 SIPPYMODEM_V8_SYNC_PATTERN_BITS) |
		((1u << 4) - 1u),
	V8_FRAMED_OCTET_MASK =
		(1u << SIPPYMODEM_V8_FRAMED_OCTET_BITS) - 1u,
};

static size_t control_bit_count(
	const struct sippymodem_v8_control_generator *generator)
{
	return SIPPYMODEM_V8_PREAMBLE_BITS +
	       generator->information_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
}

static bool control_generator_is_valid(
	const struct sippymodem_v8_control_generator *generator)
{
	return generator != NULL && generator->information_count != 0 &&
	       generator->information_count <=
		       SIPPYMODEM_V8_MAX_INFORMATION_OCTETS &&
	       generator->bit_index < control_bit_count(generator);
}

enum sippymodem_v8_framing_result sippymodem_v8_control_generator_init(
	struct sippymodem_v8_control_generator *generator,
	const uint8_t *information, size_t information_count)
{
	struct sippymodem_v8_control_generator initialized = {
		.information_count = information_count,
	};

	if (generator == NULL || information == NULL ||
	    information_count == 0 ||
	    information_count > SIPPYMODEM_V8_MAX_INFORMATION_OCTETS)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	memcpy(initialized.information, information, information_count);
	if (!control_generator_is_valid(&initialized))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*generator = initialized;
	return SIPPYMODEM_V8_FRAMING_OK;
}

static uint8_t framed_octet_bit(uint8_t octet, size_t bit_index)
{
	if (bit_index == 0)
		return 0;
	if (bit_index == SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u)
		return 1;
	return (uint8_t)((octet >> (bit_index - 1u)) & 1u);
}

static uint8_t control_bit(
	const struct sippymodem_v8_control_generator *generator)
{
	size_t bit_index = generator->bit_index;

	if (bit_index < SIPPYMODEM_V8_SYNC_MARK_BITS)
		return 1;
	if (bit_index < SIPPYMODEM_V8_PREAMBLE_BITS)
		return bit_index >=
		       SIPPYMODEM_V8_PREAMBLE_BITS - 4u;
	bit_index -= SIPPYMODEM_V8_PREAMBLE_BITS;
	return framed_octet_bit(
		generator->information[
			bit_index / SIPPYMODEM_V8_FRAMED_OCTET_BITS],
		bit_index % SIPPYMODEM_V8_FRAMED_OCTET_BITS);
}

enum sippymodem_v8_framing_result sippymodem_v8_control_next_bit(
	struct sippymodem_v8_control_generator *generator, uint8_t *bit,
	bool *sequence_complete)
{
	struct sippymodem_v8_control_generator next;
	bool complete;
	uint8_t generated;

	if (!control_generator_is_valid(generator) || bit == NULL ||
	    sequence_complete == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	next = *generator;
	generated = control_bit(&next);
	complete = ++next.bit_index == control_bit_count(&next);
	if (complete)
		next.bit_index = 0;
	*generator = next;
	*bit = generated;
	*sequence_complete = complete;
	return SIPPYMODEM_V8_FRAMING_OK;
}

static bool control_receiver_is_valid(
	const struct sippymodem_v8_control_receiver *receiver)
{
	uint16_t octet_mask = 0;

	if (receiver == NULL ||
	    (receiver->sync_window & ~V8_SYNC_WINDOW_MASK) != 0 ||
	    receiver->sync_bit_count > SIPPYMODEM_V8_PREAMBLE_BITS ||
	    (receiver->sync_bit_count < SIPPYMODEM_V8_PREAMBLE_BITS &&
	     receiver->sync_window >= (1u << receiver->sync_bit_count)) ||
	    receiver->frame_bit_index >=
		    SIPPYMODEM_V8_FRAMED_OCTET_BITS)
		return false;
	if (!receiver->receiving)
		return receiver->frame_bit_index == 0 && receiver->octet == 0;
	if (receiver->frame_bit_index > 1)
		octet_mask = (uint16_t)(
			(1u << (receiver->frame_bit_index - 1u)) - 1u);
	return (receiver->octet & ~octet_mask) == 0;
}

enum sippymodem_v8_framing_result sippymodem_v8_control_receiver_init(
	struct sippymodem_v8_control_receiver *receiver)
{
	struct sippymodem_v8_control_receiver initialized = { 0 };

	if (receiver == NULL || !control_receiver_is_valid(&initialized))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V8_FRAMING_OK;
}

void sippymodem_v8_control_receiver_reset(
	struct sippymodem_v8_control_receiver *receiver)
{
	if (receiver != NULL)
		(void)sippymodem_v8_control_receiver_init(receiver);
}

static bool observe_sync_bit(
	struct sippymodem_v8_control_receiver *receiver, uint8_t bit)
{
	receiver->sync_window =
		((receiver->sync_window << 1) | bit) & V8_SYNC_WINDOW_MASK;
	if (receiver->sync_bit_count < SIPPYMODEM_V8_PREAMBLE_BITS)
		receiver->sync_bit_count++;
	return receiver->sync_bit_count == SIPPYMODEM_V8_PREAMBLE_BITS &&
	       receiver->sync_window == V8_SYNC_PATTERN;
}

static void abandon_control_frame(
	struct sippymodem_v8_control_receiver *receiver)
{
	receiver->frame_bit_index = 0;
	receiver->octet = 0;
	receiver->receiving = false;
}

enum sippymodem_v8_framing_result sippymodem_v8_control_receive_bit(
	struct sippymodem_v8_control_receiver *receiver, uint8_t bit,
	struct sippymodem_v8_control_event *event)
{
	struct sippymodem_v8_control_receiver next;
	struct sippymodem_v8_control_event emitted = {
		.type = SIPPYMODEM_V8_CONTROL_NO_EVENT,
	};

	if (!control_receiver_is_valid(receiver) || bit > 1 || event == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	next = *receiver;
	if (observe_sync_bit(&next, bit)) {
		next.frame_bit_index = 0;
		next.octet = 0;
		next.receiving = true;
		emitted.type = SIPPYMODEM_V8_CONTROL_SEQUENCE_START;
	} else if (next.receiving && next.frame_bit_index == 0) {
		if (bit == 0)
			next.frame_bit_index = 1;
		else
			abandon_control_frame(&next);
	} else if (next.receiving &&
		   next.frame_bit_index <
			   SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u) {
		next.octet |= (uint8_t)(
			bit << (next.frame_bit_index - 1u));
		next.frame_bit_index++;
	} else if (next.receiving) {
		if (bit == 1) {
			emitted.type = SIPPYMODEM_V8_CONTROL_INFORMATION_OCTET;
			emitted.octet = next.octet;
			next.frame_bit_index = 0;
			next.octet = 0;
		} else {
			abandon_control_frame(&next);
		}
	}
	if (!control_receiver_is_valid(&next))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return SIPPYMODEM_V8_FRAMING_OK;
}

enum sippymodem_v8_framing_result sippymodem_v8_cj_bit(
	size_t bit_index, uint8_t *bit)
{
	if (bit_index >= SIPPYMODEM_V8_CJ_BITS || bit == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*bit = bit_index % SIPPYMODEM_V8_FRAMED_OCTET_BITS ==
	       SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u;
	return SIPPYMODEM_V8_FRAMING_OK;
}

static bool cj_detector_is_valid(
	const struct sippymodem_v8_cj_detector *detector)
{
	return detector != NULL &&
	       (detector->window & ~V8_FRAMED_OCTET_MASK) == 0 &&
	       detector->window_bit_count <=
		       SIPPYMODEM_V8_FRAMED_OCTET_BITS &&
	       (detector->window_bit_count <
			SIPPYMODEM_V8_FRAMED_OCTET_BITS ?
			detector->window <
				(1u << detector->window_bit_count) : true) &&
	       detector->matched_octets <= SIPPYMODEM_V8_CJ_OCTETS &&
	       detector->continuation_bit_index <
		       SIPPYMODEM_V8_FRAMED_OCTET_BITS &&
	       (detector->matched_octets != 0 ||
		detector->continuation_bit_index == 0) &&
	       detector->detected ==
		       (detector->matched_octets == SIPPYMODEM_V8_CJ_OCTETS) &&
	       (!detector->detected ||
		detector->continuation_bit_index == 0);
}

enum sippymodem_v8_framing_result sippymodem_v8_cj_detector_init(
	struct sippymodem_v8_cj_detector *detector)
{
	struct sippymodem_v8_cj_detector initialized = { 0 };

	if (detector == NULL || !cj_detector_is_valid(&initialized))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*detector = initialized;
	return SIPPYMODEM_V8_FRAMING_OK;
}

void sippymodem_v8_cj_detector_reset(
	struct sippymodem_v8_cj_detector *detector)
{
	if (detector != NULL)
		(void)sippymodem_v8_cj_detector_init(detector);
}

static void observe_cj_window(
	struct sippymodem_v8_cj_detector *detector, uint8_t bit)
{
	detector->window = (uint16_t)(
		((detector->window << 1) | bit) & V8_FRAMED_OCTET_MASK);
	if (detector->window_bit_count < SIPPYMODEM_V8_FRAMED_OCTET_BITS)
		detector->window_bit_count++;
}

static bool cj_window_matches(
	const struct sippymodem_v8_cj_detector *detector)
{
	return detector->window_bit_count ==
		       SIPPYMODEM_V8_FRAMED_OCTET_BITS &&
	       detector->window == 1;
}

enum sippymodem_v8_framing_result sippymodem_v8_cj_detect_bit(
	struct sippymodem_v8_cj_detector *detector, uint8_t bit,
	bool *detected)
{
	struct sippymodem_v8_cj_detector next;

	if (!cj_detector_is_valid(detector) || bit > 1 || detected == NULL)
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	next = *detector;
	if (!next.detected) {
		bool expected;

		observe_cj_window(&next, bit);
		if (next.matched_octets == 0) {
			if (cj_window_matches(&next))
				next.matched_octets = 1;
		} else {
			expected = next.continuation_bit_index ==
				   SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u;
			if (bit == expected) {
				next.continuation_bit_index++;
				if (next.continuation_bit_index ==
				    SIPPYMODEM_V8_FRAMED_OCTET_BITS) {
					next.matched_octets++;
					next.continuation_bit_index = 0;
					next.detected =
						next.matched_octets ==
						SIPPYMODEM_V8_CJ_OCTETS;
				}
			} else {
				next.matched_octets =
					cj_window_matches(&next) ? 1 : 0;
				next.continuation_bit_index = 0;
			}
		}
	}
	if (!cj_detector_is_valid(&next))
		return SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT;
	*detector = next;
	*detected = next.detected;
	return SIPPYMODEM_V8_FRAMING_OK;
}

const char *sippymodem_v8_framing_result_name(
	enum sippymodem_v8_framing_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_FRAMING_OK:
		return "ok";
	case SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
