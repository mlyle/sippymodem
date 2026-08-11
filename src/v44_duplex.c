// SPDX-License-Identifier: BSD-3-Clause

#include "v44_duplex.h"

#include "v44_decoder.h"
#include "v44_encoder.h"

#include <stdlib.h>
#include <string.h>

enum v44_transmit_state {
	V44_TRANSMIT_READY = 0,
	V44_TRANSMIT_MANUAL_SWITCHING,
	V44_TRANSMIT_AUTOMATIC_SWITCHING,
	V44_TRANSMIT_FLUSHING,
	V44_TRANSMIT_FAILED,
};

enum v44_receive_state {
	V44_RECEIVE_READY = 0,
	V44_RECEIVE_PARAMETER,
	V44_RECEIVE_FINISHED,
	V44_RECEIVE_FAILED,
};

struct v44_duplex {
	struct v44_encoder *transmit;
	struct v44_tx_monitor *transmit_monitor;
	enum v44_transmit_state transmit_state;
	enum v44_encoder_mode transmit_mode;
	enum v44_encoder_mode requested_transmit_mode;
	bool transmit_adaptation_active;
	struct v44_decoder *receive;
	enum v44_receive_state receive_state;
};

static bool span_arguments_are_valid(
	const uint8_t **input,
	const size_t *input_length,
	const uint8_t *output,
	size_t output_capacity,
	const size_t *output_length)
{
	return input != NULL && input_length != NULL && output_length != NULL &&
	       (*input_length == 0 || *input != NULL) &&
	       (output_capacity == 0 || output != NULL);
}

static enum v44_duplex_result copy_literal(
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	const size_t length = *input_length < output_capacity ?
			      *input_length : output_capacity;

	if (length != 0) {
		memcpy(output, *input, length);
		*input += length;
		*input_length -= length;
	}
	*output_length = length;
	return *input_length == 0 ? V44_DUPLEX_NEED_INPUT :
				    V44_DUPLEX_NEED_OUTPUT;
}

struct v44_duplex *v44_duplex_create(
	const struct v44_duplex_config *config)
{
	struct v44_duplex *duplex;

	if (config == NULL ||
	    (config->transmit_adaptation_enabled &&
	     !config->transmit_enabled) ||
	    (config->transmit_enabled &&
	     !v44_parameters_are_valid(
		     &config->transmit_parameters)) ||
	    (config->receive_enabled &&
	     !v44_parameters_are_valid(
		     &config->receive_parameters)))
		return NULL;
	duplex = calloc(1, sizeof(*duplex));
	if (duplex == NULL)
		return NULL;
	if (config->transmit_enabled)
		duplex->transmit = v44_encoder_create(
			&config->transmit_parameters, 2);
	if (config->transmit_adaptation_enabled)
		duplex->transmit_monitor = v44_tx_monitor_create(
			&config->transmit_parameters,
			&config->transmit_adaptation);
	if (config->receive_enabled)
		duplex->receive = v44_decoder_create(
			&config->receive_parameters,
			config->receive_parameters.max_string_length);
	if ((config->transmit_enabled && duplex->transmit == NULL) ||
	    (config->transmit_adaptation_enabled &&
	     duplex->transmit_monitor == NULL) ||
	    (config->receive_enabled && duplex->receive == NULL)) {
		v44_duplex_destroy(duplex);
		return NULL;
	}
	duplex->transmit_mode = V44_ENCODER_COMPRESSED;
	duplex->transmit_adaptation_active =
		config->transmit_adaptation_enabled;
	return duplex;
}

void v44_duplex_destroy(
	struct v44_duplex *duplex)
{
	if (duplex == NULL)
		return;
	v44_decoder_destroy(duplex->receive);
	v44_tx_monitor_destroy(duplex->transmit_monitor);
	v44_encoder_destroy(duplex->transmit);
	free(duplex);
}

static enum v44_duplex_result fail_transmit(
	struct v44_duplex *duplex)
{
	duplex->transmit_state = V44_TRANSMIT_FAILED;
	return V44_DUPLEX_PROCEDURAL_ERROR;
}

static enum v44_duplex_result drain_transmit(
	struct v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	const size_t pending =
		v44_encoder_pending(duplex->transmit);
	const size_t available = output_capacity - *output_length;
	const size_t expected = pending < available ? pending : available;
	size_t length;

	if (pending == 0)
		return V44_DUPLEX_OK;
	if (available == 0)
		return V44_DUPLEX_NEED_OUTPUT;
	length = v44_encoder_read(
		duplex->transmit, output + *output_length, available);
	if (length != expected)
		return fail_transmit(duplex);
	*output_length += length;
	return v44_encoder_pending(duplex->transmit) == 0 ?
		       V44_DUPLEX_OK :
		       V44_DUPLEX_NEED_OUTPUT;
}

static void stop_transmit_adaptation(
	struct v44_duplex *duplex)
{
	duplex->transmit_adaptation_active = false;
}

static bool transmit_state_is_switching(enum v44_transmit_state state)
{
	return state == V44_TRANSMIT_MANUAL_SWITCHING ||
	       state == V44_TRANSMIT_AUTOMATIC_SWITCHING;
}

static void apply_monitor_decision(
	struct v44_duplex *duplex,
	enum v44_tx_monitor_decision decision)
{
	enum v44_encoder_mode target;

	if (decision == V44_TX_MONITOR_STAY)
		return;
	if (duplex->transmit_state != V44_TRANSMIT_READY) {
		stop_transmit_adaptation(duplex);
		return;
	}
	if (decision == V44_TX_MONITOR_ENTER_COMPRESSED)
		target = V44_ENCODER_COMPRESSED;
	else if (decision == V44_TX_MONITOR_ENTER_TRANSPARENT)
		target = V44_ENCODER_TRANSPARENT;
	else {
		stop_transmit_adaptation(duplex);
		return;
	}
	if (target == duplex->transmit_mode) {
		stop_transmit_adaptation(duplex);
		return;
	}
	duplex->requested_transmit_mode = target;
	duplex->transmit_state = V44_TRANSMIT_AUTOMATIC_SWITCHING;
}

static void confirm_monitor_mode(
	struct v44_duplex *duplex,
	enum v44_encoder_mode mode)
{
	if (duplex->transmit_adaptation_active &&
	    v44_tx_monitor_confirm_mode(
		    duplex->transmit_monitor, mode) !=
		    V44_TX_MONITOR_OK)
		stop_transmit_adaptation(duplex);
}

static enum v44_duplex_result drive_transmit_mode(
	struct v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	for (;;) {
		enum v44_encode_result result =
			v44_encoder_request_mode(
				duplex->transmit,
				duplex->requested_transmit_mode);

		switch (result) {
		case V44_ENCODE_OK:
			duplex->transmit_mode =
				duplex->requested_transmit_mode;
			confirm_monitor_mode(
				duplex, duplex->transmit_mode);
			duplex->transmit_state = V44_TRANSMIT_READY;
			return V44_DUPLEX_OK;
		case V44_ENCODE_NEED_OUTPUT: {
			enum v44_duplex_result drain_result =
				drain_transmit(
					duplex, output, output_capacity,
					output_length);

			if (drain_result != V44_DUPLEX_OK)
				return drain_result;
			break;
		}
		case V44_ENCODE_NEED_INPUT:
		case V44_ENCODE_FLUSHED:
		case V44_ENCODE_INVALID_ARGUMENT:
		case V44_ENCODE_INVALID_STATE:
		case V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

enum v44_duplex_result
v44_duplex_request_transmit_mode(
	struct v44_duplex *duplex,
	enum v44_encoder_mode target,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || output_length == NULL ||
	    (output_capacity != 0 && output == NULL) ||
	    target < V44_ENCODER_COMPRESSED ||
	    target > V44_ENCODER_TRANSPARENT)
		return V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit == NULL ||
	    duplex->transmit_state == V44_TRANSMIT_FLUSHING)
		return V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit_state == V44_TRANSMIT_READY) {
		duplex->requested_transmit_mode = target;
		duplex->transmit_state = V44_TRANSMIT_MANUAL_SWITCHING;
	} else if (!transmit_state_is_switching(duplex->transmit_state) ||
		   target != duplex->requested_transmit_mode) {
		return V44_DUPLEX_INVALID_STATE;
	}
	return drive_transmit_mode(
		duplex, output, output_capacity, output_length);
}

static void observe_transmit_octet(
	struct v44_duplex *duplex, uint8_t octet)
{
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_STAY;

	if (!duplex->transmit_adaptation_active)
		return;
	if (v44_tx_monitor_observe(
		    duplex->transmit_monitor, octet, &decision) !=
	    V44_TX_MONITOR_OK) {
		stop_transmit_adaptation(duplex);
		return;
	}
	apply_monitor_decision(duplex, decision);
}

static void observe_transmit_flush(
	struct v44_duplex *duplex)
{
	enum v44_tx_monitor_decision decision =
		V44_TX_MONITOR_STAY;

	if (!duplex->transmit_adaptation_active ||
	    duplex->transmit_mode != V44_ENCODER_COMPRESSED)
		return;
	if (v44_tx_monitor_flush(
		    duplex->transmit_monitor, &decision) !=
	    V44_TX_MONITOR_OK) {
		stop_transmit_adaptation(duplex);
		return;
	}
	apply_monitor_decision(duplex, decision);
}

enum v44_duplex_result v44_duplex_transmit(
	struct v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || !span_arguments_are_valid(
				      input, input_length, output,
				      output_capacity, output_length))
		return V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit_state != V44_TRANSMIT_READY &&
	    duplex->transmit_state != V44_TRANSMIT_AUTOMATIC_SWITCHING)
		return V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit == NULL)
		return copy_literal(
			input, input_length, output, output_capacity, output_length);

	for (;;) {
		enum v44_duplex_result drain_result;
		enum v44_encode_result result;
		const uint8_t *previous_input;
		size_t previous_input_length;
		size_t consumed;

		if (duplex->transmit_state ==
		    V44_TRANSMIT_AUTOMATIC_SWITCHING) {
			enum v44_duplex_result mode_result =
				drive_transmit_mode(
					duplex, output, output_capacity,
					output_length);

			if (mode_result != V44_DUPLEX_OK)
				return mode_result;
		}
		drain_result = drain_transmit(
			duplex, output, output_capacity, output_length);

		if (drain_result != V44_DUPLEX_OK)
			return drain_result;
		previous_input = *input;
		previous_input_length = *input_length;
		result = v44_encoder_process(
			duplex->transmit, input, input_length);
		if (*input_length > previous_input_length)
			return fail_transmit(duplex);
		consumed = previous_input_length - *input_length;
		if (consumed > 1 ||
		    (consumed == 0 && *input != previous_input) ||
		    (consumed == 1 &&
		     (previous_input == NULL || *input != previous_input + 1)))
			return fail_transmit(duplex);
		switch (result) {
		case V44_ENCODE_OK:
		case V44_ENCODE_NEED_OUTPUT:
			if (consumed == 1)
				observe_transmit_octet(
					duplex, previous_input[0]);
			break;
		case V44_ENCODE_NEED_INPUT:
			if (consumed != 0)
				return fail_transmit(duplex);
			return V44_DUPLEX_NEED_INPUT;
		case V44_ENCODE_FLUSHED:
		case V44_ENCODE_INVALID_ARGUMENT:
		case V44_ENCODE_INVALID_STATE:
		case V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

enum v44_duplex_result v44_duplex_flush_transmit(
	struct v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || output_length == NULL ||
	    (output_capacity != 0 && output == NULL))
		return V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit_state != V44_TRANSMIT_READY &&
	    duplex->transmit_state != V44_TRANSMIT_FLUSHING)
		return V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit == NULL)
		return V44_DUPLEX_FLUSHED;
	duplex->transmit_state = V44_TRANSMIT_FLUSHING;

	for (;;) {
		enum v44_duplex_result drain_result =
			drain_transmit(
				duplex, output, output_capacity, output_length);
		enum v44_encode_result result;

		if (drain_result != V44_DUPLEX_OK)
			return drain_result;
		result = v44_encoder_flush(duplex->transmit);
		switch (result) {
		case V44_ENCODE_OK:
		case V44_ENCODE_NEED_OUTPUT:
			break;
		case V44_ENCODE_FLUSHED:
			duplex->transmit_state = V44_TRANSMIT_READY;
			observe_transmit_flush(duplex);
			return V44_DUPLEX_FLUSHED;
		case V44_ENCODE_NEED_INPUT:
		case V44_ENCODE_INVALID_ARGUMENT:
		case V44_ENCODE_INVALID_STATE:
		case V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

static enum v44_duplex_result fail_receive(
	struct v44_duplex *duplex)
{
	duplex->receive_state = V44_RECEIVE_FAILED;
	return V44_DUPLEX_PROCEDURAL_ERROR;
}

enum v44_duplex_result v44_duplex_receive(
	struct v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || !span_arguments_are_valid(
				      input, input_length, output,
				      output_capacity, output_length))
		return V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state == V44_RECEIVE_PARAMETER)
		return V44_DUPLEX_ENTER_PARAMETER;
	if (duplex->receive_state != V44_RECEIVE_READY)
		return V44_DUPLEX_INVALID_STATE;
	if (duplex->receive == NULL)
		return copy_literal(
			input, input_length, output, output_capacity, output_length);

	for (;;) {
		size_t length = 0;
		uint8_t *next_output = *output_length == output_capacity ?
				       NULL : output + *output_length;
		enum v44_decode_result result =
			v44_decoder_process(
				duplex->receive, input, input_length, next_output,
				output_capacity - *output_length, &length);

		if (length > output_capacity - *output_length)
			return fail_receive(duplex);
		*output_length += length;
		switch (result) {
		case V44_DECODE_OK:
			break;
		case V44_DECODE_NEED_INPUT:
			return V44_DUPLEX_NEED_INPUT;
		case V44_DECODE_NEED_OUTPUT:
			return V44_DUPLEX_NEED_OUTPUT;
		case V44_DECODE_FLUSHED:
			return V44_DUPLEX_FLUSHED;
		case V44_DECODE_ENTER_PARAMETER:
			duplex->receive_state = V44_RECEIVE_PARAMETER;
			return V44_DUPLEX_ENTER_PARAMETER;
		case V44_DECODE_INVALID_ARGUMENT:
		case V44_DECODE_PROCEDURAL_ERROR:
			return fail_receive(duplex);
		}
	}
}

enum v44_duplex_result
v44_duplex_resume_receive_transparent(
	struct v44_duplex *duplex)
{
	enum v44_decode_result result;

	if (duplex == NULL)
		return V44_DUPLEX_INVALID_ARGUMENT;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state != V44_RECEIVE_PARAMETER ||
	    duplex->receive == NULL)
		return V44_DUPLEX_INVALID_STATE;
	result = v44_decoder_resume_transparent(duplex->receive);
	if (result == V44_DECODE_OK) {
		duplex->receive_state = V44_RECEIVE_READY;
		return V44_DUPLEX_OK;
	}
	return fail_receive(duplex);
}

enum v44_duplex_result v44_duplex_finish_receive(
	struct v44_duplex *duplex)
{
	enum v44_decode_result result;

	if (duplex == NULL)
		return V44_DUPLEX_INVALID_ARGUMENT;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state == V44_RECEIVE_PARAMETER)
		return V44_DUPLEX_ENTER_PARAMETER;
	if (duplex->receive_state != V44_RECEIVE_READY)
		return V44_DUPLEX_INVALID_STATE;
	if (duplex->receive == NULL) {
		duplex->receive_state = V44_RECEIVE_FINISHED;
		return V44_DUPLEX_OK;
	}
	result = v44_decoder_finish(duplex->receive);
	switch (result) {
	case V44_DECODE_OK:
		duplex->receive_state = V44_RECEIVE_FINISHED;
		return V44_DUPLEX_OK;
	case V44_DECODE_NEED_OUTPUT:
		return V44_DUPLEX_NEED_OUTPUT;
	case V44_DECODE_ENTER_PARAMETER:
		duplex->receive_state = V44_RECEIVE_PARAMETER;
		return V44_DUPLEX_ENTER_PARAMETER;
	case V44_DECODE_NEED_INPUT:
	case V44_DECODE_FLUSHED:
	case V44_DECODE_INVALID_ARGUMENT:
	case V44_DECODE_PROCEDURAL_ERROR:
		return fail_receive(duplex);
	}
	return fail_receive(duplex);
}

bool v44_duplex_get_transmit_mode(
	const struct v44_duplex *duplex,
	enum v44_encoder_mode *mode)
{
	if (duplex == NULL || duplex->transmit == NULL || mode == NULL)
		return false;
	*mode = duplex->transmit_mode;
	return true;
}

bool v44_duplex_transmit_adaptation_active(
	const struct v44_duplex *duplex)
{
	return duplex != NULL && duplex->transmit_monitor != NULL &&
	       duplex->transmit_state != V44_TRANSMIT_FAILED &&
	       duplex->transmit_adaptation_active;
}

bool v44_duplex_get_last_transmit_sample(
	const struct v44_duplex *duplex,
	struct v44_tx_monitor_sample *sample)
{
	return duplex != NULL && duplex->transmit_monitor != NULL &&
	       v44_tx_monitor_get_last_sample(
		       duplex->transmit_monitor, sample);
}

const char *v44_duplex_result_name(
	enum v44_duplex_result result)
{
	switch (result) {
	case V44_DUPLEX_OK:
		return "ok";
	case V44_DUPLEX_NEED_INPUT:
		return "need input";
	case V44_DUPLEX_NEED_OUTPUT:
		return "need output";
	case V44_DUPLEX_FLUSHED:
		return "flushed";
	case V44_DUPLEX_ENTER_PARAMETER:
		return "enter parameter";
	case V44_DUPLEX_INVALID_ARGUMENT:
		return "invalid argument";
	case V44_DUPLEX_INVALID_STATE:
		return "invalid state";
	case V44_DUPLEX_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
