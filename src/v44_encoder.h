// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_ENCODER_H
#define V44_ENCODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum v44_encode_result {
	V44_ENCODE_OK = 0,
	V44_ENCODE_NEED_INPUT,
	V44_ENCODE_NEED_OUTPUT,
	V44_ENCODE_FLUSHED,
	V44_ENCODE_INVALID_ARGUMENT,
	V44_ENCODE_INVALID_STATE,
	V44_ENCODE_PROCEDURAL_ERROR,
};

enum v44_encoder_mode {
	V44_ENCODER_COMPRESSED = 0,
	V44_ENCODER_TRANSPARENT,
};

struct v44_encoder;

struct v44_encoder_progress {
	enum v44_encoder_mode mode;
	uint64_t represented_octets;
	uint64_t wire_bits;
};

/* The bounded encoded-output queue must hold at least two octets. */
struct v44_encoder *v44_encoder_create(
	const struct v44_parameters *parameters,
	size_t output_capacity);
void v44_encoder_destroy(
	struct v44_encoder *encoder);

/*
 * Accept at most one input octet. Input may remain in bounded lookahead while
 * the encoder determines a longest dictionary match or string extension.
 * Calls can therefore make progress without consuming another input octet.
 *
 * NEED_OUTPUT may follow input consumption; drain encoded output and retry
 * with the updated input pointer. No further input is consumed while a field
 * remains pending.
 */
enum v44_encode_result v44_encoder_process(
	struct v44_encoder *encoder,
	const uint8_t **input,
	size_t *input_length);

/*
 * Explicitly change stream mode; automatic compressibility policy belongs to
 * the caller. A request finishes all accepted data before ETM or ECM. Drain
 * output and repeat the same request on NEED_OUTPUT. Until it returns OK,
 * process and flush return INVALID_STATE and a conflicting request is
 * rejected. ESCAPE evolves across mode changes and starts at zero on create.
 */
enum v44_encode_result v44_encoder_request_mode(
	struct v44_encoder *encoder,
	enum v44_encoder_mode target);

/*
 * Drive a flush through FLUSHED, draining on NEED_OUTPUT. Once requested,
 * input and mode changes are rejected until the flush completes. Compressed
 * mode emits C-FLUSH; transparent mode only drains already accepted octets.
 */
enum v44_encode_result v44_encoder_flush(
	struct v44_encoder *encoder);

size_t v44_encoder_pending(
	const struct v44_encoder *encoder);
size_t v44_encoder_read(
	struct v44_encoder *encoder,
	uint8_t *output,
	size_t output_capacity);

/*
 * Return monotonic progress independent of output draining. Input counts only
 * after its complete representation enters the bit writer; wire_bits includes
 * controls and alignment. During a mode transition, mode is the source mode.
 */
bool v44_encoder_get_progress(
	const struct v44_encoder *encoder,
	struct v44_encoder_progress *progress);

const char *v44_encode_result_name(
	enum v44_encode_result result);

#endif
