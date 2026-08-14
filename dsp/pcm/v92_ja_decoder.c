// SPDX-License-Identifier: BSD-3-Clause

#include "v92_ja_decoder.h"

#include "packed_bits.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void store_bit(
	uint8_t *descriptor, size_t bit_index, uint8_t bit)
{
	if (bit != 0u)
		descriptor[bit_index / 8u] |=
			(uint8_t)(1u << (7u - bit_index % 8u));
}

bool v92_ja_decoder_is_valid(const struct v92_ja_decoder *decoder)
{
	uint32_t decoded_mask;

	if (decoder == NULL ||
	    !pcm_binary_sign_is_valid(&decoder->coding) ||
	    decoder->coding.scrambler.polynomial != V34_POLYNOMIAL_GPA ||
	    decoder->bit_index > V92_PCM_DIL_ZERO_DESCRIPTOR_BITS ||
	    !packed_msb_tail_is_zero(
		    decoder->descriptor, decoder->bit_index,
		    sizeof(decoder->descriptor)))
		return false;
	if (!decoder->complete)
		return decoder->bit_index < V92_PCM_DIL_ZERO_DESCRIPTOR_BITS &&
		       decoder->rate_mask == 0u;
	return decoder->bit_index == V92_PCM_DIL_ZERO_DESCRIPTOR_BITS &&
	       v92_pcm_dil_decode_zero(
		       decoder->descriptor, sizeof(decoder->descriptor),
		       &decoded_mask) == V92_PCM_DIL_OK &&
	       decoded_mask == decoder->rate_mask;
}

enum v92_ja_decoder_result v92_ja_decoder_init(
	struct v92_ja_decoder *decoder,
	const struct v92_phase3_sync *sync)
{
	struct v92_ja_decoder initialized = { 0 };

	if (decoder == NULL ||
	    !v92_phase3_sync_handoff(sync, &initialized.coding))
		return V92_JA_DECODER_INVALID_ARGUMENT;
	for (size_t bit = 0; bit < V92_PHASE3_FRAME_SYNC_BITS; ++bit)
		store_bit(initialized.descriptor, bit, 1u);
	initialized.bit_index = V92_PHASE3_FRAME_SYNC_BITS + 1u;
	if (!v92_ja_decoder_is_valid(&initialized))
		return V92_JA_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return V92_JA_DECODER_OK;
}

bool v92_ja_decoder_is_complete(
	const struct v92_ja_decoder *decoder)
{
	return v92_ja_decoder_is_valid(decoder) && decoder->complete;
}

enum v92_ja_decoder_result v92_ja_decoder_receive(
	struct v92_ja_decoder *decoder, uint8_t sign)
{
	struct v92_ja_decoder next;
	enum v92_pcm_dil_result result;
	uint8_t bit;

	if (decoder == NULL || sign > 1u)
		return V92_JA_DECODER_INVALID_ARGUMENT;
	if (!v92_ja_decoder_is_valid(decoder))
		return V92_JA_DECODER_INVALID_STATE;
	if (decoder->complete)
		return V92_JA_DECODER_COMPLETE;
	next = *decoder;
	if (pcm_binary_sign_decode(&next.coding, sign, &bit) !=
	    PCM_BINARY_SIGN_OK)
		return V92_JA_DECODER_INVALID_STATE;
	store_bit(next.descriptor, next.bit_index, bit);
	next.bit_index++;
	if (next.bit_index < V92_PCM_DIL_ZERO_DESCRIPTOR_BITS) {
		if (!v92_ja_decoder_is_valid(&next))
			return V92_JA_DECODER_INVALID_STATE;
		*decoder = next;
		return V92_JA_DECODER_OK;
	}
	result = v92_pcm_dil_decode_zero(
		next.descriptor, sizeof(next.descriptor), &next.rate_mask);
	if (result == V92_PCM_DIL_OK) {
		next.complete = true;
	} else {
		memset(next.descriptor, 0, sizeof(next.descriptor));
		next.rate_mask = 0u;
		next.bit_index = 0u;
	}
	if (!v92_ja_decoder_is_valid(&next))
		return V92_JA_DECODER_INVALID_STATE;
	*decoder = next;
	if (next.complete)
		return V92_JA_DECODER_DETECTED;
	return V92_JA_DECODER_REJECTED;
}

bool v92_ja_decoder_rate_mask(
	const struct v92_ja_decoder *decoder, uint32_t *rate_mask)
{
	if (!v92_ja_decoder_is_complete(decoder) || rate_mask == NULL)
		return false;
	*rate_mask = decoder->rate_mask;
	return true;
}

const char *v92_ja_decoder_result_name(
	enum v92_ja_decoder_result result)
{
	switch (result) {
	case V92_JA_DECODER_OK:
		return "ok";
	case V92_JA_DECODER_DETECTED:
		return "detected";
	case V92_JA_DECODER_REJECTED:
		return "rejected";
	case V92_JA_DECODER_COMPLETE:
		return "complete";
	case V92_JA_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_JA_DECODER_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
