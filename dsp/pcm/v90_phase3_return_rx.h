// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_RX_H
#define V90_PHASE3_RETURN_RX_H

#include "v34_equalizer_handoff.h"
#include "v34_phase3_s_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_RETURN_RX_ONSET_SYMBOLS =
		V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS,
	V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS =
		V90_PHASE3_RETURN_RX_ONSET_SYMBOLS *
		V34_PHASE3_S_RECEIVER_MAX_S_ERRORS /
		V34_PHASE3_S_SYMBOLS,
};

enum v90_phase3_return_rx_stage {
	V90_PHASE3_RETURN_RX_STAGE_SEARCH_S,
	V90_PHASE3_RETURN_RX_STAGE_S,
	V90_PHASE3_RETURN_RX_STAGE_COMPLETE,
};

enum v90_phase3_return_rx_result {
	V90_PHASE3_RETURN_RX_OK,
	V90_PHASE3_RETURN_RX_S_DETECTED,
	V90_PHASE3_RETURN_RX_S_BAR_DETECTED,
	V90_PHASE3_RETURN_RX_COMPLETE,
	V90_PHASE3_RETURN_RX_INVALID_ARGUMENT,
	V90_PHASE3_RETURN_RX_INVALID_STATE,
	V90_PHASE3_RETURN_RX_NUMERIC_FAILURE,
};

/* Continue the digital modem's trained equalizer after Ja. A bounded
 * alternating-point correlation reports S early enough to finish Jd. The
 * same equalizer and hard decisions then locate the complete S-bar boundary.
 * Decision-directed adaptation begins only after S is qualified, avoiding
 * adaptation to the intervening silence. */
struct v90_phase3_return_rx {
	struct qam_equalizer equalizer;
	struct v34_phase3_s_receiver marker;
	double adaptation;
	uint8_t onset_decisions[V90_PHASE3_RETURN_RX_ONSET_SYMBOLS];
	uint8_t next_onset_decision;
	uint8_t buffered_onset_decisions;
	uint8_t onset_offset;
	uint8_t onset_errors;
	enum v90_phase3_return_rx_stage stage;
};

/* The equalizer has already accepted its decision-delay points following the
 * final S-bar decision. Its next input therefore yields the first undelivered
 * Phase 4 decision without replay. */
struct v90_phase3_return_handoff {
	struct v34_phase3_s_detection marker;
	struct v34_equalizer_handoff equalizer;
};

enum v90_phase3_return_rx_result v90_phase3_return_rx_init(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	double adaptation);
bool v90_phase3_return_rx_is_valid(
	const struct v90_phase3_return_rx *rx);
bool v90_phase3_return_rx_is_complete(
	const struct v90_phase3_return_rx *rx);

/* Consume one finite carrier- and timing-recovered point. Complete and
 * invalid calls consume nothing. */
enum v90_phase3_return_rx_result v90_phase3_return_rx_point(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received);

bool v90_phase3_return_handoff_is_valid(
	const struct v90_phase3_return_handoff *handoff);
enum v90_phase3_return_rx_result v90_phase3_return_rx_handoff(
	const struct v90_phase3_return_rx *rx,
	struct v90_phase3_return_handoff *handoff);

const char *v90_phase3_return_rx_result_name(
	enum v90_phase3_return_rx_result result);

#endif /* V90_PHASE3_RETURN_RX_H */
