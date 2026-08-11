// SPDX-License-Identifier: BSD-3-Clause

#ifndef MODEM_HOST_H
#define MODEM_HOST_H

#include "audio_path.h"
#include "audio_scheduler.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	MODEM_HOST_MAXIMUM_CATCHUP_FRAMES = 4,
	MODEM_HOST_SIGNALING_POLL_NS = 20000000,
};

enum modem_host_result {
	MODEM_HOST_OK,
	MODEM_HOST_IDLE,
	MODEM_HOST_INVALID_ARGUMENT,
	MODEM_HOST_SESSION_ERROR,
	MODEM_HOST_MEDIA_BINDING_ERROR,
	MODEM_HOST_AUDIO_ERROR,
	MODEM_HOST_MODEM_STOPPED,
};

struct modem_host;

struct modem_host *modem_host_create(
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *modem_core);
/* Call destroy outside signaling and modem-core callbacks. */
void modem_host_destroy(struct modem_host *host);

enum sip_session_result modem_host_open(
	struct modem_host *host);
enum sip_session_result modem_host_close(
	struct modem_host *host);

enum sip_signaling_result modem_host_dial(
	struct modem_host *host, const char *target);
enum sip_signaling_result modem_host_answer(
	struct modem_host *host);
enum sip_signaling_result modem_host_hangup(
	struct modem_host *host);

/* now_ns uses one monotonic nanosecond clock. This step never blocks in
 * signaling, and both counters are optional outputs. */
enum modem_host_result modem_host_step(
	struct modem_host *host,
	uint64_t now_ns,
	size_t *datagrams_received,
	size_t *frames_processed);
/* UINT64_MAX means that a closed host has no pending work. */
uint64_t modem_host_wait_ns(
	const struct modem_host *host, uint64_t now_ns);

size_t modem_host_frame_samples(const struct modem_host *host);
bool modem_host_is_open(const struct modem_host *host);
bool modem_host_media_is_configured(
	const struct modem_host *host);
bool modem_host_audio_is_active(const struct modem_host *host);
enum sip_session_result modem_host_last_session_result(
	const struct modem_host *host);
enum audio_scheduler_result
modem_host_last_audio_scheduler_result(
	const struct modem_host *host);
enum audio_path_result modem_host_last_audio_path_result(
	const struct modem_host *host);
const char *modem_host_result_name(enum modem_host_result result);

#endif
