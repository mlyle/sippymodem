// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIP_SESSION_H
#define SIP_SESSION_H

#include "rtp_endpoint.h"
#include "sip_rtp.h"
#include "sip_signaling.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sip_session_result {
	SIP_SESSION_OK,
	SIP_SESSION_INVALID_ARGUMENT,
	SIP_SESSION_ALREADY_OPEN,
	SIP_SESSION_NOT_OPEN,
	SIP_SESSION_RTP_ERROR,
	SIP_SESSION_SIGNALING_ERROR,
	SIP_SESSION_MEDIA_BINDING_ERROR,
};

struct sip_session;

struct sip_session *sip_session_create(
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks);
/* Call destroy outside callbacks. close is callback-safe. */
void sip_session_destroy(struct sip_session *session);

enum sip_session_result sip_session_open(
	struct sip_session *session);
enum sip_session_result sip_session_close(
	struct sip_session *session);
enum sip_session_result sip_session_step(
	struct sip_session *session,
	unsigned int signaling_timeout_ms,
	size_t *datagrams_received);

enum sip_signaling_result sip_session_dial(
	struct sip_session *session, const char *target);
enum sip_signaling_result sip_session_answer(
	struct sip_session *session);
enum sip_signaling_result sip_session_hangup(
	struct sip_session *session);

size_t sip_session_frame_samples(
	const struct sip_session *session);
uint16_t sip_session_rtp_local_port(
	const struct sip_session *session);
bool sip_session_is_open(const struct sip_session *session);
bool sip_session_media_is_configured(
	const struct sip_session *session);

enum media_result sip_session_play(
	struct sip_session *session,
	int16_t *samples,
	size_t count,
	struct rtp_playout_report *report);
enum rtp_endpoint_result sip_session_send(
	struct sip_session *session,
	const int16_t *samples,
	size_t count);

enum sip_rtp_result sip_session_last_media_binding(
	const struct sip_session *session);
enum sip_signaling_result sip_session_last_signaling_result(
	const struct sip_session *session);
enum rtp_endpoint_result sip_session_last_rtp_result(
	const struct sip_session *session);
const char *sip_session_result_name(
	enum sip_session_result result);

#endif
