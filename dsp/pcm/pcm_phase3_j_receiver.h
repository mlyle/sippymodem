// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_J_RECEIVER_H
#define PCM_PHASE3_J_RECEIVER_H

#include "pcm_protocol.h"
#include "v34_framing.h"
#include "v90_jd.h"
#include "v90_pcm_symbol.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase3_j_receiver_stage {
	PCM_PHASE3_J_RECEIVER_TRN1D,
	PCM_PHASE3_J_RECEIVER_JD_SEARCH,
	PCM_PHASE3_J_RECEIVER_JD,
	PCM_PHASE3_J_RECEIVER_JP,
	PCM_PHASE3_J_RECEIVER_DONE,
};

enum pcm_phase3_j_receiver_result {
	PCM_PHASE3_J_RECEIVER_OK,
	PCM_PHASE3_J_RECEIVER_TRN1D_READY,
	PCM_PHASE3_J_RECEIVER_JD_DETECTED,
	PCM_PHASE3_J_RECEIVER_JD_REPEATED,
	PCM_PHASE3_J_RECEIVER_JP_DETECTED,
	PCM_PHASE3_J_RECEIVER_JP_REPEATED,
	PCM_PHASE3_J_RECEIVER_COMPLETE,
	PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT,
	PCM_PHASE3_J_RECEIVER_INVALID_STATE,
	PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE,
};

/* Owns sign continuity from TRN1d through the digital Phase 3 J records.
 * V.90 ends at Jd-prime; V.92 requires Jp and Jp-prime. The selected protocol
 * controls record interpretation and makes V.92-only states unreachable
 * for a V.90 call. */
struct pcm_phase3_j_receiver {
	struct v34_scrambler descrambler;
	union {
		struct v90_jd_parameters v90;
		struct {
			struct v92_jd jd;
			struct v92_jp jp;
		} v92;
	} parameters;
	enum pcm_protocol protocol;
	enum g711_law law;
	enum pcm_phase3_j_receiver_stage stage;
	uint32_t trn1d_symbols;
	uint8_t frame[PCM_J_FRAME_BYTES];
	uint8_t training_ucode;
	uint8_t last_sign;
	uint8_t frame_bits;
	uint8_t prime_zero_bits;
	bool prime_candidate;
};

enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_init(
	struct pcm_phase3_j_receiver *receiver, enum pcm_protocol protocol,
	enum g711_law law, uint8_t training_ucode);
bool pcm_phase3_j_receiver_is_valid(
	const struct pcm_phase3_j_receiver *receiver);
bool pcm_phase3_j_receiver_is_complete(
	const struct pcm_phase3_j_receiver *receiver);

/* Start differential decoding after at least 2040 TRN1d characters on a
 * six-character boundary. Jd repeats, so an early start can reacquire after
 * the self-synchronizing descrambler settles. */
enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_begin_jd_search(
	struct pcm_phase3_j_receiver *receiver);

/* Consume one raw G.711 network character after Sd has qualified the signal.
 * Decode signs despite amplitude-code deviations and isolated training errors;
 * Jd/Jp remain CRC-protected. Rejected calls leave state unchanged. Calls after
 * completion return COMPLETE without consuming the supplied character. */
enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_receive(
	struct pcm_phase3_j_receiver *receiver, uint8_t network_octet);

bool pcm_phase3_j_receiver_v90_jd(
	const struct pcm_phase3_j_receiver *receiver,
	struct v90_jd_parameters *jd);
bool pcm_phase3_j_receiver_v92_jd(
	const struct pcm_phase3_j_receiver *receiver, struct v92_jd *jd);
bool pcm_phase3_j_receiver_v92_jp(
	const struct pcm_phase3_j_receiver *receiver, struct v92_jp *jp);

const char *pcm_phase3_j_receiver_result_name(
	enum pcm_phase3_j_receiver_result result);

#endif /* PCM_PHASE3_J_RECEIVER_H */
