// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_DIGITAL_RX_H
#define V92_PHASE4_DIGITAL_RX_H

#include "v92_upstream_channel.h"
#include "v92_upstream_control_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_digital_rx_stage {
	V92_PHASE4_DIGITAL_RX_TRN2U,
	V92_PHASE4_DIGITAL_RX_CONTROL,
};

enum v92_phase4_digital_rx_result {
	V92_PHASE4_DIGITAL_RX_OK,
	V92_PHASE4_DIGITAL_RX_ALIGNED,
	V92_PHASE4_DIGITAL_RX_EVENT,
	V92_PHASE4_DIGITAL_RX_FAILED,
	V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT,
	V92_PHASE4_DIGITAL_RX_INVALID_STATE,
	V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE,
};

union v92_phase4_digital_receive {
	struct v92_pcm_phase4_trn2u_line_rx trn2u;
	struct v92_upstream_control_line_rx control;
};

/* Own the digital modem's continuous PCM-upstream Phase 4 line from the
 * CPt/E1u boundary through TRN2u and repeated SUVu/CPu records. The physical
 * aligned boundary transfers the warm equalizer, demodulator history, and
 * delayed known targets without resetting the line. One instance is
 * serialized. */
struct v92_phase4_digital_rx {
	union v92_phase4_digital_receive signal;
	struct v92_upstream_channel channel;
	enum v92_phase4_digital_rx_stage stage;
	bool failed;
};

enum v92_phase4_digital_rx_result v92_phase4_digital_rx_init(
	struct v92_phase4_digital_rx *receiver,
	const struct v92_pcm_phase3_cpt_line_handoff *handoff,
	enum v92_constellation_size constellation);
bool v92_phase4_digital_rx_is_valid(
	const struct v92_phase4_digital_rx *receiver);
bool v92_phase4_digital_rx_has_failed(
	const struct v92_phase4_digital_rx *receiver);
bool v92_phase4_digital_rx_is_receiving_control(
	const struct v92_phase4_digital_rx *receiver);

/* Consume one signed 8 kHz line sample. ALIGNED identifies the exact physical
 * TRN2u boundary and EVENT carries one complete or rejected control record.
 * Successful calls replace event, while invalid and terminal calls leave both
 * state and event unchanged. Receiver and event storage must not overlap. A
 * new DSP failure consumes no sample and latches failure without changing
 * event. */
enum v92_phase4_digital_rx_result v92_phase4_digital_rx_sample(
	struct v92_phase4_digital_rx *receiver, int16_t sample,
	struct v92_control_event *event);

const char *v92_phase4_digital_rx_result_name(
	enum v92_phase4_digital_rx_result result);

#endif /* V92_PHASE4_DIGITAL_RX_H */
