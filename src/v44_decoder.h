// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_DECODER_H
#define SIPPYMODEM_V44_DECODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_decode_result {
	SIPPYMODEM_V44_DECODE_OK = 0,
	SIPPYMODEM_V44_DECODE_NEED_INPUT,
	SIPPYMODEM_V44_DECODE_NEED_OUTPUT,
	SIPPYMODEM_V44_DECODE_FLUSHED,
	SIPPYMODEM_V44_DECODE_ENTER_PARAMETER,
	SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT,
	SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR,
};

struct sippymodem_v44_decoder;

/* The compressed-output capacity must hold one maximum-length string. */
struct sippymodem_v44_decoder *sippymodem_v44_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t compressed_output_capacity);
void sippymodem_v44_decoder_destroy(
	struct sippymodem_v44_decoder *decoder);

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
enum sippymodem_v44_decode_result sippymodem_v44_decoder_process(
	struct sippymodem_v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

enum sippymodem_v44_decode_result
sippymodem_v44_decoder_resume_transparent(
	struct sippymodem_v44_decoder *decoder);

/* Finish fails closed on a partial field or transparent escape sequence. */
enum sippymodem_v44_decode_result sippymodem_v44_decoder_finish(
	struct sippymodem_v44_decoder *decoder);

const char *sippymodem_v44_decode_result_name(
	enum sippymodem_v44_decode_result result);

#endif
