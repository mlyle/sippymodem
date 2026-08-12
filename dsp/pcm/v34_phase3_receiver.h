// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_RECEIVER_H
#define V34_PHASE3_RECEIVER_H

#include "qam_gain.h"
#include "v34_phase3_j_equalizer_receiver.h"
#include "v34_phase3_pp_receiver.h"
#include "v34_phase3_s_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* The largest legal 127 * 35 ms interval at 24000/7 symbols/s. */
	V34_PHASE3_RECEIVER_MAX_MD_SYMBOLS = 15240,
};

enum v34_phase3_receiver_stage {
	V34_PHASE3_RECEIVER_STAGE_S,
	V34_PHASE3_RECEIVER_STAGE_MD,
	V34_PHASE3_RECEIVER_STAGE_REPEATED_S,
	V34_PHASE3_RECEIVER_STAGE_PP,
	V34_PHASE3_RECEIVER_STAGE_J,
	V34_PHASE3_RECEIVER_STAGE_COMPLETE,
};

enum v34_phase3_receiver_result {
	V34_PHASE3_RECEIVER_OK,
	V34_PHASE3_RECEIVER_S_DETECTED,
	V34_PHASE3_RECEIVER_MD_COMPLETE,
	V34_PHASE3_RECEIVER_REPEATED_S_DETECTED,
	V34_PHASE3_RECEIVER_PP_COMPLETE,
	V34_PHASE3_RECEIVER_J_DETECTED,
	V34_PHASE3_RECEIVER_COMPLETE,
	V34_PHASE3_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_RECEIVER_INVALID_STATE,
	V34_PHASE3_RECEIVER_OUT_OF_RANGE,
	V34_PHASE3_RECEIVER_NUMERIC_FAILURE,
};

struct v34_phase3_receiver_configuration {
	enum v34_station_role transmitting_role;
	/* Exact symbol count derived from the peer's validated INFO1 profile. */
	uint32_t md_symbols;
	double s_gain_adaptation;
	double pp_equalizer_adaptation;
	double tracking_equalizer_adaptation;
};

bool v34_phase3_receiver_configuration_is_valid(
	const struct v34_phase3_receiver_configuration *configuration);

struct v34_phase3_s_point_receiver {
	struct qam_gain gain;
	struct v34_phase3_s_receiver detector;
	bool gain_seeded;
};

union v34_phase3_receiver_state {
	struct v34_phase3_s_point_receiver s;
	struct v34_phase3_pp_receiver pp;
	struct v34_phase3_j_equalizer_receiver j;
};

/* Atomic handoff at the end of a qualified J window. The equalizer's delayed
 * raw-point history and the J sequence state describe the same continuous
 * point timeline. */
struct v34_phase3_handoff {
	struct v34_phase3_s_detection s;
	struct v34_phase3_j_handoff j;
};

/* Shared V.34-family training boundary after S/S-bar and PP. The equalizer
 * retains the delayed TRN inputs needed to produce the first post-PP
 * decision. This boundary is available only on the PP_COMPLETE event, before
 * the protocol-specific suffix consumes another point. */
struct v34_phase3_prefix_handoff {
	struct v34_phase3_s_detection s;
	struct v34_equalizer_handoff equalizer;
};

bool v34_phase3_handoff_is_valid(
	const struct v34_phase3_handoff *handoff);
bool v34_phase3_prefix_handoff_is_valid(
	const struct v34_phase3_prefix_handoff *handoff);

/* Join S/S-bar acquisition, PP equalizer training, and TRN-to-J detection on
 * one carrier- and timing-recovered point stream. The upstream owner supplies
 * the configured S suffix and a complete S-bar marker; the first nonzero point
 * seeds a scalar gain estimate. No sample clock or carrier state is owned
 * here. */
struct v34_phase3_receiver {
	union v34_phase3_receiver_state state;
	struct v34_phase3_receiver_configuration configuration;
	struct v34_phase3_s_detection s_detection;
	uint32_t md_symbols_remaining;
	enum v34_phase3_receiver_stage stage;
};

enum v34_phase3_receiver_result v34_phase3_receiver_init(
	struct v34_phase3_receiver *receiver,
	const struct v34_phase3_receiver_configuration *configuration);
/* Use a qualified S suffix when an upstream sample receiver acquires timing
 * after the finite S marker has begun. Repeated S after MD still requires its
 * complete 128T marker. */
enum v34_phase3_receiver_result v34_phase3_receiver_init_suffix(
	struct v34_phase3_receiver *receiver,
	const struct v34_phase3_receiver_configuration *configuration,
	uint8_t s_symbols);

bool v34_phase3_receiver_is_valid(
	const struct v34_phase3_receiver *receiver);
bool v34_phase3_receiver_is_complete(
	const struct v34_phase3_receiver *receiver);

/* Accept one finite recovered point. S, PP, and J transition events include
 * the point that crossed the boundary. Invalid, numeric-failure,
 * out-of-range, and already-complete calls consume nothing and leave the
 * receiver unchanged. */
enum v34_phase3_receiver_result v34_phase3_receive_point(
	struct v34_phase3_receiver *receiver,
	const struct qam_point *received);

/* Copy the most recently completed S marker, including the first marker while
 * MD or repeated-S reception is active, or the complete Phase 3 handoff after
 * J_DETECTED. Failed calls leave their destination unchanged. */
bool v34_phase3_receiver_get_s_detection(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_s_detection *detection);
enum v34_phase3_receiver_result v34_phase3_receiver_prefix_handoff(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_prefix_handoff *handoff);
enum v34_phase3_receiver_result v34_phase3_receiver_handoff(
	const struct v34_phase3_receiver *receiver,
	struct v34_phase3_handoff *handoff);

const char *v34_phase3_receiver_result_name(
	enum v34_phase3_receiver_result result);

#endif /* V34_PHASE3_RECEIVER_H */
