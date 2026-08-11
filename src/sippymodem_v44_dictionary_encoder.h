// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_DICTIONARY_ENCODER_H
#define SIPPYMODEM_V44_DICTIONARY_ENCODER_H

#include "sippymodem_v44_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_dictionary_encoder_result {
	SIPPYMODEM_V44_DICTIONARY_ENCODER_OK = 0,
	SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL,
	SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT,
};

struct sippymodem_v44_dictionary_encoder;

struct sippymodem_v44_dictionary_match {
	uint32_t codeword;
	uint32_t length;
	bool needs_more_input;
};

struct sippymodem_v44_dictionary_extension {
	uint32_t length;
	bool needs_more_input;
};

struct sippymodem_v44_dictionary_encoder *
sippymodem_v44_dictionary_encoder_create(
	const struct sippymodem_v44_parameters *parameters);
void sippymodem_v44_dictionary_encoder_destroy(
	struct sippymodem_v44_dictionary_encoder *encoder);

enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_reinitialize(
	struct sippymodem_v44_dictionary_encoder *encoder);

/* Append one source octet to bounded history and return its stable index. */
enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_append(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint8_t value,
	uint32_t *history_index);

/*
 * Add a string segment beneath an ordinal root or an existing codeword.
 * The segment must already reside wholly in encoder history. The assigned
 * codeword is returned only when the insertion commits successfully.
 */
enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_add_root_segment(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint8_t root,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword);
enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_add_child_segment(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t parent_codeword,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword);

/*
 * Find the longest codeword whose string prefixes the indicated history.
 * A zero-length match means that no complete codeword matched.  In either
 * case, needs_more_input reports whether appending source octets could extend
 * a viable dictionary path.
 */
enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_find_longest(
	const struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t history_start,
	uint32_t available,
	struct sippymodem_v44_dictionary_match *match);

/* Compare source history with the continuation following a matched node. */
enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_find_extension(
	const struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t codeword,
	uint32_t history_start,
	uint32_t available,
	struct sippymodem_v44_dictionary_extension *extension);

size_t sippymodem_v44_dictionary_encoder_history_length(
	const struct sippymodem_v44_dictionary_encoder *encoder);
uint32_t sippymodem_v44_dictionary_encoder_next_codeword(
	const struct sippymodem_v44_dictionary_encoder *encoder);

const char *sippymodem_v44_dictionary_encoder_result_name(
	enum sippymodem_v44_dictionary_encoder_result result);

#endif
