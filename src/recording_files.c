// SPDX-License-Identifier: BSD-3-Clause

#include "recording_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	RECORDING_STREAM_COUNT =
		RECORD_TX_RTP_PAYLOAD_SENT + 1,
	PCM_CHUNK_SAMPLES = 256,
};

struct recording_files {
	char prefix[RECORDING_PREFIX_MAX];
	int descriptors[RECORDING_STREAM_COUNT];
	struct media_recorder recorder;
	enum recording_files_result last_result;
	int system_error;
	struct recording_files_stats stats;
};

static bool stream_is_valid(enum recording_stream stream)
{
	return stream >= RECORD_RX_RTP_PAYLOAD_ARRIVAL &&
	       stream <= RECORD_TX_RTP_PAYLOAD_SENT;
}

static bool stream_contains_pcm(enum recording_stream stream)
{
	return stream == RECORD_RX_LINE_PCM ||
	       stream == RECORD_RX_MODEM_PCM ||
	       stream == RECORD_TX_MODEM_PCM ||
	       stream == RECORD_TX_LINE_PCM;
}

static bool build_path(
	const char *prefix, enum recording_stream stream,
	char *path, size_t capacity)
{
	const char *name;
	const char *extension;
	int length;

	if (prefix == NULL || prefix[0] == '\0' ||
	    !stream_is_valid(stream) || path == NULL || capacity == 0)
		return false;
	name = recording_stream_name(stream);
	if (name == NULL)
		return false;
	extension = stream_contains_pcm(stream) ? "8000hz.s16le" :
						  "8000hz.octets";
	length = snprintf(path, capacity, "%s.%s.%s", prefix, name, extension);
	return length >= 0 && (size_t)length < capacity;
}

static void add_saturated(uint64_t *value, uint64_t increment)
{
	*value = increment > UINT64_MAX - *value ? UINT64_MAX :
						    *value + increment;
}

static bool write_all(int descriptor, const void *data, size_t size)
{
	const uint8_t *cursor = data;

	while (size != 0) {
		ssize_t written = write(descriptor, cursor, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			if (written == 0)
				errno = EIO;
			return false;
		}
		cursor += (size_t)written;
		size -= (size_t)written;
	}
	return true;
}

static bool write_little_endian_pcm(
	int descriptor, const void *data, size_t count)
{
	const uint8_t *samples = data;
	uint8_t bytes[PCM_CHUNK_SAMPLES * sizeof(int16_t)];

	while (count != 0) {
		size_t chunk = count < PCM_CHUNK_SAMPLES ? count :
						       PCM_CHUNK_SAMPLES;

		for (size_t index = 0; index < chunk; ++index) {
			int16_t native_sample;
			uint16_t sample;

			memcpy(&native_sample, samples + index * sizeof(native_sample),
			       sizeof(native_sample));
			sample = (uint16_t)native_sample;
			bytes[index * 2] = (uint8_t)sample;
			bytes[index * 2 + 1] = (uint8_t)(sample >> 8);
		}
		if (!write_all(descriptor, bytes, chunk * sizeof(int16_t)))
			return false;
		samples += chunk * sizeof(int16_t);
		count -= chunk;
	}
	return true;
}

static void record_block(
	void *context, const struct recording_block *block)
{
	struct recording_files *files = context;
	bool pcm;
	size_t bytes;
	bool written;

	if (files->last_result != RECORDING_FILES_OK)
		return;
	pcm = block != NULL && stream_is_valid(block->stream) &&
	      stream_contains_pcm(block->stream);
	if (block == NULL || !stream_is_valid(block->stream) ||
	    block->format != (pcm ? RECORD_S16_NATIVE :
				   RECORD_OCTETS) ||
	    block->sample_rate != MEDIA_SAMPLE_RATE ||
	    (block->data == NULL && block->count != 0) ||
	    (pcm && block->count > SIZE_MAX / sizeof(int16_t))) {
		files->last_result = RECORDING_FILES_INVALID_BLOCK;
		return;
	}
	if (block->count == 0)
		return;

	bytes = pcm ? block->count * sizeof(int16_t) : block->count;
	written = pcm ?
			  write_little_endian_pcm(
				  files->descriptors[block->stream], block->data,
				  block->count) :
			  write_all(files->descriptors[block->stream],
				    block->data, block->count);
	if (!written) {
		files->last_result = RECORDING_FILES_WRITE_ERROR;
		files->system_error = errno;
		return;
	}
	add_saturated(&files->stats.blocks_written, 1);
	add_saturated(&files->stats.bytes_written, bytes);
}

static void close_descriptors(struct recording_files *files)
{
	for (size_t index = 0; index < RECORDING_STREAM_COUNT; ++index) {
		if (files->descriptors[index] >= 0) {
			close(files->descriptors[index]);
			files->descriptors[index] = -1;
		}
	}
}

static void remove_paths(
	const struct recording_files *files, size_t count)
{
	char path[PATH_MAX];

	for (size_t index = 0; index < count; ++index) {
		if (build_path(files->prefix,
			       (enum recording_stream)index,
			       path, sizeof(path)))
			unlink(path);
	}
}

struct recording_files *recording_files_create(
	const char *prefix)
{
	struct recording_files *files;
	char path[PATH_MAX];
	size_t prefix_length;
	size_t created = 0;
	int failure;

	if (prefix == NULL || prefix[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	prefix_length = strlen(prefix);
	if (prefix_length >= RECORDING_PREFIX_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	files = calloc(1, sizeof(*files));
	if (files == NULL)
		return NULL;
	memcpy(files->prefix, prefix, prefix_length + 1);
	for (size_t index = 0; index < RECORDING_STREAM_COUNT; ++index)
		files->descriptors[index] = -1;
	files->recorder.context = files;
	files->recorder.write = record_block;

	while (created < RECORDING_STREAM_COUNT) {
		if (!build_path(
			    files->prefix,
			    (enum recording_stream)created,
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

void recording_files_destroy(
	struct recording_files *files)
{
	if (files == NULL)
		return;
	close_descriptors(files);
	free(files);
}

const struct media_recorder *recording_files_recorder(
	const struct recording_files *files)
{
	return files != NULL ? &files->recorder : NULL;
}

bool recording_files_path(
	const struct recording_files *files,
	enum recording_stream stream, char *path, size_t capacity)
{
	return files != NULL &&
	       build_path(files->prefix, stream, path, capacity);
}

enum recording_files_result recording_files_last_result(
	const struct recording_files *files)
{
	return files != NULL ? files->last_result :
			       RECORDING_FILES_INVALID_BLOCK;
}

int recording_files_system_error(
	const struct recording_files *files)
{
	return files != NULL ? files->system_error : EINVAL;
}

void recording_files_get_stats(
	const struct recording_files *files,
	struct recording_files_stats *stats)
{
	if (stats == NULL)
		return;
	if (files != NULL)
		*stats = files->stats;
	else
		memset(stats, 0, sizeof(*stats));
}

const char *recording_files_result_name(
	enum recording_files_result result)
{
	switch (result) {
	case RECORDING_FILES_OK:
		return "ok";
	case RECORDING_FILES_INVALID_BLOCK:
		return "recording block does not match its evidence stream";
	case RECORDING_FILES_WRITE_ERROR:
		return "recording write failed";
	}
	return "unknown recording result";
}
