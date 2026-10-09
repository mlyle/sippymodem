// SPDX-License-Identifier: BSD-3-Clause

#include "host.h"

#include "media.h"
#include "frame_pacer.h"

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
	bool shared_signaling;
	bool remote_clock;
	bool remote_seed_pending;
	bool remote_clock_started;
	uint64_t remote_clock_time_ns;
	size_t remote_frames_pending;
	uint64_t remote_gap_window_ns;
	uint64_t remote_gap_start_ns;
	struct rtp_playout_status remote_gap_cursor;
	bool remote_gap_active;
	struct frame_pacer remote_pacer;
	uint64_t remote_now_ns;
	bool remote_time_valid;
	bool remote_stream_valid;
	uint32_t remote_ssrc;
	bool remote_partial_active;
	uint64_t remote_partial_start_ns;
	uint32_t remote_partial_timestamp;
	uint64_t remote_partial_window_ns;
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
		host->remote_gap_active = false;
		frame_pacer_reset(&host->remote_pacer);
		host->remote_time_valid = false;
		host->remote_stream_valid = false;
		host->remote_partial_active = false;
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
	host->shared_signaling = server != NULL;
	host->remote_gap_window_ns =
		(uint64_t)config->media.jitter_buffer_ms * UINT64_C(1000000);
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
	/* Exactly 4/3 of the configured sample clock, without percentage rounding.
	 * Integer-millisecond RTP frames have an exactly representable quarter. */
	if (!frame_pacer_init(&host->remote_pacer,
		host->audio_scheduler.clock.interval_ns / 4 * 3,
		MEDIA_MAX_CATCHUP_FRAMES))
		goto fail;
	host->remote_partial_window_ns = host->remote_gap_window_ns;
	if (host->remote_partial_window_ns < host->audio_scheduler.clock.interval_ns)
		host->remote_partial_window_ns = host->audio_scheduler.clock.interval_ns;
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
	host->remote_gap_active = false;
	frame_pacer_reset(&host->remote_pacer);
	host->remote_time_valid = false;
	host->remote_stream_valid = false;
	host->remote_partial_active = false;
	host->last_session_result = sip_session_close(host->session);
	return host->last_session_result;
}

enum sip_signaling_result modem_host_set_endpoint(struct modem_host *host, struct sip_endpoint *endpoint)
{
	return host != NULL ? sip_session_set_endpoint(host->session, endpoint) : SIP_SIGNALING_INVALID_ARGUMENT;
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
	/* The SDK owner retires failed shared calls after collecting their media job. */
	if (!host->shared_signaling)
		sip_session_hangup(host->session);
	synchronize_audio(host);
	return result;
}

static uint64_t remote_gap_wait_ns(
	const struct modem_host *host, uint64_t now_ns)
{
	if (!host->remote_gap_active)
		return 0;
	uint64_t elapsed = now_ns >= host->remote_gap_start_ns ?
		now_ns - host->remote_gap_start_ns : 0;
	return elapsed < host->remote_gap_window_ns ?
		host->remote_gap_window_ns - elapsed : 0;
}

static uint64_t check_remote_gap(
	struct modem_host *host, uint64_t now_ns)
{
	struct rtp_playout_status status = sip_session_playout_status(host->session);

	if (!status.gap || host->remote_gap_window_ns == 0) {
		host->remote_gap_active = false;
		return 0;
	}
	/* An expired contiguous hole spans multiple frames without a new hold. */
	if (!host->remote_gap_active ||
	    status.ssrc != host->remote_gap_cursor.ssrc ||
	    (status.timestamp != host->remote_gap_cursor.timestamp &&
	     (!status.starts_with_gap ||
	      (int32_t)(status.timestamp - host->remote_gap_cursor.timestamp) <= 0))) {
		host->remote_gap_active = true;
		host->remote_gap_start_ns = now_ns;
		host->remote_gap_cursor = status;
	}
	return remote_gap_wait_ns(host, now_ns);
}

static uint64_t remote_partial_wait_ns(
	const struct modem_host *host, uint64_t now_ns)
{
	if (!host->remote_partial_active)
		return UINT64_MAX;
	uint64_t elapsed = now_ns >= host->remote_partial_start_ns ?
		now_ns - host->remote_partial_start_ns : 0;
	return elapsed < host->remote_partial_window_ns ?
		host->remote_partial_window_ns - elapsed : 0;
}

static void synchronize_remote_stream(struct modem_host *host, uint64_t now_ns)
{
	struct rtp_playout_status status = sip_session_playout_status(host->session);

	if (status.stream_active &&
	    (!host->remote_stream_valid || host->remote_ssrc != status.ssrc)) {
		host->remote_frames_pending = 0;
		host->remote_gap_active = false;
		host->remote_partial_active = false;
		host->remote_stream_valid = true;
		host->remote_ssrc = status.ssrc;
	}
	if (status.partial_samples == 0) {
		host->remote_partial_active = false;
	} else if (!host->remote_partial_active ||
		   host->remote_partial_timestamp != status.partial_timestamp) {
		host->remote_partial_active = true;
		host->remote_partial_timestamp = status.partial_timestamp;
		host->remote_partial_start_ns = now_ns;
	}
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

	if (sip_session_receive_draining(host->session)) {
		/* Retired media needs no wall-clock/gap hold. Keep the usual per-step
		 * DSP budget, and never generate transport output during this drain. */
		size_t tail = sip_session_receive_tail_frames(host->session);
		if (tail > MEDIA_MAX_CATCHUP_FRAMES)
			tail = MEDIA_MAX_CATCHUP_FRAMES;
		host->last_audio_scheduler_result = audio_scheduler_step_remote(
			&host->audio_scheduler, tail, &frames);
		if (sip_session_receive_tail_frames(host->session) == 0) {
			sip_session_finish_receive_tail(host->session);
			synchronize_audio(host);
		}
	} else if (host->remote_clock) {
		if (audio_scheduler_is_active(&host->audio_scheduler)) {
			if (host->remote_time_valid && now_ns < host->remote_now_ns)
				now_ns = host->remote_now_ns;
			host->remote_now_ns = now_ns;
			host->remote_time_valid = true;
			synchronize_remote_stream(host, now_ns);
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
			/* Incoming media already supplies the startup clock. Seeding
			 * its receiver again would consume audio without RTP credit. */
			if (!sip_session_playout_status(host->session).stream_active)
				host->remote_frames_pending++;
			host->remote_seed_pending = false;
		}
		if (audio_scheduler_is_active(&host->audio_scheduler)) {
			if (remote_frames > SIZE_MAX - host->remote_frames_pending)
				return suspend_failed_audio(host, MODEM_HOST_AUDIO_ERROR);
			host->remote_frames_pending += remote_frames;
		}
		host->last_audio_scheduler_result = AUDIO_SCHEDULER_IDLE;
		while (audio_scheduler_is_active(&host->audio_scheduler) &&
		       frames < MEDIA_MAX_CATCHUP_FRAMES) {
			size_t processed = 0;

			if (host->remote_frames_pending == 0) {
				if (remote_partial_wait_ns(host, now_ns) != 0 ||
				    frame_pacer_wait_ns(&host->remote_pacer, now_ns) != 0)
					break;
				host->remote_partial_active = false;
				if (!sip_session_pad_clock_frame(host->session))
					break;
				host->remote_frames_pending = 1;
			}
			if (check_remote_gap(host, now_ns) != 0 ||
			    !frame_pacer_take(&host->remote_pacer, now_ns))
				break;
			/* DSP may synchronously hang up and retire pending credit. */
			host->remote_frames_pending--;
			host->last_audio_scheduler_result = audio_scheduler_step_remote(
				&host->audio_scheduler, 1, &processed);
			frames += processed;
			if (host->last_audio_scheduler_result != AUDIO_SCHEDULER_OK)
				break;
		}
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
	if (sip_session_media_cleanup_pending(host->session) ||
	    sip_session_receive_draining(host->session))
		return 0;
	if (host->remote_clock) {
		if (host->remote_time_valid && now_ns < host->remote_now_ns)
			now_ns = host->remote_now_ns;
		if (host->remote_seed_pending)
			return 0;
		if (host->remote_frames_pending != 0 || host->remote_partial_active) {
			uint64_t wait = host->remote_frames_pending != 0 ?
				remote_gap_wait_ns(host, now_ns) : remote_partial_wait_ns(host, now_ns);
			uint64_t pacing_wait = frame_pacer_wait_ns(&host->remote_pacer, now_ns);

			if (wait < pacing_wait)
				wait = pacing_wait;

			return wait < MODEM_HOST_SIGNALING_POLL_NS ?
				wait : MODEM_HOST_SIGNALING_POLL_NS;
		}
		return MODEM_HOST_SIGNALING_POLL_NS;
	}
	audio_wait = audio_scheduler_wait_ns(
		&host->audio_scheduler, now_ns);
	return audio_wait < MODEM_HOST_SIGNALING_POLL_NS ?
		       audio_wait :
		       MODEM_HOST_SIGNALING_POLL_NS;
}

net_socket modem_host_media_poll_fd(
	const struct modem_host *host)
{
	return host != NULL ? sip_session_media_fd(host->session) : NET_INVALID_SOCKET;
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

enum sip_session_result modem_host_prepare_media(struct modem_host *host)
{
	return host != NULL ? sip_session_begin_media_batch(host->session) :
		SIP_SESSION_INVALID_ARGUMENT;
}

enum sip_session_result modem_host_finish_media(struct modem_host *host)
{
	return host != NULL ? sip_session_finish_media_batch(host->session) :
		SIP_SESSION_INVALID_ARGUMENT;
}
