// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_RECORD_FIXTURE_H
#define V92_PHASE4_RECORD_FIXTURE_H

#include <v92_phase4_record_tx.h>
#include <v92_suvu.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A non-NULL modulator starts a new stream; NULL replaces a completed record
 * without disturbing the retained modulation state. */
static bool v92_record_fixture_start_suvu(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const struct v92_suvu_parameters *parameters)
{
	uint8_t frame[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	enum v92_constellation_size constellation;

	if (tx == NULL || parameters == NULL)
		return false;
	if (modulator != NULL)
		constellation = modulator->constellation;
	else if (v92_phase4_record_tx_is_valid(tx))
		constellation = tx->modulator.constellation;
	else
		return false;
	if (v92_suvu_encode(
		    parameters, constellation, frame, sizeof(frame),
		    &frame_bits) != V92_SUVU_OK)
		return false;
	if (modulator != NULL)
		return v92_phase4_record_tx_init(
			       tx, modulator, frame, sizeof(frame), frame_bits) ==
		       V92_PHASE4_RECORD_TX_OK;
	return v92_phase4_record_tx_start_next(
		       tx, frame, sizeof(frame), frame_bits) ==
	       V92_PHASE4_RECORD_TX_OK;
}

static bool v92_record_fixture_start_cpu(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const struct v92_cpu_parameters *parameters)
{
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	unsigned int bits_per_symbol;

	if (tx == NULL || parameters == NULL)
		return false;
	if (modulator != NULL)
		bits_per_symbol = v92_phase4_modulator_bits_per_symbol(
			modulator->constellation);
	else if (v92_phase4_record_tx_is_valid(tx))
		bits_per_symbol = v92_phase4_modulator_bits_per_symbol(
			tx->modulator.constellation);
	else
		return false;
	if (v92_cpu_encode(
		    parameters, bits_per_symbol, frame, sizeof(frame),
		    &frame_bits) != V92_CPU_OK)
		return false;
	if (modulator != NULL)
		return v92_phase4_record_tx_init(
			       tx, modulator, frame, sizeof(frame), frame_bits) ==
		       V92_PHASE4_RECORD_TX_OK;
	return v92_phase4_record_tx_start_next(
		       tx, frame, sizeof(frame), frame_bits) ==
	       V92_PHASE4_RECORD_TX_OK;
}

#endif /* V92_PHASE4_RECORD_FIXTURE_H */
