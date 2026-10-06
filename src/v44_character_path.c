// SPDX-License-Identifier: BSD-3-Clause

#include "v44_character_path.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum character_path_direction_state {
	CHARACTER_PATH_READY = 0,
	CHARACTER_PATH_IO_FAILED,
	CHARACTER_PATH_CODEC_FAILED,
};

struct v44_character_path {
	struct v44_duplex *codec;
	struct v44_duplex_config codec_config;
	void *context;
	v44_character_read read;
	v44_character_write write;
	enum character_path_direction_state transmit_state;
	enum character_path_direction_state receive_state;
	bool transmit_has_octet;
	bool transmit_dirty;
	bool transmit_flushing;
	bool receive_finished;
	uint8_t transmit_octet;
	size_t receive_capacity;
	size_t receive_offset;
	size_t receive_length;
	uint8_t receive_buffer[];
};

static enum v44_character_path_result failed_result(
	enum character_path_direction_state state)
{
	return state == CHARACTER_PATH_IO_FAILED ?
		       V44_CHARACTER_PATH_IO_ERROR :
		       V44_CHARACTER_PATH_CODEC_ERROR;
}

bool v44_character_path_transmit_idle(const struct v44_character_path *path)
{
	return path != NULL && path->transmit_state == CHARACTER_PATH_READY &&
	       !path->transmit_has_octet && !path->transmit_dirty &&
	       !path->transmit_flushing;
}

struct v44_character_path *v44_character_path_create(
	const struct v44_character_path_config *config)
{
	struct v44_character_path *path;

	if (config == NULL || config->read == NULL || config->write == NULL ||
	    config->receive_buffer_capacity == 0 ||
	    config->receive_buffer_capacity > SIZE_MAX - sizeof(*path))
		return NULL;
	path = calloc(1, sizeof(*path) + config->receive_buffer_capacity);
	if (path == NULL)
		return NULL;
	path->codec = v44_duplex_create(&config->codec);
	if (path->codec == NULL) {
		free(path);
		return NULL;
	}
	path->codec_config = config->codec;
	path->context = config->context;
	path->read = config->read;
	path->write = config->write;
	path->receive_capacity = config->receive_buffer_capacity;
	return path;
}

void v44_character_path_destroy(
	struct v44_character_path *path)
{
	if (path == NULL)
		return;
	v44_duplex_destroy(path->codec);
	free(path);
}

enum v44_character_path_result
v44_character_path_reinitialize(
	struct v44_character_path *path)
{
	struct v44_duplex *replacement;

	if (path == NULL)
		return V44_CHARACTER_PATH_INVALID_ARGUMENT;
	replacement = v44_duplex_create(&path->codec_config);
	if (replacement == NULL)
		return V44_CHARACTER_PATH_CODEC_ERROR;
	v44_duplex_destroy(path->codec);
	path->codec = replacement;
	path->transmit_state = CHARACTER_PATH_READY;
	path->receive_state = CHARACTER_PATH_READY;
	path->transmit_has_octet = false;
	path->transmit_dirty = false;
	path->transmit_flushing = false;
	path->receive_finished = false;
	path->transmit_octet = 0;
	path->receive_offset = 0;
	path->receive_length = 0;
	return V44_CHARACTER_PATH_OK;
}

static enum v44_character_path_result fail_transmit(
	struct v44_character_path *path,
	enum character_path_direction_state state)
{
	path->transmit_state = state;
	return failed_result(state);
}

static enum v44_character_path_result flush_transmit(
	struct v44_character_path *path,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	size_t length = 0;
	enum v44_duplex_result result;

	path->transmit_flushing = true;
	result = v44_duplex_flush_transmit(
		path->codec, output, output_capacity, &length);
	if (length > output_capacity)
		return fail_transmit(path, CHARACTER_PATH_CODEC_FAILED);
	*output_length += length;
	if (result == V44_DUPLEX_NEED_OUTPUT)
		return V44_CHARACTER_PATH_OK;
	if (result != V44_DUPLEX_FLUSHED)
		return fail_transmit(path, CHARACTER_PATH_CODEC_FAILED);
	path->transmit_flushing = false;
	path->transmit_dirty = false;
	return V44_CHARACTER_PATH_OK;
}

enum v44_character_path_result
v44_character_path_transmit(
	struct v44_character_path *path,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (path == NULL || output_length == NULL ||
	    (output_capacity != 0 && output == NULL))
		return V44_CHARACTER_PATH_INVALID_ARGUMENT;
	*output_length = 0;
	if (path->transmit_state != CHARACTER_PATH_READY)
		return failed_result(path->transmit_state);
	if (path->transmit_flushing)
		return flush_transmit(path, output, output_capacity,
				      output_length);

	for (;;) {
		const uint8_t *input;
		size_t input_length;
		size_t length = 0;
		size_t available = output_capacity - *output_length;
		uint8_t *next_output = available == 0 ?
				       NULL : output + *output_length;
		enum v44_duplex_result result;

		if (!path->transmit_has_octet) {
			int read_result = path->read(
				path->context, &path->transmit_octet, 1);

			if (read_result < 0 || read_result > 1)
				return fail_transmit(
					path, CHARACTER_PATH_IO_FAILED);
			if (read_result == 0) {
				if (!path->transmit_dirty)
					return *output_length == 0 ?
						       V44_CHARACTER_PATH_IDLE :
						       V44_CHARACTER_PATH_OK;
				return flush_transmit(
					path, next_output, available,
					output_length);
			}
			path->transmit_has_octet = true;
		}

		input = &path->transmit_octet;
		input_length = 1;
		result = v44_duplex_transmit(
			path->codec, &input, &input_length, next_output,
			available, &length);
		if (length > available || input_length > 1 ||
		    (input_length == 1 && input != &path->transmit_octet) ||
		    (input_length == 0 && input != &path->transmit_octet + 1))
			return fail_transmit(
				path, CHARACTER_PATH_CODEC_FAILED);
		*output_length += length;
		if (input_length == 0) {
			path->transmit_has_octet = false;
			path->transmit_dirty = true;
		}
		if (result == V44_DUPLEX_NEED_OUTPUT)
			return V44_CHARACTER_PATH_OK;
		if (result != V44_DUPLEX_NEED_INPUT ||
		    input_length != 0)
			return fail_transmit(
				path, CHARACTER_PATH_CODEC_FAILED);
		if (*output_length == output_capacity)
			return V44_CHARACTER_PATH_OK;
	}
}

static enum v44_character_path_result fail_receive(
	struct v44_character_path *path,
	enum character_path_direction_state state)
{
	path->receive_state = state;
	return failed_result(state);
}

static enum v44_character_path_result drain_receive(
	struct v44_character_path *path)
{
	while (path->receive_length != 0) {
		int write_result = path->write(
			path->context,
			path->receive_buffer + path->receive_offset,
			path->receive_length);

		if (write_result < 0 ||
		    (size_t)write_result > path->receive_length)
			return fail_receive(path, CHARACTER_PATH_IO_FAILED);
		if (write_result == 0)
			return V44_CHARACTER_PATH_NEED_OUTPUT;
		path->receive_offset += (size_t)write_result;
		path->receive_length -= (size_t)write_result;
	}
	path->receive_offset = 0;
	return V44_CHARACTER_PATH_OK;
}

enum v44_character_path_result
v44_character_path_receive(
	struct v44_character_path *path,
	const uint8_t *input,
	size_t input_length,
	size_t *input_consumed)
{
	const uint8_t *cursor = input;
	size_t remaining = input_length;
	enum v44_character_path_result drain_result;

	if (path == NULL || input_consumed == NULL ||
	    (input_length != 0 && input == NULL))
		return V44_CHARACTER_PATH_INVALID_ARGUMENT;
	*input_consumed = 0;
	if (path->receive_state != CHARACTER_PATH_READY)
		return failed_result(path->receive_state);
	if (path->receive_finished)
		return V44_CHARACTER_PATH_CODEC_ERROR;
	drain_result = drain_receive(path);
	if (drain_result != V44_CHARACTER_PATH_OK)
		return drain_result;

	for (;;) {
		const uint8_t *previous_cursor = cursor;
		size_t previous_remaining = remaining;
		size_t decoded = 0;
		enum v44_duplex_result result =
			v44_duplex_receive(
				path->codec, &cursor, &remaining,
				path->receive_buffer,
				path->receive_capacity, &decoded);
		size_t consumed;

		if (remaining > previous_remaining ||
		    decoded > path->receive_capacity)
			return fail_receive(
				path, CHARACTER_PATH_CODEC_FAILED);
		consumed = previous_remaining - remaining;
		if ((consumed == 0 && cursor != previous_cursor) ||
		    (consumed != 0 &&
		     (previous_cursor == NULL ||
		      cursor != previous_cursor + consumed)))
			return fail_receive(
				path, CHARACTER_PATH_CODEC_FAILED);
		*input_consumed = input_length - remaining;
		path->receive_offset = 0;
		path->receive_length = decoded;
		drain_result = drain_receive(path);
		if (drain_result != V44_CHARACTER_PATH_OK)
			return drain_result;

		switch (result) {
		case V44_DUPLEX_NEED_INPUT:
			if (remaining != 0)
				return fail_receive(
					path, CHARACTER_PATH_CODEC_FAILED);
			return V44_CHARACTER_PATH_OK;
		case V44_DUPLEX_NEED_OUTPUT:
			if (consumed == 0 && decoded == 0)
				return fail_receive(
					path, CHARACTER_PATH_CODEC_FAILED);
			break;
		case V44_DUPLEX_FLUSHED:
			if (remaining == 0)
				return V44_CHARACTER_PATH_OK;
			break;
		case V44_DUPLEX_OK:
			if (consumed == 0 && decoded == 0)
				return fail_receive(
					path, CHARACTER_PATH_CODEC_FAILED);
			break;
		case V44_DUPLEX_ENTER_PARAMETER:
		case V44_DUPLEX_INVALID_ARGUMENT:
		case V44_DUPLEX_INVALID_STATE:
		case V44_DUPLEX_PROCEDURAL_ERROR:
			return fail_receive(
				path, CHARACTER_PATH_CODEC_FAILED);
		default:
			return fail_receive(
				path, CHARACTER_PATH_CODEC_FAILED);
		}
	}
}

enum v44_character_path_result
v44_character_path_finish_receive(
	struct v44_character_path *path)
{
	if (path == NULL)
		return V44_CHARACTER_PATH_INVALID_ARGUMENT;
	if (path->receive_state != CHARACTER_PATH_READY)
		return failed_result(path->receive_state);
	if (path->receive_finished)
		return V44_CHARACTER_PATH_OK;

	for (;;) {
		enum v44_character_path_result drain_result =
			drain_receive(path);
		enum v44_duplex_result result;

		if (drain_result != V44_CHARACTER_PATH_OK)
			return drain_result;
		result = v44_duplex_finish_receive(path->codec);
		if (result == V44_DUPLEX_OK) {
			path->receive_finished = true;
			return V44_CHARACTER_PATH_OK;
		}
		if (result == V44_DUPLEX_NEED_OUTPUT) {
			size_t consumed;
			enum v44_character_path_result receive_result =
				v44_character_path_receive(
					path, NULL, 0, &consumed);

			if (receive_result != V44_CHARACTER_PATH_OK)
				return receive_result;
			if (consumed != 0)
				return fail_receive(
					path, CHARACTER_PATH_CODEC_FAILED);
			continue;
		}
		return fail_receive(path, CHARACTER_PATH_CODEC_FAILED);
	}
}

const char *v44_character_path_result_name(
	enum v44_character_path_result result)
{
	switch (result) {
	case V44_CHARACTER_PATH_OK:
		return "ok";
	case V44_CHARACTER_PATH_IDLE:
		return "idle";
	case V44_CHARACTER_PATH_NEED_OUTPUT:
		return "need output";
	case V44_CHARACTER_PATH_IO_ERROR:
		return "I/O error";
	case V44_CHARACTER_PATH_CODEC_ERROR:
		return "codec error";
	case V44_CHARACTER_PATH_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
