// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_RRN_RX_H
#define V90_DOWNSTREAM_RRN_RX_H

#include "v90_pcm_mapping.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS = 240,
	V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS = 24,
	V90_DOWNSTREAM_RRN_RX_HYPOTHESES =
		2 * V90_CP_FRAME_INTERVALS,
};

enum v90_downstream_rrn_rx_stage {
	V90_DOWNSTREAM_RRN_RX_SEARCH,
	V90_DOWNSTREAM_RRN_RX_RD,
	V90_DOWNSTREAM_RRN_RX_RD_BAR,
	V90_DOWNSTREAM_RRN_RX_COMPLETE,
};

enum v90_downstream_rrn_rx_result {
	V90_DOWNSTREAM_RRN_RX_OK,
	V90_DOWNSTREAM_RRN_RX_BUFFERING,
	V90_DOWNSTREAM_RRN_RX_RD_DETECTED,
	V90_DOWNSTREAM_RRN_RX_RD_BAR_STARTED,
	V90_DOWNSTREAM_RRN_RX_RD_BAR_COMPLETE,
	V90_DOWNSTREAM_RRN_RX_SEQUENCE_MISMATCH,
	V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT,
	V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS,
	V90_DOWNSTREAM_RRN_RX_INVALID_STATE,
	V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR,
};

/* Detect the digital station's V.90 8.6.4 Rd/Rd-bar signal directly in the
 * authoritative G.711 character stream.  Six phase hypotheses and both
 * polarities avoid assuming where an online monitor first entered the data
 * frame.  Each symbol must be the highest-power member of its current data
 * constellation; unlike a sign-only line detector this rejects unrelated
 * data with the same +++--- sign pattern. */
struct v90_downstream_rrn_rx {
	struct v90_pcm_mapping mapping;
	uint16_t candidate_runs[V90_DOWNSTREAM_RRN_RX_HYPOTHESES];
	uint16_t locked_hypotheses;
	enum v90_pcm_mapping_plane decision_plane;
	enum v90_downstream_rrn_rx_stage stage;
	uint8_t phase;
	uint8_t rd_bar_symbols;
};

enum v90_downstream_rrn_rx_result v90_downstream_rrn_rx_init(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_mapping *current_data_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool v90_downstream_rrn_rx_is_valid(
	const struct v90_downstream_rrn_rx *receiver);
bool v90_downstream_rrn_rx_rd_detected(
	const struct v90_downstream_rrn_rx *receiver);
bool v90_downstream_rrn_rx_is_complete(
	const struct v90_downstream_rrn_rx *receiver);

/* BUFFERING consumes one character.  RD_DETECTED is reported on the 240th
 * consecutive Rd character, RD_BAR_STARTED on the exact frame-boundary
 * polarity reversal, and RD_BAR_COMPLETE on the 24th Rd-bar character.  The
 * next character remains unconsumed for the TRN2d receiver.  A post-lock
 * SEQUENCE_MISMATCH consumes that character and restarts the phase/polarity
 * search with it as the first new candidate. */
enum v90_downstream_rrn_rx_result v90_downstream_rrn_rx_receive(
	struct v90_downstream_rrn_rx *receiver, uint8_t network_octet);

const char *v90_downstream_rrn_rx_result_name(
	enum v90_downstream_rrn_rx_result result);

#endif /* V90_DOWNSTREAM_RRN_RX_H */
