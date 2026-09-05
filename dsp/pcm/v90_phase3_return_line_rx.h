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

enum {
	/* After the shared raw S prefix re-seeds the selected equalizer span, allow
	 * its decision delay plus a complete tracked-onset window and bounded margin.
	 * Failure to qualify inside this window rearms raw acquisition. */
	V90_PHASE3_RETURN_LINE_RX_REACQUISITION_SYMBOLS =
		V34_EQUALIZER_DECISION_DELAY +
		V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS + 8,
};

/* Continue the digital modem's V.90 return parser and fixed V.34 physical
 * receiver after Ja while the analogue modem finishes Ja, goes silent, and
 * returns S followed by S-bar. */
struct v90_phase3_return_line_rx {
	struct qam_demodulator demodulator;
	struct v90_phase3_return_rx points;
	struct v90_upstream_training_rx upstream;
	struct v34_phase3_s_prefix s_prefix;
	struct v34_phase3_profile transmitting_profile;
	uint8_t reacquisition_symbols;
	bool reacquiring_equalizer;
	bool failed;
};

struct v90_phase3_return_line_handoff {
	struct qam_demodulator demodulator;
	struct v90_phase3_return_handoff phase3;
	struct v90_upstream_training_rx upstream;
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

/* After the complete S-bar has fixed the Phase 4 handoff, advance both line
 * clocks without interpreting the following CPt samples. This keeps a later
 * handoff sample-exact while an independently owned digital DIL segment
 * drains. */
enum v90_phase3_return_line_rx_result
v90_phase3_return_line_rx_advance_complete(
	struct v90_phase3_return_line_rx *rx, int16_t sample);

bool v90_phase3_return_line_handoff_is_valid(
	const struct v90_phase3_return_line_handoff *handoff);
enum v90_phase3_return_line_rx_result v90_phase3_return_line_rx_handoff(
	const struct v90_phase3_return_line_rx *rx,
	struct v90_phase3_return_line_handoff *handoff);

const char *v90_phase3_return_line_rx_result_name(
	enum v90_phase3_return_line_rx_result result);

#endif /* V90_PHASE3_RETURN_LINE_RX_H */
