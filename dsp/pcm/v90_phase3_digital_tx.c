// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_digital_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool stage_is_valid(enum v90_phase3_digital_tx_stage stage)
{
	return stage >= V90_PHASE3_DIGITAL_TX_SD &&
	       stage <= V90_PHASE3_DIGITAL_TX_COMPLETE;
}

static bool jd_frame_is_valid(const uint8_t *frame)
{
	struct v90_jd_parameters parameters;
	uint8_t canonical[V90_JD_FRAME_BYTES];

	return v90_jd_decode(
		       frame, V90_JD_FRAME_BYTES, &parameters) == V90_JD_OK &&
	       v90_jd_encode(
		       &parameters, canonical, sizeof(canonical)) == V90_JD_OK &&
	       memcmp(frame, canonical, sizeof(canonical)) == 0;
}

static bool counters_are_valid(
	const struct v90_phase3_digital_tx *transmitter)
{
	if (transmitter->stage < V90_PHASE3_DIGITAL_TX_JD)
		return transmitter->jd_records == 0 &&
		       transmitter->bit_index == 0 &&
		       !transmitter->finish_jd_requested;
	if (transmitter->jd_records == UINT32_MAX)
		return false;
	if (transmitter->stage == V90_PHASE3_DIGITAL_TX_JD)
		return transmitter->bit_index < V90_JD_FRAME_BITS &&
		       !(transmitter->finish_jd_requested &&
			 transmitter->jd_records != 0 &&
			 transmitter->bit_index == 0);
	if (transmitter->jd_records == 0 ||
	    !transmitter->finish_jd_requested)
		return false;
	if (transmitter->stage == V90_PHASE3_DIGITAL_TX_JD_PRIME)
		return transmitter->bit_index < V90_PHASE3_JD_PRIME_BITS;
	return transmitter->stage == V90_PHASE3_DIGITAL_TX_COMPLETE &&
	       transmitter->bit_index == V90_PHASE3_JD_PRIME_BITS;
}

bool v90_phase3_digital_tx_is_valid(
	const struct v90_phase3_digital_tx *transmitter)
{
	if (transmitter == NULL || !stage_is_valid(transmitter->stage) ||
	    !jd_frame_is_valid(transmitter->jd_frame) ||
	    !counters_are_valid(transmitter))
		return false;
	switch (transmitter->stage) {
	case V90_PHASE3_DIGITAL_TX_SD:
		return v90_phase3_sd_is_valid(&transmitter->sequence.sd) &&
		       transmitter->sequence.sd.kind == V90_PHASE3_SD &&
		       v90_phase3_sd_remaining(&transmitter->sequence.sd) != 0;
	case V90_PHASE3_DIGITAL_TX_SD_BAR:
		return v90_phase3_sd_is_valid(&transmitter->sequence.sd) &&
		       transmitter->sequence.sd.kind == V90_PHASE3_SD_BAR &&
		       v90_phase3_sd_remaining(&transmitter->sequence.sd) != 0;
	case V90_PHASE3_DIGITAL_TX_TRN1D:
		return v90_phase3_signs_is_valid(&transmitter->sequence.signs) &&
		       transmitter->sequence.signs.phase ==
			       V90_PHASE3_SIGNS_TRN1D &&
		       transmitter->sequence.signs.trn1d_symbols <
			       V90_PHASE3_TRN1D_MIN_SYMBOLS;
	case V90_PHASE3_DIGITAL_TX_JD:
	case V90_PHASE3_DIGITAL_TX_JD_PRIME:
	case V90_PHASE3_DIGITAL_TX_COMPLETE:
		return v90_phase3_signs_is_valid(&transmitter->sequence.signs) &&
		       transmitter->sequence.signs.phase == V90_PHASE3_SIGNS_JD &&
		       transmitter->sequence.signs.trn1d_symbols ==
			       V90_PHASE3_TRN1D_MIN_SYMBOLS;
	}
	return false;
}

enum v90_phase3_digital_tx_result v90_phase3_digital_tx_init(
	struct v90_phase3_digital_tx *transmitter, enum g711_law law,
	uint8_t training_ucode,
	const struct v90_jd_parameters *jd_parameters)
{
	struct v90_phase3_digital_tx initialized = {
		.stage = V90_PHASE3_DIGITAL_TX_SD,
	};

	if (transmitter == NULL || jd_parameters == NULL ||
	    v90_jd_encode(
		    jd_parameters, initialized.jd_frame,
		    sizeof(initialized.jd_frame)) != V90_JD_OK ||
	    v90_phase3_sd_init(
		    &initialized.sequence.sd, law, training_ucode,
		    V90_PHASE3_SD) != V90_PHASE3_SD_OK ||
	    !v90_phase3_digital_tx_is_valid(&initialized))
		return V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V90_PHASE3_DIGITAL_TX_OK;
}

static enum v90_phase3_digital_tx_result next_sd(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	enum g711_law law = transmitter->sequence.sd.law;
	uint8_t training_ucode = transmitter->sequence.sd.training_ucode;
	enum v90_phase3_sd_kind kind = transmitter->sequence.sd.kind;

	if (v90_phase3_sd_next(&transmitter->sequence.sd, symbol) !=
	    V90_PHASE3_SD_OK)
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	if (v90_phase3_sd_remaining(&transmitter->sequence.sd) != 0)
		return V90_PHASE3_DIGITAL_TX_OK;
	transmitter->sequence =
		(union v90_phase3_digital_tx_sequence){ 0 };
	if (kind == V90_PHASE3_SD) {
		if (v90_phase3_sd_init(
			    &transmitter->sequence.sd, law, training_ucode,
			    V90_PHASE3_SD_BAR) != V90_PHASE3_SD_OK)
			return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
		transmitter->stage = V90_PHASE3_DIGITAL_TX_SD_BAR;
	} else {
		if (v90_phase3_signs_init(
			    &transmitter->sequence.signs, law,
			    training_ucode) != V90_PHASE3_SIGNS_OK)
			return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
		transmitter->stage = V90_PHASE3_DIGITAL_TX_TRN1D;
	}
	return V90_PHASE3_DIGITAL_TX_OK;
}

static enum v90_phase3_digital_tx_result next_trn1d(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_trn1d(
		    &transmitter->sequence.signs, symbol) !=
	    V90_PHASE3_SIGNS_OK)
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	if (transmitter->sequence.signs.trn1d_symbols ==
	    V90_PHASE3_TRN1D_MIN_SYMBOLS) {
		if (v90_phase3_signs_begin_jd(&transmitter->sequence.signs) !=
		    V90_PHASE3_SIGNS_OK)
			return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
		transmitter->stage = V90_PHASE3_DIGITAL_TX_JD;
	}
	return V90_PHASE3_DIGITAL_TX_OK;
}

static uint8_t jd_bit(
	const struct v90_phase3_digital_tx *transmitter)
{
	return (uint8_t)(
		(transmitter->jd_frame[transmitter->bit_index / 8u] >>
		 (7u - transmitter->bit_index % 8u)) &
		1u);
}

static enum v90_phase3_digital_tx_result next_jd(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, jd_bit(transmitter),
		    symbol) != V90_PHASE3_SIGNS_OK)
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index != V90_JD_FRAME_BITS)
		return V90_PHASE3_DIGITAL_TX_OK;
	if (transmitter->jd_records == UINT32_MAX - 1u)
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->jd_records++;
	transmitter->bit_index = 0;
	if (transmitter->finish_jd_requested)
		transmitter->stage = V90_PHASE3_DIGITAL_TX_JD_PRIME;
	return V90_PHASE3_DIGITAL_TX_OK;
}

static enum v90_phase3_digital_tx_result next_jd_prime(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	if (v90_phase3_signs_next_jd_bit(
		    &transmitter->sequence.signs, 0, symbol) !=
	    V90_PHASE3_SIGNS_OK)
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	transmitter->bit_index++;
	if (transmitter->bit_index == V90_PHASE3_JD_PRIME_BITS)
		transmitter->stage = V90_PHASE3_DIGITAL_TX_COMPLETE;
	return V90_PHASE3_DIGITAL_TX_OK;
}

enum v90_phase3_digital_tx_result v90_phase3_digital_tx_next(
	struct v90_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct v90_phase3_digital_tx next;
	struct v90_pcm_symbol generated;
	enum v90_phase3_digital_tx_result result;

	if (!v90_phase3_digital_tx_is_valid(transmitter) || symbol == NULL)
		return V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage == V90_PHASE3_DIGITAL_TX_COMPLETE)
		return V90_PHASE3_DIGITAL_TX_DONE;
	next = *transmitter;
	switch (next.stage) {
	case V90_PHASE3_DIGITAL_TX_SD:
	case V90_PHASE3_DIGITAL_TX_SD_BAR:
		result = next_sd(&next, &generated);
		break;
	case V90_PHASE3_DIGITAL_TX_TRN1D:
		result = next_trn1d(&next, &generated);
		break;
	case V90_PHASE3_DIGITAL_TX_JD:
		result = next_jd(&next, &generated);
		break;
	case V90_PHASE3_DIGITAL_TX_JD_PRIME:
		result = next_jd_prime(&next, &generated);
		break;
	case V90_PHASE3_DIGITAL_TX_COMPLETE:
		return V90_PHASE3_DIGITAL_TX_DONE;
	default:
		return V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	}
	if (result != V90_PHASE3_DIGITAL_TX_OK)
		return result;
	if (!v90_phase3_digital_tx_is_valid(&next))
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return V90_PHASE3_DIGITAL_TX_OK;
}

enum v90_phase3_digital_tx_result v90_phase3_digital_tx_finish_jd(
	struct v90_phase3_digital_tx *transmitter)
{
	struct v90_phase3_digital_tx next;

	if (!v90_phase3_digital_tx_is_valid(transmitter))
		return V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT;
	if (transmitter->stage != V90_PHASE3_DIGITAL_TX_JD)
		return V90_PHASE3_DIGITAL_TX_INVALID_STATE;
	next = *transmitter;
	next.finish_jd_requested = true;
	if (next.jd_records != 0 && next.bit_index == 0)
		next.stage = V90_PHASE3_DIGITAL_TX_JD_PRIME;
	if (!v90_phase3_digital_tx_is_valid(&next))
		return V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_PHASE3_DIGITAL_TX_OK;
}

const char *v90_phase3_digital_tx_result_name(
	enum v90_phase3_digital_tx_result result)
{
	switch (result) {
	case V90_PHASE3_DIGITAL_TX_OK:
		return "ok";
	case V90_PHASE3_DIGITAL_TX_DONE:
		return "done";
	case V90_PHASE3_DIGITAL_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_DIGITAL_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_DIGITAL_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
