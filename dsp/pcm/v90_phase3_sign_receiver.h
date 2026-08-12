// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_SIGN_RECEIVER_H
#define V90_PHASE3_SIGN_RECEIVER_H

#include "v34_framing.h"
#include "v90_jd.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_sign_receiver_stage {
	V90_PHASE3_SIGN_RECEIVER_TRN1D,
	V90_PHASE3_SIGN_RECEIVER_JD_SEARCH,
	V90_PHASE3_SIGN_RECEIVER_JD,
	V90_PHASE3_SIGN_RECEIVER_DONE,
};

enum v90_phase3_sign_receiver_result {
	V90_PHASE3_SIGN_RECEIVER_OK,
	V90_PHASE3_SIGN_RECEIVER_TRN1D_READY,
	V90_PHASE3_SIGN_RECEIVER_JD_DETECTED,
	V90_PHASE3_SIGN_RECEIVER_JD_REPEATED,
	V90_PHASE3_SIGN_RECEIVER_COMPLETE,
	V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT,
	V90_PHASE3_SIGN_RECEIVER_INVALID_STATE,
	V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE,
};

/* Owns GPC descrambling and differential decoding from the first TRN1d
 * character through a complete Jd-prime. */
struct v90_phase3_sign_receiver {
	struct v34_scrambler descrambler;
	struct v90_jd_parameters jd;
	enum g711_law law;
	enum v90_phase3_sign_receiver_stage stage;
	uint32_t trn1d_symbols;
	uint8_t frame[V90_JD_FRAME_BYTES];
	uint8_t training_ucode;
	uint8_t last_sign;
	uint8_t frame_bits;
	uint8_t prime_zero_bits;
	bool prime_candidate;
};

enum v90_phase3_sign_receiver_result v90_phase3_sign_receiver_init(
	struct v90_phase3_sign_receiver *receiver, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase3_sign_receiver_is_valid(
	const struct v90_phase3_sign_receiver *receiver);
bool v90_phase3_sign_receiver_is_complete(
	const struct v90_phase3_sign_receiver *receiver);

/* Start differential decoding after at least 2040 TRN1d characters on a
 * six-character boundary. Jd repeats, so starting before its true boundary
 * remains recoverable after the self-synchronizing descrambler settles. */
enum v90_phase3_sign_receiver_result
v90_phase3_sign_receiver_begin_jd_search(
	struct v90_phase3_sign_receiver *receiver);

/* Consume one raw G.711 network character. Rejected calls leave state
 * unchanged. Calls after completion return COMPLETE without consuming
 * the supplied character. */
enum v90_phase3_sign_receiver_result v90_phase3_sign_receiver_receive(
	struct v90_phase3_sign_receiver *receiver, uint8_t network_octet);

bool v90_phase3_sign_receiver_jd(
	const struct v90_phase3_sign_receiver *receiver,
	struct v90_jd_parameters *parameters);
const char *v90_phase3_sign_receiver_result_name(
	enum v90_phase3_sign_receiver_result result);

#endif
