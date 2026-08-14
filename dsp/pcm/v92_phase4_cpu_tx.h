// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_CPU_TX_H
#define V92_PHASE4_CPU_TX_H

#include "v92_cpu.h"
#include "v92_phase4_record_tx.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_cpu_tx_result {
	V92_PHASE4_CPU_TX_OK,
	V92_PHASE4_CPU_TX_RECORD_COMPLETE,
	V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS,
	V92_PHASE4_CPU_TX_INVALID_ARGUMENT,
	V92_PHASE4_CPU_TX_INVALID_PARAMETERS,
	V92_PHASE4_CPU_TX_INVALID_STATE,
	V92_PHASE4_CPU_TX_INTERNAL_ERROR,
};

/* Own canonical long CPu records and the continuous Phase 4 coding state. */
struct v92_phase4_cpu_tx {
	struct v92_phase4_record_tx record;
};

/* modulator is the exact next-group handoff from the preceding TRN2u-
 * modulated sequence. Initialization copies it without resetting history. */
enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_init(
	struct v92_phase4_cpu_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const struct v92_cpu_parameters *parameters);
bool v92_phase4_cpu_tx_is_valid(
	const struct v92_phase4_cpu_tx *tx);
bool v92_phase4_cpu_tx_record_is_complete(
	const struct v92_phase4_cpu_tx *tx);

/* Long CPu records repeated as a group carry identical parameters. The
 * caller owns that procedure-level rule; this boundary preserves coding
 * history while permitting an updated acknowledged record. */
enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_start_next_record(
	struct v92_phase4_cpu_tx *tx,
	const struct v92_cpu_parameters *parameters);

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_next(
	struct v92_phase4_cpu_tx *tx, int16_t *sample);

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_handoff(
	const struct v92_phase4_cpu_tx *tx,
	struct v92_phase4_modulator *modulator);

const char *v92_phase4_cpu_tx_result_name(
	enum v92_phase4_cpu_tx_result result);

#endif /* V92_PHASE4_CPU_TX_H */
