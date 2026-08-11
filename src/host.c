// SPDX-License-Identifier: BSD-3-Clause

#include "host.h"

#include "media.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct sippymodem_host {
	struct sippymodem_session *session;
	struct audio_path *audio_path;
	struct audio_scheduler audio_scheduler;
	enum sippymodem_session_result last_session_result;
	enum audio_scheduler_result last_audio_scheduler_result;
	bool media_was_configured;
	bool audio_suspended;
};

static void synchronize_audio(struct sippymodem_host *host)
{
	bool media_configured = sippymodem_session_is_open(host->session) &&
				sippymodem_session_media_is_configured(
					host->session);

	if (!media_configured || !host->media_was_configured)
		host->audio_suspended = false;
	audio_scheduler_set_active(
		&host->audio_scheduler,
		media_configured && !host->audio_suspended);
	host->media_was_configured = media_configured;
}

struct sippymodem_host *sippymodem_host_create(
	const struct sippymodem_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *modem_core)
{
	struct sippymodem_host *host;

	if (config == NULL || modem_core == NULL || modem_core->process == NULL)
		return NULL;
	host = calloc(1, sizeof(*host));
	if (host == NULL)
		return NULL;
	host->session = sippymodem_session_create(config, recorder, callbacks);
	if (host->session == NULL)
		goto fail;
	host->audio_path = audio_path_create(
		host->session, &config->media, recorder, modem_core);
	if (host->audio_path == NULL ||
	    !audio_scheduler_init(
		    &host->audio_scheduler, host->audio_path,
		    MEDIA_SAMPLE_RATE,
		    SIPPYMODEM_HOST_MAXIMUM_CATCHUP_FRAMES))
		goto fail;
	return host;

fail:
	audio_path_destroy(host->audio_path);
	sippymodem_session_destroy(host->session);
	free(host);
	return NULL;
}

void sippymodem_host_destroy(struct sippymodem_host *host)
{
	if (host == NULL)
		return;
	if (sippymodem_session_is_open(host->session))
		sippymodem_host_close(host);
	audio_path_destroy(host->audio_path);
	sippymodem_session_destroy(host->session);
	free(host);
}

enum sippymodem_session_result sippymodem_host_open(
	struct sippymodem_host *host)
{
	enum sippymodem_session_result result;

	if (host == NULL)
		return SIPPYMODEM_SESSION_INVALID_ARGUMENT;
	result = sippymodem_session_open(host->session);
	host->last_session_result = result;
	synchronize_audio(host);
	return result;
}

enum sippymodem_session_result sippymodem_host_close(
	struct sippymodem_host *host)
{
	if (host == NULL)
		return SIPPYMODEM_SESSION_INVALID_ARGUMENT;
	audio_scheduler_set_active(&host->audio_scheduler, false);
	host->media_was_configured = false;
	host->audio_suspended = false;
	host->last_session_result = sippymodem_session_close(host->session);
	return host->last_session_result;
}

enum sip_signaling_result sippymodem_host_dial(
	struct sippymodem_host *host, const char *target)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_dial(host->session, target);
	synchronize_audio(host);
	return result;
}

static enum sippymodem_host_result suspend_failed_audio(
	struct sippymodem_host *host, enum sippymodem_host_result result)
{
	host->audio_suspended = true;
	audio_scheduler_set_active(&host->audio_scheduler, false);
	sippymodem_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

enum sippymodem_host_result sippymodem_host_step(
	struct sippymodem_host *host, uint64_t now_ns,
	size_t *datagrams_received, size_t *frames_processed)
{
	size_t datagrams = 0;
	size_t frames = 0;
	bool binding_failed;

	if (datagrams_received != NULL)
		*datagrams_received = 0;
	if (frames_processed != NULL)
		*frames_processed = 0;
	if (host == NULL)
		return SIPPYMODEM_HOST_INVALID_ARGUMENT;
	if (!sippymodem_session_is_open(host->session)) {
		synchronize_audio(host);
		return SIPPYMODEM_HOST_IDLE;
	}

	host->last_session_result =
		sippymodem_session_step(host->session, 0, &datagrams);
	if (datagrams_received != NULL)
		*datagrams_received = datagrams;
	synchronize_audio(host);
	binding_failed = host->last_session_result ==
			 SIPPYMODEM_SESSION_MEDIA_BINDING_ERROR;
	if (host->last_session_result != SIPPYMODEM_SESSION_OK &&
	    !binding_failed)
		return SIPPYMODEM_HOST_SESSION_ERROR;

	host->last_audio_scheduler_result = audio_scheduler_step(
		&host->audio_scheduler, now_ns, &frames);
	if (frames_processed != NULL)
		*frames_processed = frames;
	if (host->last_audio_scheduler_result ==
	    AUDIO_SCHEDULER_MODEM_STOPPED)
		return suspend_failed_audio(
			host, SIPPYMODEM_HOST_MODEM_STOPPED);
	if (host->last_audio_scheduler_result !=
		    AUDIO_SCHEDULER_OK &&
	    host->last_audio_scheduler_result !=
		    AUDIO_SCHEDULER_IDLE)
		return suspend_failed_audio(host, SIPPYMODEM_HOST_AUDIO_ERROR);
	if (binding_failed)
		return SIPPYMODEM_HOST_MEDIA_BINDING_ERROR;
	return host->last_audio_scheduler_result ==
			       AUDIO_SCHEDULER_OK ?
		       SIPPYMODEM_HOST_OK :
		       SIPPYMODEM_HOST_IDLE;
}

uint64_t sippymodem_host_wait_ns(
	const struct sippymodem_host *host, uint64_t now_ns)
{
	uint64_t audio_wait;

	if (host == NULL || !sippymodem_session_is_open(host->session))
		return UINT64_MAX;
	audio_wait = audio_scheduler_wait_ns(
		&host->audio_scheduler, now_ns);
	return audio_wait < SIPPYMODEM_HOST_SIGNALING_POLL_NS ?
		       audio_wait :
		       SIPPYMODEM_HOST_SIGNALING_POLL_NS;
}

enum sip_signaling_result sippymodem_host_answer(
	struct sippymodem_host *host)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_answer(host->session);
	synchronize_audio(host);
	return result;
}

enum sip_signaling_result sippymodem_host_hangup(
	struct sippymodem_host *host)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

size_t sippymodem_host_frame_samples(const struct sippymodem_host *host)
{
	return host != NULL ?
		       audio_path_frame_samples(host->audio_path) :
		       0;
}

bool sippymodem_host_is_open(const struct sippymodem_host *host)
{
	return host != NULL && sippymodem_session_is_open(host->session);
}

bool sippymodem_host_media_is_configured(
	const struct sippymodem_host *host)
{
	return host != NULL &&
	       sippymodem_session_media_is_configured(host->session);
}

bool sippymodem_host_audio_is_active(const struct sippymodem_host *host)
{
	return host != NULL && audio_scheduler_is_active(
			       &host->audio_scheduler);
}

enum sippymodem_session_result sippymodem_host_last_session_result(
	const struct sippymodem_host *host)
{
	return host != NULL ? host->last_session_result :
			      SIPPYMODEM_SESSION_INVALID_ARGUMENT;
}

enum audio_scheduler_result
sippymodem_host_last_audio_scheduler_result(
	const struct sippymodem_host *host)
{
	return host != NULL ? host->last_audio_scheduler_result :
			      AUDIO_SCHEDULER_INVALID_ARGUMENT;
}

enum audio_path_result sippymodem_host_last_audio_path_result(
	const struct sippymodem_host *host)
{
	return host != NULL ? host->audio_scheduler.last_path_result :
			      AUDIO_PATH_INVALID_ARGUMENT;
}

const char *sippymodem_host_result_name(enum sippymodem_host_result result)
{
	switch (result) {
	case SIPPYMODEM_HOST_OK:
		return "ok";
	case SIPPYMODEM_HOST_IDLE:
		return "idle";
	case SIPPYMODEM_HOST_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_HOST_SESSION_ERROR:
		return "SIP/RTP session failed";
	case SIPPYMODEM_HOST_MEDIA_BINDING_ERROR:
		return "negotiated media could not be bound";
	case SIPPYMODEM_HOST_AUDIO_ERROR:
		return "scheduled audio failed";
	case SIPPYMODEM_HOST_MODEM_STOPPED:
		return "modem core stopped";
	}
	return "unknown host result";
}
