// SPDX-License-Identifier: BSD-3-Clause

#include "v90_trn2d_rx.h"

#include "v90_pcm_symbol.h"

#include <string.h>

static bool pending_is_valid(const struct v90_trn2d_rx *receiver)
{
	const struct v90_pcm_frame_decoder *decoder = &receiver->decoder;

	if (receiver->pending_count >= V90_CP_FRAME_INTERVALS)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (interval >= receiver->pending_count) {
			if (receiver->pending_ucodes[interval] != 0 ||
			    receiver->pending_signs[interval] != 0)
				return false;
			continue;
		}
		uint8_t label;

		if (receiver->pending_signs[interval] > 1 ||
		    v90_pcm_mapping_label(
			    &decoder->mapping, decoder->decision_plane,
			    interval, receiver->pending_ucodes[interval],
			    &label) != V90_PCM_MAPPING_OK)
			return false;
	}
	return true;
}

static bool accounting_is_valid(const struct v90_trn2d_rx *receiver)
{
	uint64_t frame_bits = receiver->decoder.mapping.frame_bits;

	return receiver->received_frames ==
		       receiver->received_symbols / V90_CP_FRAME_INTERVALS &&
	       receiver->pending_count ==
		       receiver->received_symbols % V90_CP_FRAME_INTERVALS &&
	       receiver->received_frames <= UINT64_MAX / frame_bits &&
	       receiver->received_bits ==
		       receiver->received_frames * frame_bits &&
	       receiver->training_bit_errors <= receiver->received_bits;
}

bool v90_trn2d_rx_is_valid(const struct v90_trn2d_rx *receiver)
{
	return receiver != NULL &&
	       v90_pcm_frame_decoder_is_valid(&receiver->decoder) &&
	       receiver->decoder.mapping.kind == V90_CP_TRAINING &&
	       v34_scrambler_is_valid(&receiver->descrambler) &&
	       receiver->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       pending_is_valid(receiver) && accounting_is_valid(receiver);
}

enum v90_trn2d_rx_result v90_trn2d_rx_init(
	struct v90_trn2d_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_trn2d_rx initialized = { 0 };
	enum v90_pcm_frame_decoder_result decoder_result;

	if (receiver == NULL || training_mapping == NULL)
		return V90_TRN2D_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING)
		return V90_TRN2D_RX_INVALID_PARAMETERS;
	decoder_result = v90_pcm_frame_decoder_init(
		&initialized.decoder, training_mapping, decision_plane);
	if (decoder_result == V90_PCM_FRAME_DECODER_INVALID_ARGUMENT)
		return V90_TRN2D_RX_INVALID_ARGUMENT;
	if (decoder_result != V90_PCM_FRAME_DECODER_OK ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_TRN2D_RX_INTERNAL_ERROR;
	if (!v90_trn2d_rx_is_valid(&initialized))
		return V90_TRN2D_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_TRN2D_RX_OK;
}

bool v90_trn2d_rx_minimum_received(
	const struct v90_trn2d_rx *receiver)
{
	return v90_trn2d_rx_is_valid(receiver) &&
	       receiver->received_symbols >= V90_TRN2D_MINIMUM_SYMBOLS;
}

static bool decision_is_valid(
	const struct v90_trn2d_rx *receiver, uint8_t ucode)
{
	uint8_t label;

	return v90_pcm_mapping_label(
		       &receiver->decoder.mapping,
		       receiver->decoder.decision_plane,
		       receiver->pending_count, ucode, &label) ==
	       V90_PCM_MAPPING_OK;
}

static enum v90_trn2d_rx_result decode_frame(
	struct v90_trn2d_rx *receiver,
	struct v90_trn2d_frame *generated)
{
	struct v90_pcm_frame_decode_output decoded;
	struct v90_trn2d_frame decoded_frame = { 0 };
	uint8_t errors = 0;
	enum v90_pcm_frame_decoder_result result;

	if (receiver->received_bits >
	    UINT64_MAX - receiver->decoder.mapping.frame_bits)
		return V90_TRN2D_RX_OUT_OF_RANGE;
	result = v90_pcm_frame_decoder_decode(
		&receiver->decoder, receiver->pending_ucodes,
		receiver->pending_signs, receiver->pending_count, &decoded);
	if (result == V90_PCM_FRAME_DECODER_INVALID_SYMBOLS)
		return V90_TRN2D_RX_INVALID_SYMBOL;
	if (result != V90_PCM_FRAME_DECODER_OK)
		return V90_TRN2D_RX_INTERNAL_ERROR;
	for (size_t bit = 0; bit < decoded.bit_count; ++bit) {
		if (v34_descramble_group(
			    &receiver->descrambler, decoded.bits[bit], 1,
			    &decoded_frame.bits[bit]) != V34_FRAMING_OK)
			return V90_TRN2D_RX_INTERNAL_ERROR;
		if (decoded_frame.bits[bit] != 1)
			errors++;
	}
	memcpy(decoded_frame.rules, decoded.rules,
	       decoded.rule_count * sizeof(decoded.rules[0]));
	decoded_frame.bit_count = decoded.bit_count;
	decoded_frame.rule_count = decoded.rule_count;
	decoded_frame.training_bit_errors = errors;
	receiver->received_frames++;
	receiver->received_bits += decoded.bit_count;
	receiver->training_bit_errors += errors;
	receiver->pending_count = 0;
	memset(receiver->pending_ucodes, 0,
	       sizeof(receiver->pending_ucodes));
	memset(receiver->pending_signs, 0,
	       sizeof(receiver->pending_signs));
	*generated = decoded_frame;
	return V90_TRN2D_RX_FRAME_READY;
}

static enum v90_trn2d_rx_result receive_valid_decision(
	struct v90_trn2d_rx *receiver, uint8_t ucode, bool positive,
	struct v90_trn2d_frame *frame)
{
	struct v90_trn2d_rx next;
	struct v90_trn2d_frame generated;
	enum v90_trn2d_rx_result result;

	if (!decision_is_valid(receiver, ucode))
		return V90_TRN2D_RX_INVALID_SYMBOL;
	next = *receiver;
	next.pending_ucodes[next.pending_count] = ucode;
	next.pending_signs[next.pending_count] = positive ? 1u : 0u;
	next.pending_count++;
	next.received_symbols++;
	if (next.pending_count < V90_CP_FRAME_INTERVALS)
		result = V90_TRN2D_RX_BUFFERING;
	else
		result = decode_frame(&next, &generated);
	if (result != V90_TRN2D_RX_BUFFERING &&
	    result != V90_TRN2D_RX_FRAME_READY)
		return result;
	if (!v90_trn2d_rx_is_valid(&next))
		return V90_TRN2D_RX_INTERNAL_ERROR;
	*receiver = next;
	if (result == V90_TRN2D_RX_FRAME_READY)
		*frame = generated;
	return result;
}

enum v90_trn2d_rx_result v90_trn2d_rx_receive_decision(
	struct v90_trn2d_rx *receiver, uint8_t ucode, bool positive,
	struct v90_trn2d_frame *frame)
{
	if (receiver == NULL || frame == NULL)
		return V90_TRN2D_RX_INVALID_ARGUMENT;
	if (!v90_trn2d_rx_is_valid(receiver))
		return V90_TRN2D_RX_INVALID_STATE;
	return receive_valid_decision(receiver, ucode, positive, frame);
}

enum v90_trn2d_rx_result v90_trn2d_rx_receive_octet(
	struct v90_trn2d_rx *receiver, uint8_t network_octet,
	struct v90_trn2d_frame *frame)
{
	struct v90_pcm_symbol symbol;

	if (receiver == NULL || frame == NULL)
		return V90_TRN2D_RX_INVALID_ARGUMENT;
	if (!v90_trn2d_rx_is_valid(receiver))
		return V90_TRN2D_RX_INVALID_STATE;
	if (!v90_pcm_symbol_parse(
		    &symbol, receiver->decoder.mapping.law, network_octet))
		return V90_TRN2D_RX_INVALID_SYMBOL;
	return receive_valid_decision(
		receiver, symbol.ucode, symbol.positive, frame);
}

const char *v90_trn2d_rx_result_name(
	enum v90_trn2d_rx_result result)
{
	switch (result) {
	case V90_TRN2D_RX_OK:
		return "ok";
	case V90_TRN2D_RX_BUFFERING:
		return "buffering";
	case V90_TRN2D_RX_FRAME_READY:
		return "frame ready";
	case V90_TRN2D_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_TRN2D_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_TRN2D_RX_INVALID_STATE:
		return "invalid state";
	case V90_TRN2D_RX_INVALID_SYMBOL:
		return "invalid symbol";
	case V90_TRN2D_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_TRN2D_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
