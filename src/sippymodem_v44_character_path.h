// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_CHARACTER_PATH_H
#define SIPPYMODEM_V44_CHARACTER_PATH_H

#include "sippymodem_v44_duplex.h"

#include <stddef.h>
#include <stdint.h>

struct sippymodem_v44_character_path;

/*
 * Return the number transferred or -1 on failure. A zero reader result marks
 * the current burst empty and causes one C-FLUSH after accepted input; a zero
 * writer result reports sink backpressure.
 */
typedef int (*sippymodem_v44_character_read)(
	void *context, uint8_t *output, size_t output_capacity);
typedef int (*sippymodem_v44_character_write)(
	void *context, const uint8_t *input, size_t input_length);

struct sippymodem_v44_character_path_config {
	struct sippymodem_v44_duplex_config codec;
	size_t receive_buffer_capacity;
	void *context;
	sippymodem_v44_character_read read;
	sippymodem_v44_character_write write;
};

enum sippymodem_v44_character_path_result {
	SIPPYMODEM_V44_CHARACTER_PATH_OK = 0,
	SIPPYMODEM_V44_CHARACTER_PATH_IDLE,
	SIPPYMODEM_V44_CHARACTER_PATH_NEED_OUTPUT,
	SIPPYMODEM_V44_CHARACTER_PATH_IO_ERROR,
	SIPPYMODEM_V44_CHARACTER_PATH_CODEC_ERROR,
	SIPPYMODEM_V44_CHARACTER_PATH_INVALID_ARGUMENT,
};

/*
 * The path owns one duplex codec and all incremental buffering. The callbacks
 * and their context remain caller-owned and must outlive the path. Calls in
 * each direction must be serialized; one transmit and one receive call may
 * run concurrently when the callbacks permit it. Destruction must be
 * serialized with both directions.
 */
struct sippymodem_v44_character_path *sippymodem_v44_character_path_create(
	const struct sippymodem_v44_character_path_config *config);
void sippymodem_v44_character_path_destroy(
	struct sippymodem_v44_character_path *path);

/*
 * Reinitialize both codec directions to their negotiated C-INIT state and
 * discard all buffered character and wire data. The operation is
 * transactional: if replacement codec allocation fails, the existing stream
 * remains unchanged. Serialize reinitialization with both data directions.
 */
enum sippymodem_v44_character_path_result
sippymodem_v44_character_path_reinitialize(
	struct sippymodem_v44_character_path *path);

/*
 * Pull uncompressed characters through the configured reader and produce a
 * bounded V.44 wire span. The path requests at most one character at a time,
 * so a byte is retained until the codec accepts it. Once the reader is empty,
 * accepted data is terminated by exactly one explicit stream flush. IDLE
 * means no character, codec output, or incomplete flush remains.
 *
 * Valid calls always set output_length. A zero-capacity output may be NULL.
 */
enum sippymodem_v44_character_path_result
sippymodem_v44_character_path_transmit(
	struct sippymodem_v44_character_path *path,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Consume V.44 wire bytes and push decoded characters through the configured
 * writer. Partial writer progress is retained in the bounded receive buffer;
 * NEED_OUTPUT reports a zero-progress writer. input_consumed is exact,
 * allowing the caller to retain the rest of a LAPM information field without
 * replaying bytes already accepted.
 *
 * Valid calls always set input_consumed. Empty input may be NULL.
 */
enum sippymodem_v44_character_path_result
sippymodem_v44_character_path_receive(
	struct sippymodem_v44_character_path *path,
	const uint8_t *input,
	size_t input_length,
	size_t *input_consumed);

/*
 * Drain decoded characters and verify a complete receive stream at call
 * teardown. NEED_OUTPUT retains sink backpressure; repeat it after the writer
 * can make progress. A malformed or truncated stream fails closed.
 */
enum sippymodem_v44_character_path_result
sippymodem_v44_character_path_finish_receive(
	struct sippymodem_v44_character_path *path);

const char *sippymodem_v44_character_path_result_name(
	enum sippymodem_v44_character_path_result result);

#endif
