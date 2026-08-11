// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_DECODER_H
#define V44_DECODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum v44_decode_result {
	V44_DECODE_OK = 0,
	V44_DECODE_NEED_INPUT,
	V44_DECODE_NEED_OUTPUT,
	V44_DECODE_FLUSHED,
	V44_DECODE_ENTER_PARAMETER,
	V44_DECODE_INVALID_ARGUMENT,
	V44_DECODE_PROCEDURAL_ERROR,
};

struct v44_decoder;

/* The compressed-output capacity must hold one maximum-length string. */
struct v44_decoder *v44_decoder_create(
	const struct v44_parameters *parameters,
	size_t compressed_output_capacity);
void v44_decoder_destroy(
	struct v44_decoder *decoder);

/*
 * Process at most one compressed code, transparent character, or mode
 * command. Queued compressed output is always delivered before more input is
 * consumed, so transparent bytes cannot overtake data decoded before ETM.
 *
 * A compressed code may be consumed before NEED_OUTPUT is returned. Retry
 * with output space and the updated input pointer; queued bytes are then
 * drained before any further input. FLUSHED is reported only after preceding
 * decoded output has been delivered. Parameter mode is latched until the
 * external parameter parser reaches End and calls resume_transparent().
 */
enum v44_decode_result v44_decoder_process(
	struct v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

enum v44_decode_result
v44_decoder_resume_transparent(
	struct v44_decoder *decoder);

/* Finish fails closed on a partial field or transparent escape sequence. */
enum v44_decode_result v44_decoder_finish(
	struct v44_decoder *decoder);

const char *v44_decode_result_name(
	enum v44_decode_result result);

#endif
