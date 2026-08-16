// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_LINE_RX_H
#define V90_PHASE3_RETURN_LINE_RX_H

#include "v90_phase3_analog_line_rx.h"
#include "v90_phase3_return_rx.h"

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

/* Continue the digital modem's locked 8 kHz line receiver after Ja while the
 * analogue modem finishes Ja, goes silent, and returns S followed by S-bar. */
struct v90_phase3_return_line_rx {
	struct qam_demodulator demodulator;
	struct v90_phase3_return_rx points;
	struct v34_phase3_profile transmitting_profile;
	bool failed;
};

struct v90_phase3_return_line_handoff {
	struct qam_demodulator demodulator;
	struct v90_phase3_return_handoff phase3;
	struct v34_phase3_profile transmitting_profile;
};

enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_init(
	struct v90_phase3_return_line_rx *rx,
	const struct v90_phase3_analog_line_handoff *handoff,
	double adaptation);
enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_allow_implicit_s_end(
	struct v90_phase3_return_line_rx *rx);

/* Rearm S/S-bar detection after one complete return while preserving the
 * continuous demodulator, trained equalizer, and exact next-sample boundary.
 * This is the V.90 nonzero-DIL transition between its two return signals. */
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

bool v90_phase3_return_line_handoff_is_valid(
	const struct v90_phase3_return_line_handoff *handoff);
enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_handoff(
	const struct v90_phase3_return_line_rx *rx,
	struct v90_phase3_return_line_handoff *handoff);

const char *v90_phase3_return_line_rx_result_name(
	enum v90_phase3_return_line_rx_result result);

#endif /* V90_PHASE3_RETURN_LINE_RX_H */
