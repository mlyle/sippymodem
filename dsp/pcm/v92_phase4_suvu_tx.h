// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_SUVU_TX_H
#define V92_PHASE4_SUVU_TX_H

#include "v92_pcm_phase4_trn2u.h"
#include "v92_suvu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_phase4_suvu_tx_result {
	V92_PHASE4_SUVU_TX_OK,
	V92_PHASE4_SUVU_TX_RECORD_COMPLETE,
	V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS,
	V92_PHASE4_SUVU_TX_INVALID_ARGUMENT,
	V92_PHASE4_SUVU_TX_INVALID_PARAMETERS,
	V92_PHASE4_SUVU_TX_INVALID_STATE,
	V92_PHASE4_SUVU_TX_INTERNAL_ERROR,
};

/* Own a canonical SUVu record and the continuous Phase 4 modulation state.
 * The first record begins only at an aligned initial-TRN2u handoff. Repeated
 * records preserve GPA scrambler and differential-sign history. */
struct v92_phase4_suvu_tx {
	struct v92_phase4_modulator modulator;
	uint8_t frame[V92_SUVU_FRAME_BYTES];
	size_t bit_index;
};

enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_init(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_pcm_phase4_trn2u *completed_trn2u,
	const struct v92_suvu_parameters *parameters);
bool v92_phase4_suvu_tx_is_valid(
	const struct v92_phase4_suvu_tx *tx);
bool v92_phase4_suvu_tx_record_is_complete(
	const struct v92_phase4_suvu_tx *tx);

/* Replace a completed record without resetting either coding history. */
enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_start_next_record(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_suvu_parameters *parameters);

/* Emit one 8000-symbol/s signed level. The first stored wire bit enters the
 * GPA scrambler first and becomes bit zero of the modulation input group.
 * Rejected calls leave both state and sample unchanged. */
enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_next(
	struct v92_phase4_suvu_tx *tx, int16_t *sample);

/* Export the exact next-group modulation state after a complete record. */
enum v92_phase4_suvu_tx_result v92_phase4_suvu_tx_handoff(
	const struct v92_phase4_suvu_tx *tx,
	struct v92_phase4_modulator *modulator);

const char *v92_phase4_suvu_tx_result_name(
	enum v92_phase4_suvu_tx_result result);

#endif /* V92_PHASE4_SUVU_TX_H */
