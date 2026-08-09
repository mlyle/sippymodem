// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RECORDING_FILES_H
#define SIPPYMODEM_RECORDING_FILES_H

#include "sippymodem_media.h"

#include <stdbool.h>
#include <stddef.h>

enum {
	SIPPYMODEM_RECORDING_PREFIX_MAX = 256,
};

struct sippymodem_recording_files;

/* Creates every evidence file exclusively with mode 0600. On failure, returns
 * NULL, removes files created by this call, and leaves errno set. */
struct sippymodem_recording_files *sippymodem_recording_files_create(
	const char *prefix);
void sippymodem_recording_files_destroy(
	struct sippymodem_recording_files *files);

bool sippymodem_recording_files_path(
	const struct sippymodem_recording_files *files,
	enum sippymodem_recording_stream stream,
	char *path,
	size_t capacity);

#endif
