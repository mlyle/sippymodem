// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_cpt_tx.h"

#include "packed_bits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool stage_is_valid(enum v92_pcm_phase3_cpt_tx_stage stage)
{
	return stage >= V92_PCM_PHASE3_CPT_TX_TRN1U &&
	       stage <= V92_PCM_PHASE3_CPT_TX_COMPLETE;
}

static bool counters_are_valid(
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	if (transmitter->completed_records == UINT32_MAX)
		return false;
	switch (transmitter->stage) {
	case V92_PCM_PHASE3_CPT_TX_TRN1U:
		return transmitter->preamble_symbols == 0u &&
		       transmitter->frame_bit_index == 0u &&
		       transmitter->completed_records == 0u &&
		       transmitter->e1u_symbols == 0u &&
		       !transmitter->finish_requested;
	case V92_PCM_PHASE3_CPT_TX_PREAMBLE:
		return transmitter->preamble_symbols <
			       V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS &&
		       transmitter->frame_bit_index == 0u &&
		       transmitter->completed_records == 0u &&
		       transmitter->e1u_symbols == 0u &&
		       !transmitter->finish_requested;
	case V92_PCM_PHASE3_CPT_TX_RECORD:
		return transmitter->preamble_symbols ==
			       V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS &&
		       transmitter->frame_bit_index <
			       transmitter->frame_bit_count &&
		       transmitter->e1u_symbols == 0u &&
		       (!transmitter->finish_requested ||
			transmitter->completed_records != 0u) &&
		       !(transmitter->finish_requested &&
			 transmitter->frame_bit_index == 0u);
	case V92_PCM_PHASE3_CPT_TX_E1U:
		return transmitter->preamble_symbols ==
			       V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS &&
		       transmitter->frame_bit_index == 0u &&
		       transmitter->completed_records != 0u &&
		       transmitter->e1u_symbols < V92_PCM_PHASE3_E1U_SYMBOLS &&
		       transmitter->finish_requested;
	case V92_PCM_PHASE3_CPT_TX_COMPLETE:
		return transmitter->preamble_symbols ==
			       V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS &&
		       transmitter->frame_bit_index == 0u &&
		       transmitter->completed_records != 0u &&
		       transmitter->e1u_symbols == V92_PCM_PHASE3_E1U_SYMBOLS &&
		       transmitter->finish_requested;
	}
	return false;
}

static bool state_is_valid(
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	bool uses_coding;

	if (transmitter == NULL || !stage_is_valid(transmitter->stage) ||
	    !v92_pcm_phase3_trn1u_is_valid(&transmitter->trn1u) ||
	    transmitter->frame_bit_count < V92_CPT_MIN_FRAME_BITS ||
	    transmitter->frame_bit_count > V92_CPT_MAX_FRAME_BITS ||
	    transmitter->frame_bit_count % V92_CPT_SYMBOLS_PER_FRAME != 0u ||
	    !packed_msb_tail_is_zero(
		    transmitter->frame, transmitter->frame_bit_count,
		    sizeof(transmitter->frame)) ||
	    !counters_are_valid(transmitter))
		return false;
	uses_coding = transmitter->stage != V92_PCM_PHASE3_CPT_TX_TRN1U;
	return !uses_coding ||
	       (v92_pcm_phase3_trn1u_can_handoff(&transmitter->trn1u) &&
		pcm_binary_sign_is_valid(&transmitter->coding) &&
		transmitter->coding.scrambler.polynomial == V34_POLYNOMIAL_GPA);
}

static bool frame_is_valid(
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	struct v92_cpt_parameters parameters;
	uint8_t canonical[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t canonical_bits = 0;
	size_t frame_bytes =
		(transmitter->frame_bit_count + 7u) / 8u;

	return v92_cpt_decode(
		       transmitter->frame, frame_bytes,
		       transmitter->frame_bit_count, &parameters) == V92_CPT_OK &&
	       parameters.law == transmitter->trn1u.law &&
	       v92_cpt_encode(
		       &parameters, canonical, sizeof(canonical),
		       &canonical_bits) == V92_CPT_OK &&
	       canonical_bits == transmitter->frame_bit_count &&
	       memcmp(transmitter->frame, canonical, sizeof(canonical)) == 0;
}

bool v92_pcm_phase3_cpt_tx_is_valid(
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	return state_is_valid(transmitter) && frame_is_valid(transmitter);
}

bool v92_pcm_phase3_cpt_tx_is_complete(
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	return v92_pcm_phase3_cpt_tx_is_valid(transmitter) &&
	       transmitter->stage == V92_PCM_PHASE3_CPT_TX_COMPLETE;
}

enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_init(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	enum g711_law law, uint8_t lu_ucode,
	const struct v92_cpt_parameters *parameters)
{
	struct v92_pcm_phase3_cpt_tx initialized = {
		.stage = V92_PCM_PHASE3_CPT_TX_TRN1U,
	};
	enum v92_cpt_result result;

	if (transmitter == NULL || parameters == NULL ||
	    !v92_cpt_parameters_are_valid(parameters) ||
	    parameters->law != law ||
	    v92_pcm_phase3_trn1u_init(
		    &initialized.trn1u, law, lu_ucode) !=
		    V92_PCM_PHASE3_TRN1U_OK)
		return V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT;
	result = v92_cpt_encode(
		parameters, initialized.frame, sizeof(initialized.frame),
		&initialized.frame_bit_count);
	if (result != V92_CPT_OK ||
	    !v92_pcm_phase3_cpt_tx_is_valid(&initialized))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_start(
	struct v92_pcm_phase3_cpt_tx *transmitter)
{
	struct v92_pcm_phase3_cpt_tx next;

	if (!v92_pcm_phase3_cpt_tx_is_valid(transmitter))
		return V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT;
	if (transmitter->stage != V92_PCM_PHASE3_CPT_TX_TRN1U ||
	    !v92_pcm_phase3_trn1u_can_handoff(&transmitter->trn1u))
		return V92_PCM_PHASE3_CPT_TX_INVALID_STATE;
	next = *transmitter;
	if (pcm_binary_sign_init_from_scrambler(
		    &next.coding, &next.trn1u.scrambler,
		    (uint8_t)(next.trn1u.scrambler.history & 1u)) !=
	    PCM_BINARY_SIGN_OK)
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	next.stage = V92_PCM_PHASE3_CPT_TX_PREAMBLE;
	if (!v92_pcm_phase3_cpt_tx_is_valid(&next))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

enum v92_pcm_phase3_cpt_tx_result
v92_pcm_phase3_cpt_tx_request_finish(
	struct v92_pcm_phase3_cpt_tx *transmitter)
{
	struct v92_pcm_phase3_cpt_tx next;

	if (!v92_pcm_phase3_cpt_tx_is_valid(transmitter))
		return V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT;
	if (transmitter->stage != V92_PCM_PHASE3_CPT_TX_RECORD ||
	    transmitter->completed_records == 0u ||
	    transmitter->finish_requested)
		return V92_PCM_PHASE3_CPT_TX_INVALID_STATE;
	next = *transmitter;
	next.finish_requested = true;
	if (next.frame_bit_index == 0u)
		next.stage = V92_PCM_PHASE3_CPT_TX_E1U;
	if (!v92_pcm_phase3_cpt_tx_is_valid(&next))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

static enum v92_pcm_phase3_cpt_tx_result next_encoded_bit(
	struct v92_pcm_phase3_cpt_tx *transmitter, uint8_t input,
	struct v90_pcm_symbol *symbol)
{
	uint8_t sign;

	if (pcm_binary_sign_encode(&transmitter->coding, input, &sign) !=
		    PCM_BINARY_SIGN_OK ||
	    !v90_pcm_symbol_make(
		    symbol, transmitter->trn1u.law,
		    transmitter->trn1u.lu_ucode, sign == 0u))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

static enum v92_pcm_phase3_cpt_tx_result next_preamble(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	enum v92_pcm_phase3_cpt_tx_result result =
		next_encoded_bit(transmitter, 1u, symbol);

	if (result != V92_PCM_PHASE3_CPT_TX_OK)
		return result;
	transmitter->preamble_symbols++;
	if (transmitter->preamble_symbols ==
	    V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS)
		transmitter->stage = V92_PCM_PHASE3_CPT_TX_RECORD;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

static enum v92_pcm_phase3_cpt_tx_result next_record(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	enum v92_pcm_phase3_cpt_tx_result result;

	if (transmitter->frame_bit_index == 0u &&
	    !frame_is_valid(transmitter))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	result = next_encoded_bit(
		transmitter,
		packed_msb_bit(
			transmitter->frame, transmitter->frame_bit_index),
		symbol);
	if (result != V92_PCM_PHASE3_CPT_TX_OK)
		return result;
	transmitter->frame_bit_index++;
	if (transmitter->frame_bit_index != transmitter->frame_bit_count)
		return V92_PCM_PHASE3_CPT_TX_OK;
	if (transmitter->completed_records == UINT32_MAX - 1u)
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	transmitter->completed_records++;
	transmitter->frame_bit_index = 0;
	if (transmitter->finish_requested)
		transmitter->stage = V92_PCM_PHASE3_CPT_TX_E1U;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

static enum v92_pcm_phase3_cpt_tx_result next_e1u(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	enum v92_pcm_phase3_cpt_tx_result result =
		next_encoded_bit(transmitter, 0u, symbol);

	if (result != V92_PCM_PHASE3_CPT_TX_OK)
		return result;
	transmitter->e1u_symbols++;
	if (transmitter->e1u_symbols == V92_PCM_PHASE3_E1U_SYMBOLS)
		transmitter->stage = V92_PCM_PHASE3_CPT_TX_COMPLETE;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

enum v92_pcm_phase3_cpt_tx_result v92_pcm_phase3_cpt_tx_next(
	struct v92_pcm_phase3_cpt_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct v92_pcm_phase3_cpt_tx next;
	struct v90_pcm_symbol generated;
	enum v92_pcm_phase3_cpt_tx_result result;

	if (!state_is_valid(transmitter) || symbol == NULL)
		return V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT;
	if (transmitter->stage == V92_PCM_PHASE3_CPT_TX_COMPLETE)
		return V92_PCM_PHASE3_CPT_TX_DONE;
	next = *transmitter;
	switch (next.stage) {
	case V92_PCM_PHASE3_CPT_TX_TRN1U:
		result = v92_pcm_phase3_trn1u_next(
				 &next.trn1u, &generated) ==
				 V92_PCM_PHASE3_TRN1U_OK ?
			 V92_PCM_PHASE3_CPT_TX_OK :
			 V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
		break;
	case V92_PCM_PHASE3_CPT_TX_PREAMBLE:
		result = next_preamble(&next, &generated);
		break;
	case V92_PCM_PHASE3_CPT_TX_RECORD:
		result = next_record(&next, &generated);
		break;
	case V92_PCM_PHASE3_CPT_TX_E1U:
		result = next_e1u(&next, &generated);
		break;
	case V92_PCM_PHASE3_CPT_TX_COMPLETE:
		return V92_PCM_PHASE3_CPT_TX_DONE;
	default:
		return V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT;
	}
	if (result != V92_PCM_PHASE3_CPT_TX_OK)
		return result;
	if (!state_is_valid(&next))
		return V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return V92_PCM_PHASE3_CPT_TX_OK;
}

const char *v92_pcm_phase3_cpt_tx_result_name(
	enum v92_pcm_phase3_cpt_tx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_CPT_TX_OK:
		return "ok";
	case V92_PCM_PHASE3_CPT_TX_DONE:
		return "done";
	case V92_PCM_PHASE3_CPT_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_CPT_TX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_CPT_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
