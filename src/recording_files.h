// SPDX-License-Identifier: BSD-3-Clause

#ifndef RECORDING_FILES_H
#define RECORDING_FILES_H

#include "media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RECORDING_PREFIX_MAX = 256,
};

enum recording_files_result {
	RECORDING_FILES_OK,
	RECORDING_FILES_INVALID_BLOCK,
	RECORDING_FILES_WRITE_ERROR,
};

struct recording_files_stats {
	uint64_t blocks_written;
	uint64_t bytes_written;
};

struct recording_files;

/* Creates every evidence file exclusively with mode 0600. On failure, returns
 * NULL, removes files created by this call, and leaves errno set. */
struct recording_files *recording_files_create(
	const char *prefix);
void recording_files_destroy(
	struct recording_files *files);

/* Borrowed recorder valid until files is destroyed. */
const struct media_recorder *recording_files_recorder(
	const struct recording_files *files);
bool recording_files_path(
	const struct recording_files *files,
	enum recording_stream stream,
	char *path,
	size_t capacity);

enum recording_files_result recording_files_last_result(
	const struct recording_files *files);
int recording_files_system_error(
	const struct recording_files *files);
void recording_files_get_stats(
	const struct recording_files *files,
	struct recording_files_stats *stats);
const char *recording_files_result_name(
	enum recording_files_result result);

#endif
