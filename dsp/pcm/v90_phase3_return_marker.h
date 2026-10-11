// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_MARKER_H
#define V90_PHASE3_RETURN_MARKER_H

#include "v34_phase3_s_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* V.90 ends Jd as soon as the ordinary V.34 S detector qualifies the
	 * analogue return.  Nine consecutive decisions match that detector's
	 * limit; the longer suffix below remains responsible for the final S-bar
	 * boundary. */
	V90_PHASE3_RETURN_RX_ONSET_SYMBOLS = 9,
	/* Carrier correction can turn the structured signal following Ja into a
	 * nine-decision alias of S. Require the following decision to agree before
	 * a tracked receiver is allowed to terminate Jd. */
	V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS = 10,
	V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS =
		V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS,
	V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS =
		V90_PHASE3_RETURN_RX_ONSET_SYMBOLS *
		V34_PHASE3_S_RECEIVER_MAX_S_ERRORS /
		V34_PHASE3_S_SYMBOLS,
	V90_PHASE3_RETURN_RX_MIN_IMPLICIT_S_SYMBOLS = 96,
	/* The second return is specified as 128T of S followed by 16T of S-bar.
	 * Admit a narrow error/timing margin around that fixed S interval so an
	 * alternating slice of permitted DIL SCR cannot end DIL early. Count a
	 * continuous run in full, including any overlong first-return tail. */
	V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS = 96,
	V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS = 192,
	V90_PHASE3_RETURN_RX_EXIT_SYMBOLS = 16,
	V90_PHASE3_RETURN_RX_MIN_EXIT_ERRORS = 8,
};

enum v90_phase3_return_rx_stage {
	V90_PHASE3_RETURN_RX_STAGE_SEARCH_S,
	V90_PHASE3_RETURN_RX_STAGE_S,
	V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END,
	V90_PHASE3_RETURN_RX_STAGE_COMPLETE,
};

enum v90_phase3_return_rx_result {
	V90_PHASE3_RETURN_RX_OK,
	V90_PHASE3_RETURN_RX_S_DETECTED,
	/* A legacy first return left every S phase without a distinct S-bar. */
	V90_PHASE3_RETURN_RX_S_ENDED,
	V90_PHASE3_RETURN_RX_S_BAR_DETECTED,
	V90_PHASE3_RETURN_RX_COMPLETE,
	V90_PHASE3_RETURN_RX_INVALID_ARGUMENT,
	V90_PHASE3_RETURN_RX_INVALID_STATE,
	V90_PHASE3_RETURN_RX_NUMERIC_FAILURE,
};

/* Own first/second return markers, independent of carrier, timing and
 * equalizer state. Each call consumes one hard four-point rotation. */
struct v90_phase3_return_marker {
	struct v34_phase3_s_receiver marker;
	uint8_t onset_decisions[V90_PHASE3_RETURN_RX_ONSET_SYMBOLS];
	uint8_t next_onset_decision;
	uint8_t buffered_onset_decisions;
	uint8_t onset_offset;
	uint8_t onset_errors;
	uint16_t confirmed_s_symbols;
	uint8_t exit_symbols;
	uint8_t exit_errors[4];
	enum v90_phase3_return_rx_stage stage;
	bool allow_implicit_s_end;
	bool second_return;
	/* Retain the existing extra onset decision for a carrier-tracked line. */
	bool confirm_onset;
};

enum v90_phase3_return_rx_result v90_phase3_return_marker_init(
	struct v90_phase3_return_marker *rx, bool confirm_onset);
bool v90_phase3_return_marker_is_valid(
	const struct v90_phase3_return_marker *rx);
bool v90_phase3_return_marker_is_complete(
	const struct v90_phase3_return_marker *rx);

/* Legacy compatibility is enabled only while searching for the first return.
 * Restart clears it: the second return always needs a complete S-bar. */
enum v90_phase3_return_rx_result
v90_phase3_return_marker_allow_implicit_s_end(
	struct v90_phase3_return_marker *rx);
enum v90_phase3_return_rx_result v90_phase3_return_marker_restart(
	struct v90_phase3_return_marker *rx);
/* Errors and COMPLETE leave the receiver unchanged. */
enum v90_phase3_return_rx_result v90_phase3_return_marker_observe(
	struct v90_phase3_return_marker *rx, uint8_t rotation);

/* Return only a complete, distinct S/S-bar marker. No physical decision delay
 * is consumed here. The caller retains any marker.post_marker_symbols already
 * observed while confirming S-bar, followed by its next physical decision. */
bool v90_phase3_return_marker_get_detection(
	const struct v90_phase3_return_marker *rx,
	struct v34_phase3_s_detection *detection);

#endif /* V90_PHASE3_RETURN_MARKER_H */
