// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_COMPRESSED_DECODER_H
#define V44_COMPRESSED_DECODER_H

#include "v44_stream.h"

#include <stddef.h>
#include <stdint.h>

enum v44_compressed_result {
	V44_COMPRESSED_OK = 0,
	V44_COMPRESSED_NEED_INPUT,
	V44_COMPRESSED_NEED_OUTPUT,
	V44_COMPRESSED_FLUSHED,
	V44_COMPRESSED_ENTER_TRANSPARENT,
	V44_COMPRESSED_INVALID_ARGUMENT,
	V44_COMPRESSED_PROCEDURAL_ERROR,
};

struct v44_compressed_decoder;

/*
 * Create a decoder for validated V.44 parameters. The output ring must hold
 * at least one maximum-length decoded string so a wire code can be committed
 * atomically; smaller capacities are rejected.
 */
struct v44_compressed_decoder *
v44_compressed_decoder_create(
	const struct v44_parameters *parameters,
	size_t output_capacity);
void v44_compressed_decoder_destroy(
	struct v44_compressed_decoder *decoder);

/*
 * Enter compressed mode after an ECM command. This is valid only after ETM
 * has latched ENTER_TRANSPARENT. It restores the initial dictionary and field
 * widths without discarding decoded output that is still queued.
 */
enum v44_compressed_result
v44_compressed_decoder_enter(
	struct v44_compressed_decoder *decoder);

/*
 * Process at most one complete compressed-mode code. NEED_INPUT can consume
 * an incomplete field and retains it for the next call. NEED_OUTPUT means a
 * complete wire code is pending; drain output and retry without rewinding the
 * input pointer. FLUSHED reports an aligned expedited-data boundary without
 * resetting dictionary continuity. ENTER_TRANSPARENT is latched after
 * consuming ETM and its alignment padding.
 */
enum v44_compressed_result
v44_compressed_decoder_process(
	struct v44_compressed_decoder *decoder,
	const uint8_t **input,
	size_t *input_length);

/*
 * Finish a complete compressed stream. Decoded output may remain queued, but
 * a partial field, unresolved STEPUP, or non-aligned trailing bits fail closed.
 */
enum v44_compressed_result
v44_compressed_decoder_finish(
	struct v44_compressed_decoder *decoder);

size_t v44_compressed_decoder_pending(
	const struct v44_compressed_decoder *decoder);
size_t v44_compressed_decoder_read(
	struct v44_compressed_decoder *decoder,
	uint8_t *output,
	size_t output_capacity);

const char *v44_compressed_result_name(
	enum v44_compressed_result result);

#endif
