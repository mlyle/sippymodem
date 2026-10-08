// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_CPT_TX_H
#define V92_PCM_PHASE3_CPT_TX_H

#include "pcm_binary_sign.h"
#include "v92_cpt.h"
#include "v92_pcm_phase3_trn1u.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS = 24,
	V92_PCM_PHASE3_E1U_SYMBOLS = 12,
};

enum v92_pcm_phase3_cpt_tx_stage {
	V92_PCM_PHASE3_CPT_TX_TRN1U,
	V92_PCM_PHASE3_CPT_TX_PREAMBLE,
	V92_PCM_PHASE3_CPT_TX_RECORD,
	V92_PCM_PHASE3_CPT_TX_E1U,
	V92_PCM_PHASE3_CPT_TX_COMPLETE,
};

enum v92_pcm_phase3_cpt_tx_result {
	V92_PCM_PHASE3_CPT_TX_OK,
	V92_PCM_PHASE3_CPT_TX_DONE,
	V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_CPT_TX_INVALID_STATE,
	V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR,
};

/* Own the analogue modem's second TRN1u segment and its continuous CPt/E1u
 * sign stream. The call controller decides when DIL study or Ri permits CPt
 * to start and when the peer's later Ri transition requests completion. */
struct v92_pcm_phase3_cpt_tx {
	struct v92_pcm_phase3_trn1u trn1u;
	struct pcm_binary_sign coding;
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bit_count;
	size_t frame_bit_index;
	uint32_t completed_records;
	enum v92_pcm_phase3_cpt_tx_stage stage;
	uint8_t preamble_symbols;
	uint8_t e1u_symbols;
	bool finish_requested;
};

enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_init(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	enum g711_law law, uint8_t lu_ucode,
	const struct v92_cpt_parameters *parameters);
bool v92_pcm_phase3_cpt_tx_is_valid(
	const struct v92_pcm_phase3_cpt_tx *transmitter);
bool v92_pcm_phase3_cpt_tx_is_complete(
	const struct v92_pcm_phase3_cpt_tx *transmitter);

/* Start the 24-one prefix only after the second TRN1u has reached an aligned
 * endpoint of at least 2040 symbols. */
enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_start(
	struct v92_pcm_phase3_cpt_tx *transmitter);

/* Complete the current CPt record and then emit the twelve-zero E1u frame.
 * At least one complete CPt record must already have been transmitted. */
enum v92_pcm_phase3_cpt_tx_result
v92_pcm_phase3_cpt_tx_request_finish(
	struct v92_pcm_phase3_cpt_tx *transmitter);

/* Emit one 8000-symbol/s network character. Rejected calls leave both the
 * state and output unchanged. DONE is returned only after E1u was emitted. */
enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_next(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct v90_pcm_symbol *symbol);

#endif /* V92_PCM_PHASE3_CPT_TX_H */
