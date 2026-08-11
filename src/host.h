// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_HOST_H
#define SIPPYMODEM_HOST_H

#include "audio_path.h"
#include "audio_scheduler.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_HOST_MAXIMUM_CATCHUP_FRAMES = 4,
	SIPPYMODEM_HOST_SIGNALING_POLL_NS = 20000000,
};

enum sippymodem_host_result {
	SIPPYMODEM_HOST_OK,
	SIPPYMODEM_HOST_IDLE,
	SIPPYMODEM_HOST_INVALID_ARGUMENT,
	SIPPYMODEM_HOST_SESSION_ERROR,
	SIPPYMODEM_HOST_MEDIA_BINDING_ERROR,
	SIPPYMODEM_HOST_AUDIO_ERROR,
	SIPPYMODEM_HOST_MODEM_STOPPED,
};

struct sippymodem_host;

struct sippymodem_host *sippymodem_host_create(
	const struct sippymodem_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *modem_core);
/* Call destroy outside signaling and modem-core callbacks. */
void sippymodem_host_destroy(struct sippymodem_host *host);

enum sippymodem_session_result sippymodem_host_open(
	struct sippymodem_host *host);
enum sippymodem_session_result sippymodem_host_close(
	struct sippymodem_host *host);

enum sip_signaling_result sippymodem_host_dial(
	struct sippymodem_host *host, const char *target);
enum sip_signaling_result sippymodem_host_answer(
	struct sippymodem_host *host);
enum sip_signaling_result sippymodem_host_hangup(
	struct sippymodem_host *host);

/* now_ns uses one monotonic nanosecond clock. This step never blocks in
 * signaling, and both counters are optional outputs. */
enum sippymodem_host_result sippymodem_host_step(
	struct sippymodem_host *host,
	uint64_t now_ns,
	size_t *datagrams_received,
	size_t *frames_processed);
/* UINT64_MAX means that a closed host has no pending work. */
uint64_t sippymodem_host_wait_ns(
	const struct sippymodem_host *host, uint64_t now_ns);

size_t sippymodem_host_frame_samples(const struct sippymodem_host *host);
bool sippymodem_host_is_open(const struct sippymodem_host *host);
bool sippymodem_host_media_is_configured(
	const struct sippymodem_host *host);
bool sippymodem_host_audio_is_active(const struct sippymodem_host *host);
enum sippymodem_session_result sippymodem_host_last_session_result(
	const struct sippymodem_host *host);
enum audio_scheduler_result
sippymodem_host_last_audio_scheduler_result(
	const struct sippymodem_host *host);
enum audio_path_result sippymodem_host_last_audio_path_result(
	const struct sippymodem_host *host);
const char *sippymodem_host_result_name(enum sippymodem_host_result result);

#endif
