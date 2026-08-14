// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_sign_receiver.h"

#include "v90_info1.h"
#include "v90_phase3_signs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool stage_is_valid(enum v90_phase3_sign_receiver_stage stage)
{
	return stage >= V90_PHASE3_SIGN_RECEIVER_TRN1D &&
	       stage <= V90_PHASE3_SIGN_RECEIVER_DONE;
}

bool v90_phase3_sign_receiver_is_valid(
	const struct v90_phase3_sign_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_scrambler_is_valid(&receiver->descrambler) ||
	    receiver->descrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    !g711_law_is_valid(receiver->law) || !stage_is_valid(receiver->stage) ||
	    receiver->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    receiver->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    receiver->last_sign > 1 ||
	    receiver->frame_bits > V90_JD_FRAME_BITS ||
	    receiver->prime_zero_bits > V90_PHASE3_JD_PRIME_BITS)
		return false;

	switch (receiver->stage) {
	case V90_PHASE3_SIGN_RECEIVER_TRN1D:
		return receiver->frame_bits == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate &&
		       (receiver->trn1d_symbols != 0 ||
			(receiver->last_sign == 0 &&
			 receiver->descrambler.history == 0));
	case V90_PHASE3_SIGN_RECEIVER_JD_SEARCH:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->trn1d_symbols %
			       V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate;
	case V90_PHASE3_SIGN_RECEIVER_JD:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->frame_bits == V90_JD_FRAME_BITS &&
		       v90_jd_parameters_are_valid(&receiver->jd) &&
		       receiver->prime_zero_bits < V90_PHASE3_JD_PRIME_BITS &&
		       (receiver->prime_candidate ||
			receiver->prime_zero_bits == 0);
	case V90_PHASE3_SIGN_RECEIVER_DONE:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->frame_bits == V90_JD_FRAME_BITS &&
		       v90_jd_parameters_are_valid(&receiver->jd) &&
		       receiver->prime_candidate &&
		       receiver->prime_zero_bits == V90_PHASE3_JD_PRIME_BITS;
	}
	return false;
}

bool v90_phase3_sign_receiver_is_complete(
	const struct v90_phase3_sign_receiver *receiver)
{
	return v90_phase3_sign_receiver_is_valid(receiver) &&
	       receiver->stage == V90_PHASE3_SIGN_RECEIVER_DONE;
}

enum v90_phase3_sign_receiver_result v90_phase3_sign_receiver_init(
	struct v90_phase3_sign_receiver *receiver, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase3_sign_receiver initialized = {
		.law = law,
		.stage = V90_PHASE3_SIGN_RECEIVER_TRN1D,
		.training_ucode = training_ucode,
	};

	if (receiver == NULL || !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_PHASE3_SIGN_RECEIVER_OK;
}

enum v90_phase3_sign_receiver_result
v90_phase3_sign_receiver_begin_jd_search(
	struct v90_phase3_sign_receiver *receiver)
{
	if (receiver == NULL)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT;
	if (!v90_phase3_sign_receiver_is_valid(receiver) ||
	    receiver->stage != V90_PHASE3_SIGN_RECEIVER_TRN1D ||
	    receiver->trn1d_symbols < V90_PHASE3_TRN1D_MIN_SYMBOLS ||
	    receiver->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT != 0)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_STATE;
	receiver->stage = V90_PHASE3_SIGN_RECEIVER_JD_SEARCH;
	return V90_PHASE3_SIGN_RECEIVER_OK;
}

static enum v90_phase3_sign_receiver_result receive_trn1d(
	struct v90_phase3_sign_receiver *receiver, uint8_t sign)
{
	uint8_t bit;

	if (receiver->trn1d_symbols == UINT32_MAX ||
	    v34_descramble_group(
		    &receiver->descrambler, sign, 1, &bit) !=
		    V34_FRAMING_OK ||
	    bit != 1)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE;
	receiver->last_sign = sign;
	receiver->trn1d_symbols++;
	return receiver->trn1d_symbols == V90_PHASE3_TRN1D_MIN_SYMBOLS ?
		       V90_PHASE3_SIGN_RECEIVER_TRN1D_READY :
		       V90_PHASE3_SIGN_RECEIVER_OK;
}

static void push_bit(
	struct v90_phase3_sign_receiver *receiver, uint8_t bit)
{
	for (size_t index = 0; index + 1u < V90_JD_FRAME_BYTES; ++index)
		receiver->frame[index] = (uint8_t)(
			(receiver->frame[index] << 1) |
			(receiver->frame[index + 1u] >> 7));
	receiver->frame[V90_JD_FRAME_BYTES - 1u] = (uint8_t)(
		(receiver->frame[V90_JD_FRAME_BYTES - 1u] << 1) | bit);
	if (receiver->frame_bits < V90_JD_FRAME_BITS)
		receiver->frame_bits++;
}

static bool decode_frame(
	const struct v90_phase3_sign_receiver *receiver,
	struct v90_jd_parameters *parameters)
{
	if (receiver->frame_bits < V90_JD_FRAME_BITS)
		return false;
	return v90_jd_decode(
		       receiver->frame, sizeof(receiver->frame), parameters) ==
	       V90_JD_OK;
}

static bool parameters_equal(
	const struct v90_jd_parameters *left,
	const struct v90_jd_parameters *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static enum v90_phase3_sign_receiver_result receive_jd_bit(
	struct v90_phase3_sign_receiver *receiver, uint8_t bit)
{
	struct v90_jd_parameters decoded;

	push_bit(receiver, bit);
	if (decode_frame(receiver, &decoded)) {
		if (receiver->stage == V90_PHASE3_SIGN_RECEIVER_JD &&
		    !parameters_equal(&receiver->jd, &decoded))
			return V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE;
		if (receiver->stage == V90_PHASE3_SIGN_RECEIVER_JD) {
			receiver->prime_candidate = true;
			receiver->prime_zero_bits = 0;
			return V90_PHASE3_SIGN_RECEIVER_JD_REPEATED;
		}
		receiver->jd = decoded;
		receiver->stage = V90_PHASE3_SIGN_RECEIVER_JD;
		receiver->prime_candidate = true;
		return V90_PHASE3_SIGN_RECEIVER_JD_DETECTED;
	}
	if (receiver->stage != V90_PHASE3_SIGN_RECEIVER_JD ||
	    !receiver->prime_candidate)
		return V90_PHASE3_SIGN_RECEIVER_OK;
	if (bit != 0) {
		receiver->prime_candidate = false;
		receiver->prime_zero_bits = 0;
		return V90_PHASE3_SIGN_RECEIVER_OK;
	}
	receiver->prime_zero_bits++;
	if (receiver->prime_zero_bits < V90_PHASE3_JD_PRIME_BITS)
		return V90_PHASE3_SIGN_RECEIVER_OK;
	receiver->stage = V90_PHASE3_SIGN_RECEIVER_DONE;
	return V90_PHASE3_SIGN_RECEIVER_COMPLETE;
}

static enum v90_phase3_sign_receiver_result receive_jd(
	struct v90_phase3_sign_receiver *receiver, uint8_t sign)
{
	uint8_t differential = sign ^ receiver->last_sign;
	uint8_t bit;

	receiver->last_sign = sign;
	if (v34_descramble_group(
		    &receiver->descrambler, differential, 1, &bit) !=
	    V34_FRAMING_OK)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_STATE;
	return receive_jd_bit(receiver, bit);
}

enum v90_phase3_sign_receiver_result v90_phase3_sign_receiver_receive(
	struct v90_phase3_sign_receiver *receiver, uint8_t network_octet)
{
	struct v90_phase3_sign_receiver next;
	struct v90_pcm_symbol symbol;
	enum v90_phase3_sign_receiver_result result;

	if (receiver == NULL)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT;
	if (!v90_phase3_sign_receiver_is_valid(receiver))
		return V90_PHASE3_SIGN_RECEIVER_INVALID_STATE;
	if (receiver->stage == V90_PHASE3_SIGN_RECEIVER_DONE)
		return V90_PHASE3_SIGN_RECEIVER_COMPLETE;
	if (!v90_pcm_symbol_parse(&symbol, receiver->law, network_octet) ||
	    symbol.ucode != receiver->training_ucode)
		return V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE;
	next = *receiver;
	result = next.stage == V90_PHASE3_SIGN_RECEIVER_TRN1D ?
			 receive_trn1d(&next, symbol.positive ? 1u : 0u) :
			 receive_jd(&next, symbol.positive ? 1u : 0u);
	if (result != V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE &&
	    result != V90_PHASE3_SIGN_RECEIVER_INVALID_STATE)
		*receiver = next;
	return result;
}

bool v90_phase3_sign_receiver_jd(
	const struct v90_phase3_sign_receiver *receiver,
	struct v90_jd_parameters *parameters)
{
	if (!v90_phase3_sign_receiver_is_valid(receiver) ||
	    parameters == NULL ||
	    receiver->stage < V90_PHASE3_SIGN_RECEIVER_JD)
		return false;
	*parameters = receiver->jd;
	return true;
}

const char *v90_phase3_sign_receiver_result_name(
	enum v90_phase3_sign_receiver_result result)
{
	switch (result) {
	case V90_PHASE3_SIGN_RECEIVER_OK:
		return "ok";
	case V90_PHASE3_SIGN_RECEIVER_TRN1D_READY:
		return "TRN1d ready";
	case V90_PHASE3_SIGN_RECEIVER_JD_DETECTED:
		return "Jd detected";
	case V90_PHASE3_SIGN_RECEIVER_JD_REPEATED:
		return "Jd repeated";
	case V90_PHASE3_SIGN_RECEIVER_COMPLETE:
		return "complete";
	case V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_SIGN_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
