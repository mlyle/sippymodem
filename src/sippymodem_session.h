// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SESSION_H
#define SIPPYMODEM_SESSION_H

#include "sippymodem_rtp_endpoint.h"
#include "sippymodem_sip_rtp.h"
#include "sippymodem_sip_signaling.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_session_result {
	SIPPYMODEM_SESSION_OK,
	SIPPYMODEM_SESSION_INVALID_ARGUMENT,
	SIPPYMODEM_SESSION_ALREADY_OPEN,
	SIPPYMODEM_SESSION_NOT_OPEN,
	SIPPYMODEM_SESSION_RTP_ERROR,
	SIPPYMODEM_SESSION_SIGNALING_ERROR,
	SIPPYMODEM_SESSION_MEDIA_BINDING_ERROR,
};

struct sippymodem_session;

struct sippymodem_session *sippymodem_session_create(
	const struct sippymodem_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_sip_signaling_callbacks *callbacks);
/* Call destroy outside callbacks. close is callback-safe. */
void sippymodem_session_destroy(struct sippymodem_session *session);

enum sippymodem_session_result sippymodem_session_open(
	struct sippymodem_session *session);
enum sippymodem_session_result sippymodem_session_close(
	struct sippymodem_session *session);
enum sippymodem_session_result sippymodem_session_step(
	struct sippymodem_session *session,
	unsigned int signaling_timeout_ms,
	size_t *datagrams_received);

enum sippymodem_sip_signaling_result sippymodem_session_dial(
	struct sippymodem_session *session, const char *target);
enum sippymodem_sip_signaling_result sippymodem_session_answer(
	struct sippymodem_session *session);
enum sippymodem_sip_signaling_result sippymodem_session_hangup(
	struct sippymodem_session *session);

size_t sippymodem_session_frame_samples(
	const struct sippymodem_session *session);
uint16_t sippymodem_session_rtp_local_port(
	const struct sippymodem_session *session);
bool sippymodem_session_is_open(const struct sippymodem_session *session);

enum sippymodem_media_result sippymodem_session_play(
	struct sippymodem_session *session,
	int16_t *samples,
	size_t count,
	struct sippymodem_rtp_playout_report *report);
enum sippymodem_rtp_endpoint_result sippymodem_session_send(
	struct sippymodem_session *session,
	const int16_t *samples,
	size_t count);

enum sippymodem_sip_rtp_result sippymodem_session_last_media_binding(
	const struct sippymodem_session *session);
enum sippymodem_sip_signaling_result sippymodem_session_last_signaling_result(
	const struct sippymodem_session *session);
enum sippymodem_rtp_endpoint_result sippymodem_session_last_rtp_result(
	const struct sippymodem_session *session);
const char *sippymodem_session_result_name(
	enum sippymodem_session_result result);

#endif
