// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_JA_RX_H
#define V90_PHASE3_JA_RX_H

#include "v34_precoder_trainer.h"
#include "v90_ja_search.h"

enum v90_phase3_ja_rx_result {
	V90_PHASE3_JA_RX_OK,
	V90_PHASE3_JA_RX_DETECTED,
	V90_PHASE3_JA_RX_REJECTED,
	V90_PHASE3_JA_RX_COMPLETE,
	V90_PHASE3_JA_RX_INVALID_ARGUMENT,
	V90_PHASE3_JA_RX_INVALID_STATE,
	V90_PHASE3_JA_RX_OUT_OF_RANGE,
	V90_PHASE3_JA_RX_NUMERIC_FAILURE,
};

/* Own only the post-PP TRN guard, precoder learning and Ja descriptor search.
 * The caller supplies each equalized unit-power decision exactly once, starting
 * with the first TRN symbol. Carrier, timing and equalizer state stay with the
 * line receiver; this owner neither resamples nor equalizes its input. */
struct v90_phase3_ja_rx {
	struct v34_precoder_trainer precoder;
	struct v90_ja_search search;
};

struct v90_phase3_ja_handoff {
	struct v34_precoder_recommendation precoder;
	struct v90_dil_parameters dil;
};

enum v90_phase3_ja_rx_result v90_phase3_ja_rx_init(
	struct v90_phase3_ja_rx *rx);
bool v90_phase3_ja_rx_is_valid(const struct v90_phase3_ja_rx *rx);
bool v90_phase3_ja_rx_is_complete(const struct v90_phase3_ja_rx *rx);

/* Receiver and input must be disjoint. Errors and COMPLETE leave both
 * unchanged; REJECTED consumes a bad descriptor and keeps searching. */
enum v90_phase3_ja_rx_result v90_phase3_ja_rx_observe(
	struct v90_phase3_ja_rx *rx, const struct qam_point *decision);

/* Export only protocol semantics, not a second physical-receiver handoff.
 * Receiver and destination must be disjoint. Errors leave both unchanged. */
enum v90_phase3_ja_rx_result v90_phase3_ja_rx_handoff(
	const struct v90_phase3_ja_rx *rx,
	struct v90_phase3_ja_handoff *handoff);

#endif /* V90_PHASE3_JA_RX_H */
