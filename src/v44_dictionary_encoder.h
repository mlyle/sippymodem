// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_DICTIONARY_ENCODER_H
#define V44_DICTIONARY_ENCODER_H

#include "v44_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v44_dictionary_encoder_result {
	V44_DICTIONARY_ENCODER_OK = 0,
	V44_DICTIONARY_ENCODER_FULL,
	V44_DICTIONARY_ENCODER_INVALID_ARGUMENT,
};

struct v44_dictionary_encoder;

struct v44_dictionary_match {
	uint32_t codeword;
	uint32_t length;
	bool needs_more_input;
};

struct v44_dictionary_extension {
	uint32_t length;
	bool needs_more_input;
};

struct v44_dictionary_encoder *
v44_dictionary_encoder_create(
	const struct v44_parameters *parameters);
void v44_dictionary_encoder_destroy(
	struct v44_dictionary_encoder *encoder);

enum v44_dictionary_encoder_result
v44_dictionary_encoder_reinitialize(
	struct v44_dictionary_encoder *encoder);

/* Append one source octet to bounded history and return its stable index. */
enum v44_dictionary_encoder_result
v44_dictionary_encoder_append(
	struct v44_dictionary_encoder *encoder,
	uint8_t value,
	uint32_t *history_index);

/*
 * Add a string segment beneath an ordinal root or an existing codeword.
 * The segment must already reside wholly in encoder history. The assigned
 * codeword is returned only when the insertion commits successfully.
 */
enum v44_dictionary_encoder_result
v44_dictionary_encoder_add_root_segment(
	struct v44_dictionary_encoder *encoder,
	uint8_t root,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword);
enum v44_dictionary_encoder_result
v44_dictionary_encoder_add_child_segment(
	struct v44_dictionary_encoder *encoder,
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
enum v44_dictionary_encoder_result
v44_dictionary_encoder_find_longest(
	const struct v44_dictionary_encoder *encoder,
	uint32_t history_start,
	uint32_t available,
	struct v44_dictionary_match *match);

/* Compare source history with the continuation following a matched node. */
enum v44_dictionary_encoder_result
v44_dictionary_encoder_find_extension(
	const struct v44_dictionary_encoder *encoder,
	uint32_t codeword,
	uint32_t history_start,
	uint32_t available,
	struct v44_dictionary_extension *extension);

size_t v44_dictionary_encoder_history_length(
	const struct v44_dictionary_encoder *encoder);
uint32_t v44_dictionary_encoder_next_codeword(
	const struct v44_dictionary_encoder *encoder);

#endif
