// SPDX-License-Identifier: BSD-3-Clause

#include "session.h"

#include "rtp_runtime.h"
#include "sip_media_bridge.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct sip_session {
	struct app_config config;
	struct rtp_runtime *rtp;
	struct sip_signaling *signaling;
	struct sip_media_bridge bridge;
	enum sip_signaling_result last_signaling_result;
	enum rtp_endpoint_result last_rtp_result;
	bool signaling_destroy_deferred;
	bool opened;
};

static void finish_deferred_signaling(struct sip_session *session)
{
	if (session->signaling == NULL ||
	    sip_media_bridge_is_in_callback(&session->bridge) ||
	    !session->signaling_destroy_deferred)
		return;
	sip_signaling_destroy(session->signaling);
	session->signaling = NULL;
	session->signaling_destroy_deferred = false;
}

struct sip_session *sip_session_create(
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks)
{
	struct sip_session *session;

	if (config == NULL || config->media.rtp_listen_port > UINT16_MAX)
		return NULL;
	session = calloc(1, sizeof(*session));
	if (session == NULL)
		return NULL;
	session->config = *config;
	session->rtp = rtp_runtime_create(&config->media, recorder);
	if (session->rtp == NULL ||
	    !sip_media_bridge_init(
		    &session->bridge,
		    rtp_runtime_endpoint(session->rtp), callbacks)) {
		rtp_runtime_destroy(session->rtp);
		free(session);
		return NULL;
	}
	return session;
}

void sip_session_destroy(struct sip_session *session)
{
	if (session == NULL ||
	    sip_media_bridge_is_in_callback(&session->bridge))
		return;
	if (session->opened)
		sip_session_close(session);
	finish_deferred_signaling(session);
	rtp_runtime_destroy(session->rtp);
	free(session);
}

enum sip_session_result sip_session_open(
	struct sip_session *session)
{
	struct app_config signaling_config;
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	uint64_t binding_failures;

	if (session == NULL)
		return SIP_SESSION_INVALID_ARGUMENT;
	if (session->opened)
		return SIP_SESSION_ALREADY_OPEN;
	finish_deferred_signaling(session);
	local.sin_port = htons((uint16_t)session->config.media.rtp_listen_port);
	session->last_rtp_result =
		rtp_runtime_open(session->rtp, &local);
	if (session->last_rtp_result != RTP_ENDPOINT_OK)
		return SIP_SESSION_RTP_ERROR;

	signaling_config = session->config;
	signaling_config.media.rtp_listen_port =
		rtp_runtime_local_port(session->rtp);
	session->signaling = sip_signaling_create(
		&signaling_config,
		sip_media_bridge_callbacks(&session->bridge));
	if (session->signaling == NULL)
		goto signaling_error;
	binding_failures = sip_media_bridge_binding_failures(
		&session->bridge);
	session->opened = true;
	session->last_signaling_result =
		sip_signaling_open(session->signaling);
	if (session->last_signaling_result != SIP_SIGNALING_OK ||
	    !session->opened)
		goto signaling_error;
	finish_deferred_signaling(session);
	return sip_media_bridge_binding_failures(&session->bridge) !=
			       binding_failures ?
		       SIP_SESSION_MEDIA_BINDING_ERROR :
		       SIP_SESSION_OK;

signaling_error:
	session->opened = false;
	if (session->signaling != NULL) {
		sip_signaling_close(session->signaling);
		session->signaling_destroy_deferred = true;
		finish_deferred_signaling(session);
	}
	sip_media_bridge_clear_media(&session->bridge);
	rtp_runtime_close(session->rtp);
	return SIP_SESSION_SIGNALING_ERROR;
}

enum sip_session_result sip_session_close(
	struct sip_session *session)
{
	if (session == NULL)
		return SIP_SESSION_INVALID_ARGUMENT;
	if (!session->opened) {
		finish_deferred_signaling(session);
		return SIP_SESSION_NOT_OPEN;
	}
	session->opened = false;
	session->last_signaling_result =
		sip_signaling_close(session->signaling);
	session->signaling_destroy_deferred = true;
	sip_media_bridge_clear_media(&session->bridge);
	rtp_runtime_close(session->rtp);
	finish_deferred_signaling(session);
	return session->last_signaling_result == SIP_SIGNALING_OK ?
		       SIP_SESSION_OK :
		       SIP_SESSION_SIGNALING_ERROR;
}

enum sip_session_result sip_session_step(
	struct sip_session *session, unsigned int signaling_timeout_ms,
	size_t *datagrams_received)
{
	uint64_t binding_failures;

	if (session == NULL)
		return SIP_SESSION_INVALID_ARGUMENT;
	if (!session->opened)
		return SIP_SESSION_NOT_OPEN;
	if (datagrams_received != NULL)
		*datagrams_received = 0;
	binding_failures = sip_media_bridge_binding_failures(
		&session->bridge);
	session->last_signaling_result = sip_signaling_step(
		session->signaling, signaling_timeout_ms);
	finish_deferred_signaling(session);
	if (session->last_signaling_result != SIP_SIGNALING_OK)
		return SIP_SESSION_SIGNALING_ERROR;
	if (!session->opened)
		return sip_media_bridge_binding_failures(
			       &session->bridge) != binding_failures ?
			       SIP_SESSION_MEDIA_BINDING_ERROR :
			       SIP_SESSION_OK;
	session->last_rtp_result =
		rtp_runtime_drain(session->rtp, datagrams_received);
	if (session->last_rtp_result != RTP_ENDPOINT_OK)
		return SIP_SESSION_RTP_ERROR;
	return sip_media_bridge_binding_failures(&session->bridge) !=
			       binding_failures ?
		       SIP_SESSION_MEDIA_BINDING_ERROR :
		       SIP_SESSION_OK;
}

static enum sip_signaling_result not_open_result(
	const struct sip_session *session)
{
	return session == NULL ? SIP_SIGNALING_INVALID_ARGUMENT :
			       SIP_SIGNALING_NOT_OPEN;
}

enum sip_signaling_result sip_session_dial(
	struct sip_session *session, const char *target)
{
	if (session == NULL || !session->opened)
		return not_open_result(session);
	session->last_signaling_result =
		sip_signaling_dial(session->signaling, target);
	finish_deferred_signaling(session);
	return session->last_signaling_result;
}

enum sip_signaling_result sip_session_answer(
	struct sip_session *session)
{
	if (session == NULL || !session->opened)
		return not_open_result(session);
	session->last_signaling_result =
		sip_signaling_answer(session->signaling);
	finish_deferred_signaling(session);
	return session->last_signaling_result;
}

enum sip_signaling_result sip_session_hangup(
	struct sip_session *session)
{
	if (session == NULL || !session->opened)
		return not_open_result(session);
	session->last_signaling_result =
		sip_signaling_hangup(session->signaling);
	finish_deferred_signaling(session);
	return session->last_signaling_result;
}

size_t sip_session_frame_samples(
	const struct sip_session *session)
{
	return session != NULL ?
		       rtp_runtime_frame_samples(session->rtp) :
		       0;
}

int sip_session_media_fd(
	const struct sip_session *session)
{
	return session != NULL ? rtp_runtime_fd(session->rtp) : -1;
}

uint16_t sip_session_rtp_local_port(
	const struct sip_session *session)
{
	return session != NULL ?
		       rtp_runtime_local_port(session->rtp) :
		       0;
}

size_t sip_session_remote_clock_frames(
	const struct sip_session *session)
{
	return session != NULL ?
		       rtp_runtime_remote_clock_frames(session->rtp) :
		       0;
}

bool sip_session_is_open(const struct sip_session *session)
{
	return session != NULL && session->opened;
}

bool sip_session_media_is_configured(
	const struct sip_session *session)
{
	return session != NULL && session->opened &&
	       sip_media_bridge_is_configured(&session->bridge);
}

enum media_result sip_session_play(
	struct sip_session *session, int16_t *samples,
	uint8_t *source_octets, size_t count,
	struct rtp_playout_report *report)
{
	return session != NULL ?
		       rtp_runtime_play(session->rtp, samples, source_octets, count,
						    report) :
		       MEDIA_INVALID_ARGUMENT;
}

enum rtp_endpoint_result sip_session_send(
	struct sip_session *session, const int16_t *samples, size_t count)
{
	return session != NULL ?
		       rtp_runtime_send(session->rtp, samples, count) :
		       RTP_ENDPOINT_INVALID_ARGUMENT;
}

enum sip_rtp_result sip_session_last_media_binding(
	const struct sip_session *session)
{
	return session != NULL ?
		       sip_media_bridge_last_binding(&session->bridge) :
		       SIP_RTP_INVALID_ARGUMENT;
}

enum sip_signaling_result sip_session_last_signaling_result(
	const struct sip_session *session)
{
	return session != NULL ? session->last_signaling_result :
			       SIP_SIGNALING_INVALID_ARGUMENT;
}

enum rtp_endpoint_result sip_session_last_rtp_result(
	const struct sip_session *session)
{
	return session != NULL ? session->last_rtp_result :
			       RTP_ENDPOINT_INVALID_ARGUMENT;
}

const char *sip_session_result_name(
	enum sip_session_result result)
{
	switch (result) {
	case SIP_SESSION_OK:
		return "ok";
	case SIP_SESSION_INVALID_ARGUMENT:
		return "invalid argument";
	case SIP_SESSION_ALREADY_OPEN:
		return "session is already open";
	case SIP_SESSION_NOT_OPEN:
		return "session is not open";
	case SIP_SESSION_RTP_ERROR:
		return "RTP runtime failed";
	case SIP_SESSION_SIGNALING_ERROR:
		return "SIP signaling failed";
	case SIP_SESSION_MEDIA_BINDING_ERROR:
		return "negotiated media could not be bound";
	}
	return "unknown session result";
}
