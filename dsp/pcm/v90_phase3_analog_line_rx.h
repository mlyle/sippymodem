// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ANALOG_LINE_RX_H
#define V90_PHASE3_ANALOG_LINE_RX_H

#include "v34_phase3_profile.h"
#include "v90_phase3.h"
#include "v90_phase3_analog_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_analog_line_rx_result {
	V90_PHASE3_ANALOG_LINE_RX_OK,
	V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED,
	V90_PHASE3_ANALOG_LINE_RX_S_DETECTED,
	V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE,
	V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED,
	V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED,
	V90_PHASE3_ANALOG_LINE_RX_COMPLETE,
	V90_PHASE3_ANALOG_LINE_RX_FAILED,
	V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT,
	V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE,
	V90_PHASE3_ANALOG_LINE_RX_NUMERIC_FAILURE,
	V90_PHASE3_ANALOG_LINE_RX_OUT_OF_RANGE,
};

/* Own carrier and symbol-timing recovery around one analogue Phase 3 point
 * receiver. The copied profile must describe the analogue modem's zero-MD
 * 8 kHz training waveform. */
struct v90_phase3_analog_line_rx {
	struct qam_demodulator demodulator;
	struct v90_phase3_analog_rx points;
	struct v34_phase3_s_prefix s_prefix;
	struct v34_phase3_profile transmitting_profile;
	bool timing_rebased;
	bool failed;
};

struct v90_phase3_analog_line_handoff {
	struct qam_demodulator demodulator;
	struct v90_phase3_analog_handoff phase3;
	struct v34_phase3_profile transmitting_profile;
};

enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_init(
	struct v90_phase3_analog_line_rx *rx,
	const struct v34_phase3_profile *analog_profile,
	const struct v90_phase3_analog_rx_configuration *configuration);
bool v90_phase3_analog_line_rx_is_valid(
	const struct v90_phase3_analog_line_rx *rx);
bool v90_phase3_analog_line_rx_has_failed(
	const struct v90_phase3_analog_line_rx *rx);

/* Consume exactly one signed 8 kHz sample. A terminal DSP error consumes the
 * sample and latches failure; invalid, complete, and failed calls do not. */
enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_sample(
	struct v90_phase3_analog_line_rx *rx, int16_t sample);

bool v90_phase3_analog_line_handoff_is_valid(
	const struct v90_phase3_analog_line_handoff *handoff);
enum v90_phase3_analog_line_rx_result v90_phase3_analog_line_rx_handoff(
	const struct v90_phase3_analog_line_rx *rx,
	struct v90_phase3_analog_line_handoff *handoff);

const char *v90_phase3_analog_line_rx_result_name(
	enum v90_phase3_analog_line_rx_result result);

#endif /* V90_PHASE3_ANALOG_LINE_RX_H */
