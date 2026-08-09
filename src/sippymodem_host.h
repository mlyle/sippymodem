// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_HOST_H
#define SIPPYMODEM_HOST_H

#include "sippymodem_audio_path.h"
#include "sippymodem_audio_scheduler.h"
#include "sippymodem_session.h"

#include <stdbool.h>
#include <stddef.h>

enum {
	SIPPYMODEM_HOST_MAXIMUM_CATCHUP_FRAMES = 4,
};

struct sippymodem_host;

struct sippymodem_host *sippymodem_host_create(
	const struct sippymodem_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_sip_signaling_callbacks *callbacks,
	const struct sippymodem_modem_core *modem_core);
/* Call destroy outside signaling and modem-core callbacks. */
void sippymodem_host_destroy(struct sippymodem_host *host);

enum sippymodem_session_result sippymodem_host_open(
	struct sippymodem_host *host);
enum sippymodem_session_result sippymodem_host_close(
	struct sippymodem_host *host);

enum sippymodem_sip_signaling_result sippymodem_host_dial(
	struct sippymodem_host *host, const char *target);
enum sippymodem_sip_signaling_result sippymodem_host_answer(
	struct sippymodem_host *host);
enum sippymodem_sip_signaling_result sippymodem_host_hangup(
	struct sippymodem_host *host);

size_t sippymodem_host_frame_samples(const struct sippymodem_host *host);
bool sippymodem_host_is_open(const struct sippymodem_host *host);
bool sippymodem_host_media_is_configured(
	const struct sippymodem_host *host);
bool sippymodem_host_audio_is_active(const struct sippymodem_host *host);

#endif
