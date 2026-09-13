// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase3_j_receiver.h"

#include "v90_info1.h"
#include "v90_phase3_signs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool stage_is_valid(enum v92_phase3_j_receiver_stage stage)
{
	return stage >= V92_PHASE3_J_RECEIVER_TRN1D &&
	       stage <= V92_PHASE3_J_RECEIVER_DONE;
}

static bool base_state_is_valid(
	const struct v92_phase3_j_receiver *receiver)
{
	return v34_scrambler_is_valid(&receiver->descrambler) &&
	       receiver->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       g711_law_is_valid(receiver->law) && stage_is_valid(receiver->stage) &&
	       receiver->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       receiver->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       receiver->last_sign <= 1 &&
	       receiver->frame_bits <= V92_J_FRAME_BITS &&
	       receiver->prime_zero_bits <= V92_PHASE3_JP_PRIME_BITS;
}

bool v92_phase3_j_receiver_is_valid(
	const struct v92_phase3_j_receiver *receiver)
{
	if (receiver == NULL || !base_state_is_valid(receiver))
		return false;
	switch (receiver->stage) {
	case V92_PHASE3_J_RECEIVER_TRN1D:
		return receiver->frame_bits == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate &&
		       (receiver->trn1d_symbols != 0 ||
			(receiver->last_sign == 0 &&
			 receiver->descrambler.history == 0));
	case V92_PHASE3_J_RECEIVER_JD_SEARCH:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->trn1d_symbols %
			       V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate;
	case V92_PHASE3_J_RECEIVER_JD:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->frame_bits == V92_J_FRAME_BITS &&
		       v92_jd_is_valid(&receiver->jd) &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate;
	case V92_PHASE3_J_RECEIVER_JP:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->frame_bits == V92_J_FRAME_BITS &&
		       v92_jd_is_valid(&receiver->jd) &&
		       v92_jp_is_valid(&receiver->jp) &&
		       receiver->prime_zero_bits <
			       V92_PHASE3_JP_PRIME_BITS &&
		       (receiver->prime_candidate ||
			receiver->prime_zero_bits == 0);
	case V92_PHASE3_J_RECEIVER_DONE:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->frame_bits == V92_J_FRAME_BITS &&
		       v92_jd_is_valid(&receiver->jd) &&
		       v92_jp_is_valid(&receiver->jp) &&
		       receiver->prime_candidate &&
		       receiver->prime_zero_bits ==
			       V92_PHASE3_JP_PRIME_BITS;
	}
	return false;
}

bool v92_phase3_j_receiver_is_complete(
	const struct v92_phase3_j_receiver *receiver)
{
	return v92_phase3_j_receiver_is_valid(receiver) &&
	       receiver->stage == V92_PHASE3_J_RECEIVER_DONE;
}

enum v92_phase3_j_receiver_result v92_phase3_j_receiver_init(
	struct v92_phase3_j_receiver *receiver, enum g711_law law,
	uint8_t training_ucode)
{
	struct v92_phase3_j_receiver initialized = {
		.law = law,
		.stage = V92_PHASE3_J_RECEIVER_TRN1D,
		.training_ucode = training_ucode,
	};

	if (receiver == NULL || !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_PHASE3_J_RECEIVER_OK;
}

enum v92_phase3_j_receiver_result v92_phase3_j_receiver_begin_jd_search(
	struct v92_phase3_j_receiver *receiver)
{
	if (receiver == NULL)
		return V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (!v92_phase3_j_receiver_is_valid(receiver) ||
	    receiver->stage != V92_PHASE3_J_RECEIVER_TRN1D ||
	    receiver->trn1d_symbols < V90_PHASE3_TRN1D_MIN_SYMBOLS ||
	    receiver->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT != 0)
		return V92_PHASE3_J_RECEIVER_INVALID_STATE;
	receiver->stage = V92_PHASE3_J_RECEIVER_JD_SEARCH;
	return V92_PHASE3_J_RECEIVER_OK;
}

static enum v92_phase3_j_receiver_result receive_trn1d(
	struct v92_phase3_j_receiver *receiver, uint8_t sign)
{
	uint8_t bit;

	if (receiver->trn1d_symbols == UINT32_MAX ||
	    v34_descramble_group(
		    &receiver->descrambler, sign, 1, &bit) !=
		    V34_FRAMING_OK)
		return V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
	/* As in V.90, keep the self-synchronizing GPC history moving across
	 * individual slicing errors. Repeated CRC-protected J records, not
	 * error-free training ones, authorize the next protocol transitions. */
	(void)bit;
	receiver->last_sign = sign;
	receiver->trn1d_symbols++;
	return receiver->trn1d_symbols == V90_PHASE3_TRN1D_MIN_SYMBOLS ?
		       V92_PHASE3_J_RECEIVER_TRN1D_READY :
		       V92_PHASE3_J_RECEIVER_OK;
}

static void push_bit(struct v92_phase3_j_receiver *receiver, uint8_t bit)
{
	for (size_t index = 0; index + 1u < V92_J_FRAME_BYTES; ++index)
		receiver->frame[index] = (uint8_t)(
			(receiver->frame[index] << 1) |
			(receiver->frame[index + 1u] >> 7));
	receiver->frame[V92_J_FRAME_BYTES - 1u] = (uint8_t)(
		(receiver->frame[V92_J_FRAME_BYTES - 1u] << 1) | bit);
	if (receiver->frame_bits < V92_J_FRAME_BITS)
		receiver->frame_bits++;
}

static bool jd_equal(const struct v92_jd *left, const struct v92_jd *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static bool jp_equal(const struct v92_jp *left, const struct v92_jp *right)
{
	return left->su_phase_extension_q16 == right->su_phase_extension_q16 &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation;
}

static enum v92_phase3_j_receiver_result detect_jd(
	struct v92_phase3_j_receiver *receiver)
{
	struct v92_jd decoded;

	if (receiver->frame_bits < V92_J_FRAME_BITS ||
	    v92_jd_decode(receiver->frame, sizeof(receiver->frame), &decoded) !=
		    V92_J_OK)
		return V92_PHASE3_J_RECEIVER_OK;
	if (receiver->stage == V92_PHASE3_J_RECEIVER_JD) {
		if (!jd_equal(&receiver->jd, &decoded))
			return V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
		return V92_PHASE3_J_RECEIVER_JD_REPEATED;
	}
	receiver->jd = decoded;
	receiver->stage = V92_PHASE3_J_RECEIVER_JD;
	return V92_PHASE3_J_RECEIVER_JD_DETECTED;
}

static enum v92_phase3_j_receiver_result detect_jp(
	struct v92_phase3_j_receiver *receiver)
{
	struct v92_jp decoded;

	if (v92_jp_decode(receiver->frame, sizeof(receiver->frame), &decoded) !=
	    V92_J_OK)
		return V92_PHASE3_J_RECEIVER_OK;
	if (receiver->stage == V92_PHASE3_J_RECEIVER_JP) {
		if (!jp_equal(&receiver->jp, &decoded))
			return V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
		receiver->prime_candidate = true;
		receiver->prime_zero_bits = 0;
		return V92_PHASE3_J_RECEIVER_JP_REPEATED;
	}
	receiver->jp = decoded;
	receiver->stage = V92_PHASE3_J_RECEIVER_JP;
	receiver->prime_candidate = true;
	receiver->prime_zero_bits = 0;
	return V92_PHASE3_J_RECEIVER_JP_DETECTED;
}

static enum v92_phase3_j_receiver_result receive_record_bit(
	struct v92_phase3_j_receiver *receiver, uint8_t bit)
{
	enum v92_phase3_j_receiver_result result;

	push_bit(receiver, bit);
	if (receiver->stage == V92_PHASE3_J_RECEIVER_JD_SEARCH ||
	    receiver->stage == V92_PHASE3_J_RECEIVER_JD) {
		result = detect_jd(receiver);
		if (result != V92_PHASE3_J_RECEIVER_OK)
			return result;
	}
	if (receiver->stage == V92_PHASE3_J_RECEIVER_JD ||
	    receiver->stage == V92_PHASE3_J_RECEIVER_JP) {
		result = detect_jp(receiver);
		if (result != V92_PHASE3_J_RECEIVER_OK)
			return result;
	}
	if (receiver->stage != V92_PHASE3_J_RECEIVER_JP ||
	    !receiver->prime_candidate)
		return V92_PHASE3_J_RECEIVER_OK;
	if (bit != 0) {
		receiver->prime_candidate = false;
		receiver->prime_zero_bits = 0;
		return V92_PHASE3_J_RECEIVER_OK;
	}
	receiver->prime_zero_bits++;
	if (receiver->prime_zero_bits < V92_PHASE3_JP_PRIME_BITS)
		return V92_PHASE3_J_RECEIVER_OK;
	receiver->stage = V92_PHASE3_J_RECEIVER_DONE;
	return V92_PHASE3_J_RECEIVER_COMPLETE;
}

static enum v92_phase3_j_receiver_result receive_record(
	struct v92_phase3_j_receiver *receiver, uint8_t sign)
{
	uint8_t differential = sign ^ receiver->last_sign;
	uint8_t bit;

	receiver->last_sign = sign;
	if (v34_descramble_group(
		    &receiver->descrambler, differential, 1, &bit) !=
	    V34_FRAMING_OK)
		return V92_PHASE3_J_RECEIVER_INVALID_STATE;
	return receive_record_bit(receiver, bit);
}

enum v92_phase3_j_receiver_result v92_phase3_j_receiver_receive(
	struct v92_phase3_j_receiver *receiver, uint8_t network_octet)
{
	struct v92_phase3_j_receiver next;
	struct v90_pcm_symbol symbol;
	enum v92_phase3_j_receiver_result result;

	if (receiver == NULL)
		return V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (!v92_phase3_j_receiver_is_valid(receiver))
		return V92_PHASE3_J_RECEIVER_INVALID_STATE;
	if (receiver->stage == V92_PHASE3_J_RECEIVER_DONE)
		return V92_PHASE3_J_RECEIVER_COMPLETE;
	if (!v90_pcm_symbol_parse(&symbol, receiver->law, network_octet))
		return V92_PHASE3_J_RECEIVER_INVALID_STATE;
	/* Sd has qualified the training signal. The information in TRN1d and
	 * Jd/Jp is carried by sign; an analogue channel need not preserve the
	 * transmitter's amplitude code. */
	next = *receiver;
	result = next.stage == V92_PHASE3_J_RECEIVER_TRN1D ?
			 receive_trn1d(&next, symbol.positive ? 1u : 0u) :
			 receive_record(&next, symbol.positive ? 1u : 0u);
	if (result != V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE &&
	    result != V92_PHASE3_J_RECEIVER_INVALID_STATE)
		*receiver = next;
	return result;
}

bool v92_phase3_j_receiver_jd(
	const struct v92_phase3_j_receiver *receiver, struct v92_jd *jd)
{
	if (!v92_phase3_j_receiver_is_valid(receiver) || jd == NULL ||
	    receiver->stage < V92_PHASE3_J_RECEIVER_JD)
		return false;
	*jd = receiver->jd;
	return true;
}

bool v92_phase3_j_receiver_jp(
	const struct v92_phase3_j_receiver *receiver, struct v92_jp *jp)
{
	if (!v92_phase3_j_receiver_is_valid(receiver) || jp == NULL ||
	    receiver->stage < V92_PHASE3_J_RECEIVER_JP)
		return false;
	*jp = receiver->jp;
	return true;
}

const char *v92_phase3_j_receiver_result_name(
	enum v92_phase3_j_receiver_result result)
{
	switch (result) {
	case V92_PHASE3_J_RECEIVER_OK:
		return "ok";
	case V92_PHASE3_J_RECEIVER_TRN1D_READY:
		return "TRN1d ready";
	case V92_PHASE3_J_RECEIVER_JD_DETECTED:
		return "Jd detected";
	case V92_PHASE3_J_RECEIVER_JD_REPEATED:
		return "Jd repeated";
	case V92_PHASE3_J_RECEIVER_JP_DETECTED:
		return "Jp detected";
	case V92_PHASE3_J_RECEIVER_JP_REPEATED:
		return "Jp repeated";
	case V92_PHASE3_J_RECEIVER_COMPLETE:
		return "complete";
	case V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE3_J_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
