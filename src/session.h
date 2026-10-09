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

/* The shared server must outlive this object; step drains media only. */
struct sip_session *sip_session_create_shared(
	struct sip_server *server,
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks);

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
void sip_session_trace_media(struct sip_session *session, uint64_t now_ns);

enum sip_signaling_result sip_session_set_endpoint(struct sip_session *session,
	struct sip_endpoint *endpoint);
enum sip_signaling_result sip_session_dial(
	struct sip_session *session, const char *target);
enum sip_signaling_result sip_session_answer(
	struct sip_session *session);
enum sip_signaling_result sip_session_hangup(
	struct sip_session *session);

size_t sip_session_frame_samples(
	const struct sip_session *session);
net_socket sip_session_media_fd(
	const struct sip_session *session);
uint16_t sip_session_rtp_local_port(
	const struct sip_session *session);
/* Admission waits until old call datagrams have been discarded in bounded batches. */
bool sip_session_media_cleanup_pending(const struct sip_session *session);
size_t sip_session_remote_clock_frames(
	const struct sip_session *session);
struct rtp_playout_status sip_session_playout_status(
	const struct sip_session *session);
bool sip_session_pad_clock_frame(struct sip_session *session);
bool sip_session_receive_draining(const struct sip_session *session);
size_t sip_session_receive_tail_frames(const struct sip_session *session);
void sip_session_finish_receive_tail(struct sip_session *session);
bool sip_session_is_open(const struct sip_session *session);
bool sip_session_media_is_configured(
	const struct sip_session *session);

enum media_result sip_session_play(
	struct sip_session *session,
	int16_t *samples,
	uint8_t *source_octets, bool *received_mask,
	size_t count,
	struct rtp_playout_report *report);
enum rtp_endpoint_result sip_session_send(
	struct sip_session *session,
	const int16_t *samples,
	size_t count);
enum rtp_endpoint_result sip_session_send_g711(
	struct sip_session *session,
	const uint8_t *codewords,
	const int16_t *decoded_samples,
	size_t count);

enum sip_rtp_result sip_session_last_media_binding(
	const struct sip_session *session);
enum sip_signaling_result sip_session_last_signaling_result(
	const struct sip_session *session);
enum rtp_endpoint_result sip_session_last_rtp_result(
	const struct sip_session *session);

/* Owner-side socket drain/flush around an exclusive worker media job. */
enum sip_session_result sip_session_begin_media_batch(struct sip_session *session);
enum sip_session_result sip_session_finish_media_batch(struct sip_session *session);

#endif
