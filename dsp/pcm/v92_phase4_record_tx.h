// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_RECORD_TX_H
#define V92_PHASE4_RECORD_TX_H

#include "v92_cpu.h"
#include "v92_phase4_modulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* E2u is one 12-symbol upstream data frame, optionally extended by
	 * CPd bit 29. */
	V92_PHASE4_E2U_SYMBOLS = 12,
	V92_PHASE4_E2U_EXTENDED_SYMBOLS = 13,
	/* Long CPu is the largest packed record carried by this modulator. */
	V92_PHASE4_RECORD_MAX_BITS = V92_CPU_MAX_FRAME_BITS,
	V92_PHASE4_RECORD_MAX_BYTES =
		(V92_PHASE4_RECORD_MAX_BITS + 7) / 8,
};

enum v92_phase4_record_tx_result {
	V92_PHASE4_RECORD_TX_OK,
	V92_PHASE4_RECORD_TX_COMPLETE,
	V92_PHASE4_RECORD_TX_IN_PROGRESS,
	V92_PHASE4_RECORD_TX_INVALID_ARGUMENT,
	V92_PHASE4_RECORD_TX_INVALID_RECORD,
	V92_PHASE4_RECORD_TX_INVALID_STATE,
};

/* Own a bounded packed record and the continuous Phase 4 modulation state. */
struct v92_phase4_record_tx {
	struct v92_phase4_modulator modulator;
	uint8_t frame[V92_PHASE4_RECORD_MAX_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
};

enum v92_phase4_record_tx_result v92_phase4_record_tx_init(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count);

/* Begin E2u at the exact modulation state exported by the final Phase 4
 * control record. E2u is one frame of input zeroes and CPd may extend it by
 * one symbol. GPA scrambling and differential-sign history remain continuous.
 */
enum v92_phase4_record_tx_result v92_phase4_record_tx_init_e2u(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator, bool extend_one_symbol);
bool v92_phase4_record_tx_is_valid(
	const struct v92_phase4_record_tx *tx);
bool v92_phase4_record_tx_is_complete(
	const struct v92_phase4_record_tx *tx);

/* Replace only a complete record, preserving coding history. */
enum v92_phase4_record_tx_result v92_phase4_record_tx_start_next(
	struct v92_phase4_record_tx *tx,
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count);

/* The sample must not overlap the record owner. Rejected calls and completed
 * records leave both the owner and sample unchanged. */
enum v92_phase4_record_tx_result v92_phase4_record_tx_next(
	struct v92_phase4_record_tx *tx, int16_t *sample);

enum v92_phase4_record_tx_result v92_phase4_record_tx_handoff(
	const struct v92_phase4_record_tx *tx,
	struct v92_phase4_modulator *modulator);

#endif /* V92_PHASE4_RECORD_TX_H */
