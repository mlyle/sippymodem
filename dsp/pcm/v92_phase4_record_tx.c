// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_record_tx.h"

#include "memory_region.h"
#include "packed_bits.h"

#include <string.h>

static bool record_shape_is_valid(
	const struct v92_phase4_modulator *modulator,
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count)
{
	unsigned int bits;
	size_t frame_bytes;

	if (!v92_phase4_modulator_is_valid(modulator) || frame == NULL ||
	    frame_bit_count == 0u ||
	    frame_bit_count > V92_PHASE4_RECORD_MAX_BITS)
		return false;
	bits = v92_phase4_modulator_bits_per_symbol(
		modulator->constellation);
	frame_bytes = (frame_bit_count + 7u) / 8u;
	return bits != 0u && frame_bit_count % bits == 0u &&
	       frame_capacity >= frame_bytes &&
	       packed_msb_tail_is_zero(
		       frame, frame_bit_count, frame_bytes);
}

bool v92_phase4_record_tx_is_valid(
	const struct v92_phase4_record_tx *tx)
{
	unsigned int bits;
	size_t frame_bytes;

	if (tx == NULL ||
	    !record_shape_is_valid(
		    &tx->modulator, tx->frame, sizeof(tx->frame),
		    tx->frame_bit_count))
		return false;
	bits = v92_phase4_modulator_bits_per_symbol(
		tx->modulator.constellation);
	frame_bytes = (tx->frame_bit_count + 7u) / 8u;
	return tx->bit_index <= tx->frame_bit_count &&
	       tx->bit_index % bits == 0u &&
	       memory_region_is_zero(
		       tx->frame + frame_bytes,
		       sizeof(tx->frame) - frame_bytes);
}

bool v92_phase4_record_tx_is_complete(
	const struct v92_phase4_record_tx *tx)
{
	return v92_phase4_record_tx_is_valid(tx) &&
	       tx->bit_index == tx->frame_bit_count;
}

static void copy_record(
	struct v92_phase4_record_tx *tx,
	const uint8_t *frame, size_t frame_bit_count)
{
	size_t frame_bytes = (frame_bit_count + 7u) / 8u;

	memset(tx->frame, 0, sizeof(tx->frame));
	memcpy(tx->frame, frame, frame_bytes);
	tx->frame_bit_count = frame_bit_count;
	tx->bit_index = 0u;
}

enum v92_phase4_record_tx_result v92_phase4_record_tx_init(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count)
{
	struct v92_phase4_record_tx initialized = { 0 };

	if (tx == NULL || modulator == NULL || frame == NULL)
		return V92_PHASE4_RECORD_TX_INVALID_ARGUMENT;
	if (!record_shape_is_valid(
		    modulator, frame, frame_capacity, frame_bit_count))
		return V92_PHASE4_RECORD_TX_INVALID_RECORD;
	initialized.modulator = *modulator;
	copy_record(&initialized, frame, frame_bit_count);
	if (!v92_phase4_record_tx_is_valid(&initialized))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	*tx = initialized;
	return V92_PHASE4_RECORD_TX_OK;
}

enum v92_phase4_record_tx_result v92_phase4_record_tx_init_e2u(
	struct v92_phase4_record_tx *tx,
	const struct v92_phase4_modulator *modulator, bool extend_one_symbol)
{
	static const uint8_t zeroes[V92_PHASE4_RECORD_MAX_BYTES];
	unsigned int bits;
	size_t symbols;

	if (tx == NULL || modulator == NULL)
		return V92_PHASE4_RECORD_TX_INVALID_ARGUMENT;
	if (!v92_phase4_modulator_is_valid(modulator))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	bits = v92_phase4_modulator_bits_per_symbol(
		modulator->constellation);
	symbols = extend_one_symbol ?
		V92_PHASE4_E2U_EXTENDED_SYMBOLS : V92_PHASE4_E2U_SYMBOLS;
	return v92_phase4_record_tx_init(
		tx, modulator, zeroes, sizeof(zeroes), symbols * bits);
}

enum v92_phase4_record_tx_result v92_phase4_record_tx_start_next(
	struct v92_phase4_record_tx *tx,
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count)
{
	struct v92_phase4_record_tx next;

	if (tx == NULL || frame == NULL)
		return V92_PHASE4_RECORD_TX_INVALID_ARGUMENT;
	if (!v92_phase4_record_tx_is_valid(tx))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	if (tx->bit_index != tx->frame_bit_count)
		return V92_PHASE4_RECORD_TX_IN_PROGRESS;
	if (!record_shape_is_valid(
		    &tx->modulator, frame, frame_capacity, frame_bit_count))
		return V92_PHASE4_RECORD_TX_INVALID_RECORD;
	next = *tx;
	copy_record(&next, frame, frame_bit_count);
	if (!v92_phase4_record_tx_is_valid(&next))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	*tx = next;
	return V92_PHASE4_RECORD_TX_OK;
}

enum v92_phase4_record_tx_result v92_phase4_record_tx_next(
	struct v92_phase4_record_tx *tx, int16_t *sample)
{
	unsigned int bits;
	int16_t generated;

	if (tx == NULL || sample == NULL ||
	    memory_regions_overlap(tx, sizeof(*tx), sample, sizeof(*sample)))
		return V92_PHASE4_RECORD_TX_INVALID_ARGUMENT;
	if (!v92_phase4_record_tx_is_valid(tx))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	if (tx->bit_index == tx->frame_bit_count)
		return V92_PHASE4_RECORD_TX_COMPLETE;
	bits = v92_phase4_modulator_bits_per_symbol(
		tx->modulator.constellation);
	if (v92_phase4_modulator_next(
		    &tx->modulator, (uint8_t)packed_msb_read_lsb_field(
			    tx->frame, tx->bit_index, bits), &generated) !=
	    V92_PHASE4_MODULATOR_OK)
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	tx->bit_index += bits;
	*sample = generated;
	return V92_PHASE4_RECORD_TX_OK;
}

enum v92_phase4_record_tx_result v92_phase4_record_tx_handoff(
	const struct v92_phase4_record_tx *tx,
	struct v92_phase4_modulator *modulator)
{
	if (tx == NULL || modulator == NULL)
		return V92_PHASE4_RECORD_TX_INVALID_ARGUMENT;
	if (!v92_phase4_record_tx_is_valid(tx))
		return V92_PHASE4_RECORD_TX_INVALID_STATE;
	if (tx->bit_index != tx->frame_bit_count)
		return V92_PHASE4_RECORD_TX_IN_PROGRESS;
	*modulator = tx->modulator;
	return V92_PHASE4_RECORD_TX_OK;
}
