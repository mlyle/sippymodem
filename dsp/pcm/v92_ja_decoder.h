// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_JA_DECODER_H
#define V92_JA_DECODER_H

#include "v92_phase3_sync.h"
#include "v92_pcm_dil.h"

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

/* Decode repeated Ja descriptors from polarity-normalized hard signs.
 * Initialization copies the coding epoch immediately after the sync field
 * and start bit; the caller must then retire the completed sync owner. A
 * rejected descriptor is discarded while coding history continues. Frame
 * sync is also recognized inside an incomplete candidate so a corrupt length
 * cannot hide the next repetition. */
struct v92_ja_decoder {
	struct pcm_binary_sign coding;
	uint8_t descriptor[V92_PCM_DIL_MAX_DESCRIPTOR_BYTES];
	uint16_t bit_index;
	uint16_t descriptor_bits;
	uint8_t consecutive_ones;
	bool complete;
};

enum v92_ja_decoder_result v92_ja_decoder_init(
	struct v92_ja_decoder *decoder,
	const struct v92_phase3_sync *sync);
bool v92_ja_decoder_is_valid(
	const struct v92_ja_decoder *decoder);
bool v92_ja_decoder_is_complete(
	const struct v92_ja_decoder *decoder);

enum v92_ja_decoder_result v92_ja_decoder_receive(
	struct v92_ja_decoder *decoder, uint8_t sign);

/* Zero is a syntactically valid mask; availability policy belongs to the
 * later session selection boundary. Failed calls leave parameters unchanged. */
bool v92_ja_decoder_parameters(
	const struct v92_ja_decoder *decoder,
	struct v92_pcm_dil_parameters *parameters);

const char *v92_ja_decoder_result_name(
	enum v92_ja_decoder_result result);

#endif /* V92_JA_DECODER_H */
