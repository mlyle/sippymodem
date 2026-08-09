// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RECORDING_FILES_H
#define SIPPYMODEM_RECORDING_FILES_H

#include "sippymodem_media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_RECORDING_PREFIX_MAX = 256,
};

enum sippymodem_recording_files_result {
	SIPPYMODEM_RECORDING_FILES_OK,
	SIPPYMODEM_RECORDING_FILES_INVALID_BLOCK,
	SIPPYMODEM_RECORDING_FILES_WRITE_ERROR,
};

struct sippymodem_recording_files_stats {
	uint64_t blocks_written;
	uint64_t bytes_written;
};

struct sippymodem_recording_files;

/* Creates every evidence file exclusively with mode 0600. On failure, returns
 * NULL, removes files created by this call, and leaves errno set. */
struct sippymodem_recording_files *sippymodem_recording_files_create(
	const char *prefix);
void sippymodem_recording_files_destroy(
	struct sippymodem_recording_files *files);

/* Borrowed recorder valid until files is destroyed. */
const struct sippymodem_recorder *sippymodem_recording_files_recorder(
	const struct sippymodem_recording_files *files);
bool sippymodem_recording_files_path(
	const struct sippymodem_recording_files *files,
	enum sippymodem_recording_stream stream,
	char *path,
	size_t capacity);

enum sippymodem_recording_files_result sippymodem_recording_files_last_result(
	const struct sippymodem_recording_files *files);
int sippymodem_recording_files_system_error(
	const struct sippymodem_recording_files *files);
void sippymodem_recording_files_get_stats(
	const struct sippymodem_recording_files *files,
	struct sippymodem_recording_files_stats *stats);
const char *sippymodem_recording_files_result_name(
	enum sippymodem_recording_files_result result);

#endif
