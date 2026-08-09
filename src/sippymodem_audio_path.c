// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_audio_path.h"

#include "sippymodem_media.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct sippymodem_audio_path {
	struct sippymodem_session *session;
	struct sippymodem_recorder recorder;
	struct sippymodem_modem_core modem_core;
	double receive_gain;
	double transmit_gain;
	size_t frame_samples;
	enum sippymodem_media_result last_media_result;
	enum sippymodem_rtp_endpoint_result last_rtp_result;
	int16_t samples[];
};

static bool gain_is_valid(double gain)
{
	return isfinite(gain) && gain >= 0.0 && gain <= 4.0;
}

static int16_t apply_gain_to_sample(int16_t sample, double gain)
{
	double scaled = (double)sample * gain;

	if (scaled >= INT16_MAX)
		return INT16_MAX;
	if (scaled <= INT16_MIN)
		return INT16_MIN;
	return (int16_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

static void apply_gain(int16_t *samples, size_t count, double gain)
{
	if (gain == 1.0)
		return;
	for (size_t index = 0; index < count; ++index)
		samples[index] = apply_gain_to_sample(samples[index], gain);
}

struct sippymodem_audio_path *sippymodem_audio_path_create(
	struct sippymodem_session *session,
	const struct sippymodem_media_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_modem_core *modem_core)
{
	struct sippymodem_audio_path *path;
	size_t frame_samples;
	size_t allocation_size;

	if (session == NULL || config == NULL || modem_core == NULL ||
	    modem_core->process == NULL ||
	    !gain_is_valid(config->receive_gain) ||
	    !gain_is_valid(config->transmit_gain))
		return NULL;
	frame_samples = sippymodem_session_frame_samples(session);
	if (frame_samples == 0 ||
	    frame_samples >
		    (SIZE_MAX - sizeof(*path)) / (2 * sizeof(int16_t)))
		return NULL;
	allocation_size =
		sizeof(*path) + 2 * frame_samples * sizeof(int16_t);
	path = calloc(1, allocation_size);
	if (path == NULL)
		return NULL;

	path->session = session;
	path->modem_core = *modem_core;
	path->receive_gain = config->receive_gain;
	path->transmit_gain = config->transmit_gain;
	path->frame_samples = frame_samples;
	if (recorder != NULL)
		path->recorder = *recorder;
	return path;
}

void sippymodem_audio_path_destroy(struct sippymodem_audio_path *path)
{
	free(path);
}

size_t sippymodem_audio_path_frame_samples(
	const struct sippymodem_audio_path *path)
{
	return path != NULL ? path->frame_samples : 0;
}

enum sippymodem_audio_path_result sippymodem_audio_path_process_frame(
	struct sippymodem_audio_path *path)
{
	int16_t *receive_samples;
	int16_t *transmit_samples;
	enum sippymodem_modem_core_result core_result;

	if (path == NULL)
		return SIPPYMODEM_AUDIO_PATH_INVALID_ARGUMENT;
	receive_samples = path->samples;
	transmit_samples = path->samples + path->frame_samples;
	path->last_media_result = sippymodem_session_play(
		path->session, receive_samples, path->frame_samples, NULL);
	if (path->last_media_result != SIPPYMODEM_MEDIA_OK)
		return SIPPYMODEM_AUDIO_PATH_PLAYOUT_ERROR;

	apply_gain(receive_samples, path->frame_samples, path->receive_gain);
	sippymodem_recording_emit(
		&path->recorder, SIPPYMODEM_RECORD_RX_MODEM_PCM,
		SIPPYMODEM_RECORD_S16_NATIVE, SIPPYMODEM_MEDIA_SAMPLE_RATE,
		receive_samples, path->frame_samples);
	memset(transmit_samples, 0,
	       path->frame_samples * sizeof(transmit_samples[0]));
	core_result = path->modem_core.process(
		path->modem_core.context, receive_samples, transmit_samples,
		path->frame_samples);
	if (core_result == SIPPYMODEM_MODEM_CORE_STOPPED)
		return SIPPYMODEM_AUDIO_PATH_MODEM_STOPPED;
	if (core_result != SIPPYMODEM_MODEM_CORE_OK)
		return SIPPYMODEM_AUDIO_PATH_MODEM_ERROR;

	sippymodem_recording_emit(
		&path->recorder, SIPPYMODEM_RECORD_TX_MODEM_PCM,
		SIPPYMODEM_RECORD_S16_NATIVE, SIPPYMODEM_MEDIA_SAMPLE_RATE,
		transmit_samples, path->frame_samples);
	apply_gain(transmit_samples, path->frame_samples, path->transmit_gain);
	path->last_rtp_result = sippymodem_session_send(
		path->session, transmit_samples, path->frame_samples);
	return path->last_rtp_result == SIPPYMODEM_RTP_ENDPOINT_OK ?
		       SIPPYMODEM_AUDIO_PATH_OK :
		       SIPPYMODEM_AUDIO_PATH_TRANSMIT_ERROR;
}

enum sippymodem_media_result sippymodem_audio_path_last_media_result(
	const struct sippymodem_audio_path *path)
{
	return path != NULL ? path->last_media_result :
			      SIPPYMODEM_MEDIA_INVALID_ARGUMENT;
}

enum sippymodem_rtp_endpoint_result sippymodem_audio_path_last_rtp_result(
	const struct sippymodem_audio_path *path)
{
	return path != NULL ? path->last_rtp_result :
			      SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT;
}

const char *sippymodem_audio_path_result_name(
	enum sippymodem_audio_path_result result)
{
	switch (result) {
	case SIPPYMODEM_AUDIO_PATH_OK:
		return "ok";
	case SIPPYMODEM_AUDIO_PATH_MODEM_STOPPED:
		return "modem core stopped";
	case SIPPYMODEM_AUDIO_PATH_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_AUDIO_PATH_PLAYOUT_ERROR:
		return "RTP playout failed";
	case SIPPYMODEM_AUDIO_PATH_MODEM_ERROR:
		return "modem core failed";
	case SIPPYMODEM_AUDIO_PATH_TRANSMIT_ERROR:
		return "RTP transmit failed";
	}
	return "unknown audio path result";
}
