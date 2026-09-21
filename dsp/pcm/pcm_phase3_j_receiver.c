// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_j_receiver.h"

#include "v90_info1.h"
#include "v90_phase3_signs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert((int)V90_PHASE3_JD_PRIME_BITS == (int)V92_PHASE3_JP_PRIME_BITS,
	       "Jd-prime and Jp-prime have the same length");

static bool stage_is_valid(enum pcm_phase3_j_receiver_stage stage)
{
	return stage >= PCM_PHASE3_J_RECEIVER_TRN1D &&
	       stage <= PCM_PHASE3_J_RECEIVER_DONE;
}

static bool base_state_is_valid(
	const struct pcm_phase3_j_receiver *receiver)
{
	return pcm_protocol_is_valid(receiver->protocol) &&
	       v34_scrambler_is_valid(&receiver->descrambler) &&
	       receiver->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       g711_law_is_valid(receiver->law) && stage_is_valid(receiver->stage) &&
	       receiver->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       receiver->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       receiver->last_sign <= 1 &&
	       receiver->frame_bits <= PCM_J_FRAME_BITS &&
	       receiver->prime_zero_bits <= V90_PHASE3_JD_PRIME_BITS;
}

bool pcm_phase3_j_receiver_is_valid(
	const struct pcm_phase3_j_receiver *receiver)
{
	if (receiver == NULL || !base_state_is_valid(receiver))
		return false;
	switch (receiver->stage) {
	case PCM_PHASE3_J_RECEIVER_TRN1D:
		return receiver->frame_bits == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate &&
		       (receiver->trn1d_symbols != 0 ||
			(receiver->last_sign == 0 &&
			 receiver->descrambler.history == 0));
	case PCM_PHASE3_J_RECEIVER_JD_SEARCH:
		return receiver->trn1d_symbols >=
			       V90_PHASE3_TRN1D_MIN_SYMBOLS &&
		       receiver->trn1d_symbols %
			       V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
		       receiver->prime_zero_bits == 0 &&
		       !receiver->prime_candidate;
	case PCM_PHASE3_J_RECEIVER_JD:
	case PCM_PHASE3_J_RECEIVER_JP:
	case PCM_PHASE3_J_RECEIVER_DONE:
		break;
	}
	if (receiver->trn1d_symbols < V90_PHASE3_TRN1D_MIN_SYMBOLS ||
	    receiver->frame_bits != PCM_J_FRAME_BITS)
		return false;
	if (receiver->protocol == PCM_PROTOCOL_V90) {
		if (receiver->stage == PCM_PHASE3_J_RECEIVER_JP ||
		    !v90_jd_parameters_are_valid(&receiver->parameters.v90))
			return false;
	} else {
		if (!v92_jd_is_valid(&receiver->parameters.v92.jd))
			return false;
		if (receiver->stage == PCM_PHASE3_J_RECEIVER_JD)
			return receiver->prime_zero_bits == 0 &&
			       !receiver->prime_candidate;
		if (!v92_jp_is_valid(&receiver->parameters.v92.jp))
			return false;
	}
	if (receiver->stage == PCM_PHASE3_J_RECEIVER_DONE)
		return receiver->prime_candidate &&
		       receiver->prime_zero_bits == V90_PHASE3_JD_PRIME_BITS;
	return receiver->prime_zero_bits < V90_PHASE3_JD_PRIME_BITS &&
	       (receiver->prime_candidate || receiver->prime_zero_bits == 0);
}

bool pcm_phase3_j_receiver_is_complete(
	const struct pcm_phase3_j_receiver *receiver)
{
	return pcm_phase3_j_receiver_is_valid(receiver) &&
	       receiver->stage == PCM_PHASE3_J_RECEIVER_DONE;
}

enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_init(
	struct pcm_phase3_j_receiver *receiver, enum pcm_protocol protocol,
	enum g711_law law, uint8_t training_ucode)
{
	struct pcm_phase3_j_receiver initialized = {
		.protocol = protocol,
		.law = law,
		.stage = PCM_PHASE3_J_RECEIVER_TRN1D,
		.training_ucode = training_ucode,
	};

	if (receiver == NULL || !pcm_protocol_is_valid(protocol) ||
	    !g711_law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return PCM_PHASE3_J_RECEIVER_OK;
}

enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_begin_jd_search(
	struct pcm_phase3_j_receiver *receiver)
{
	if (receiver == NULL)
		return PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (!pcm_phase3_j_receiver_is_valid(receiver) ||
	    receiver->stage != PCM_PHASE3_J_RECEIVER_TRN1D ||
	    receiver->trn1d_symbols < V90_PHASE3_TRN1D_MIN_SYMBOLS ||
	    receiver->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT != 0)
		return PCM_PHASE3_J_RECEIVER_INVALID_STATE;
	receiver->stage = PCM_PHASE3_J_RECEIVER_JD_SEARCH;
	return PCM_PHASE3_J_RECEIVER_OK;
}

static enum pcm_phase3_j_receiver_result receive_trn1d(
	struct pcm_phase3_j_receiver *receiver, uint8_t sign)
{
	uint8_t bit;

	if (receiver->trn1d_symbols == UINT32_MAX ||
	    v34_descramble_group(
		    &receiver->descrambler, sign, 1, &bit) !=
		    V34_FRAMING_OK)
		return PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
	/* Keep the self-synchronizing GPC history moving across
	 * individual slicing errors. Repeated CRC-protected J records, not
	 * error-free training ones, authorize the next protocol transitions. */
	(void)bit;
	receiver->last_sign = sign;
	receiver->trn1d_symbols++;
	return receiver->trn1d_symbols == V90_PHASE3_TRN1D_MIN_SYMBOLS ?
		       PCM_PHASE3_J_RECEIVER_TRN1D_READY :
		       PCM_PHASE3_J_RECEIVER_OK;
}

static void push_bit(struct pcm_phase3_j_receiver *receiver, uint8_t bit)
{
	for (size_t index = 0; index + 1u < PCM_J_FRAME_BYTES; ++index)
		receiver->frame[index] = (uint8_t)(
			(receiver->frame[index] << 1) |
			(receiver->frame[index + 1u] >> 7));
	receiver->frame[PCM_J_FRAME_BYTES - 1u] = (uint8_t)(
		(receiver->frame[PCM_J_FRAME_BYTES - 1u] << 1) | bit);
	if (receiver->frame_bits < PCM_J_FRAME_BITS)
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

static enum pcm_phase3_j_receiver_result detect_v90_jd(
	struct pcm_phase3_j_receiver *receiver)
{
	struct v90_jd_parameters decoded;
	const struct v90_jd_parameters *jd = &receiver->parameters.v90;
	bool repeated = receiver->stage == PCM_PHASE3_J_RECEIVER_JD;

	if (v90_jd_decode(receiver->frame, sizeof(receiver->frame), &decoded) !=
	    V90_JD_OK)
		return PCM_PHASE3_J_RECEIVER_OK;
	if (repeated &&
	    (jd->data_rate_capability_mask != decoded.data_rate_capability_mask ||
	     jd->training_constellation != decoded.training_constellation ||
	     jd->rate_renegotiation_constellation !=
		     decoded.rate_renegotiation_constellation ||
	     jd->maximum_spectral_shaping_lookahead !=
		     decoded.maximum_spectral_shaping_lookahead))
		return PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
	if (!repeated)
		receiver->parameters.v90 = decoded;
	receiver->stage = PCM_PHASE3_J_RECEIVER_JD;
	receiver->prime_candidate = true;
	receiver->prime_zero_bits = 0;
	return repeated ? PCM_PHASE3_J_RECEIVER_JD_REPEATED :
			  PCM_PHASE3_J_RECEIVER_JD_DETECTED;
}

static enum pcm_phase3_j_receiver_result detect_v92_jd(
	struct pcm_phase3_j_receiver *receiver)
{
	struct v92_jd decoded;

	if (v92_jd_decode(receiver->frame, sizeof(receiver->frame), &decoded) !=
		    V92_J_OK)
		return PCM_PHASE3_J_RECEIVER_OK;
	if (receiver->stage == PCM_PHASE3_J_RECEIVER_JD) {
		if (!jd_equal(&receiver->parameters.v92.jd, &decoded))
			return PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
		return PCM_PHASE3_J_RECEIVER_JD_REPEATED;
	}
	receiver->parameters.v92.jd = decoded;
	receiver->stage = PCM_PHASE3_J_RECEIVER_JD;
	return PCM_PHASE3_J_RECEIVER_JD_DETECTED;
}

static enum pcm_phase3_j_receiver_result detect_jp(
	struct pcm_phase3_j_receiver *receiver)
{
	struct v92_jp decoded;

	if (v92_jp_decode(receiver->frame, sizeof(receiver->frame), &decoded) !=
	    V92_J_OK)
		return PCM_PHASE3_J_RECEIVER_OK;
	if (receiver->stage == PCM_PHASE3_J_RECEIVER_JP) {
		if (!jp_equal(&receiver->parameters.v92.jp, &decoded))
			return PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE;
		receiver->prime_candidate = true;
		receiver->prime_zero_bits = 0;
		return PCM_PHASE3_J_RECEIVER_JP_REPEATED;
	}
	receiver->parameters.v92.jp = decoded;
	receiver->stage = PCM_PHASE3_J_RECEIVER_JP;
	receiver->prime_candidate = true;
	receiver->prime_zero_bits = 0;
	return PCM_PHASE3_J_RECEIVER_JP_DETECTED;
}

static enum pcm_phase3_j_receiver_result receive_record_bit(
	struct pcm_phase3_j_receiver *receiver, uint8_t bit)
{
	enum pcm_phase3_j_receiver_result result;

	push_bit(receiver, bit);
	if (receiver->stage == PCM_PHASE3_J_RECEIVER_JD_SEARCH ||
	    receiver->stage == PCM_PHASE3_J_RECEIVER_JD) {
		if (receiver->frame_bits < PCM_J_FRAME_BITS)
			return PCM_PHASE3_J_RECEIVER_OK;
		result = receiver->protocol == PCM_PROTOCOL_V90 ?
				 detect_v90_jd(receiver) : detect_v92_jd(receiver);
		if (result != PCM_PHASE3_J_RECEIVER_OK)
			return result;
	}
	if (receiver->protocol == PCM_PROTOCOL_V92 &&
	    (receiver->stage == PCM_PHASE3_J_RECEIVER_JD ||
	     receiver->stage == PCM_PHASE3_J_RECEIVER_JP)) {
		result = detect_jp(receiver);
		if (result != PCM_PHASE3_J_RECEIVER_OK)
			return result;
	}
	if (!receiver->prime_candidate)
		return PCM_PHASE3_J_RECEIVER_OK;
	if (bit != 0) {
		receiver->prime_candidate = false;
		receiver->prime_zero_bits = 0;
		return PCM_PHASE3_J_RECEIVER_OK;
	}
	receiver->prime_zero_bits++;
	if (receiver->prime_zero_bits < V90_PHASE3_JD_PRIME_BITS)
		return PCM_PHASE3_J_RECEIVER_OK;
	receiver->stage = PCM_PHASE3_J_RECEIVER_DONE;
	return PCM_PHASE3_J_RECEIVER_COMPLETE;
}

static enum pcm_phase3_j_receiver_result receive_record(
	struct pcm_phase3_j_receiver *receiver, uint8_t sign)
{
	uint8_t differential = sign ^ receiver->last_sign;
	uint8_t bit;

	receiver->last_sign = sign;
	if (v34_descramble_group(
		    &receiver->descrambler, differential, 1, &bit) !=
	    V34_FRAMING_OK)
		return PCM_PHASE3_J_RECEIVER_INVALID_STATE;
	return receive_record_bit(receiver, bit);
}

enum pcm_phase3_j_receiver_result pcm_phase3_j_receiver_receive(
	struct pcm_phase3_j_receiver *receiver, uint8_t network_octet)
{
	struct pcm_phase3_j_receiver next;
	struct v90_pcm_symbol symbol;
	enum pcm_phase3_j_receiver_result result;

	if (receiver == NULL)
		return PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT;
	if (!pcm_phase3_j_receiver_is_valid(receiver))
		return PCM_PHASE3_J_RECEIVER_INVALID_STATE;
	if (receiver->stage == PCM_PHASE3_J_RECEIVER_DONE)
		return PCM_PHASE3_J_RECEIVER_COMPLETE;
	if (!v90_pcm_symbol_parse(&symbol, receiver->law, network_octet))
		return PCM_PHASE3_J_RECEIVER_INVALID_STATE;
	/* Sd has qualified the training signal. The information in TRN1d and
	 * Jd/Jp is carried by sign; an analogue channel need not preserve the
	 * transmitter's amplitude code. */
	next = *receiver;
	result = next.stage == PCM_PHASE3_J_RECEIVER_TRN1D ?
			 receive_trn1d(&next, symbol.positive ? 1u : 0u) :
			 receive_record(&next, symbol.positive ? 1u : 0u);
	if (result != PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE &&
	    result != PCM_PHASE3_J_RECEIVER_INVALID_STATE)
		*receiver = next;
	return result;
}

bool pcm_phase3_j_receiver_v90_jd(
	const struct pcm_phase3_j_receiver *receiver,
	struct v90_jd_parameters *jd)
{
	if (!pcm_phase3_j_receiver_is_valid(receiver) || jd == NULL ||
	    receiver->protocol != PCM_PROTOCOL_V90 ||
	    receiver->stage < PCM_PHASE3_J_RECEIVER_JD)
		return false;
	*jd = receiver->parameters.v90;
	return true;
}

bool pcm_phase3_j_receiver_v92_jd(
	const struct pcm_phase3_j_receiver *receiver, struct v92_jd *jd)
{
	if (!pcm_phase3_j_receiver_is_valid(receiver) || jd == NULL ||
	    receiver->protocol != PCM_PROTOCOL_V92 ||
	    receiver->stage < PCM_PHASE3_J_RECEIVER_JD)
		return false;
	*jd = receiver->parameters.v92.jd;
	return true;
}

bool pcm_phase3_j_receiver_v92_jp(
	const struct pcm_phase3_j_receiver *receiver, struct v92_jp *jp)
{
	if (!pcm_phase3_j_receiver_is_valid(receiver) || jp == NULL ||
	    receiver->protocol != PCM_PROTOCOL_V92 ||
	    receiver->stage < PCM_PHASE3_J_RECEIVER_JP)
		return false;
	*jp = receiver->parameters.v92.jp;
	return true;
}

const char *pcm_phase3_j_receiver_result_name(
	enum pcm_phase3_j_receiver_result result)
{
	switch (result) {
	case PCM_PHASE3_J_RECEIVER_OK:
		return "ok";
	case PCM_PHASE3_J_RECEIVER_TRN1D_READY:
		return "TRN1d ready";
	case PCM_PHASE3_J_RECEIVER_JD_DETECTED:
		return "Jd detected";
	case PCM_PHASE3_J_RECEIVER_JD_REPEATED:
		return "Jd repeated";
	case PCM_PHASE3_J_RECEIVER_JP_DETECTED:
		return "Jp detected";
	case PCM_PHASE3_J_RECEIVER_JP_REPEATED:
		return "Jp repeated";
	case PCM_PHASE3_J_RECEIVER_COMPLETE:
		return "complete";
	case PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_PHASE3_J_RECEIVER_INVALID_STATE:
		return "invalid state";
	case PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
