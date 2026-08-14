// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPT_RECEIVER_H
#define V92_CPT_RECEIVER_H

#include "v92_cpt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_cpt_receive_event_type {
	V92_CPT_RECEIVE_NO_EVENT,
	V92_CPT_RECEIVE_FRAME,
	V92_CPT_RECEIVE_REJECTED,
};

struct v92_cpt_receive_event {
	enum v92_cpt_receive_event_type type;
	/* Accepted frames report OK. Rejected frames retain the codec result. */
	enum v92_cpt_result frame_result;
	/* Candidate bits consumed before an accepted or rejected event. */
	size_t observed_bits;
	struct v92_cpt_parameters parameters;
};

enum v92_cpt_receive_stage {
	V92_CPT_RECEIVER_SEARCH_SYNC,
	V92_CPT_RECEIVER_READ_HEADER,
	V92_CPT_RECEIVER_READ_FRAME,
};

struct v92_cpt_receiver {
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	enum v92_cpt_receive_stage stage;
};

/* Search a continuous descrambled bit stream for a CPt frame, infer its
 * variable length from the six constellation indices and codec flag, and
 * emit only a validated typed record. */
enum v92_cpt_result v92_cpt_receiver_init(
	struct v92_cpt_receiver *receiver);
bool v92_cpt_receiver_is_valid(
	const struct v92_cpt_receiver *receiver);

/* Seed the already consumed 17-one sync and zero start bit after an external
 * Phase 3 marker synchronizer hands off its coding state. */
enum v92_cpt_result v92_cpt_receiver_begin_after_sync(
	struct v92_cpt_receiver *receiver);

/* Consume one decoded CPt bit. Rejected calls change neither receiver state
 * nor the caller's event. */
enum v92_cpt_result v92_cpt_receiver_receive_bit(
	struct v92_cpt_receiver *receiver, uint8_t bit,
	struct v92_cpt_receive_event *event);

#endif /* V92_CPT_RECEIVER_H */
