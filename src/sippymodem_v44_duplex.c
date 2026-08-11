// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_duplex.h"

#include "sippymodem_v44_decoder.h"
#include "sippymodem_v44_encoder.h"

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

struct sippymodem_v44_duplex {
	struct sippymodem_v44_encoder *transmit;
	struct sippymodem_v44_tx_monitor *transmit_monitor;
	enum v44_transmit_state transmit_state;
	enum sippymodem_v44_encoder_mode transmit_mode;
	enum sippymodem_v44_encoder_mode requested_transmit_mode;
	bool transmit_adaptation_active;
	struct sippymodem_v44_decoder *receive;
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

static enum sippymodem_v44_duplex_result copy_literal(
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
	return *input_length == 0 ? SIPPYMODEM_V44_DUPLEX_NEED_INPUT :
				    SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT;
}

struct sippymodem_v44_duplex *sippymodem_v44_duplex_create(
	const struct sippymodem_v44_duplex_config *config)
{
	struct sippymodem_v44_duplex *duplex;

	if (config == NULL ||
	    (config->transmit_adaptation_enabled &&
	     !config->transmit_enabled) ||
	    (config->transmit_enabled &&
	     !sippymodem_v44_parameters_are_valid(
		     &config->transmit_parameters)) ||
	    (config->receive_enabled &&
	     !sippymodem_v44_parameters_are_valid(
		     &config->receive_parameters)))
		return NULL;
	duplex = calloc(1, sizeof(*duplex));
	if (duplex == NULL)
		return NULL;
	if (config->transmit_enabled)
		duplex->transmit = sippymodem_v44_encoder_create(
			&config->transmit_parameters, 2);
	if (config->transmit_adaptation_enabled)
		duplex->transmit_monitor = sippymodem_v44_tx_monitor_create(
			&config->transmit_parameters,
			&config->transmit_adaptation);
	if (config->receive_enabled)
		duplex->receive = sippymodem_v44_decoder_create(
			&config->receive_parameters,
			config->receive_parameters.max_string_length);
	if ((config->transmit_enabled && duplex->transmit == NULL) ||
	    (config->transmit_adaptation_enabled &&
	     duplex->transmit_monitor == NULL) ||
	    (config->receive_enabled && duplex->receive == NULL)) {
		sippymodem_v44_duplex_destroy(duplex);
		return NULL;
	}
	duplex->transmit_mode = SIPPYMODEM_V44_ENCODER_COMPRESSED;
	duplex->transmit_adaptation_active =
		config->transmit_adaptation_enabled;
	return duplex;
}

void sippymodem_v44_duplex_destroy(
	struct sippymodem_v44_duplex *duplex)
{
	if (duplex == NULL)
		return;
	sippymodem_v44_decoder_destroy(duplex->receive);
	sippymodem_v44_tx_monitor_destroy(duplex->transmit_monitor);
	sippymodem_v44_encoder_destroy(duplex->transmit);
	free(duplex);
}

static enum sippymodem_v44_duplex_result fail_transmit(
	struct sippymodem_v44_duplex *duplex)
{
	duplex->transmit_state = V44_TRANSMIT_FAILED;
	return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
}

static enum sippymodem_v44_duplex_result drain_transmit(
	struct sippymodem_v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	const size_t pending =
		sippymodem_v44_encoder_pending(duplex->transmit);
	const size_t available = output_capacity - *output_length;
	const size_t expected = pending < available ? pending : available;
	size_t length;

	if (pending == 0)
		return SIPPYMODEM_V44_DUPLEX_OK;
	if (available == 0)
		return SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT;
	length = sippymodem_v44_encoder_read(
		duplex->transmit, output + *output_length, available);
	if (length != expected)
		return fail_transmit(duplex);
	*output_length += length;
	return sippymodem_v44_encoder_pending(duplex->transmit) == 0 ?
		       SIPPYMODEM_V44_DUPLEX_OK :
		       SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT;
}

static void stop_transmit_adaptation(
	struct sippymodem_v44_duplex *duplex)
{
	duplex->transmit_adaptation_active = false;
}

static bool transmit_state_is_switching(enum v44_transmit_state state)
{
	return state == V44_TRANSMIT_MANUAL_SWITCHING ||
	       state == V44_TRANSMIT_AUTOMATIC_SWITCHING;
}

static void apply_monitor_decision(
	struct sippymodem_v44_duplex *duplex,
	enum sippymodem_v44_tx_monitor_decision decision)
{
	enum sippymodem_v44_encoder_mode target;

	if (decision == SIPPYMODEM_V44_TX_MONITOR_STAY)
		return;
	if (duplex->transmit_state != V44_TRANSMIT_READY) {
		stop_transmit_adaptation(duplex);
		return;
	}
	if (decision == SIPPYMODEM_V44_TX_MONITOR_ENTER_COMPRESSED)
		target = SIPPYMODEM_V44_ENCODER_COMPRESSED;
	else if (decision == SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT)
		target = SIPPYMODEM_V44_ENCODER_TRANSPARENT;
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
	struct sippymodem_v44_duplex *duplex,
	enum sippymodem_v44_encoder_mode mode)
{
	if (duplex->transmit_adaptation_active &&
	    sippymodem_v44_tx_monitor_confirm_mode(
		    duplex->transmit_monitor, mode) !=
		    SIPPYMODEM_V44_TX_MONITOR_OK)
		stop_transmit_adaptation(duplex);
}

static enum sippymodem_v44_duplex_result drive_transmit_mode(
	struct sippymodem_v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	for (;;) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_request_mode(
				duplex->transmit,
				duplex->requested_transmit_mode);

		switch (result) {
		case SIPPYMODEM_V44_ENCODE_OK:
			duplex->transmit_mode =
				duplex->requested_transmit_mode;
			confirm_monitor_mode(
				duplex, duplex->transmit_mode);
			duplex->transmit_state = V44_TRANSMIT_READY;
			return SIPPYMODEM_V44_DUPLEX_OK;
		case SIPPYMODEM_V44_ENCODE_NEED_OUTPUT: {
			enum sippymodem_v44_duplex_result drain_result =
				drain_transmit(
					duplex, output, output_capacity,
					output_length);

			if (drain_result != SIPPYMODEM_V44_DUPLEX_OK)
				return drain_result;
			break;
		}
		case SIPPYMODEM_V44_ENCODE_NEED_INPUT:
		case SIPPYMODEM_V44_ENCODE_FLUSHED:
		case SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT:
		case SIPPYMODEM_V44_ENCODE_INVALID_STATE:
		case SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

enum sippymodem_v44_duplex_result
sippymodem_v44_duplex_request_transmit_mode(
	struct sippymodem_v44_duplex *duplex,
	enum sippymodem_v44_encoder_mode target,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || output_length == NULL ||
	    (output_capacity != 0 && output == NULL) ||
	    target < SIPPYMODEM_V44_ENCODER_COMPRESSED ||
	    target > SIPPYMODEM_V44_ENCODER_TRANSPARENT)
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit == NULL ||
	    duplex->transmit_state == V44_TRANSMIT_FLUSHING)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit_state == V44_TRANSMIT_READY) {
		duplex->requested_transmit_mode = target;
		duplex->transmit_state = V44_TRANSMIT_MANUAL_SWITCHING;
	} else if (!transmit_state_is_switching(duplex->transmit_state) ||
		   target != duplex->requested_transmit_mode) {
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	}
	return drive_transmit_mode(
		duplex, output, output_capacity, output_length);
}

static void observe_transmit_octet(
	struct sippymodem_v44_duplex *duplex, uint8_t octet)
{
	enum sippymodem_v44_tx_monitor_decision decision =
		SIPPYMODEM_V44_TX_MONITOR_STAY;

	if (!duplex->transmit_adaptation_active)
		return;
	if (sippymodem_v44_tx_monitor_observe(
		    duplex->transmit_monitor, octet, &decision) !=
	    SIPPYMODEM_V44_TX_MONITOR_OK) {
		stop_transmit_adaptation(duplex);
		return;
	}
	apply_monitor_decision(duplex, decision);
}

static void observe_transmit_flush(
	struct sippymodem_v44_duplex *duplex)
{
	enum sippymodem_v44_tx_monitor_decision decision =
		SIPPYMODEM_V44_TX_MONITOR_STAY;

	if (!duplex->transmit_adaptation_active ||
	    duplex->transmit_mode != SIPPYMODEM_V44_ENCODER_COMPRESSED)
		return;
	if (sippymodem_v44_tx_monitor_flush(
		    duplex->transmit_monitor, &decision) !=
	    SIPPYMODEM_V44_TX_MONITOR_OK) {
		stop_transmit_adaptation(duplex);
		return;
	}
	apply_monitor_decision(duplex, decision);
}

enum sippymodem_v44_duplex_result sippymodem_v44_duplex_transmit(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || !span_arguments_are_valid(
				      input, input_length, output,
				      output_capacity, output_length))
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit_state != V44_TRANSMIT_READY &&
	    duplex->transmit_state != V44_TRANSMIT_AUTOMATIC_SWITCHING)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit == NULL)
		return copy_literal(
			input, input_length, output, output_capacity, output_length);

	for (;;) {
		enum sippymodem_v44_duplex_result drain_result;
		enum sippymodem_v44_encode_result result;
		const uint8_t *previous_input;
		size_t previous_input_length;
		size_t consumed;

		if (duplex->transmit_state ==
		    V44_TRANSMIT_AUTOMATIC_SWITCHING) {
			enum sippymodem_v44_duplex_result mode_result =
				drive_transmit_mode(
					duplex, output, output_capacity,
					output_length);

			if (mode_result != SIPPYMODEM_V44_DUPLEX_OK)
				return mode_result;
		}
		drain_result = drain_transmit(
			duplex, output, output_capacity, output_length);

		if (drain_result != SIPPYMODEM_V44_DUPLEX_OK)
			return drain_result;
		previous_input = *input;
		previous_input_length = *input_length;
		result = sippymodem_v44_encoder_process(
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
		case SIPPYMODEM_V44_ENCODE_OK:
		case SIPPYMODEM_V44_ENCODE_NEED_OUTPUT:
			if (consumed == 1)
				observe_transmit_octet(
					duplex, previous_input[0]);
			break;
		case SIPPYMODEM_V44_ENCODE_NEED_INPUT:
			if (consumed != 0)
				return fail_transmit(duplex);
			return SIPPYMODEM_V44_DUPLEX_NEED_INPUT;
		case SIPPYMODEM_V44_ENCODE_FLUSHED:
		case SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT:
		case SIPPYMODEM_V44_ENCODE_INVALID_STATE:
		case SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

enum sippymodem_v44_duplex_result sippymodem_v44_duplex_flush_transmit(
	struct sippymodem_v44_duplex *duplex,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || output_length == NULL ||
	    (output_capacity != 0 && output == NULL))
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->transmit_state == V44_TRANSMIT_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->transmit_state != V44_TRANSMIT_READY &&
	    duplex->transmit_state != V44_TRANSMIT_FLUSHING)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	if (duplex->transmit == NULL)
		return SIPPYMODEM_V44_DUPLEX_FLUSHED;
	duplex->transmit_state = V44_TRANSMIT_FLUSHING;

	for (;;) {
		enum sippymodem_v44_duplex_result drain_result =
			drain_transmit(
				duplex, output, output_capacity, output_length);
		enum sippymodem_v44_encode_result result;

		if (drain_result != SIPPYMODEM_V44_DUPLEX_OK)
			return drain_result;
		result = sippymodem_v44_encoder_flush(duplex->transmit);
		switch (result) {
		case SIPPYMODEM_V44_ENCODE_OK:
		case SIPPYMODEM_V44_ENCODE_NEED_OUTPUT:
			break;
		case SIPPYMODEM_V44_ENCODE_FLUSHED:
			duplex->transmit_state = V44_TRANSMIT_READY;
			observe_transmit_flush(duplex);
			return SIPPYMODEM_V44_DUPLEX_FLUSHED;
		case SIPPYMODEM_V44_ENCODE_NEED_INPUT:
		case SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT:
		case SIPPYMODEM_V44_ENCODE_INVALID_STATE:
		case SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR:
			return fail_transmit(duplex);
		}
	}
}

static enum sippymodem_v44_duplex_result fail_receive(
	struct sippymodem_v44_duplex *duplex)
{
	duplex->receive_state = V44_RECEIVE_FAILED;
	return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
}

enum sippymodem_v44_duplex_result sippymodem_v44_duplex_receive(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (duplex == NULL || !span_arguments_are_valid(
				      input, input_length, output,
				      output_capacity, output_length))
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	*output_length = 0;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state == V44_RECEIVE_PARAMETER)
		return SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER;
	if (duplex->receive_state != V44_RECEIVE_READY)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	if (duplex->receive == NULL)
		return copy_literal(
			input, input_length, output, output_capacity, output_length);

	for (;;) {
		size_t length = 0;
		uint8_t *next_output = *output_length == output_capacity ?
				       NULL : output + *output_length;
		enum sippymodem_v44_decode_result result =
			sippymodem_v44_decoder_process(
				duplex->receive, input, input_length, next_output,
				output_capacity - *output_length, &length);

		if (length > output_capacity - *output_length)
			return fail_receive(duplex);
		*output_length += length;
		switch (result) {
		case SIPPYMODEM_V44_DECODE_OK:
			break;
		case SIPPYMODEM_V44_DECODE_NEED_INPUT:
			return SIPPYMODEM_V44_DUPLEX_NEED_INPUT;
		case SIPPYMODEM_V44_DECODE_NEED_OUTPUT:
			return SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT;
		case SIPPYMODEM_V44_DECODE_FLUSHED:
			return SIPPYMODEM_V44_DUPLEX_FLUSHED;
		case SIPPYMODEM_V44_DECODE_ENTER_PARAMETER:
			duplex->receive_state = V44_RECEIVE_PARAMETER;
			return SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER;
		case SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT:
		case SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR:
			return fail_receive(duplex);
		}
	}
}

enum sippymodem_v44_duplex_result
sippymodem_v44_duplex_resume_receive_transparent(
	struct sippymodem_v44_duplex *duplex)
{
	enum sippymodem_v44_decode_result result;

	if (duplex == NULL)
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state != V44_RECEIVE_PARAMETER ||
	    duplex->receive == NULL)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	result = sippymodem_v44_decoder_resume_transparent(duplex->receive);
	if (result == SIPPYMODEM_V44_DECODE_OK) {
		duplex->receive_state = V44_RECEIVE_READY;
		return SIPPYMODEM_V44_DUPLEX_OK;
	}
	return fail_receive(duplex);
}

enum sippymodem_v44_duplex_result sippymodem_v44_duplex_finish_receive(
	struct sippymodem_v44_duplex *duplex)
{
	enum sippymodem_v44_decode_result result;

	if (duplex == NULL)
		return SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT;
	if (duplex->receive_state == V44_RECEIVE_FAILED)
		return SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR;
	if (duplex->receive_state == V44_RECEIVE_PARAMETER)
		return SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER;
	if (duplex->receive_state != V44_RECEIVE_READY)
		return SIPPYMODEM_V44_DUPLEX_INVALID_STATE;
	if (duplex->receive == NULL) {
		duplex->receive_state = V44_RECEIVE_FINISHED;
		return SIPPYMODEM_V44_DUPLEX_OK;
	}
	result = sippymodem_v44_decoder_finish(duplex->receive);
	switch (result) {
	case SIPPYMODEM_V44_DECODE_OK:
		duplex->receive_state = V44_RECEIVE_FINISHED;
		return SIPPYMODEM_V44_DUPLEX_OK;
	case SIPPYMODEM_V44_DECODE_NEED_OUTPUT:
		return SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT;
	case SIPPYMODEM_V44_DECODE_ENTER_PARAMETER:
		duplex->receive_state = V44_RECEIVE_PARAMETER;
		return SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER;
	case SIPPYMODEM_V44_DECODE_NEED_INPUT:
	case SIPPYMODEM_V44_DECODE_FLUSHED:
	case SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT:
	case SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR:
		return fail_receive(duplex);
	}
	return fail_receive(duplex);
}

bool sippymodem_v44_duplex_get_transmit_mode(
	const struct sippymodem_v44_duplex *duplex,
	enum sippymodem_v44_encoder_mode *mode)
{
	if (duplex == NULL || duplex->transmit == NULL || mode == NULL)
		return false;
	*mode = duplex->transmit_mode;
	return true;
}

bool sippymodem_v44_duplex_transmit_adaptation_active(
	const struct sippymodem_v44_duplex *duplex)
{
	return duplex != NULL && duplex->transmit_monitor != NULL &&
	       duplex->transmit_state != V44_TRANSMIT_FAILED &&
	       duplex->transmit_adaptation_active;
}

bool sippymodem_v44_duplex_get_last_transmit_sample(
	const struct sippymodem_v44_duplex *duplex,
	struct sippymodem_v44_tx_monitor_sample *sample)
{
	return duplex != NULL && duplex->transmit_monitor != NULL &&
	       sippymodem_v44_tx_monitor_get_last_sample(
		       duplex->transmit_monitor, sample);
}

const char *sippymodem_v44_duplex_result_name(
	enum sippymodem_v44_duplex_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_DUPLEX_OK:
		return "ok";
	case SIPPYMODEM_V44_DUPLEX_NEED_INPUT:
		return "need input";
	case SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_DUPLEX_FLUSHED:
		return "flushed";
	case SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER:
		return "enter parameter";
	case SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_DUPLEX_INVALID_STATE:
		return "invalid state";
	case SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
