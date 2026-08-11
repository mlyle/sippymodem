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
	SIPPYMODEM_V44_ENCODE_PROCEDURAL_ERROR,
};

struct sippymodem_v44_encoder;

/* The bounded encoded-output queue must hold at least two octets. */
struct sippymodem_v44_encoder *sippymodem_v44_encoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity);
void sippymodem_v44_encoder_destroy(
	struct sippymodem_v44_encoder *encoder);

/*
 * Accept at most one input octet. The current literal stage emits ordinals
 * while maintaining the same dictionary counters and history boundaries as
 * its peer decoder. A later matcher can select codewords without changing
 * this bounded stream API.
 *
 * NEED_OUTPUT may follow consumption of one input octet; drain encoded output
 * and retry with the updated input pointer. No further input is consumed until
 * that pending octet has been encoded completely.
 */
enum sippymodem_v44_encode_result sippymodem_v44_encoder_process(
	struct sippymodem_v44_encoder *encoder,
	const uint8_t **input,
	size_t *input_length);

/* Drive a C-FLUSH request until FLUSHED, draining on NEED_OUTPUT. */
enum sippymodem_v44_encode_result sippymodem_v44_encoder_flush(
	struct sippymodem_v44_encoder *encoder);

size_t sippymodem_v44_encoder_pending(
	const struct sippymodem_v44_encoder *encoder);
size_t sippymodem_v44_encoder_read(
	struct sippymodem_v44_encoder *encoder,
	uint8_t *output,
	size_t output_capacity);

const char *sippymodem_v44_encode_result_name(
	enum sippymodem_v44_encode_result result);

#endif
