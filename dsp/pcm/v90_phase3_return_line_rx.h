// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_LINE_RX_H
#define V90_PHASE3_RETURN_LINE_RX_H

#include "v90_phase3_analog_line_rx.h"
#include "v90_phase3_return_marker.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_return_line_rx_result {
	V90_PHASE3_RETURN_LINE_RX_OK,
	V90_PHASE3_RETURN_LINE_RX_S_DETECTED,
	V90_PHASE3_RETURN_LINE_RX_S_ENDED,
	V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED,
	V90_PHASE3_RETURN_LINE_RX_COMPLETE,
	V90_PHASE3_RETURN_LINE_RX_FAILED,
	V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT,
	V90_PHASE3_RETURN_LINE_RX_INVALID_STATE,
	V90_PHASE3_RETURN_LINE_RX_NUMERIC_FAILURE,
};

/* Continue the digital modem's V.90 return parser and fixed V.34 physical
 * receiver after Ja while the analogue modem finishes Ja, goes silent, and
 * returns S followed by S-bar. */
struct v90_phase3_return_line_rx {
	struct v90_upstream_training_rx upstream;
	struct v90_phase3_return_marker marker;
	struct v34_precoder_recommendation precoder;
	bool handoff_draining;
	bool failed;
};

/* The fixed upstream owns the current sample boundary and bounded CP prefix.
 * The return parser contributes only completed protocol metadata. */
struct v90_phase3_return_line_handoff {
	struct v90_upstream_training_rx upstream;
	struct v34_phase3_s_detection marker;
	struct v34_precoder_recommendation precoder;
};

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_init(
	struct v90_phase3_return_line_rx *rx,
	const struct v90_phase3_analog_line_handoff *handoff);
enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_allow_implicit_s_end(
	struct v90_phase3_return_line_rx *rx);

/* Rearm S/S-bar detection after one complete return while preserving the
 * continuous demodulator, trained equalizer, and exact next-sample boundary.
 * This is the V.90 nonzero-DIL transition between its two return signals.
 * Once advance_complete has begun the final handoff drain, restarting is
 * rejected: the completed marker no longer owns decisions from that epoch. */
enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_restart(
	struct v90_phase3_return_line_rx *rx);
bool v90_phase3_return_line_rx_is_valid(
	const struct v90_phase3_return_line_rx *rx);
bool v90_phase3_return_line_rx_has_failed(
	const struct v90_phase3_return_line_rx *rx);

/* Consume one signed 8 kHz sample. A DSP failure consumes the sample and
 * latches failure; invalid, complete, and failed calls do not. */
enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_sample(
	struct v90_phase3_return_line_rx *rx, int16_t sample);

/* After the complete final S-bar, consume one conventional physical sample
 * only in the fixed upstream receiver. Its retained decisions and line clock
 * remain sample-exact while the independent digital DIL segment drains.
 * Freeze the completed marker; Phase 4 uses the fixed line and only the
 * completed protocol metadata. */
enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_advance_complete(
	struct v90_phase3_return_line_rx *rx, int16_t sample);

bool v90_phase3_return_line_handoff_is_valid(
	const struct v90_phase3_return_line_handoff *handoff);
/* The output must be disjoint from the receiver. Rejected handoffs leave both
 * unchanged; no unresolved physical observation may cross the boundary. */
enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_handoff(
	const struct v90_phase3_return_line_rx *rx,
	struct v90_phase3_return_line_handoff *handoff);

const char *v90_phase3_return_line_rx_result_name(
	enum v90_phase3_return_line_rx_result result);

#endif /* V90_PHASE3_RETURN_LINE_RX_H */
