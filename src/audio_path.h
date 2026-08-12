// SPDX-License-Identifier: BSD-3-Clause

#ifndef AUDIO_PATH_H
#define AUDIO_PATH_H

#include "config.h"
#include "g711.h"
#include "session.h"

#include <stddef.h>
#include <stdint.h>

enum modem_core_result {
	MODEM_CORE_OK,
	MODEM_CORE_STOPPED,
	MODEM_CORE_ERROR,
};

struct modem_core {
	void *context;
	/* Both sample buffers are writable scratch storage owned by the caller. */
	enum modem_core_result (*process)(
		void *context,
		int16_t *receive_samples,
		int16_t *transmit_samples,
		size_t count);
	/* Optional exact-codeword path. receive_samples include configured gain;
	 * receive_codewords retain the original G.711 playout values. */
	enum modem_core_result (*process_g711)(
		void *context,
		enum g711_law law,
		const uint8_t *receive_codewords,
		int16_t *receive_samples,
		int16_t *transmit_samples,
		size_t count);
};

enum audio_path_result {
	AUDIO_PATH_OK,
	AUDIO_PATH_MODEM_STOPPED,
	AUDIO_PATH_INVALID_ARGUMENT,
	AUDIO_PATH_PLAYOUT_ERROR,
	AUDIO_PATH_MODEM_ERROR,
	AUDIO_PATH_TRANSMIT_ERROR,
};

struct audio_path;

struct audio_path *audio_path_create(
	struct sip_session *session,
	const struct media_config *config,
	const struct media_recorder *recorder,
	const struct modem_core *modem_core);
void audio_path_destroy(struct audio_path *path);

size_t audio_path_frame_samples(
	const struct audio_path *path);
enum audio_path_result audio_path_process_frame(
	struct audio_path *path);

enum media_result audio_path_last_media_result(
	const struct audio_path *path);
enum rtp_endpoint_result audio_path_last_rtp_result(
	const struct audio_path *path);
const char *audio_path_result_name(
	enum audio_path_result result);

#endif
