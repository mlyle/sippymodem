// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_stream_rx.h"

#include "memory_region.h"

#include "v90_pcm_symbol.h"

#include <string.h>

/* Caller has validated the mapping, decision plane, and interval. */
static const struct pcm_constellation *constellation_at(
	const struct v90_pcm_stream_rx *receiver, size_t interval)
{
	const struct v90_pcm_frame_decoder *decoder = &receiver->decoder;
	const struct v90_pcm_mapping *mapping = &decoder->mapping;

	return decoder->decision_plane == V90_PCM_MAPPING_CODEC ?
		&mapping->codec[interval] : &mapping->transmit[interval];
}

static bool pending_is_valid(const struct v90_pcm_stream_rx *receiver)
{
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

		if (receiver->pending_signs[interval] > 1 ||
		    !pcm_constellation_contains(constellation_at(receiver, interval),
			    receiver->pending_ucodes[interval]))
			return false;
	}
	return true;
}

static bool accounting_is_valid(const struct v90_pcm_stream_rx *receiver)
{
	uint64_t frame_bits = receiver->decoder.mapping.frame_bits;

	return receiver->received_frames ==
		       receiver->received_symbols / V90_CP_FRAME_INTERVALS &&
	       receiver->pending_count ==
		       receiver->received_symbols % V90_CP_FRAME_INTERVALS &&
	       receiver->received_frames <= UINT64_MAX / frame_bits &&
	       receiver->received_bits ==
		       receiver->received_frames * frame_bits;
}

bool v90_pcm_stream_rx_is_valid(const struct v90_pcm_stream_rx *receiver)
{
	return receiver != NULL &&
	       v90_pcm_frame_decoder_is_valid(&receiver->decoder) &&
	       v34_scrambler_is_valid(&receiver->descrambler) &&
	       receiver->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       pending_is_valid(receiver) && accounting_is_valid(receiver);
}

enum v90_pcm_stream_rx_result v90_pcm_stream_rx_init(
	struct v90_pcm_stream_rx *receiver,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_pcm_stream_rx initialized = { 0 };
	enum v90_pcm_frame_decoder_result decoder_result;

	if (receiver == NULL || mapping == NULL)
		return V90_PCM_STREAM_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping))
		return V90_PCM_STREAM_RX_INVALID_PARAMETERS;
	decoder_result = v90_pcm_frame_decoder_init(
		&initialized.decoder, mapping, decision_plane);
	if (decoder_result == V90_PCM_FRAME_DECODER_INVALID_ARGUMENT)
		return V90_PCM_STREAM_RX_INVALID_ARGUMENT;
	if (decoder_result != V90_PCM_FRAME_DECODER_OK ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_PCM_STREAM_RX_INTERNAL_ERROR;
	if (!v90_pcm_stream_rx_is_valid(&initialized))
		return V90_PCM_STREAM_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_PCM_STREAM_RX_OK;
}

static enum v90_pcm_stream_rx_result complete_frame(
	struct v90_pcm_stream_rx *receiver, uint8_t ucode, bool positive,
	struct v90_pcm_stream_frame *frame)
{
	const size_t last = V90_CP_FRAME_INTERVALS - 1u;
	const struct v90_sign_decoder previous_signs = receiver->decoder.signs;
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	uint8_t signs[V90_CP_FRAME_INTERVALS];
	struct v90_pcm_frame_decode_output decoded;
	struct v90_pcm_stream_frame decoded_frame = { 0 };
	enum v90_pcm_frame_decoder_result result;

	if (receiver->received_bits >
	    UINT64_MAX - receiver->decoder.mapping.frame_bits)
		return V90_PCM_STREAM_RX_OUT_OF_RANGE;
	memcpy(ucodes, receiver->pending_ucodes, last);
	memcpy(signs, receiver->pending_signs, last);
	ucodes[last] = ucode;
	signs[last] = positive;
	result = v90_pcm_frame_decoder_decode_owned(
		&receiver->decoder, ucodes, signs, &decoded);
	if (result == V90_PCM_FRAME_DECODER_INVALID_SYMBOLS)
		return V90_PCM_STREAM_RX_INVALID_SYMBOL;
	if (result != V90_PCM_FRAME_DECODER_OK)
		return V90_PCM_STREAM_RX_INTERNAL_ERROR;
	if (v34_descramble_unpacked_bits(
		    &receiver->descrambler, decoded.bits, decoded.bit_count,
		    decoded_frame.bits) != V34_FRAMING_OK) {
		receiver->decoder.signs = previous_signs;
		return V90_PCM_STREAM_RX_INTERNAL_ERROR;
	}
	memcpy(decoded_frame.rules, decoded.rules,
	       decoded.rule_count * sizeof(decoded.rules[0]));
	decoded_frame.bit_count = decoded.bit_count;
	decoded_frame.rule_count = decoded.rule_count;
	receiver->received_symbols++;
	receiver->received_frames++;
	receiver->received_bits += decoded.bit_count;
	receiver->pending_count = 0;
	memset(receiver->pending_ucodes, 0,
	       sizeof(receiver->pending_ucodes));
	memset(receiver->pending_signs, 0,
	       sizeof(receiver->pending_signs));
	*frame = decoded_frame;
	return V90_PCM_STREAM_RX_FRAME_READY;
}

static enum v90_pcm_stream_rx_result receive_valid_decision(
	struct v90_pcm_stream_rx *receiver, uint8_t ucode, bool positive,
	struct v90_pcm_stream_frame *frame)
{
	size_t interval = receiver->pending_count;

	/* Valid mappings have at least nine bits per six-symbol frame, so
	 * bounding received_bits also bounds the symbol counter. */
	if (interval == V90_CP_FRAME_INTERVALS - 1u)
		return complete_frame(receiver, ucode, positive, frame);
	receiver->pending_ucodes[interval] = ucode;
	receiver->pending_signs[interval] = positive;
	receiver->pending_count++;
	receiver->received_symbols++;
	return V90_PCM_STREAM_RX_BUFFERING;
}

enum v90_pcm_stream_rx_result v90_pcm_stream_rx_receive_decision(
	struct v90_pcm_stream_rx *receiver, uint8_t ucode, bool positive,
	struct v90_pcm_stream_frame *frame)
{
	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V90_PCM_STREAM_RX_INVALID_ARGUMENT;
	if (!v90_pcm_stream_rx_is_valid(receiver))
		return V90_PCM_STREAM_RX_INVALID_STATE;
	if (!pcm_constellation_contains(
		    constellation_at(receiver, receiver->pending_count), ucode))
		return V90_PCM_STREAM_RX_INVALID_SYMBOL;
	return receive_valid_decision(receiver, ucode, positive, frame);
}

static bool slice_symbol(
	const struct v90_pcm_stream_rx *receiver,
	const struct v90_pcm_symbol *symbol, uint8_t *decision)
{
	const struct v90_pcm_mapping *mapping = &receiver->decoder.mapping;
	const struct pcm_constellation *constellation =
		constellation_at(receiver, receiver->pending_count);
	int32_t magnitude = symbol->sample < 0 ? -symbol->sample : symbol->sample;
	int32_t best_error = INT32_MAX;

	if (pcm_constellation_contains(constellation, symbol->ucode)) {
		*decision = symbol->ucode;
		return true;
	}
	/* Preserve exact codewords, including signed zero. For noisy input use
	 * linear distance, not companded-code distance; ties select the larger
	 * level (lower mapper label). Every sample still occupies one interval. */
	for (unsigned int candidate = PCM_CONSTELLATION_UCODES;
	     candidate > 0; --candidate) {
		uint8_t ucode = (uint8_t)(candidate - 1u);
		int16_t level;
		int32_t error;

		if (!pcm_constellation_contains(constellation, ucode))
			continue;
		if (!g711_ucode_sample(mapping->law, ucode, true, &level))
			return false;
		error = level - magnitude;
		if (error < 0)
			error = -error;
		if (error < best_error) {
			best_error = error;
			*decision = ucode;
		}
	}
	return best_error != INT32_MAX;
}

enum v90_pcm_stream_rx_result v90_pcm_stream_rx_receive_octet(
	struct v90_pcm_stream_rx *receiver, uint8_t network_octet,
	struct v90_pcm_stream_frame *frame)
{
	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V90_PCM_STREAM_RX_INVALID_ARGUMENT;
	if (!v90_pcm_stream_rx_is_valid(receiver))
		return V90_PCM_STREAM_RX_INVALID_STATE;
	return v90_pcm_stream_rx_receive_octet_owned(receiver, network_octet, frame);
}

enum v90_pcm_stream_rx_result v90_pcm_stream_rx_receive_octet_owned(
	struct v90_pcm_stream_rx *receiver, uint8_t network_octet,
	struct v90_pcm_stream_frame *frame)
{
	struct v90_pcm_symbol symbol;
	uint8_t decision;

	if (!v90_pcm_symbol_parse(
		    &symbol, receiver->decoder.mapping.law, network_octet))
		return V90_PCM_STREAM_RX_INVALID_SYMBOL;
	if (!slice_symbol(receiver, &symbol, &decision))
		return V90_PCM_STREAM_RX_INTERNAL_ERROR;
	return receive_valid_decision(
		receiver, decision, symbol.positive, frame);
}
