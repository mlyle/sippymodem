// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_INITIAL_TX_H
#define V92_PCM_PHASE3_INITIAL_TX_H

#include "v92_pcm_phase3_ja.h"
#include "v92_pcm_phase3_opening.h"
#include "v92_pcm_phase3_trn1u.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_pcm_phase3_initial_tx_stage {
	V92_PCM_PHASE3_INITIAL_TX_OPENING,
	V92_PCM_PHASE3_INITIAL_TX_TRN1U,
	V92_PCM_PHASE3_INITIAL_TX_JA,
	V92_PCM_PHASE3_INITIAL_TX_COMPLETE,
};

enum v92_pcm_phase3_initial_tx_result {
	V92_PCM_PHASE3_INITIAL_TX_OK,
	V92_PCM_PHASE3_INITIAL_TX_NEED_MD_SYMBOL,
	V92_PCM_PHASE3_INITIAL_TX_DONE,
	V92_PCM_PHASE3_INITIAL_TX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_INITIAL_TX_INVALID_STATE,
	V92_PCM_PHASE3_INITIAL_TX_INTERNAL_ERROR,
};

union v92_pcm_phase3_initial_tx_sequence {
	struct v92_pcm_phase3_opening opening;
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx ja;
};

/* Own the analogue modem's initial PCM-upstream Phase 3 stream: silence,
 * Ru/Ru-bar, optional MD and repeated Ru/Ru-bar, a caller-selected aligned
 * TRN1u interval, and Ja. The enclosing PCM call owner observes the peer's
 * downstream response and requests the aligned end of Ja. Later Su and CPt
 * signals remain separate successor stages. */
struct v92_pcm_phase3_initial_tx {
	union v92_pcm_phase3_initial_tx_sequence sequence;
	uint32_t trn1u_goal_symbols;
	uint32_t upstream_rate_mask;
	enum v92_pcm_phase3_initial_tx_stage stage;
	enum g711_law law;
	uint8_t lu_ucode;
	uint8_t md_length_276_symbol_units;
	bool finish_requested;
};

enum v92_pcm_phase3_initial_tx_result v92_pcm_phase3_initial_tx_init(
	struct v92_pcm_phase3_initial_tx *tx, enum g711_law law,
	uint8_t lu_ucode, const struct v92_pcm_info1a *info1a,
	uint32_t upstream_rate_mask, uint32_t trn1u_goal_symbols);
bool v92_pcm_phase3_initial_tx_is_valid(
	const struct v92_pcm_phase3_initial_tx *tx);
bool v92_pcm_phase3_initial_tx_is_complete(
	const struct v92_pcm_phase3_initial_tx *tx);

/* Emit one raw 8000-symbol/s G.711 character. md_symbol is required only
 * during the manufacturer-defined MD interval and rejected at every other
 * stage. NEED_MD_SYMBOL and DONE consume nothing and preserve output. */
enum v92_pcm_phase3_initial_tx_result v92_pcm_phase3_initial_tx_next(
	struct v92_pcm_phase3_initial_tx *tx,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol);

/* Latch downstream response detection. Ja continues through its minimum
 * descriptor and the next twelve-symbol boundary before becoming complete. */
enum v92_pcm_phase3_initial_tx_result
v92_pcm_phase3_initial_tx_request_finish(
	struct v92_pcm_phase3_initial_tx *tx);

const char *v92_pcm_phase3_initial_tx_stage_name(
	enum v92_pcm_phase3_initial_tx_stage stage);
const char *v92_pcm_phase3_initial_tx_result_name(
	enum v92_pcm_phase3_initial_tx_result result);

#endif /* V92_PCM_PHASE3_INITIAL_TX_H */
