// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_encoder.h"
#include "sippymodem_v44_dictionary_encoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
	V44_CONTROL_FLUSH = 1,
	V44_CONTROL_STEPUP = 2,
	V44_CONTROL_REINIT = 3,
	V44_FIRST_CODEWORD = 4,
	V44_INITIAL_CODEWORD_BITS = 6,
	V44_INITIAL_CODEWORD_THRESHOLD = 64,
	V44_INITIAL_ORDINAL_BITS = 7,
	V44_MAX_CODEWORD_BITS = 16,
};

enum v44_encode_phase {
	V44_ENCODE_IDLE = 0,
	V44_ENCODE_CONTROL_PREFIX,
	V44_ENCODE_CONTROL_VALUE,
	V44_ENCODE_ORDINAL_PREFIX,
	V44_ENCODE_ORDINAL_VALUE,
	V44_ENCODE_CODEWORD_PREFIX,
	V44_ENCODE_CODEWORD_VALUE,
	V44_ENCODE_EXTENSION_FIELD,
	V44_ENCODE_FLUSH_ALIGN,
	V44_ENCODE_FLUSH_DRAIN,
	V44_ENCODE_FAILED,
};

enum v44_control_action {
	V44_ACTION_NONE = 0,
	V44_ACTION_REINIT,
	V44_ACTION_STEPUP_ORDINAL,
	V44_ACTION_STEPUP_CODEWORD,
	V44_ACTION_FLUSH,
};

enum v44_data_action {
	V44_DATA_NONE = 0,
	V44_DATA_ORDINAL,
	V44_DATA_CODEWORD,
	V44_DATA_EXTENSION,
};

enum v44_previous_type {
	V44_PREVIOUS_NONE = 0,
	V44_PREVIOUS_ORDINAL,
	V44_PREVIOUS_CODEWORD,
	V44_PREVIOUS_EXTENSION,
};

enum v44_algorithm_state {
	V44_ALGORITHM_MATCH = 0,
	V44_ALGORITHM_EXTEND,
};

struct v44_previous_code {
	enum v44_previous_type type;
	uint32_t codeword;
	uint32_t length;
	uint8_t first;
};

struct sippymodem_v44_encoder {
	struct sippymodem_v44_parameters parameters;
	struct sippymodem_v44_bit_writer *writer;
	struct sippymodem_v44_dictionary_encoder *dictionary;
	uint8_t *lookahead;
	uint32_t lookahead_capacity;
	uint32_t lookahead_start;
	uint32_t lookahead_length;
	struct v44_previous_code previous;
	enum v44_encode_phase phase;
	enum v44_control_action control_action;
	enum v44_data_action data_action;
	enum v44_algorithm_state algorithm_state;
	uint32_t pending_value;
	uint32_t pending_length;
	uint32_t pending_field;
	uint32_t codeword_threshold;
	unsigned int pending_field_bits;
	unsigned int codeword_bits;
	unsigned int ordinal_bits;
	bool current_linked;
	bool wire_previous_codeword;
	bool flush_requested;
};

static bool phase_is_control(enum v44_encode_phase phase)
{
	return phase == V44_ENCODE_CONTROL_PREFIX ||
	       phase == V44_ENCODE_CONTROL_VALUE;
}

static bool phase_is_data(enum v44_encode_phase phase)
{
	return phase == V44_ENCODE_ORDINAL_PREFIX ||
	       phase == V44_ENCODE_ORDINAL_VALUE ||
	       phase == V44_ENCODE_CODEWORD_PREFIX ||
	       phase == V44_ENCODE_CODEWORD_VALUE ||
	       phase == V44_ENCODE_EXTENSION_FIELD;
}

static bool previous_is_valid(const struct sippymodem_v44_encoder *encoder)
{
	switch (encoder->previous.type) {
	case V44_PREVIOUS_NONE:
	case V44_PREVIOUS_EXTENSION:
		return encoder->previous.length == 0;
	case V44_PREVIOUS_ORDINAL:
		return encoder->previous.length == 1;
	case V44_PREVIOUS_CODEWORD:
		return encoder->previous.codeword >= V44_FIRST_CODEWORD &&
		       encoder->previous.codeword <
			       sippymodem_v44_dictionary_encoder_next_codeword(
				       encoder->dictionary) &&
		       encoder->previous.length >= 2 &&
		       encoder->previous.length <=
			       encoder->parameters.max_string_length;
	}
	return false;
}

static bool encoder_is_valid(const struct sippymodem_v44_encoder *encoder)
{
	size_t history_length;
	bool control_phase;
	bool data_phase;

	if (encoder == NULL ||
	    !sippymodem_v44_parameters_are_valid(&encoder->parameters) ||
	    encoder->writer == NULL || encoder->dictionary == NULL ||
	    encoder->lookahead == NULL ||
	    encoder->lookahead_capacity !=
		    encoder->parameters.max_string_length + 1U ||
	    encoder->lookahead_length > encoder->lookahead_capacity ||
	    encoder->phase < V44_ENCODE_IDLE ||
	    encoder->phase > V44_ENCODE_FAILED ||
	    encoder->control_action < V44_ACTION_NONE ||
	    encoder->control_action > V44_ACTION_FLUSH ||
	    encoder->data_action < V44_DATA_NONE ||
	    encoder->data_action > V44_DATA_EXTENSION ||
	    encoder->algorithm_state < V44_ALGORITHM_MATCH ||
	    encoder->algorithm_state > V44_ALGORITHM_EXTEND ||
	    encoder->codeword_bits < V44_INITIAL_CODEWORD_BITS ||
	    encoder->codeword_bits > V44_MAX_CODEWORD_BITS ||
	    encoder->codeword_threshold !=
		    (UINT32_C(1) << encoder->codeword_bits) ||
	    (encoder->ordinal_bits != V44_INITIAL_ORDINAL_BITS &&
	     encoder->ordinal_bits != 8) ||
	    encoder->pending_field_bits > 16 || !previous_is_valid(encoder))
		return false;
	history_length = sippymodem_v44_dictionary_encoder_history_length(
		encoder->dictionary);
	if (history_length > encoder->parameters.history_size ||
	    encoder->lookahead_start > history_length ||
	    encoder->lookahead_length >
		    history_length - encoder->lookahead_start ||
	    sippymodem_v44_dictionary_encoder_next_codeword(
		    encoder->dictionary) < V44_FIRST_CODEWORD ||
	    sippymodem_v44_dictionary_encoder_next_codeword(
		    encoder->dictionary) > encoder->parameters.codeword_limit ||
	    (encoder->algorithm_state == V44_ALGORITHM_EXTEND &&
	     encoder->previous.type != V44_PREVIOUS_CODEWORD) ||
	    (encoder->current_linked &&
	     (encoder->algorithm_state != V44_ALGORITHM_MATCH ||
	      encoder->lookahead_length == 0)))
		return false;
	control_phase = phase_is_control(encoder->phase);
	data_phase = phase_is_data(encoder->phase);
	return control_phase ==
		       (encoder->control_action != V44_ACTION_NONE) &&
	       (data_phase ||
		encoder->control_action == V44_ACTION_STEPUP_ORDINAL ||
		encoder->control_action == V44_ACTION_STEPUP_CODEWORD) ==
		       (encoder->data_action != V44_DATA_NONE);
}

static bool reset_dictionary(struct sippymodem_v44_encoder *encoder)
{
	uint32_t history_index = UINT32_MAX;

	if (sippymodem_v44_dictionary_encoder_reinitialize(
		    encoder->dictionary) !=
	    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK)
		return false;
	for (uint32_t index = 0; index < encoder->lookahead_length; ++index) {
		if (sippymodem_v44_dictionary_encoder_append(
			    encoder->dictionary, encoder->lookahead[index],
			    &history_index) !=
			    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK ||
		    history_index != index)
			return false;
	}
	encoder->lookahead_start = 0;
	encoder->previous = (struct v44_previous_code){ 0 };
	encoder->algorithm_state = V44_ALGORITHM_MATCH;
	encoder->codeword_bits = V44_INITIAL_CODEWORD_BITS;
	encoder->codeword_threshold = V44_INITIAL_CODEWORD_THRESHOLD;
	encoder->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	encoder->current_linked = false;
	encoder->wire_previous_codeword = false;
	return true;
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
	encoder->lookahead_capacity = parameters->max_string_length + 1U;
	encoder->writer = sippymodem_v44_bit_writer_create(output_capacity);
	encoder->dictionary =
		sippymodem_v44_dictionary_encoder_create(parameters);
	encoder->lookahead = malloc(encoder->lookahead_capacity);
	if (encoder->writer == NULL || encoder->dictionary == NULL ||
	    encoder->lookahead == NULL || !reset_dictionary(encoder)) {
		sippymodem_v44_encoder_destroy(encoder);
		return NULL;
	}
	return encoder;
}

void sippymodem_v44_encoder_destroy(
	struct sippymodem_v44_encoder *encoder)
{
	if (encoder == NULL)
		return;
	free(encoder->lookahead);
	sippymodem_v44_dictionary_encoder_destroy(encoder->dictionary);
	sippymodem_v44_bit_writer_destroy(encoder->writer);
	free(encoder);
}

static enum sippymodem_v44_encode_result fail_encoder(
	struct sippymodem_v44_encoder *encoder)
{
	encoder->phase = V44_ENCODE_FAILED;
	encoder->control_action = V44_ACTION_NONE;
	encoder->data_action = V44_DATA_NONE;
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

static uint32_t control_value(enum v44_control_action action)
{
	switch (action) {
	case V44_ACTION_REINIT:
		return V44_CONTROL_REINIT;
	case V44_ACTION_STEPUP_ORDINAL:
	case V44_ACTION_STEPUP_CODEWORD:
		return V44_CONTROL_STEPUP;
	case V44_ACTION_FLUSH:
		return V44_CONTROL_FLUSH;
	case V44_ACTION_NONE:
		return UINT32_MAX;
	}
	return UINT32_MAX;
}

static void consume_lookahead(
	struct sippymodem_v44_encoder *encoder,
	uint32_t length)
{
	encoder->lookahead_length -= length;
	if (encoder->lookahead_length != 0)
		memmove(
			encoder->lookahead, encoder->lookahead + length,
			encoder->lookahead_length);
	encoder->lookahead_start += length;
}

static bool complete_data(struct sippymodem_v44_encoder *encoder)
{
	struct v44_previous_code current = { 0 };

	if (encoder->lookahead_length == 0 || encoder->pending_length == 0 ||
	    encoder->pending_length > encoder->lookahead_length)
		return false;

	switch (encoder->data_action) {
	case V44_DATA_ORDINAL:
		if (encoder->pending_length != 1 || encoder->pending_value > 255)
			return false;
		current.type = V44_PREVIOUS_ORDINAL;
		current.length = 1;
		current.first = encoder->lookahead[0];
		consume_lookahead(encoder, 1);
		encoder->algorithm_state = V44_ALGORITHM_MATCH;
		encoder->current_linked = false;
		break;
	case V44_DATA_CODEWORD:
		if (encoder->pending_length < 2 ||
		    encoder->pending_value < V44_FIRST_CODEWORD ||
		    encoder->pending_value >=
			    sippymodem_v44_dictionary_encoder_next_codeword(
				    encoder->dictionary))
			return false;
		current.type = V44_PREVIOUS_CODEWORD;
		current.codeword = encoder->pending_value;
		current.length = encoder->pending_length;
		current.first = encoder->lookahead[0];
		consume_lookahead(encoder, encoder->pending_length);
		encoder->algorithm_state =
			encoder->pending_length <
				encoder->parameters.max_string_length ?
				V44_ALGORITHM_EXTEND : V44_ALGORITHM_MATCH;
		encoder->current_linked = false;
		break;
	case V44_DATA_EXTENSION:
		if (encoder->previous.type != V44_PREVIOUS_CODEWORD)
			return false;
		current.type = V44_PREVIOUS_EXTENSION;
		consume_lookahead(encoder, encoder->pending_length);
		encoder->algorithm_state = V44_ALGORITHM_MATCH;
		break;
	case V44_DATA_NONE:
		return false;
	}
	encoder->previous = current;
	encoder->data_action = V44_DATA_NONE;
	encoder->pending_value = 0;
	encoder->pending_length = 0;
	encoder->pending_field = 0;
	encoder->pending_field_bits = 0;
	encoder->phase = V44_ENCODE_IDLE;
	return true;
}

static void begin_ordinal(
	struct sippymodem_v44_encoder *encoder,
	uint8_t ordinal)
{
	encoder->data_action = V44_DATA_ORDINAL;
	encoder->pending_value = ordinal;
	encoder->pending_length = 1;
	if (ordinal > 127 &&
	    encoder->ordinal_bits == V44_INITIAL_ORDINAL_BITS)
		begin_control(encoder, V44_ACTION_STEPUP_ORDINAL);
	else
		encoder->phase = V44_ENCODE_ORDINAL_PREFIX;
}

static void begin_codeword(
	struct sippymodem_v44_encoder *encoder,
	uint32_t codeword,
	uint32_t length)
{
	encoder->data_action = V44_DATA_CODEWORD;
	encoder->pending_value = codeword;
	encoder->pending_length = length;
	if (codeword >= encoder->codeword_threshold)
		begin_control(encoder, V44_ACTION_STEPUP_CODEWORD);
	else
		encoder->phase = V44_ENCODE_CODEWORD_PREFIX;
}

static unsigned int extension_value_bits(
	const struct sippymodem_v44_encoder *encoder)
{
	if (encoder->parameters.max_string_length <= 46)
		return 5;
	if (encoder->parameters.max_string_length <= 78)
		return 6;
	if (encoder->parameters.max_string_length <= 142)
		return 7;
	return 8;
}

static void append_field_bits(
	uint32_t *field,
	unsigned int *field_bits,
	uint32_t value,
	unsigned int bit_count)
{
	*field |= value << *field_bits;
	*field_bits += bit_count;
}

static bool begin_extension(
	struct sippymodem_v44_encoder *encoder,
	uint32_t length)
{
	uint32_t field = 0;
	unsigned int field_bits = 0;

	if (length == 0 || length > 253 ||
	    length > encoder->parameters.max_string_length - 2U)
		return false;
	append_field_bits(&field, &field_bits, 0, 1);
	append_field_bits(&field, &field_bits, 1, 1);
	if (length == 1) {
		append_field_bits(&field, &field_bits, 1, 1);
	} else if (length <= 4) {
		append_field_bits(&field, &field_bits, 0, 1);
		append_field_bits(&field, &field_bits, length - 1U, 2);
	} else if (length <= 12) {
		append_field_bits(&field, &field_bits, 0, 3);
		append_field_bits(&field, &field_bits, 0, 1);
		append_field_bits(&field, &field_bits, length - 5U, 3);
	} else {
		append_field_bits(&field, &field_bits, 0, 3);
		append_field_bits(&field, &field_bits, 1, 1);
		append_field_bits(
			&field, &field_bits, length - 13U,
			extension_value_bits(encoder));
	}
	encoder->data_action = V44_DATA_EXTENSION;
	encoder->pending_length = length;
	encoder->pending_field = field;
	encoder->pending_field_bits = field_bits;
	encoder->phase = V44_ENCODE_EXTENSION_FIELD;
	return true;
}

static bool complete_control(struct sippymodem_v44_encoder *encoder)
{
	enum v44_control_action action = encoder->control_action;

	encoder->control_action = V44_ACTION_NONE;
	encoder->wire_previous_codeword = false;
	switch (action) {
	case V44_ACTION_REINIT:
		if (!reset_dictionary(encoder))
			return false;
		encoder->phase = V44_ENCODE_IDLE;
		break;
	case V44_ACTION_STEPUP_ORDINAL:
		encoder->ordinal_bits = 8;
		encoder->phase = V44_ENCODE_ORDINAL_PREFIX;
		break;
	case V44_ACTION_STEPUP_CODEWORD:
		if (encoder->codeword_bits >= V44_MAX_CODEWORD_BITS)
			return false;
		encoder->codeword_bits++;
		encoder->codeword_threshold <<= 1;
		if (encoder->pending_value >= encoder->codeword_threshold)
			begin_control(encoder, V44_ACTION_STEPUP_CODEWORD);
		else
			encoder->phase = V44_ENCODE_CODEWORD_PREFIX;
		break;
	case V44_ACTION_FLUSH:
		encoder->phase = V44_ENCODE_FLUSH_ALIGN;
		break;
	case V44_ACTION_NONE:
		return false;
	}
	return true;
}

static enum sippymodem_v44_encode_result advance_wire(
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
				encoder->codeword_bits);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			if (!complete_control(encoder))
				return fail_encoder(encoder);
			break;
		case V44_ENCODE_ORDINAL_PREFIX:
			result = put_bits(
				encoder, 0,
				encoder->wire_previous_codeword ? 2U : 1U);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->phase = V44_ENCODE_ORDINAL_VALUE;
			break;
		case V44_ENCODE_ORDINAL_VALUE:
			result = put_bits(
				encoder, encoder->pending_value,
				encoder->ordinal_bits);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->wire_previous_codeword = false;
			return complete_data(encoder) ?
				       SIPPYMODEM_V44_ENCODE_OK :
				       fail_encoder(encoder);
		case V44_ENCODE_CODEWORD_PREFIX:
			result = put_bits(encoder, 1, 1);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->phase = V44_ENCODE_CODEWORD_VALUE;
			break;
		case V44_ENCODE_CODEWORD_VALUE:
			result = put_bits(
				encoder, encoder->pending_value,
				encoder->codeword_bits);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->wire_previous_codeword = true;
			return complete_data(encoder) ?
				       SIPPYMODEM_V44_ENCODE_OK :
				       fail_encoder(encoder);
		case V44_ENCODE_EXTENSION_FIELD:
			result = put_bits(
				encoder, encoder->pending_field,
				encoder->pending_field_bits);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			encoder->wire_previous_codeword = false;
			return complete_data(encoder) ?
				       SIPPYMODEM_V44_ENCODE_OK :
				       fail_encoder(encoder);
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

static bool previous_can_link(
	const struct sippymodem_v44_encoder *encoder)
{
	return (encoder->previous.type == V44_PREVIOUS_ORDINAL ||
		encoder->previous.type == V44_PREVIOUS_CODEWORD) &&
	       encoder->previous.length < encoder->parameters.max_string_length;
}

static enum sippymodem_v44_dictionary_encoder_result link_current(
	struct sippymodem_v44_encoder *encoder)
{
	uint32_t codeword = UINT32_MAX;

	if (encoder->previous.type == V44_PREVIOUS_ORDINAL)
		return sippymodem_v44_dictionary_encoder_add_root_segment(
			encoder->dictionary, encoder->previous.first,
			encoder->lookahead_start, 1, &codeword);
	return sippymodem_v44_dictionary_encoder_add_child_segment(
		encoder->dictionary, encoder->previous.codeword,
		encoder->lookahead_start, 1, &codeword);
}

static enum sippymodem_v44_encode_result drive_algorithm(
	struct sippymodem_v44_encoder *encoder,
	bool force)
{
	if (encoder->algorithm_state == V44_ALGORITHM_EXTEND) {
		struct sippymodem_v44_dictionary_extension extension;
		enum sippymodem_v44_dictionary_encoder_result dictionary_result;

		if (previous_can_link(encoder) &&
		    sippymodem_v44_dictionary_encoder_next_codeword(
			    encoder->dictionary) >=
			    encoder->parameters.codeword_limit) {
			begin_control(encoder, V44_ACTION_REINIT);
			return advance_wire(encoder);
		}
		if (encoder->lookahead_length == 0) {
			if (!force)
				return SIPPYMODEM_V44_ENCODE_NEED_INPUT;
			encoder->algorithm_state = V44_ALGORITHM_MATCH;
			return SIPPYMODEM_V44_ENCODE_OK;
		}
		dictionary_result =
			sippymodem_v44_dictionary_encoder_find_extension(
				encoder->dictionary, encoder->previous.codeword,
				encoder->lookahead_start,
				encoder->lookahead_length, &extension);
		if (dictionary_result != SIPPYMODEM_V44_DICTIONARY_ENCODER_OK)
			return fail_encoder(encoder);
		if (extension.needs_more_input && !force)
			return SIPPYMODEM_V44_ENCODE_NEED_INPUT;
		if (extension.length != 0) {
			uint32_t codeword = UINT32_MAX;

			dictionary_result =
				sippymodem_v44_dictionary_encoder_add_child_segment(
					encoder->dictionary,
					encoder->previous.codeword,
					encoder->lookahead_start,
					extension.length, &codeword);
			if (dictionary_result !=
			    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK ||
			    !begin_extension(encoder, extension.length))
				return fail_encoder(encoder);
			return advance_wire(encoder);
		}
		encoder->algorithm_state = V44_ALGORITHM_MATCH;
	}

	if (encoder->lookahead_length == 0) {
		if (sippymodem_v44_dictionary_encoder_history_length(
			    encoder->dictionary) >=
		    encoder->parameters.history_size) {
			begin_control(encoder, V44_ACTION_REINIT);
			return advance_wire(encoder);
		}
		return SIPPYMODEM_V44_ENCODE_NEED_INPUT;
	}
	if (previous_can_link(encoder) && !encoder->current_linked) {
		enum sippymodem_v44_dictionary_encoder_result result;

		if (sippymodem_v44_dictionary_encoder_next_codeword(
			    encoder->dictionary) >=
		    encoder->parameters.codeword_limit) {
			begin_control(encoder, V44_ACTION_REINIT);
			return advance_wire(encoder);
		}
		result = link_current(encoder);
		if (result != SIPPYMODEM_V44_DICTIONARY_ENCODER_OK)
			return fail_encoder(encoder);
		encoder->current_linked = true;
	}
	{
		struct sippymodem_v44_dictionary_match match;

		if (sippymodem_v44_dictionary_encoder_find_longest(
			    encoder->dictionary, encoder->lookahead_start,
			    encoder->lookahead_length, &match) !=
		    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK)
			return fail_encoder(encoder);
		if (match.needs_more_input && !force)
			return SIPPYMODEM_V44_ENCODE_NEED_INPUT;
		if (match.length != 0)
			begin_codeword(encoder, match.codeword, match.length);
		else
			begin_ordinal(encoder, encoder->lookahead[0]);
	}
	return advance_wire(encoder);
}

static enum sippymodem_v44_encode_result append_input(
	struct sippymodem_v44_encoder *encoder,
	uint8_t value)
{
	uint32_t history_index = UINT32_MAX;

	if (encoder->lookahead_length >= encoder->lookahead_capacity ||
	    sippymodem_v44_dictionary_encoder_append(
		    encoder->dictionary, value, &history_index) !=
		    SIPPYMODEM_V44_DICTIONARY_ENCODER_OK)
		return fail_encoder(encoder);
	if (encoder->lookahead_length == 0)
		encoder->lookahead_start = history_index;
	else if (history_index !=
		 encoder->lookahead_start + encoder->lookahead_length)
		return fail_encoder(encoder);
	encoder->lookahead[encoder->lookahead_length++] = value;
	return SIPPYMODEM_V44_ENCODE_OK;
}

enum sippymodem_v44_encode_result sippymodem_v44_encoder_process(
	struct sippymodem_v44_encoder *encoder,
	const uint8_t **input,
	size_t *input_length)
{
	enum sippymodem_v44_encode_result result;
	bool force;

	if (!encoder_is_valid(encoder) || input == NULL || input_length == NULL ||
	    (*input_length != 0 && *input == NULL))
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (encoder->phase == V44_ENCODE_FAILED)
		return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
	if (encoder->flush_requested ||
	    encoder->phase == V44_ENCODE_FLUSH_ALIGN ||
	    encoder->phase == V44_ENCODE_FLUSH_DRAIN)
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (sippymodem_v44_bit_writer_pending(encoder->writer) != 0)
		return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	if (encoder->phase != V44_ENCODE_IDLE)
		return advance_wire(encoder);
	force = sippymodem_v44_dictionary_encoder_history_length(
			encoder->dictionary) >= encoder->parameters.history_size ||
		encoder->lookahead_length >= encoder->lookahead_capacity;
	result = drive_algorithm(encoder, force);
	if (result != SIPPYMODEM_V44_ENCODE_NEED_INPUT)
		return result;
	if (*input_length == 0)
		return SIPPYMODEM_V44_ENCODE_NEED_INPUT;
	result = append_input(encoder, **input);
	if (result != SIPPYMODEM_V44_ENCODE_OK)
		return result;
	(*input)++;
	(*input_length)--;
	force = sippymodem_v44_dictionary_encoder_history_length(
			encoder->dictionary) >= encoder->parameters.history_size ||
		encoder->lookahead_length >= encoder->lookahead_capacity;
	result = drive_algorithm(encoder, force);
	return result == SIPPYMODEM_V44_ENCODE_NEED_INPUT ?
		       SIPPYMODEM_V44_ENCODE_OK : result;
}

enum sippymodem_v44_encode_result sippymodem_v44_encoder_flush(
	struct sippymodem_v44_encoder *encoder)
{
	if (!encoder_is_valid(encoder))
		return SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT;
	if (encoder->phase == V44_ENCODE_FAILED)
		return SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR;
	if (sippymodem_v44_bit_writer_pending(encoder->writer) != 0)
		return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
	if (encoder->phase == V44_ENCODE_FLUSH_DRAIN) {
		encoder->phase = V44_ENCODE_IDLE;
		encoder->flush_requested = false;
		return SIPPYMODEM_V44_ENCODE_FLUSHED;
	}
	encoder->flush_requested = true;
	for (;;) {
		enum sippymodem_v44_encode_result result;

		if (encoder->phase != V44_ENCODE_IDLE) {
			result = advance_wire(encoder);
			if (result != SIPPYMODEM_V44_ENCODE_OK)
				return result;
			if (sippymodem_v44_bit_writer_pending(
				    encoder->writer) != 0)
				return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
			continue;
		}
		if (encoder->lookahead_length != 0 ||
		    encoder->algorithm_state == V44_ALGORITHM_EXTEND ||
		    sippymodem_v44_dictionary_encoder_history_length(
			    encoder->dictionary) >=
			    encoder->parameters.history_size) {
			result = drive_algorithm(encoder, true);
			if (result != SIPPYMODEM_V44_ENCODE_OK &&
			    result != SIPPYMODEM_V44_ENCODE_NEED_INPUT)
				return result;
			if (sippymodem_v44_bit_writer_pending(
				    encoder->writer) != 0)
				return SIPPYMODEM_V44_ENCODE_NEED_OUTPUT;
			continue;
		}
		begin_control(encoder, V44_ACTION_FLUSH);
		return advance_wire(encoder);
	}
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
