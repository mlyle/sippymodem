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
	enum g711_law law;
	enum media_result last_media_result;
	enum rtp_endpoint_result last_rtp_result;
	int16_t samples[];
};

static bool gain_is_valid(double gain)
{
	return isfinite(gain) && gain >= 0.0 && gain <= 4.0;
}

static bool codec_law(enum media_codec codec, enum g711_law *law)
{
	if (law == NULL)
		return false;
	if (codec == MEDIA_CODEC_PCMU)
		*law = G711_LAW_PCMU;
	else if (codec == MEDIA_CODEC_PCMA)
		*law = G711_LAW_PCMA;
	else
		return false;
	return true;
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
	enum g711_law law;

	if (session == NULL || config == NULL || modem_core == NULL ||
	    modem_core->process == NULL ||
	    !codec_law(config->codec, &law) ||
	    !gain_is_valid(config->receive_gain) ||
	    !gain_is_valid(config->transmit_gain))
		return NULL;
	frame_samples = sip_session_frame_samples(session);
	if (frame_samples == 0 ||
	    frame_samples >
		    (SIZE_MAX - sizeof(*path)) /
			    (2 * sizeof(int16_t) + 2 * sizeof(uint8_t) + sizeof(bool)))
		return NULL;
	allocation_size =
		sizeof(*path) +
		frame_samples * (2 * sizeof(int16_t) + 2 * sizeof(uint8_t) + sizeof(bool));
	path = calloc(1, allocation_size);
	if (path == NULL)
		return NULL;

	path->session = session;
	path->modem_core = *modem_core;
	path->receive_gain = config->receive_gain;
	path->transmit_gain = config->transmit_gain;
	path->frame_samples = frame_samples;
	path->law = law;
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
	uint8_t *receive_codewords;
	uint8_t *transmit_codewords;
	bool *received_mask;
	enum modem_core_result core_result;
	bool exact_output = false;

	if (path == NULL)
		return AUDIO_PATH_INVALID_ARGUMENT;
	receive_samples = path->samples;
	transmit_samples = path->samples + path->frame_samples;
	receive_codewords = (uint8_t *)(transmit_samples + path->frame_samples);
	transmit_codewords = receive_codewords + path->frame_samples;
	received_mask = (bool *)(transmit_codewords + path->frame_samples);
	path->last_media_result = sip_session_play(
		path->session, receive_samples, receive_codewords, received_mask,
		path->frame_samples, NULL);
	if (path->last_media_result != MEDIA_OK)
		return AUDIO_PATH_PLAYOUT_ERROR;

	apply_gain(receive_samples, path->frame_samples, path->receive_gain);
	recording_emit(
		&path->recorder, RECORD_RX_MODEM_PCM,
		RECORD_S16_NATIVE, MEDIA_SAMPLE_RATE,
		receive_samples, path->frame_samples);
	memset(transmit_samples, 0,
	       path->frame_samples * sizeof(transmit_samples[0]));
	memset(transmit_codewords,
	       path->law == G711_LAW_PCMA ?
		       g711_encode_pcma(0) : g711_encode_pcmu(0),
	       path->frame_samples);
	if (path->modem_core.process_g711 != NULL)
		core_result = path->modem_core.process_g711(
			path->modem_core.context, path->law, receive_codewords,
			receive_samples, received_mask, transmit_codewords, transmit_samples,
			path->frame_samples, &exact_output);
	else
		core_result = path->modem_core.process(
			path->modem_core.context, receive_samples, transmit_samples,
			path->frame_samples);
	if (core_result == MODEM_CORE_STOPPED)
		return AUDIO_PATH_MODEM_STOPPED;
	if (core_result != MODEM_CORE_OK)
		return AUDIO_PATH_MODEM_ERROR;
	if (exact_output) {
		if (path->transmit_gain != 1.0)
			return AUDIO_PATH_EXACT_OUTPUT_CONFLICT;
		for (size_t index = 0; index < path->frame_samples; ++index) {
			transmit_samples[index] = path->law == G711_LAW_PCMA ?
				g711_decode_pcma(transmit_codewords[index]) :
				g711_decode_pcmu(transmit_codewords[index]);
		}
	}

	recording_emit(
		&path->recorder, RECORD_TX_MODEM_PCM,
		RECORD_S16_NATIVE, MEDIA_SAMPLE_RATE,
		transmit_samples, path->frame_samples);
	if (exact_output)
		path->last_rtp_result = sip_session_send_g711(
			path->session, transmit_codewords, transmit_samples,
			path->frame_samples);
	else {
		apply_gain(
			transmit_samples, path->frame_samples, path->transmit_gain);
		path->last_rtp_result = sip_session_send(
			path->session, transmit_samples, path->frame_samples);
	}
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
	case AUDIO_PATH_EXACT_OUTPUT_CONFLICT:
		return "exact G.711 output conflicts with transmit gain";
	case AUDIO_PATH_TRANSMIT_ERROR:
		return "RTP transmit failed";
	}
	return "unknown audio path result";
}
