// SPDX-License-Identifier: BSD-3-Clause

#include "host.h"

#include "media.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct modem_host {
	struct sip_session *session;
	struct audio_path *audio_path;
	struct audio_scheduler audio_scheduler;
	enum sip_session_result last_session_result;
	enum audio_scheduler_result last_audio_scheduler_result;
	bool media_was_configured;
	bool audio_suspended;
	bool remote_clock;
	bool remote_seed_pending;
	bool remote_clock_started;
	uint64_t remote_clock_time_ns;
	size_t remote_frames_pending;
};

static void synchronize_audio(struct modem_host *host)
{
	bool media_configured = sip_session_is_open(host->session) &&
				sip_session_media_is_configured(
					host->session);
	bool was_active = audio_scheduler_is_active(
		&host->audio_scheduler);

	if (!media_configured || !host->media_was_configured) {
		host->audio_suspended = false;
		(void)audio_path_take_levels(host->audio_path);
	}
	audio_scheduler_set_active(
		&host->audio_scheduler,
		media_configured && !host->audio_suspended);
	if (host->remote_clock && !was_active &&
	    audio_scheduler_is_active(&host->audio_scheduler))
		host->remote_seed_pending = true;
	if (!audio_scheduler_is_active(&host->audio_scheduler)) {
		host->remote_seed_pending = false;
		host->remote_clock_started = false;
		host->remote_frames_pending = 0;
	}
	host->media_was_configured = media_configured;
}

struct modem_host *modem_host_create_shared(
	struct sip_server *server,
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *modem_core)
{
	struct modem_host *host;

	if (config == NULL || modem_core == NULL || modem_core->process == NULL)
		return NULL;
	host = calloc(1, sizeof(*host));
	if (host == NULL)
		return NULL;
	host->remote_clock = config->media.remote_clock;
	host->session = sip_session_create_shared(server, config, recorder, callbacks);
	if (host->session == NULL)
		goto fail;
	host->audio_path = audio_path_create(
		host->session, &config->media, recorder, modem_core);
	if (host->audio_path == NULL ||
	    !audio_scheduler_init(
		    &host->audio_scheduler, host->audio_path,
		    MEDIA_SAMPLE_RATE,
		    MEDIA_MAX_CATCHUP_FRAMES))
		goto fail;
	audio_path_set_level_metering(host->audio_path, config->sip.trace_enabled);
	return host;

fail:
	audio_path_destroy(host->audio_path);
	sip_session_destroy(host->session);
	free(host);
	return NULL;
}

struct modem_host *modem_host_create(
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *modem_core)
{
	return modem_host_create_shared(NULL, config, recorder, callbacks, modem_core);
}

void modem_host_destroy(struct modem_host *host)
{
	if (host == NULL)
		return;
	if (sip_session_is_open(host->session))
		modem_host_close(host);
	audio_path_destroy(host->audio_path);
	sip_session_destroy(host->session);
	free(host);
}

struct audio_levels modem_host_take_audio_levels(struct modem_host *host)
{
	return audio_path_take_levels(host != NULL ? host->audio_path : NULL);
}

enum sip_session_result modem_host_open(
	struct modem_host *host)
{
	enum sip_session_result result;

	if (host == NULL)
		return SIP_SESSION_INVALID_ARGUMENT;
	result = sip_session_open(host->session);
	host->last_session_result = result;
	synchronize_audio(host);
	return result;
}

enum sip_session_result modem_host_close(
	struct modem_host *host)
{
	if (host == NULL)
		return SIP_SESSION_INVALID_ARGUMENT;
	audio_scheduler_set_active(&host->audio_scheduler, false);
	(void)audio_path_take_levels(host->audio_path);
	host->media_was_configured = false;
	host->audio_suspended = false;
	host->remote_seed_pending = false;
	host->remote_clock_started = false;
	host->remote_frames_pending = 0;
	host->last_session_result = sip_session_close(host->session);
	return host->last_session_result;
}

enum sip_signaling_result modem_host_dial(
	struct modem_host *host, const char *target)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sip_session_dial(host->session, target);
	synchronize_audio(host);
	return result;
}

static enum modem_host_result suspend_failed_audio(
	struct modem_host *host, enum modem_host_result result)
{
	host->audio_suspended = true;
	audio_scheduler_set_active(&host->audio_scheduler, false);
	sip_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

enum modem_host_result modem_host_step(
	struct modem_host *host, uint64_t now_ns,
	size_t *datagrams_received, size_t *frames_processed)
{
	size_t datagrams = 0;
	size_t frames = 0;
	size_t remote_frames;
	bool binding_failed;

	if (datagrams_received != NULL)
		*datagrams_received = 0;
	if (frames_processed != NULL)
		*frames_processed = 0;
	if (host == NULL)
		return MODEM_HOST_INVALID_ARGUMENT;
	if (!sip_session_is_open(host->session)) {
		synchronize_audio(host);
		return MODEM_HOST_IDLE;
	}

	host->last_session_result =
		sip_session_step(host->session, 0, &datagrams);
	if (datagrams_received != NULL)
		*datagrams_received = datagrams;
	synchronize_audio(host);
	remote_frames = sip_session_remote_clock_frames(host->session);
	binding_failed = host->last_session_result ==
			 SIP_SESSION_MEDIA_BINDING_ERROR;
	if (host->last_session_result != SIP_SESSION_OK &&
	    !binding_failed)
		return MODEM_HOST_SESSION_ERROR;

	if (host->remote_clock) {
		if (audio_scheduler_is_active(&host->audio_scheduler)) {
			/* Only accepted RTP clock credit refreshes this deadline.
			 * Otherwise missing media freezes sample-based modem timers,
			 * including carrier loss and disconnect. */
			if (!host->remote_clock_started || remote_frames != 0) {
				host->remote_clock_started = true;
				host->remote_clock_time_ns = now_ns;
			} else if (now_ns >= host->remote_clock_time_ns &&
				   now_ns - host->remote_clock_time_ns >=
					   MODEM_HOST_REMOTE_MEDIA_TIMEOUT_NS) {
				return suspend_failed_audio(
					host, MODEM_HOST_MEDIA_TIMEOUT);
			}
		}
		if (host->remote_seed_pending) {
			host->remote_frames_pending++;
			host->remote_seed_pending = false;
		}
		if (audio_scheduler_is_active(&host->audio_scheduler)) {
			if (remote_frames > SIZE_MAX - host->remote_frames_pending)
				return suspend_failed_audio(host, MODEM_HOST_AUDIO_ERROR);
			host->remote_frames_pending += remote_frames;
		}
		size_t due = host->remote_frames_pending;
		if (due > MEDIA_MAX_CATCHUP_FRAMES)
			due = MEDIA_MAX_CATCHUP_FRAMES;
		/* DSP may synchronously hang up and reset this credit. Debit the
		 * bounded batch before entering that callback; teardown retires it. */
		host->remote_frames_pending -= due;
		host->last_audio_scheduler_result =
			audio_scheduler_step_remote(&host->audio_scheduler, due, &frames);
	} else {
		host->last_audio_scheduler_result = audio_scheduler_step(
			&host->audio_scheduler, now_ns, &frames);
	}
	if (frames_processed != NULL)
		*frames_processed = frames;
	sip_session_trace_media(host->session, now_ns);
	if (host->last_audio_scheduler_result ==
	    AUDIO_SCHEDULER_MODEM_STOPPED)
		return suspend_failed_audio(
			host, MODEM_HOST_MODEM_STOPPED);
	if (host->last_audio_scheduler_result !=
		    AUDIO_SCHEDULER_OK &&
	    host->last_audio_scheduler_result !=
		    AUDIO_SCHEDULER_IDLE)
		return suspend_failed_audio(host, MODEM_HOST_AUDIO_ERROR);
	if (binding_failed)
		return MODEM_HOST_MEDIA_BINDING_ERROR;
	return host->last_audio_scheduler_result ==
			       AUDIO_SCHEDULER_OK ?
		       MODEM_HOST_OK :
		       MODEM_HOST_IDLE;
}

uint64_t modem_host_wait_ns(
	const struct modem_host *host, uint64_t now_ns)
{
	uint64_t audio_wait;

	if (host == NULL || !sip_session_is_open(host->session))
		return UINT64_MAX;
	if (sip_session_media_cleanup_pending(host->session))
		return 0;
	if (host->remote_clock) {
		if (host->remote_seed_pending || host->remote_frames_pending != 0)
			return 0;
		return MODEM_HOST_SIGNALING_POLL_NS;
	}
	audio_wait = audio_scheduler_wait_ns(
		&host->audio_scheduler, now_ns);
	return audio_wait < MODEM_HOST_SIGNALING_POLL_NS ?
		       audio_wait :
		       MODEM_HOST_SIGNALING_POLL_NS;
}

int modem_host_media_poll_fd(
	const struct modem_host *host)
{
	return host != NULL ? sip_session_media_fd(host->session) : -1;
}

enum sip_signaling_result modem_host_answer(
	struct modem_host *host)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sip_session_answer(host->session);
	synchronize_audio(host);
	return result;
}

enum sip_signaling_result modem_host_hangup(
	struct modem_host *host)
{
	enum sip_signaling_result result;

	if (host == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	result = sip_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

size_t modem_host_frame_samples(const struct modem_host *host)
{
	return host != NULL ?
		       audio_path_frame_samples(host->audio_path) :
		       0;
}

bool modem_host_is_open(const struct modem_host *host)
{
	return host != NULL && sip_session_is_open(host->session);
}

bool modem_host_media_is_configured(
	const struct modem_host *host)
{
	return host != NULL &&
	       sip_session_media_is_configured(host->session);
}

bool modem_host_audio_is_active(const struct modem_host *host)
{
	return host != NULL && audio_scheduler_is_active(
			       &host->audio_scheduler);
}

enum sip_session_result modem_host_last_session_result(
	const struct modem_host *host)
{
	return host != NULL ? host->last_session_result :
			      SIP_SESSION_INVALID_ARGUMENT;
}

enum audio_scheduler_result
modem_host_last_audio_scheduler_result(
	const struct modem_host *host)
{
	return host != NULL ? host->last_audio_scheduler_result :
			      AUDIO_SCHEDULER_INVALID_ARGUMENT;
}

enum audio_path_result modem_host_last_audio_path_result(
	const struct modem_host *host)
{
	return host != NULL ? host->audio_scheduler.last_path_result :
			      AUDIO_PATH_INVALID_ARGUMENT;
}

const char *modem_host_result_name(enum modem_host_result result)
{
	switch (result) {
	case MODEM_HOST_OK:
		return "ok";
	case MODEM_HOST_IDLE:
		return "idle";
	case MODEM_HOST_INVALID_ARGUMENT:
		return "invalid argument";
	case MODEM_HOST_SESSION_ERROR:
		return "SIP/RTP session failed";
	case MODEM_HOST_MEDIA_BINDING_ERROR:
		return "negotiated media could not be bound";
	case MODEM_HOST_AUDIO_ERROR:
		return "scheduled audio failed";
	case MODEM_HOST_MODEM_STOPPED:
		return "modem core stopped";
	case MODEM_HOST_MEDIA_TIMEOUT:
		return "remote RTP clock stopped";
	}
	return "unknown host result";
}

unsigned int modem_host_rtp_local_port(const struct modem_host *host)
{
	return host != NULL ? sip_session_rtp_local_port(host->session) : 0;
}

bool modem_host_media_cleanup_pending(const struct modem_host *host)
{
	return host != NULL && sip_session_media_cleanup_pending(host->session);
}
