// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_COMPRESSED_DECODER_H
#define SIPPYMODEM_V44_COMPRESSED_DECODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_compressed_result {
	SIPPYMODEM_V44_COMPRESSED_OK = 0,
	SIPPYMODEM_V44_COMPRESSED_NEED_INPUT,
	SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT,
	SIPPYMODEM_V44_COMPRESSED_FLUSHED,
	SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT,
	SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT,
	SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR,
};

struct sippymodem_v44_compressed_decoder;

/*
 * Create a decoder for validated V.44 parameters. The output ring must hold
 * at least one maximum-length decoded string so a wire code can be committed
 * atomically; smaller capacities are rejected.
 */
struct sippymodem_v44_compressed_decoder *
sippymodem_v44_compressed_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity);
void sippymodem_v44_compressed_decoder_destroy(
	struct sippymodem_v44_compressed_decoder *decoder);

/*
 * Enter compressed mode after an ECM command. This is valid only after ETM
 * has latched ENTER_TRANSPARENT. It restores the initial dictionary and field
 * widths without discarding decoded output that is still queued.
 */
enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_enter(
	struct sippymodem_v44_compressed_decoder *decoder);

/*
 * Process at most one complete compressed-mode code. NEED_INPUT can consume
 * an incomplete field and retains it for the next call. NEED_OUTPUT means a
 * complete wire code is pending; drain output and retry without rewinding the
 * input pointer. FLUSHED reports an aligned expedited-data boundary without
 * resetting dictionary continuity. ENTER_TRANSPARENT is latched after
 * consuming ETM and its alignment padding.
 */
enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_process(
	struct sippymodem_v44_compressed_decoder *decoder,
	const uint8_t **input,
	size_t *input_length);

/*
 * Finish a complete compressed stream. Decoded output may remain queued, but
 * a partial field, unresolved STEPUP, or non-aligned trailing bits fail closed.
 */
enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_finish(
	struct sippymodem_v44_compressed_decoder *decoder);

size_t sippymodem_v44_compressed_decoder_pending(
	const struct sippymodem_v44_compressed_decoder *decoder);
size_t sippymodem_v44_compressed_decoder_read(
	struct sippymodem_v44_compressed_decoder *decoder,
	uint8_t *output,
	size_t output_capacity);

const char *sippymodem_v44_compressed_result_name(
	enum sippymodem_v44_compressed_result result);

#endif
