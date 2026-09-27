// SPDX-License-Identifier: BSD-3-Clause

#ifndef AUDIO_PATH_H
#define AUDIO_PATH_H

#include "config.h"
#include "g711.h"
#include "session.h"

#include <stdbool.h>
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
	 * receive_codewords retain the original G.711 playout values. The caller
	 * supplies a count-element received_mask: false marks generated playout,
	 * not a received silence symbol. NULL means provenance is unavailable.
	 * All input storage is borrowed only for this call. The caller
	 * initializes exact_output to false. When set true, all transmit_codewords
	 * are authoritative and transmit_samples are replaced by their canonical
	 * decodes before recording and transmission. */
	enum modem_core_result (*process_g711)(
		void *context,
		enum g711_law law,
		const uint8_t *receive_codewords,
		int16_t *receive_samples,
		const bool *received_mask,
		uint8_t *transmit_codewords,
		int16_t *transmit_samples,
		size_t count,
		bool *exact_output);
};

enum audio_path_result {
	AUDIO_PATH_OK,
	AUDIO_PATH_MODEM_STOPPED,
	AUDIO_PATH_INVALID_ARGUMENT,
	AUDIO_PATH_PLAYOUT_ERROR,
	AUDIO_PATH_MODEM_ERROR,
	AUDIO_PATH_EXACT_OUTPUT_CONFLICT,
	AUDIO_PATH_TRANSMIT_ERROR,
};

struct audio_path;

/* Absolute signed-16-bit peaks (0..32768) since the last take. RX is before
 * gain and excludes generated playout; TX is after gain, before G.711 encoding
 * (or the canonical decode of exact PCM codewords). Metering is opt-in. */
struct audio_levels {
	unsigned int receive_peak;
	unsigned int transmit_peak;
};

void audio_path_set_level_metering(struct audio_path *path, bool enabled);
struct audio_levels audio_path_take_levels(struct audio_path *path);

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
