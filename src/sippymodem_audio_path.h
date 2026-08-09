// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_AUDIO_PATH_H
#define SIPPYMODEM_AUDIO_PATH_H

#include "sippymodem_config.h"
#include "sippymodem_session.h"

#include <stddef.h>
#include <stdint.h>

enum sippymodem_modem_core_result {
	SIPPYMODEM_MODEM_CORE_OK,
	SIPPYMODEM_MODEM_CORE_STOPPED,
	SIPPYMODEM_MODEM_CORE_ERROR,
};

struct sippymodem_modem_core {
	void *context;
	enum sippymodem_modem_core_result (*process)(
		void *context,
		const int16_t *receive_samples,
		int16_t *transmit_samples,
		size_t count);
};

enum sippymodem_audio_path_result {
	SIPPYMODEM_AUDIO_PATH_OK,
	SIPPYMODEM_AUDIO_PATH_MODEM_STOPPED,
	SIPPYMODEM_AUDIO_PATH_INVALID_ARGUMENT,
	SIPPYMODEM_AUDIO_PATH_PLAYOUT_ERROR,
	SIPPYMODEM_AUDIO_PATH_MODEM_ERROR,
	SIPPYMODEM_AUDIO_PATH_TRANSMIT_ERROR,
};

struct sippymodem_audio_path;

struct sippymodem_audio_path *sippymodem_audio_path_create(
	struct sippymodem_session *session,
	const struct sippymodem_media_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_modem_core *modem_core);
void sippymodem_audio_path_destroy(struct sippymodem_audio_path *path);

size_t sippymodem_audio_path_frame_samples(
	const struct sippymodem_audio_path *path);
enum sippymodem_audio_path_result sippymodem_audio_path_process_frame(
	struct sippymodem_audio_path *path);

enum sippymodem_media_result sippymodem_audio_path_last_media_result(
	const struct sippymodem_audio_path *path);
enum sippymodem_rtp_endpoint_result sippymodem_audio_path_last_rtp_result(
	const struct sippymodem_audio_path *path);
const char *sippymodem_audio_path_result_name(
	enum sippymodem_audio_path_result result);

#endif
