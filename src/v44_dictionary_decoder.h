// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_DICTIONARY_DECODER_H
#define V44_DICTIONARY_DECODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum v44_dictionary_result {
	V44_DICTIONARY_OK = 0,
	V44_DICTIONARY_NEED_OUTPUT,
	V44_DICTIONARY_INVALID_ARGUMENT,
	V44_DICTIONARY_PROCEDURAL_ERROR,
};

struct v44_dictionary_decoder;

/*
 * The output queue must hold at least one maximum-length decoded string.
 * This makes each code operation atomic: NEED_OUTPUT leaves the dictionary,
 * history, and queued output unchanged so the caller can drain and retry it.
 */
struct v44_dictionary_decoder *
v44_dictionary_decoder_create(
	const struct v44_parameters *parameters,
	size_t output_capacity);
void v44_dictionary_decoder_destroy(
	struct v44_dictionary_decoder *decoder);

/* Reinitialize the dictionary and history without discarding queued output. */
enum v44_dictionary_result
v44_dictionary_decoder_reinitialize(
	struct v44_dictionary_decoder *decoder);

enum v44_dictionary_result
v44_dictionary_decoder_decode_ordinal(
	struct v44_dictionary_decoder *decoder,
	uint8_t ordinal);
enum v44_dictionary_result
v44_dictionary_decoder_decode_codeword(
	struct v44_dictionary_decoder *decoder,
	uint32_t codeword);
enum v44_dictionary_result
v44_dictionary_decoder_decode_extension(
	struct v44_dictionary_decoder *decoder,
	uint32_t extension_length);

size_t v44_dictionary_decoder_pending(
	const struct v44_dictionary_decoder *decoder);
size_t v44_dictionary_decoder_read(
	struct v44_dictionary_decoder *decoder,
	uint8_t *output,
	size_t output_capacity);

const char *v44_dictionary_result_name(
	enum v44_dictionary_result result);

#endif
