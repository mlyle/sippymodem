// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SUVD_RECEIVER_H
#define V92_SUVD_RECEIVER_H

#include "v92_suvd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_suvd_receiver_result {
	V92_SUVD_RECEIVER_OK,
	V92_SUVD_RECEIVER_INVALID_ARGUMENT,
	V92_SUVD_RECEIVER_INVALID_PARAMETERS,
	V92_SUVD_RECEIVER_INVALID_STATE,
	V92_SUVD_RECEIVER_INTERNAL_ERROR,
};

enum v92_suvd_event_type {
	V92_SUVD_EVENT_NONE,
	V92_SUVD_EVENT_RECORD,
	V92_SUVD_EVENT_REJECTED,
};

struct v92_suvd_event {
	enum v92_suvd_event_type type;
	enum v92_suvd_result frame_result;
	struct v92_suvd_parameters parameters;
};

enum v92_suvd_receive_stage {
	V92_SUVD_SEARCH_SYNC,
	V92_SUVD_RECEIVE_RECORD,
};

/* Incrementally find fixed-length SUVd records in one continuous descrambled
 * downstream Phase 4 stream. The saturated sync count lets a TRN2d suffix of
 * arbitrary length become the first record prefix without buffering it. */
struct v92_suvd_receiver {
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	uint8_t training_frame_bits;
	enum v92_suvd_receive_stage stage;
};

enum v92_suvd_receiver_result v92_suvd_receiver_init(
	struct v92_suvd_receiver *receiver, uint8_t training_frame_bits);
bool v92_suvd_receiver_is_valid(
	const struct v92_suvd_receiver *receiver);

/* Consume one binary decision. Wire failures are events and reset the search;
 * invalid calls leave both receiver and event unchanged. */
enum v92_suvd_receiver_result v92_suvd_receiver_receive_bit(
	struct v92_suvd_receiver *receiver, uint8_t bit,
	struct v92_suvd_event *event);

const char *v92_suvd_receiver_result_name(
	enum v92_suvd_receiver_result result);

#endif /* V92_SUVD_RECEIVER_H */
