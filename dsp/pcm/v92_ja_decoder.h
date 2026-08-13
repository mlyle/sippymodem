// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_JA_DECODER_H
#define V92_JA_DECODER_H

#include "v92_ja_sync.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_ja_decoder_result {
	V92_JA_DECODER_OK,
	V92_JA_DECODER_DETECTED,
	V92_JA_DECODER_REJECTED,
	V92_JA_DECODER_COMPLETE,
	V92_JA_DECODER_INVALID_ARGUMENT,
	V92_JA_DECODER_INVALID_STATE,
};

/* Decode repeated N=0 Ja descriptors from polarity-normalized hard signs.
 * Initialization copies the coding epoch immediately after the sync field
 * and start bit; the caller must then retire the completed sync owner. A
 * rejected descriptor is discarded at its exact boundary while coding
 * history continues into the next repetition. */
struct v92_ja_decoder {
	struct pcm_binary_sign coding;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	uint32_t rate_mask;
	uint16_t bit_index;
	bool complete;
};

enum v92_ja_decoder_result v92_ja_decoder_init(
	struct v92_ja_decoder *decoder,
	const struct v92_ja_sync *sync);
bool v92_ja_decoder_is_valid(
	const struct v92_ja_decoder *decoder);
bool v92_ja_decoder_is_complete(
	const struct v92_ja_decoder *decoder);

enum v92_ja_decoder_result v92_ja_decoder_receive(
	struct v92_ja_decoder *decoder, uint8_t sign);

/* Zero is a syntactically valid mask; availability policy belongs to the
 * later session selection boundary. Failed calls leave rate_mask unchanged. */
bool v92_ja_decoder_rate_mask(
	const struct v92_ja_decoder *decoder, uint32_t *rate_mask);

const char *v92_ja_decoder_result_name(
	enum v92_ja_decoder_result result);

#endif /* V92_JA_DECODER_H */
