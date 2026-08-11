// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_encoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum {
	V44_CONTROL_FLUSH = 1,
	V44_CONTROL_STEPUP = 2,
	V44_CONTROL_REINIT = 3,
	V44_FIRST_CODEWORD = 4,
	V44_INITIAL_CODEWORD_BITS = 6,
	V44_INITIAL_ORDINAL_BITS = 7,
};

enum v44_encode_phase {
	V44_ENCODE_IDLE = 0,
	V44_ENCODE_CONTROL_PREFIX,
	V44_ENCODE_CONTROL_VALUE,
	V44_ENCODE_ORDINAL_PREFIX,
	V44_ENCODE_ORDINAL_VALUE,
	V44_ENCODE_FLUSH_ALIGN,
	V44_ENCODE_FLUSH_DRAIN,
	V44_ENCODE_FAILED,
};

enum v44_control_action {
	V44_ACTION_NONE = 0,
	V44_ACTION_REINIT_BEFORE_INPUT,
	V44_ACTION_REINIT_AFTER_INPUT,
	V44_ACTION_STEPUP_ORDINAL,
	V44_ACTION_FLUSH,
};

struct sippymodem_v44_encoder {
	struct sippymodem_v44_parameters parameters;
	struct sippymodem_v44_bit_writer *writer;
	enum v44_encode_phase phase;
	enum v44_control_action control_action;
	uint32_t next_codeword;
	uint32_t history_length;
	unsigned int ordinal_bits;
	uint8_t pending_ordinal;
	bool have_previous;
	bool input_pending;
};

static bool phase_has_input(enum v44_encode_phase phase)
{
	return phase == V44_ENCODE_CONTROL_PREFIX ||
	       phase == V44_ENCODE_CONTROL_VALUE ||
	       phase == V44_ENCODE_ORDINAL_PREFIX ||
	       phase == V44_ENCODE_ORDINAL_VALUE;
}

static bool encoder_is_valid(const struct sippymodem_v44_encoder *encoder)
{
	bool control_phase;

	if (encoder == NULL ||
	    !sippymodem_v44_parameters_are_valid(&encoder->parameters) ||
	    encoder->writer == NULL || encoder->phase < V44_ENCODE_IDLE ||
	    encoder->phase > V44_ENCODE_FAILED ||
	    encoder->control_action < V44_ACTION_NONE ||
	    encoder->control_action > V44_ACTION_FLUSH ||
	    encoder->next_codeword < V44_FIRST_CODEWORD ||
	    encoder->next_codeword > encoder->parameters.codeword_limit ||
	    encoder->history_length > encoder->parameters.history_size ||
	    (encoder->ordinal_bits != V44_INITIAL_ORDINAL_BITS &&
	     encoder->ordinal_bits != 8))
		return false;
	control_phase = encoder->phase == V44_ENCODE_CONTROL_PREFIX ||
			encoder->phase == V44_ENCODE_CONTROL_VALUE;
	return control_phase == (encoder->control_action != V44_ACTION_NONE) &&
	       (!encoder->input_pending || phase_has_input(encoder->phase)) &&
	       (encoder->input_pending ||
		encoder->control_action == V44_ACTION_FLUSH ||
		encoder->control_action == V44_ACTION_NONE);
}

static void reset_dictionary(struct sippymodem_v44_encoder *encoder)
{
	encoder->next_codeword = V44_FIRST_CODEWORD;
	encoder->history_length = 0;
	encoder->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	encoder->have_previous = false;
}

struct sippymodem_v44_encoder *sippymodem_v44_encoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity)
{
	struct sippymodem_v44_encoder *encoder;

	if (!sippymodem_v44_parameters_are_valid(parameters))
		return NULL;
	encoder = calloc(1, sizeof(*encoder));
	if (encoder == NULL)
		return NULL;
	encoder->parameters = *parameters;
	encoder->writer = sippymodem_v44_bit_writer_create(output_capacity);
	if (encoder->writer == NULL) {
		sippymodem_v44_encoder_destroy(encoder);
		return NULL;
	}
	reset_dictionary(encoder);
	return encoder;
}

void sippymodem_v44_encoder_destroy(
	struct sippymodem_v44_encoder *encoder)
{
	if (encoder == NULL)
		return;
	sippymodem_v44_bit_writer_destroy(encoder->writer);
	free(encoder);
}

static enum sippymodem_v44_encode_result fail_encoder(
	struct sippymodem_v44_encoder *encoder)
{
	encoder->phase = V44_ENCODE_FAILED;
	encoder->control_action = V44_ACTION_NONE;
	encoder->input_pending = false;
	return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
}

static enum sippymodem_v44_encode_result put_bits(
	struct sippymodem_v44_encoder *encoder,
	uint32_t value,
	unsigned int bit_count)
{
	enum sippymodem_v44_stream_result result =
		sippymodem_v44_bit_writer_put(
			encoder->writer, value, bit_count);

	if (result == SIPPYMODEM_V44_STREAM_OK)
		return SIPPYMODEM_V44_ENCODE_OK;
	if (result == SIPPYMODEM_V44_STREAM_NEED_OUTPUT)
		return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	return fail_encoder(encoder);
}

static void begin_control(
	struct sippymodem_v44_encoder *encoder,
	enum v44_control_action action)
{
	encoder->control_action = action;
	encoder->phase = V44_ENCODE_CONTROL_PREFIX;
}

static void begin_ordinal(struct sippymodem_v44_encoder *encoder)
{
	if (encoder->pending_ordinal > 127 &&
	    encoder->ordinal_bits == V44_INITIAL_ORDINAL_BITS)
		begin_control(encoder, V44_ACTION_STEPUP_ORDINAL);
	else
		encoder->phase = V44_ENCODE_ORDINAL_PREFIX;
}

static void complete_control(struct sippymodem_v44_encoder *encoder)
{
	enum v44_control_action action = encoder->control_action;

	encoder->control_action = V44_ACTION_NONE;
	switch (action) {
	case V44_ACTION_REINIT_BEFORE_INPUT:
		reset_dictionary(encoder);
		begin_ordinal(encoder);
		break;
	case V44_ACTION_REINIT_AFTER_INPUT:
		reset_dictionary(encoder);
		encoder->input_pending = false;
		encoder->phase = V44_ENCODE_IDLE;
		break;
	case V44_ACTION_STEPUP_ORDINAL:
		encoder->ordinal_bits = 8;
		encoder->phase = V44_ENCODE_ORDINAL_PREFIX;
		break;
	case V44_ACTION_FLUSH:
		encoder->phase = V44_ENCODE_FLUSH_ALIGN;
		break;
	case V44_ACTION_NONE:
		encoder->phase = V44_ENCODE_FAILED;
		break;
	}
}

static uint32_t control_value(enum v44_control_action action)
{
	switch (action) {
	case V44_ACTION_REINIT_BEFORE_INPUT:
	case V44_ACTION_REINIT_AFTER_INPUT:
		return V44_CONTROL_REINIT;
	case V44_ACTION_STEPUP_ORDINAL:
		return V44_CONTROL_STEPUP;
	case V44_ACTION_FLUSH:
		return V44_CONTROL_FLUSH;
	case V44_ACTION_NONE:
		return UINT32_MAX;
	}
	return UINT32_MAX;
}

static enum sippymodem_v44_encode_result complete_ordinal(
	struct sippymodem_v44_encoder *encoder)
{
	if (encoder->history_length >= encoder->parameters.history_size ||
	    (encoder->have_previous &&
	     encoder->next_codeword >= encoder->parameters.codeword_limit))
		return fail_encoder(encoder);
	encoder->history_length++;
	if (encoder->have_previous)
		encoder->next_codeword++;
	encoder->have_previous = true;
	if (encoder->history_length == encoder->parameters.history_size) {
		begin_control(encoder, V44_ACTION_REINIT_AFTER_INPUT);
		return SIPPYMODEM_V44_ENCODE_OK;
	}
	encoder->input_pending = false;
	encoder->phase = V44_ENCODE_IDLE;
	return SIPPYMODEM_V44_ENCODE_OK;
}

static enum sippymodem_v44_encode_result advance_encoder(
	struct sippymodem_v44_encoder *encoder)
{
	for (;;) {
		enum sippymodem_v44_encode_result result;

		switch (encoder->phase) {
		case V44_ENCODE_CONTROL_PREFIX:
			result = put_bits(encoder, 1, 1);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->phase = V44_ENCODE_CONTROL_VALUE;
			break;
		case V44_ENCODE_CONTROL_VALUE:
			result = put_bits(
				encoder, control_value(encoder->control_action),
				V44_INITIAL_CODEWORD_BITS);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			complete_control(encoder);
			if (encoder->phase == V44_ENCODE_FAILED)
				return fail_encoder(encoder);
			break;
		case V44_ENCODE_ORDINAL_PREFIX:
			result = put_bits(encoder, 0, 1);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->phase = V44_ENCODE_ORDINAL_VALUE;
			break;
		case V44_ENCODE_ORDINAL_VALUE:
			result = put_bits(
				encoder, encoder->pending_ordinal,
				encoder->ordinal_bits);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			result = complete_ordinal(encoder);
			if (result != SIPPYMODEM_V44_ENCODE_OK ||
			    encoder->phase == V44_ENCODE_IDLE)
				return result;
			break;
		case V44_ENCODE_FLUSH_ALIGN:
			if (sippymodem_v44_bit_writer_align(encoder->writer) !=
			    SIPPYMODEM_V44_STREAM_OK)
				return sippymodem_v44_bit_writer_pending(
					       encoder->writer) != 0 ?
					       SIPPYMODEM_V44_ENCODE_NEED_OUTPUT :
					       fail_encoder(encoder);
			encoder->phase = V44_ENCODE_FLUSH_DRAIN;
			return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
		case V44_ENCODE_IDLE:
			return SIPPYMODEM_V44_ENCODE_OK;
		case V44_ENCODE_FLUSH_DRAIN:
			return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
		case V44_ENCODE_FAILED:
			return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
		}
	}
}

enum sippymodem_v44_encode_result sippymodem_v44_encoder_process(
	struct sippymodem_v44_encoder *encoder,
	const uint8_t **input,
	size_t *input_length)
{
	if (!encoder_is_valid(encoder) || input == NULL || input_length == NULL ||
	    (*input_length != 0 && *input == NULL))
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (encoder->phase == V44_ENCODE_FAILED)
		return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
	if (sippymodem_v44_bit_writer_pending(encoder->writer) != 0)
		return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	if (encoder->phase == V44_ENCODE_FLUSH_ALIGN ||
	    encoder->phase == V44_ENCODE_FLUSH_DRAIN ||
	    (!encoder->input_pending && encoder->phase != V44_ENCODE_IDLE))
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (encoder->input_pending)
		return advance_encoder(encoder);
	if (*input_length == 0)
		return SIPPYMODEM_V44_ENCODE_NEED_INPUT;

	encoder->pending_ordinal = **input;
	encoder->input_pending = true;
	(*input)++;
	(*input_length)--;
	if (encoder->history_length >= encoder->parameters.history_size ||
	    (encoder->have_previous &&
	     encoder->next_codeword >= encoder->parameters.codeword_limit))
		begin_control(encoder, V44_ACTION_REINIT_BEFORE_INPUT);
	else
		begin_ordinal(encoder);
	return advance_encoder(encoder);
}

enum sippymodem_v44_encode_result sippymodem_v44_encoder_flush(
	struct sippymodem_v44_encoder *encoder)
{
	enum sippymodem_v44_encode_result result;

	if (!encoder_is_valid(encoder))
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (encoder->phase == V44_ENCODE_FAILED)
		return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
	if (sippymodem_v44_bit_writer_pending(encoder->writer) != 0)
		return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	if (encoder->phase == V44_ENCODE_FLUSH_DRAIN) {
		encoder->phase = V44_ENCODE_IDLE;
		return SIPPYMODEM_V44_ENCODE_FLUSHED;
	}
	if (encoder->input_pending) {
		result = advance_encoder(encoder);
		if (result != SIPPYMODEM_V44_ENCODE_OK)
			return result;
		if (sippymodem_v44_bit_writer_pending(encoder->writer) != 0)
			return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	}
	if (encoder->phase == V44_ENCODE_IDLE)
		begin_control(encoder, V44_ACTION_FLUSH);
	else if (encoder->phase != V44_ENCODE_CONTROL_PREFIX &&
		 encoder->phase != V44_ENCODE_CONTROL_VALUE &&
		 encoder->phase != V44_ENCODE_FLUSH_ALIGN)
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	return advance_encoder(encoder);
}

size_t sippymodem_v44_encoder_pending(
	const struct sippymodem_v44_encoder *encoder)
{
	return encoder_is_valid(encoder) ?
		       sippymodem_v44_bit_writer_pending(encoder->writer) : 0;
}

size_t sippymodem_v44_encoder_read(
	struct sippymodem_v44_encoder *encoder,
	uint8_t *output,
	size_t output_capacity)
{
	return encoder_is_valid(encoder) ?
		       sippymodem_v44_bit_writer_read(
			       encoder->writer, output, output_capacity) : 0;
}

const char *sippymodem_v44_encode_result_name(
	enum sippymodem_v44_encode_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_ENCODE_OK:
		return "ok";
	case SIPPYMODEM_V44_ENCODE_NEED_INPUT:
		return "need input";
	case SIPPYMODEM_V44_ENCODE_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_ENCODE_FLUSHED:
		return "flushed";
	case SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
