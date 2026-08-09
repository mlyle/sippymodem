// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_host.h"

#include "sippymodem_media.h"

#include <stdbool.h>
#include <stdlib.h>

struct sippymodem_host {
	struct sippymodem_session *session;
	struct sippymodem_audio_path *audio_path;
	struct sippymodem_audio_scheduler audio_scheduler;
};

static void synchronize_audio(struct sippymodem_host *host)
{
	bool active = sippymodem_session_is_open(host->session) &&
		      sippymodem_session_media_is_configured(host->session);

	sippymodem_audio_scheduler_set_active(&host->audio_scheduler, active);
}

struct sippymodem_host *sippymodem_host_create(
	const struct sippymodem_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_sip_signaling_callbacks *callbacks,
	const struct sippymodem_modem_core *modem_core)
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
	host->audio_path = sippymodem_audio_path_create(
		host->session, &config->media, recorder, modem_core);
	if (host->audio_path == NULL ||
	    !sippymodem_audio_scheduler_init(
		    &host->audio_scheduler, host->audio_path,
		    SIPPYMODEM_MEDIA_SAMPLE_RATE,
		    SIPPYMODEM_HOST_MAXIMUM_CATCHUP_FRAMES))
		goto fail;
	return host;

fail:
	sippymodem_audio_path_destroy(host->audio_path);
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
	sippymodem_audio_path_destroy(host->audio_path);
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
	synchronize_audio(host);
	return result;
}

enum sippymodem_session_result sippymodem_host_close(
	struct sippymodem_host *host)
{
	if (host == NULL)
		return SIPPYMODEM_SESSION_INVALID_ARGUMENT;
	sippymodem_audio_scheduler_set_active(&host->audio_scheduler, false);
	return sippymodem_session_close(host->session);
}

enum sippymodem_sip_signaling_result sippymodem_host_dial(
	struct sippymodem_host *host, const char *target)
{
	enum sippymodem_sip_signaling_result result;

	if (host == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_dial(host->session, target);
	synchronize_audio(host);
	return result;
}

enum sippymodem_sip_signaling_result sippymodem_host_answer(
	struct sippymodem_host *host)
{
	enum sippymodem_sip_signaling_result result;

	if (host == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_answer(host->session);
	synchronize_audio(host);
	return result;
}

enum sippymodem_sip_signaling_result sippymodem_host_hangup(
	struct sippymodem_host *host)
{
	enum sippymodem_sip_signaling_result result;

	if (host == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	result = sippymodem_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

size_t sippymodem_host_frame_samples(const struct sippymodem_host *host)
{
	return host != NULL ?
		       sippymodem_audio_path_frame_samples(host->audio_path) :
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
	return host != NULL && sippymodem_audio_scheduler_is_active(
			       &host->audio_scheduler);
}
