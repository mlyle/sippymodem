// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_DICTIONARY_DECODER_H
#define SIPPYMODEM_V44_DICTIONARY_DECODER_H

#include "sippymodem_v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_dictionary_result {
	SIPPYMODEM_V44_DICTIONARY_OK = 0,
	SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT,
	SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT,
	SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR,
};

struct sippymodem_v44_dictionary_decoder;

/*
 * The output queue must hold at least one maximum-length decoded string.
 * This makes each code operation atomic: NEED_OUTPUT leaves the dictionary,
 * history, and queued output unchanged so the caller can drain and retry it.
 */
struct sippymodem_v44_dictionary_decoder *
sippymodem_v44_dictionary_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity);
void sippymodem_v44_dictionary_decoder_destroy(
	struct sippymodem_v44_dictionary_decoder *decoder);

/* Reinitialize the dictionary and history without discarding queued output. */
enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_reinitialize(
	struct sippymodem_v44_dictionary_decoder *decoder);

enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_ordinal(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t ordinal);
enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_codeword(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint32_t codeword);
enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_extension(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint32_t extension_length);

size_t sippymodem_v44_dictionary_decoder_pending(
	const struct sippymodem_v44_dictionary_decoder *decoder);
size_t sippymodem_v44_dictionary_decoder_read(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t *output,
	size_t output_capacity);

const char *sippymodem_v44_dictionary_result_name(
	enum sippymodem_v44_dictionary_result result);

#endif
