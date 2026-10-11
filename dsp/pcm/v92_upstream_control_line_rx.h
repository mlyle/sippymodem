// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_CONTROL_LINE_RX_H
#define V92_UPSTREAM_CONTROL_LINE_RX_H

#include "v92_control_rx.h"
#include "v92_upstream_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_upstream_control_line_rx_result {
	V92_UPSTREAM_CONTROL_LINE_RX_OK,
	V92_UPSTREAM_CONTROL_LINE_RX_EVENT,
	V92_UPSTREAM_CONTROL_LINE_RX_FAILED,
	V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT,
	V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE,
	V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR,
};

/* Continue a trained PCM-upstream line through repeated SUVu and CPu records.
 * The first decision_delay outputs are still the TRN2u tail already accepted
 * by the inherited equalizer. They prime the shared prefix hunter while known
 * targets finish training adaptation; later records use decision-directed
 * adaptation. One instance is serialized. */
struct v92_upstream_control_line_rx {
	struct v92_upstream_rx signal;
	struct v92_control_rx control;
	/* Set by the final-training owner after accepting CPu. Repeats must
	 * retain its wire length; zero leaves generic record parsing unrestricted. */
	uint16_t retained_cpu_bits;
	/* Two CRC-valid control records at the same period confirm repetition.
	 * Preserve that clock through damaged repeats, never their parameters.
	 * A valid record with a different length establishes a new period. */
	uint16_t repeat_symbols;
	uint16_t repeat_remaining;
	bool repeat_confirmed;
};

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_init(
	struct v92_upstream_control_line_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff);
bool v92_upstream_control_line_rx_is_valid(
	const struct v92_upstream_control_line_rx *receiver);
bool v92_upstream_control_line_rx_has_failed(
	const struct v92_upstream_control_line_rx *receiver);

/* Consume one signed 8 kHz line sample. Valid calls always consume the sample
 * and replace event; EVENT carries one complete or rejected control record.
 * Invalid and terminal calls change neither state nor event. Internal failure
 * latches without consuming the sample and leaves event unchanged. */
enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_sample(
	struct v92_upstream_control_line_rx *receiver, int16_t sample,
	struct v92_control_event *event);

const char *v92_upstream_control_line_rx_result_name(
	enum v92_upstream_control_line_rx_result result);

#endif /* V92_UPSTREAM_CONTROL_LINE_RX_H */
