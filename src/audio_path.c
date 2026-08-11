// SPDX-License-Identifier: BSD-3-Clause

#include "audio_path.h"

#include "media.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct audio_path {
	struct sip_session *session;
	struct media_recorder recorder;
	struct modem_core modem_core;
	double receive_gain;
	double transmit_gain;
	size_t frame_samples;
	enum media_result last_media_result;
	enum rtp_endpoint_result last_rtp_result;
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

struct audio_path *audio_path_create(
	struct sip_session *session,
	const struct media_config *config,
	const struct media_recorder *recorder,
	const struct modem_core *modem_core)
{
	struct audio_path *path;
	size_t frame_samples;
	size_t allocation_size;

	if (session == NULL || config == NULL || modem_core == NULL ||
	    modem_core->process == NULL ||
	    !gain_is_valid(config->receive_gain) ||
	    !gain_is_valid(config->transmit_gain))
		return NULL;
	frame_samples = sip_session_frame_samples(session);
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

void audio_path_destroy(struct audio_path *path)
{
	free(path);
}

size_t audio_path_frame_samples(
	const struct audio_path *path)
{
	return path != NULL ? path->frame_samples : 0;
}

enum audio_path_result audio_path_process_frame(
	struct audio_path *path)
{
	int16_t *receive_samples;
	int16_t *transmit_samples;
	enum modem_core_result core_result;

	if (path == NULL)
		return AUDIO_PATH_INVALID_ARGUMENT;
	receive_samples = path->samples;
	transmit_samples = path->samples + path->frame_samples;
	path->last_media_result = sip_session_play(
		path->session, receive_samples, path->frame_samples, NULL);
	if (path->last_media_result != MEDIA_OK)
		return AUDIO_PATH_PLAYOUT_ERROR;

	apply_gain(receive_samples, path->frame_samples, path->receive_gain);
	recording_emit(
		&path->recorder, RECORD_RX_MODEM_PCM,
		RECORD_S16_NATIVE, MEDIA_SAMPLE_RATE,
		receive_samples, path->frame_samples);
	memset(transmit_samples, 0,
	       path->frame_samples * sizeof(transmit_samples[0]));
	core_result = path->modem_core.process(
		path->modem_core.context, receive_samples, transmit_samples,
		path->frame_samples);
	if (core_result == MODEM_CORE_STOPPED)
		return AUDIO_PATH_MODEM_STOPPED;
	if (core_result != MODEM_CORE_OK)
		return AUDIO_PATH_MODEM_ERROR;

	recording_emit(
		&path->recorder, RECORD_TX_MODEM_PCM,
		RECORD_S16_NATIVE, MEDIA_SAMPLE_RATE,
		transmit_samples, path->frame_samples);
	apply_gain(transmit_samples, path->frame_samples, path->transmit_gain);
	path->last_rtp_result = sip_session_send(
		path->session, transmit_samples, path->frame_samples);
	return path->last_rtp_result == RTP_ENDPOINT_OK ?
		       AUDIO_PATH_OK :
		       AUDIO_PATH_TRANSMIT_ERROR;
}

enum media_result audio_path_last_media_result(
	const struct audio_path *path)
{
	return path != NULL ? path->last_media_result :
			      MEDIA_INVALID_ARGUMENT;
}

enum rtp_endpoint_result audio_path_last_rtp_result(
	const struct audio_path *path)
{
	return path != NULL ? path->last_rtp_result :
			      RTP_ENDPOINT_INVALID_ARGUMENT;
}

const char *audio_path_result_name(
	enum audio_path_result result)
{
	switch (result) {
	case AUDIO_PATH_OK:
		return "ok";
	case AUDIO_PATH_MODEM_STOPPED:
		return "modem core stopped";
	case AUDIO_PATH_INVALID_ARGUMENT:
		return "invalid argument";
	case AUDIO_PATH_PLAYOUT_ERROR:
		return "RTP playout failed";
	case AUDIO_PATH_MODEM_ERROR:
		return "modem core failed";
	case AUDIO_PATH_TRANSMIT_ERROR:
		return "RTP transmit failed";
	}
	return "unknown audio path result";
}
