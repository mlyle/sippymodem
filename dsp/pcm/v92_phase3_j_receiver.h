// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE3_J_RECEIVER_H
#define V92_PHASE3_J_RECEIVER_H

#include "v34_framing.h"
#include "v90_pcm_symbol.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase3_j_receiver_stage {
	V92_PHASE3_J_RECEIVER_TRN1D,
	V92_PHASE3_J_RECEIVER_JD_SEARCH,
	V92_PHASE3_J_RECEIVER_JD,
	V92_PHASE3_J_RECEIVER_JP,
	V92_PHASE3_J_RECEIVER_DONE,
};

enum v92_phase3_j_receiver_result {
	V92_PHASE3_J_RECEIVER_OK,
	V92_PHASE3_J_RECEIVER_TRN1D_READY,
	V92_PHASE3_J_RECEIVER_JD_DETECTED,
	V92_PHASE3_J_RECEIVER_JD_REPEATED,
	V92_PHASE3_J_RECEIVER_JP_DETECTED,
	V92_PHASE3_J_RECEIVER_JP_REPEATED,
	V92_PHASE3_J_RECEIVER_COMPLETE,
	V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT,
	V92_PHASE3_J_RECEIVER_INVALID_STATE,
	V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE,
};

/* Receives the V.92 digital Phase 3 J sequence after Sd-bar. It owns GPC
 * descrambling and differential-sign continuity from the first TRN1d
 * character through Jd, Jp, and the terminating Jp-prime. */
struct v92_phase3_j_receiver {
	struct v34_scrambler descrambler;
	struct v92_jd jd;
	struct v92_jp jp;
	enum g711_law law;
	enum v92_phase3_j_receiver_stage stage;
	uint32_t trn1d_symbols;
	uint8_t frame[V92_J_FRAME_BYTES];
	uint8_t training_ucode;
	uint8_t last_sign;
	uint8_t frame_bits;
	uint8_t prime_zero_bits;
	bool prime_candidate;
};

enum v92_phase3_j_receiver_result v92_phase3_j_receiver_init(
	struct v92_phase3_j_receiver *receiver, enum g711_law law,
	uint8_t training_ucode);
bool v92_phase3_j_receiver_is_valid(
	const struct v92_phase3_j_receiver *receiver);
bool v92_phase3_j_receiver_is_complete(
	const struct v92_phase3_j_receiver *receiver);

/* Start differential decoding after at least 2040 TRN1d characters on a
 * six-character boundary. Jd repeats, so an early start can reacquire after
 * the self-synchronizing descrambler settles. */
enum v92_phase3_j_receiver_result v92_phase3_j_receiver_begin_jd_search(
	struct v92_phase3_j_receiver *receiver);

/* Consume one raw G.711 network character after Sd has qualified the signal.
 * Decode signs despite amplitude-code deviations and isolated training errors;
 * Jd/Jp remain CRC-protected. Rejected calls leave state unchanged. Calls after
 * completion return COMPLETE without consuming the supplied character. */
enum v92_phase3_j_receiver_result v92_phase3_j_receiver_receive(
	struct v92_phase3_j_receiver *receiver, uint8_t network_octet);

bool v92_phase3_j_receiver_jd(
	const struct v92_phase3_j_receiver *receiver, struct v92_jd *jd);
bool v92_phase3_j_receiver_jp(
	const struct v92_phase3_j_receiver *receiver, struct v92_jp *jp);

const char *v92_phase3_j_receiver_result_name(
	enum v92_phase3_j_receiver_result result);

#endif /* V92_PHASE3_J_RECEIVER_H */
