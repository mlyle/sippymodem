// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_cpu_tx.h"

#include <string.h>

static unsigned int modulation_width(
	const struct v92_phase4_cpu_tx *tx)
{
	return v92_phase4_modulator_bits_per_symbol(
		tx->record.modulator.constellation);
}

static bool frame_is_valid(const struct v92_phase4_cpu_tx *tx)
{
	struct v92_cpu_parameters parameters;
	uint8_t canonical[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count = 0u;
	size_t frame_bytes =
		(tx->record.frame_bit_count + 7u) / 8u;
	unsigned int bits = modulation_width(tx);

	return bits != 0u &&
	       v92_cpu_decode(
		       tx->record.frame, frame_bytes,
		       tx->record.frame_bit_count, bits,
		       &parameters) == V92_CPU_OK &&
	       v92_cpu_encode(
		       &parameters, bits, canonical, sizeof(canonical),
		       &bit_count) == V92_CPU_OK &&
	       bit_count == tx->record.frame_bit_count &&
	       memcmp(tx->record.frame, canonical, frame_bytes) == 0;
}

bool v92_phase4_cpu_tx_is_valid(
	const struct v92_phase4_cpu_tx *tx)
{
	return tx != NULL &&
	       v92_phase4_record_tx_is_valid(&tx->record) &&
	       frame_is_valid(tx);
}

bool v92_phase4_cpu_tx_record_is_complete(
	const struct v92_phase4_cpu_tx *tx)
{
	return v92_phase4_cpu_tx_is_valid(tx) &&
	       v92_phase4_record_tx_is_complete(&tx->record);
}

static enum v92_phase4_cpu_tx_result encode_record(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol,
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES],
	size_t *frame_bit_count)
{
	enum v92_cpu_result result;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_PHASE4_CPU_TX_INVALID_ARGUMENT;
	if (!v92_cpu_parameters_are_valid(parameters))
		return V92_PHASE4_CPU_TX_INVALID_PARAMETERS;
	result = v92_cpu_encode(
		parameters, bits_per_symbol, frame,
		V92_CPU_MAX_FRAME_BYTES, frame_bit_count);
	if (result == V92_CPU_INVALID_PARAMETERS)
		return V92_PHASE4_CPU_TX_INVALID_PARAMETERS;
	if (result != V92_CPU_OK)
		return V92_PHASE4_CPU_TX_INTERNAL_ERROR;
	return V92_PHASE4_CPU_TX_OK;
}

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_init(
	struct v92_phase4_cpu_tx *tx,
	const struct v92_phase4_modulator *modulator,
	const struct v92_cpu_parameters *parameters)
{
	struct v92_phase4_cpu_tx initialized = { 0 };
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bit_count = 0u;
	unsigned int bits;
	enum v92_phase4_cpu_tx_result result;

	if (tx == NULL || modulator == NULL || parameters == NULL)
		return V92_PHASE4_CPU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_modulator_is_valid(modulator))
		return V92_PHASE4_CPU_TX_INVALID_STATE;
	bits = v92_phase4_modulator_bits_per_symbol(
		modulator->constellation);
	result = encode_record(parameters, bits, frame, &frame_bit_count);
	if (result != V92_PHASE4_CPU_TX_OK)
		return result;
	if (v92_phase4_record_tx_init(
		    &initialized.record, modulator, frame, sizeof(frame),
		    frame_bit_count) != V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_cpu_tx_is_valid(&initialized))
		return V92_PHASE4_CPU_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V92_PHASE4_CPU_TX_OK;
}

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_start_next_record(
	struct v92_phase4_cpu_tx *tx,
	const struct v92_cpu_parameters *parameters)
{
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bit_count = 0u;
	enum v92_phase4_cpu_tx_result result;

	if (tx == NULL || parameters == NULL)
		return V92_PHASE4_CPU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_cpu_tx_is_valid(tx))
		return V92_PHASE4_CPU_TX_INVALID_STATE;
	if (!v92_phase4_cpu_tx_record_is_complete(tx))
		return V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS;
	result = encode_record(
		parameters, modulation_width(tx), frame, &frame_bit_count);
	if (result != V92_PHASE4_CPU_TX_OK)
		return result;
	if (v92_phase4_record_tx_start_next(
		    &tx->record, frame, sizeof(frame), frame_bit_count) !=
		    V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_cpu_tx_is_valid(tx))
		return V92_PHASE4_CPU_TX_INTERNAL_ERROR;
	return V92_PHASE4_CPU_TX_OK;
}

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_next(
	struct v92_phase4_cpu_tx *tx, int16_t *sample)
{
	enum v92_phase4_record_tx_result result;

	if (tx == NULL || sample == NULL)
		return V92_PHASE4_CPU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_cpu_tx_is_valid(tx))
		return V92_PHASE4_CPU_TX_INVALID_STATE;
	if (v92_phase4_cpu_tx_record_is_complete(tx))
		return V92_PHASE4_CPU_TX_RECORD_COMPLETE;
	result = v92_phase4_record_tx_next(&tx->record, sample);
	if (result != V92_PHASE4_RECORD_TX_OK ||
	    !v92_phase4_cpu_tx_is_valid(tx))
		return V92_PHASE4_CPU_TX_INTERNAL_ERROR;
	return V92_PHASE4_CPU_TX_OK;
}

enum v92_phase4_cpu_tx_result v92_phase4_cpu_tx_handoff(
	const struct v92_phase4_cpu_tx *tx,
	struct v92_phase4_modulator *modulator)
{
	if (tx == NULL || modulator == NULL)
		return V92_PHASE4_CPU_TX_INVALID_ARGUMENT;
	if (!v92_phase4_cpu_tx_is_valid(tx))
		return V92_PHASE4_CPU_TX_INVALID_STATE;
	if (!v92_phase4_cpu_tx_record_is_complete(tx))
		return V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS;
	return v92_phase4_record_tx_handoff(&tx->record, modulator) ==
		       V92_PHASE4_RECORD_TX_OK ?
		V92_PHASE4_CPU_TX_OK : V92_PHASE4_CPU_TX_INTERNAL_ERROR;
}

const char *v92_phase4_cpu_tx_result_name(
	enum v92_phase4_cpu_tx_result result)
{
	switch (result) {
	case V92_PHASE4_CPU_TX_OK:
		return "ok";
	case V92_PHASE4_CPU_TX_RECORD_COMPLETE:
		return "record complete";
	case V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS:
		return "record in progress";
	case V92_PHASE4_CPU_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_CPU_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PHASE4_CPU_TX_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_CPU_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
