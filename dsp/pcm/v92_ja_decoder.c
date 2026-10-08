// SPDX-License-Identifier: BSD-3-Clause

#include "v92_ja_decoder.h"

#include "packed_bits.h"
#include "pcm_control_frame.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool v92_ja_decoder_is_valid(const struct v92_ja_decoder *decoder)
{
	struct pcm_dil_descriptor_layout layout;
	struct v92_pcm_dil_parameters parameters;

	if (decoder == NULL ||
	    !pcm_binary_sign_is_valid(&decoder->coding) ||
	    decoder->coding.scrambler.polynomial != V34_POLYNOMIAL_GPA ||
	    decoder->bit_index > V92_PCM_DIL_MAX_DESCRIPTOR_BITS ||
	    decoder->consecutive_ones > V92_PHASE3_FRAME_SYNC_BITS ||
	    !packed_msb_tail_is_zero(
		    decoder->descriptor, decoder->bit_index,
		    sizeof(decoder->descriptor)))
		return false;
	if (decoder->bit_index < V90_DIL_HEADER_BITS)
		return !decoder->complete && decoder->descriptor_bits == 0u &&
		       (decoder->bit_index == 0u ||
			decoder->bit_index >= V92_PHASE3_FRAME_SYNC_BITS + 1u);
	if (v92_pcm_dil_layout(
		    decoder->descriptor, sizeof(decoder->descriptor),
		    decoder->bit_index, &layout) != V92_PCM_DIL_OK ||
	    layout.bit_count != decoder->descriptor_bits)
		return false;
	if (!decoder->complete)
		return decoder->bit_index < decoder->descriptor_bits;
	return decoder->bit_index == decoder->descriptor_bits &&
	       decoder->consecutive_ones == 0u &&
	       v92_pcm_dil_decode(
		       decoder->descriptor, sizeof(decoder->descriptor),
		       decoder->descriptor_bits, &parameters) == V92_PCM_DIL_OK;
}

static void start_descriptor(struct v92_ja_decoder *decoder)
{
	memset(decoder->descriptor, 0, sizeof(decoder->descriptor));
	pcm_control_frame_write_prefix(decoder->descriptor);
	decoder->bit_index = V92_PHASE3_FRAME_SYNC_BITS + 1u;
	decoder->descriptor_bits = 0u;
	decoder->consecutive_ones = 0u;
}

static void discard_descriptor(struct v92_ja_decoder *decoder)
{
	memset(decoder->descriptor, 0, sizeof(decoder->descriptor));
	decoder->bit_index = 0u;
	decoder->descriptor_bits = 0u;
}

enum v92_ja_decoder_result v92_ja_decoder_init(
	struct v92_ja_decoder *decoder,
	const struct v92_phase3_sync *sync)
{
	struct v92_ja_decoder initialized = { 0 };

	if (decoder == NULL ||
	    !v92_phase3_sync_handoff(sync, &initialized.coding))
		return V92_JA_DECODER_INVALID_ARGUMENT;
	start_descriptor(&initialized);
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
	struct pcm_dil_descriptor_layout layout;
	struct v92_pcm_dil_parameters parameters;
	enum v92_ja_decoder_result result = V92_JA_DECODER_OK;
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
	if (pcm_control_frame_sync_bit(&next.consecutive_ones, bit != 0u)) {
		if (next.bit_index != 0u)
			result = V92_JA_DECODER_REJECTED;
		start_descriptor(&next);
		goto commit;
	}
	if (next.bit_index == 0u)
		goto commit;
	packed_msb_set_bit(
		next.descriptor, next.bit_index, bit != 0u);
	next.bit_index++;
	if (next.bit_index == V90_DIL_HEADER_BITS) {
		if (v92_pcm_dil_layout(
			    next.descriptor, sizeof(next.descriptor),
			    next.bit_index, &layout) != V92_PCM_DIL_OK) {
			discard_descriptor(&next);
			result = V92_JA_DECODER_REJECTED;
			goto commit;
		}
		next.descriptor_bits = (uint16_t)layout.bit_count;
	}
	if (next.descriptor_bits == 0u || next.bit_index < next.descriptor_bits)
		goto commit;
	if (v92_pcm_dil_decode(
		    next.descriptor, sizeof(next.descriptor),
		    next.descriptor_bits, &parameters) == V92_PCM_DIL_OK) {
		next.complete = true;
		result = V92_JA_DECODER_DETECTED;
	} else {
		discard_descriptor(&next);
		result = V92_JA_DECODER_REJECTED;
	}
commit:
	if (!v92_ja_decoder_is_valid(&next))
		return V92_JA_DECODER_INVALID_STATE;
	*decoder = next;
	return result;
}

bool v92_ja_decoder_parameters(
	const struct v92_ja_decoder *decoder,
	struct v92_pcm_dil_parameters *parameters)
{
	return v92_ja_decoder_is_complete(decoder) &&
	       v92_pcm_dil_decode(
		       decoder->descriptor, sizeof(decoder->descriptor),
		       decoder->descriptor_bits, parameters) == V92_PCM_DIL_OK;
}
