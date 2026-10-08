// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CONTROL_RX_H
#define V92_CONTROL_RX_H

#include "v92_cpd.h"
#include "v92_cpu.h"
#include "v92_suvd.h"
#include "v92_suvu.h"

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
	V92_CONTROL_EVENT_SUVU,
	V92_CONTROL_EVENT_CPU,
	V92_CONTROL_EVENT_SUVU_REJECTED,
	V92_CONTROL_EVENT_CPU_REJECTED,
	V92_CONTROL_EVENT_CPUS,
	V92_CONTROL_EVENT_CPUS_REJECTED,
};

struct v92_control_suvd_event {
	enum v92_suvd_result frame_result;
	struct v92_suvd_parameters parameters;
};

struct v92_control_cpd_event {
	enum v92_cpd_result frame_result;
	struct v92_cpd_parameters parameters;
};

struct v92_control_suvu_event {
	enum v92_suvu_result frame_result;
	struct v92_suvu_parameters parameters;
};

struct v92_control_cpu_event {
	enum v92_cpu_result frame_result;
	struct v92_cpu_parameters parameters;
};

struct v92_control_cpus_event {
	enum v92_cpu_result frame_result;
	struct v92_cpus_parameters parameters;
};

union v92_control_record_event {
	struct v92_control_suvd_event suvd;
	struct v92_control_cpd_event cpd;
	struct v92_control_suvu_event suvu;
	struct v92_control_cpu_event cpu;
	struct v92_control_cpus_event cpus;
};

struct v92_control_event {
	union v92_control_record_event record;
	enum v92_control_event_type type;
};

enum v92_control_direction {
	V92_CONTROL_DOWNSTREAM,
	V92_CONTROL_UPSTREAM,
};

enum v92_control_rx_stage {
	V92_CONTROL_RX_SEARCH_SYNC,
	V92_CONTROL_RX_READ_MARKER,
	V92_CONTROL_RX_RECEIVE_STATUS,
	V92_CONTROL_RX_RECEIVE_PARAMETERS,
};

/* Incrementally find status updates and parameter records in one continuous
 * descrambled training or data-mode control stream. Both directions share
 * the 17-one/zero prefix; marker bit 18 selects the fixed status or variable
 * parameter branch. A saturated sync count lets a TRN2 suffix become the
 * first prefix. */
struct v92_control_rx {
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	uint8_t alignment_bits;
	enum v92_control_direction direction;
	enum v92_control_rx_stage stage;
	/* The last bit completed a whole record, even if its CRC failed.
	 * Sync hunting and early rejection of an invalid header are not boundaries. */
	bool record_boundary;
};

/* Both directions take the negotiated mapping-frame bit width: 9..42
 * downstream; 24/36 during upstream training, or even 36..72 during FPE.
 * The caller owns demodulation, descrambling and the aligned coding epoch. */
enum v92_control_rx_result v92_control_rx_init_downstream(
	struct v92_control_rx *receiver, uint8_t mapping_frame_bits);
enum v92_control_rx_result v92_control_rx_init_upstream(
	struct v92_control_rx *receiver,
	uint8_t mapping_frame_bits);
bool v92_control_rx_is_valid(
	const struct v92_control_rx *receiver);

/* Consume one binary decision. Wire failures are events and reset the search;
 * invalid calls leave both receiver and event unchanged. */
enum v92_control_rx_result v92_control_rx_receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event);

#endif /* V92_CONTROL_RX_H */
