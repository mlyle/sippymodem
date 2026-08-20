// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_LINE_RECEIVER_H
#define V34_PHASE3_LINE_RECEIVER_H

#include "v34_line_frontend.h"
#include "v34_phase3_profile.h"
#include "v34_phase3_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE3_LINE_ACQUISITION_SYMBOLS = 64,
	V34_PHASE3_LINE_TRACKING_SYMBOLS =
		QAM_DEFAULT_TIMING_WINDOW_SYMBOLS,
	V34_PHASE3_LINE_S_SUFFIX_SYMBOLS =
		V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS,
};

enum v34_phase3_line_receiver_result {
	V34_PHASE3_LINE_RECEIVER_OK,
	V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED,
	V34_PHASE3_LINE_RECEIVER_S_DETECTED,
	V34_PHASE3_LINE_RECEIVER_MD_COMPLETE,
	V34_PHASE3_LINE_RECEIVER_REPEATED_S_DETECTED,
	V34_PHASE3_LINE_RECEIVER_PP_COMPLETE,
	V34_PHASE3_LINE_RECEIVER_J_DETECTED,
	V34_PHASE3_LINE_RECEIVER_COMPLETE,
	V34_PHASE3_LINE_RECEIVER_FAILED,
	V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_LINE_RECEIVER_INVALID_STATE,
	V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE,
	V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE,
};

struct v34_phase3_line_receiver_configuration {
	double s_gain_adaptation;
	double pp_equalizer_adaptation;
	double tracking_equalizer_adaptation;
};

/* Own the carrier, symbol timing, and point-domain Phase 3 receiver on one
 * continuous line timeline. The transmitting profile is copied so no mutable
 * negotiation state is borrowed. */
struct v34_phase3_line_receiver {
	struct v34_line_frontend line;
	struct v34_phase3_receiver points;
	struct v34_phase3_profile transmitting_profile;
	bool failed;
};

/* The handoff describes one exact sample boundary after J. Phase 4 must copy
 * all three members rather than restart carrier timing or equalization. */
struct v34_phase3_line_handoff {
	struct v34_line_frontend line;
	struct v34_phase3_handoff phase3;
	struct v34_phase3_profile transmitting_profile;
};

bool v34_phase3_line_receiver_configuration_is_valid(
	const struct v34_phase3_line_receiver_configuration *configuration);
bool v34_phase3_line_receiver_is_valid(
	const struct v34_phase3_line_receiver *receiver);
bool v34_phase3_line_receiver_has_failed(
	const struct v34_phase3_line_receiver *receiver);
bool v34_phase3_line_handoff_is_valid(
	const struct v34_phase3_line_handoff *handoff);

enum v34_phase3_line_receiver_result v34_phase3_line_receiver_init(
	struct v34_phase3_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	uint32_t sample_rate,
	const struct v34_phase3_line_receiver_configuration *configuration);

/* Select the persistent fixed receiver at the 8-kHz V.34 call boundary. */
enum v34_phase3_line_receiver_result
v34_phase3_line_receiver_init_fixed(
	struct v34_phase3_line_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	const struct v34_phase3_line_receiver_configuration *configuration);

/* Consume exactly one signed line sample. A terminal point-domain error
 * consumes the sample and latches failure because carrier and timing history
 * have advanced. Invalid, complete, and already-failed calls consume nothing. */
enum v34_phase3_line_receiver_result v34_phase3_line_receiver_sample(
	struct v34_phase3_line_receiver *receiver, int16_t sample);

/* After J detection, keep the carrier, matched filter, and recovered symbol
 * clock continuous while discarding points that no longer belong to Phase 3.
 * Calls before the point receiver completes consume nothing. */
enum v34_phase3_line_receiver_result
v34_phase3_line_receiver_track_sample(
	struct v34_phase3_line_receiver *receiver, int16_t sample);

/* Copy the continuous carrier/equalizer boundary after J detection. A failed
 * call leaves the destination unchanged. */
enum v34_phase3_line_receiver_result v34_phase3_line_receiver_handoff(
	const struct v34_phase3_line_receiver *receiver,
	struct v34_phase3_line_handoff *handoff);

const char *v34_phase3_line_receiver_result_name(
	enum v34_phase3_line_receiver_result result);

#endif /* V34_PHASE3_LINE_RECEIVER_H */
