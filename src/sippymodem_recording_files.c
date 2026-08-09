// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_recording_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	RECORDING_STREAM_COUNT =
		SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1,
};

struct sippymodem_recording_files {
	char prefix[SIPPYMODEM_RECORDING_PREFIX_MAX];
	int descriptors[RECORDING_STREAM_COUNT];
};

static bool stream_is_valid(enum sippymodem_recording_stream stream)
{
	return stream >= SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL &&
	       stream <= SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT;
}

static bool stream_contains_pcm(enum sippymodem_recording_stream stream)
{
	return stream == SIPPYMODEM_RECORD_RX_LINE_PCM ||
	       stream == SIPPYMODEM_RECORD_RX_MODEM_PCM ||
	       stream == SIPPYMODEM_RECORD_TX_MODEM_PCM ||
	       stream == SIPPYMODEM_RECORD_TX_LINE_PCM;
}

static bool build_path(
	const char *prefix, enum sippymodem_recording_stream stream,
	char *path, size_t capacity)
{
	const char *name;
	const char *extension;
	int length;

	if (prefix == NULL || prefix[0] == '\0' ||
	    !stream_is_valid(stream) || path == NULL || capacity == 0)
		return false;
	name = sippymodem_recording_stream_name(stream);
	if (name == NULL)
		return false;
	extension = stream_contains_pcm(stream) ? "8000hz.s16le" :
						  "8000hz.octets";
	length = snprintf(path, capacity, "%s.%s.%s", prefix, name, extension);
	return length >= 0 && (size_t)length < capacity;
}

static void close_descriptors(struct sippymodem_recording_files *files)
{
	for (size_t index = 0; index < RECORDING_STREAM_COUNT; ++index) {
		if (files->descriptors[index] >= 0) {
			close(files->descriptors[index]);
			files->descriptors[index] = -1;
		}
	}
}

static void remove_paths(
	const struct sippymodem_recording_files *files, size_t count)
{
	char path[PATH_MAX];

	for (size_t index = 0; index < count; ++index) {
		if (build_path(files->prefix,
			       (enum sippymodem_recording_stream)index,
			       path, sizeof(path)))
			unlink(path);
	}
}

struct sippymodem_recording_files *sippymodem_recording_files_create(
	const char *prefix)
{
	struct sippymodem_recording_files *files;
	char path[PATH_MAX];
	size_t prefix_length;
	size_t created = 0;
	int failure;

	if (prefix == NULL || prefix[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	prefix_length = strlen(prefix);
	if (prefix_length >= SIPPYMODEM_RECORDING_PREFIX_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	files = calloc(1, sizeof(*files));
	if (files == NULL)
		return NULL;
	memcpy(files->prefix, prefix, prefix_length + 1);
	for (size_t index = 0; index < RECORDING_STREAM_COUNT; ++index)
		files->descriptors[index] = -1;

	while (created < RECORDING_STREAM_COUNT) {
		if (!build_path(
			    files->prefix,
			    (enum sippymodem_recording_stream)created,
			    path, sizeof(path))) {
			errno = ENAMETOOLONG;
			goto fail;
		}
		files->descriptors[created] = open(
			path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
			S_IRUSR | S_IWUSR);
		if (files->descriptors[created] < 0)
			goto fail;
		created++;
		if (fchmod(files->descriptors[created - 1],
			   S_IRUSR | S_IWUSR) != 0)
			goto fail;
	}
	return files;

fail:
	failure = errno;
	close_descriptors(files);
	remove_paths(files, created);
	free(files);
	errno = failure;
	return NULL;
}

void sippymodem_recording_files_destroy(
	struct sippymodem_recording_files *files)
{
	if (files == NULL)
		return;
	close_descriptors(files);
	free(files);
}

bool sippymodem_recording_files_path(
	const struct sippymodem_recording_files *files,
	enum sippymodem_recording_stream stream, char *path, size_t capacity)
{
	return files != NULL &&
	       build_path(files->prefix, stream, path, capacity);
}
