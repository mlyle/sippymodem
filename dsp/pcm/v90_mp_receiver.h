// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_MP_RECEIVER_H
#define V90_MP_RECEIVER_H

#include "v90_mp.h"
#include "pcm_mp_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_mp_receiver_result {
	V90_MP_RECEIVER_OK,
	V90_MP_RECEIVER_INVALID_ARGUMENT,
	V90_MP_RECEIVER_INVALID_PARAMETERS,
	V90_MP_RECEIVER_INVALID_STATE,
	V90_MP_RECEIVER_INTERNAL_ERROR,
};

enum v90_mp_event_type {
	V90_MP_EVENT_NONE,
	V90_MP_EVENT_TYPE0,
	V90_MP_EVENT_TYPE1,
	V90_MP_EVENT_REJECTED,
};

struct v90_mp_event {
	enum v90_mp_event_type type;
	/* OK for accepted records; the decoder failure for rejected records. */
	enum v90_mp_result frame_result;
	struct v90_mp_record record;
};

/* Incremental detector for one continuous, descrambled Phase 4 bit stream.
 * The saturated sync count allows an arbitrary TRN2d suffix of ones to flow
 * directly into the first MP start bit. */
struct v90_mp_receiver {
	struct pcm_mp_receiver wire;
	uint8_t training_frame_bits;
};

enum v90_mp_receiver_result v90_mp_receiver_init(
	struct v90_mp_receiver *receiver, uint8_t training_frame_bits);
bool v90_mp_receiver_is_valid(
	const struct v90_mp_receiver *receiver);

/* Consume exactly one binary decision. Valid input always advances state;
 * rejected wire records are reported as events rather than call failures.
 * Invalid calls leave receiver and event unchanged. */
enum v90_mp_receiver_result v90_mp_receive_bit(
	struct v90_mp_receiver *receiver, uint8_t bit,
	struct v90_mp_event *event);

const char *v90_mp_receiver_result_name(
	enum v90_mp_receiver_result result);

#endif /* V90_MP_RECEIVER_H */
