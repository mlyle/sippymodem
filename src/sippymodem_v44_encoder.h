// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_ENCODER_H
#define SIPPYMODEM_V44_ENCODER_H

#include "sippymodem_v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_encode_result {
	SIPPYMODEM_V44_ENCODE_OK = 0,
	SIPPYMODEM_V44_ENCODE_NEED_INPUT,
	SIPPYMODEM_V44_ENCODE_NEED_OUTPUT,
	SIPPYMODEM_V44_ENCODE_FLUSHED,
	SIPPYMODEM_V44_ENCODE_INVALID_ARGUMENT,
	SIPPYMODEM_V44_ENCODE_INVALID_STATE,
	SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR,
};

enum sippymodem_v44_encoder_mode {
	SIPPYMODEM_V44_ENCODER_COMPRESSED = 0,
	SIPPYMODEM_V44_ENCODER_TRANSPARENT,
};

struct sippymodem_v44_encoder;

struct sippymodem_v44_encoder_progress {
	enum sippymodem_v44_encoder_mode mode;
	uint64_t represented_octets;
	uint64_t wire_bits;
};

/* The bounded encoded-output queue must hold at least two octets. */
struct sippymodem_v44_encoder *sippymodem_v44_encoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity);
void sippymodem_v44_encoder_destroy(
	struct sippymodem_v44_encoder *encoder);

/*
 * Accept at most one input octet. Input may remain in bounded lookahead while
 * the encoder determines a longest dictionary match or string extension.
 * Calls can therefore make progress without consuming another input octet.
 *
 * NEED_OUTPUT may follow input consumption; drain encoded output and retry
 * with the updated input pointer. No further input is consumed while a field
 * remains pending.
 */
enum sippymodem_v44_encode_result sippymodem_v44_encoder_process(
	struct sippymodem_v44_encoder *encoder,
	const uint8_t **input,
	size_t *input_length);

/*
 * Explicitly change stream mode; automatic compressibility policy belongs to
 * the caller. A request finishes all accepted data before ETM or ECM. Drain
 * output and repeat the same request on NEED_OUTPUT. Until it returns OK,
 * process and flush return INVALID_STATE and a conflicting request is
 * rejected. ESCAPE evolves across mode changes and starts at zero on create.
 */
enum sippymodem_v44_encode_result sippymodem_v44_encoder_request_mode(
	struct sippymodem_v44_encoder *encoder,
	enum sippymodem_v44_encoder_mode target);

/*
 * Drive a flush through FLUSHED, draining on NEED_OUTPUT. Once requested,
 * input and mode changes are rejected until the flush completes. Compressed
 * mode emits C-FLUSH; transparent mode only drains already accepted octets.
 */
enum sippymodem_v44_encode_result sippymodem_v44_encoder_flush(
	struct sippymodem_v44_encoder *encoder);

size_t sippymodem_v44_encoder_pending(
	const struct sippymodem_v44_encoder *encoder);
size_t sippymodem_v44_encoder_read(
	struct sippymodem_v44_encoder *encoder,
	uint8_t *output,
	size_t output_capacity);

/*
 * Return monotonic progress independent of output draining. Input counts only
 * after its complete representation enters the bit writer; wire_bits includes
 * controls and alignment. During a mode transition, mode is the source mode.
 */
bool sippymodem_v44_encoder_get_progress(
	const struct sippymodem_v44_encoder *encoder,
	struct sippymodem_v44_encoder_progress *progress);

const char *sippymodem_v44_encode_result_name(
	enum sippymodem_v44_encode_result result);

#endif
