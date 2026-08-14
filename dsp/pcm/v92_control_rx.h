// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CONTROL_RX_H
#define V92_CONTROL_RX_H

#include "v92_cpd.h"
#include "v92_suvd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_control_rx_result {
	V92_CONTROL_RX_OK,
	V92_CONTROL_RX_INVALID_ARGUMENT,
	V92_CONTROL_RX_INVALID_PARAMETERS,
	V92_CONTROL_RX_INVALID_STATE,
	V92_CONTROL_RX_INTERNAL_ERROR,
};

enum v92_control_event_type {
	V92_CONTROL_EVENT_NONE,
	V92_CONTROL_EVENT_SUVD,
	V92_CONTROL_EVENT_CPD,
	V92_CONTROL_EVENT_SUVD_REJECTED,
	V92_CONTROL_EVENT_CPD_REJECTED,
};

struct v92_control_suvd_event {
	enum v92_suvd_result frame_result;
	struct v92_suvd_parameters parameters;
};

struct v92_control_cpd_event {
	enum v92_cpd_result frame_result;
	struct v92_cpd_parameters parameters;
};

union v92_control_record_event {
	struct v92_control_suvd_event suvd;
	struct v92_control_cpd_event cpd;
};

struct v92_control_event {
	union v92_control_record_event record;
	enum v92_control_event_type type;
};

enum v92_control_rx_stage {
	V92_CONTROL_RX_SEARCH_SYNC,
	V92_CONTROL_RX_READ_MARKER,
	V92_CONTROL_RX_RECEIVE_SUVD,
	V92_CONTROL_RX_RECEIVE_CPD,
};

/* Incrementally find SUVd and CPd in one continuous descrambled downstream
 * Phase 4 stream. Their shared 17-one/zero prefix is owned once; marker bit 18
 * selects the fixed SUVd or variable CPd branch. A saturated sync count lets
 * a TRN2d suffix become the first prefix without buffering it. */
struct v92_control_rx {
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	uint8_t training_frame_bits;
	enum v92_control_rx_stage stage;
};

enum v92_control_rx_result v92_control_rx_init(
	struct v92_control_rx *receiver, uint8_t training_frame_bits);
bool v92_control_rx_is_valid(
	const struct v92_control_rx *receiver);

/* Consume one binary decision. Wire failures are events and reset the search;
 * invalid calls leave both receiver and event unchanged. */
enum v92_control_rx_result v92_control_rx_receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event);

const char *v92_control_rx_result_name(
	enum v92_control_rx_result result);

#endif /* V92_CONTROL_RX_H */
