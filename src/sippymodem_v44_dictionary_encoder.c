// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_dictionary_encoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
	V44_FIRST_CODEWORD = 4,
	V44_ROOT_COUNT = 256,
};

struct v44_encoder_node {
	uint32_t history_start;
	uint32_t segment_length;
	uint32_t string_length;
	uint32_t first_child;
	uint32_t next_sibling;
};

struct sippymodem_v44_dictionary_encoder {
	struct sippymodem_v44_parameters parameters;
	struct v44_encoder_node *nodes;
	uint8_t *history;
	uint32_t roots[V44_ROOT_COUNT];
	uint32_t next_codeword;
	uint32_t history_length;
};

static bool dictionary_is_valid(
	const struct sippymodem_v44_dictionary_encoder *encoder)
{
	return encoder != NULL &&
	       sippymodem_v44_parameters_are_valid(&encoder->parameters) &&
	       encoder->nodes != NULL && encoder->history != NULL &&
	       encoder->next_codeword >= V44_FIRST_CODEWORD &&
	       encoder->next_codeword <= encoder->parameters.codeword_limit &&
	       encoder->history_length <= encoder->parameters.history_size;
}

static void reset_dictionary(
	struct sippymodem_v44_dictionary_encoder *encoder)
{
	memset(encoder->roots, 0, sizeof(encoder->roots));
	encoder->next_codeword = V44_FIRST_CODEWORD;
	encoder->history_length = 0;
}

struct sippymodem_v44_dictionary_encoder *
sippymodem_v44_dictionary_encoder_create(
	const struct sippymodem_v44_parameters *parameters)
{
	struct sippymodem_v44_dictionary_encoder *encoder;

	if (!sippymodem_v44_parameters_are_valid(parameters))
		return NULL;
	encoder = calloc(1, sizeof(*encoder));
	if (encoder == NULL)
		return NULL;
	encoder->parameters = *parameters;
	encoder->nodes = calloc(
		parameters->codeword_limit, sizeof(*encoder->nodes));
	encoder->history = malloc(parameters->history_size);
	if (encoder->nodes == NULL || encoder->history == NULL) {
		sippymodem_v44_dictionary_encoder_destroy(encoder);
		return NULL;
	}
	reset_dictionary(encoder);
	return encoder;
}

void sippymodem_v44_dictionary_encoder_destroy(
	struct sippymodem_v44_dictionary_encoder *encoder)
{
	if (encoder == NULL)
		return;
	free(encoder->nodes);
	free(encoder->history);
	free(encoder);
}

enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_reinitialize(
	struct sippymodem_v44_dictionary_encoder *encoder)
{
	if (!dictionary_is_valid(encoder))
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT;
	reset_dictionary(encoder);
	return SIPPYMODEM_V44_DICTIONARY_ENCODER_OK;
}

enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_append(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint8_t value,
	uint32_t *history_index)
{
	if (!dictionary_is_valid(encoder) || history_index == NULL)
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT;
	if (encoder->history_length >= encoder->parameters.history_size)
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL;
	*history_index = encoder->history_length;
	encoder->history[encoder->history_length++] = value;
	return SIPPYMODEM_V44_DICTIONARY_ENCODER_OK;
}

static enum sippymodem_v44_dictionary_encoder_result add_segment(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t *siblings,
	uint32_t parent_string_length,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword)
{
	uint32_t assigned;
	struct v44_encoder_node *node;

	if (siblings == NULL || codeword == NULL || length == 0 ||
	    history_start >= encoder->history_length ||
	    length > encoder->history_length - history_start ||
	    parent_string_length >= encoder->parameters.max_string_length ||
	    length > encoder->parameters.max_string_length -
			     parent_string_length)
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT;
	if (encoder->next_codeword >= encoder->parameters.codeword_limit)
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL;

	assigned = encoder->next_codeword;
	node = &encoder->nodes[assigned];
	*node = (struct v44_encoder_node){
		.history_start = history_start,
		.segment_length = length,
		.string_length = parent_string_length + length,
		.next_sibling = *siblings,
	};
	*siblings = assigned;
	encoder->next_codeword++;
	*codeword = assigned;
	return SIPPYMODEM_V44_DICTIONARY_ENCODER_OK;
}

enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_add_root_segment(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint8_t root,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword)
{
	if (!dictionary_is_valid(encoder))
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT;
	return add_segment(
		encoder, &encoder->roots[root], 1, history_start, length,
		codeword);
}

enum sippymodem_v44_dictionary_encoder_result
sippymodem_v44_dictionary_encoder_add_child_segment(
	struct sippymodem_v44_dictionary_encoder *encoder,
	uint32_t parent_codeword,
	uint32_t history_start,
	uint32_t length,
	uint32_t *codeword)
{
	struct v44_encoder_node *parent;

	if (!dictionary_is_valid(encoder) ||
	    parent_codeword < V44_FIRST_CODEWORD ||
	    parent_codeword >= encoder->next_codeword)
		return SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT;
	parent = &encoder->nodes[parent_codeword];
	return add_segment(
		encoder, &parent->first_child, parent->string_length,
		history_start, length, codeword);
}

size_t sippymodem_v44_dictionary_encoder_history_length(
	const struct sippymodem_v44_dictionary_encoder *encoder)
{
	return dictionary_is_valid(encoder) ? encoder->history_length : 0;
}

uint32_t sippymodem_v44_dictionary_encoder_next_codeword(
	const struct sippymodem_v44_dictionary_encoder *encoder)
{
	return dictionary_is_valid(encoder) ? encoder->next_codeword : 0;
}

const char *sippymodem_v44_dictionary_encoder_result_name(
	enum sippymodem_v44_dictionary_encoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_DICTIONARY_ENCODER_OK:
		return "ok";
	case SIPPYMODEM_V44_DICTIONARY_ENCODER_FULL:
		return "full";
	case SIPPYMODEM_V44_DICTIONARY_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
