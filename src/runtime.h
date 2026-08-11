// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RUNTIME_H
#define SIPPYMODEM_RUNTIME_H

#include "config.h"
#include "host.h"
#include "recording_files.h"
#include "session.h"
#include "slmodem_calls.h"
#include "slmodem_terminal.h"
#include "terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_RUNTIME_NAME_MAX = 64,
	SIPPYMODEM_RUNTIME_MAX_CALL_TRANSITIONS = 16,
};

enum sippymodem_runtime_result {
	SIPPYMODEM_RUNTIME_OK,
	SIPPYMODEM_RUNTIME_IDLE,
	SIPPYMODEM_RUNTIME_MEDIA_BINDING_ERROR,
	SIPPYMODEM_RUNTIME_INVALID_ARGUMENT,
	SIPPYMODEM_RUNTIME_TERMINAL_ERROR,
	SIPPYMODEM_RUNTIME_SESSION_ERROR,
	SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR,
	SIPPYMODEM_RUNTIME_HOST_ERROR,
	SIPPYMODEM_RUNTIME_CALL_ERROR,
	SIPPYMODEM_RUNTIME_RECORDING_ERROR,
	SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR,
};

struct modem;
struct sippymodem_runtime;

/* Owns the modem, closed SIP host, optional recording files, and every adapter
 * between them. Construction does not open a transport. */
struct sippymodem_runtime *sippymodem_runtime_create(
	const struct app_config *config, const char *modem_name);
void sippymodem_runtime_destroy(struct sippymodem_runtime *runtime);

enum sippymodem_runtime_result sippymodem_runtime_open(
	struct sippymodem_runtime *runtime);
enum sippymodem_runtime_result sippymodem_runtime_close(
	struct sippymodem_runtime *runtime);
/* Advance bounded terminal, host, and queued call-transition work without
 * blocking. All counters are optional outputs. */
enum sippymodem_runtime_result sippymodem_runtime_step(
	struct sippymodem_runtime *runtime,
	uint64_t now_ns,
	size_t *terminal_bytes_received,
	size_t *datagrams_received,
	size_t *frames_processed);
/* Return the next host deadline relative to now_ns; UINT64_MAX means none. */
uint64_t sippymodem_runtime_wait_ns(
	const struct sippymodem_runtime *runtime, uint64_t now_ns);
bool sippymodem_runtime_is_open(const struct sippymodem_runtime *runtime);
int sippymodem_runtime_terminal_fd(
	const struct sippymodem_runtime *runtime);
/* Return the terminal descriptor only when polling it can make progress. */
int sippymodem_runtime_terminal_poll_fd(
	const struct sippymodem_runtime *runtime);
const char *sippymodem_runtime_device_path(
	const struct sippymodem_runtime *runtime);
enum sippymodem_host_result sippymodem_runtime_last_host_result(
	const struct sippymodem_runtime *runtime);
enum sip_session_result sippymodem_runtime_last_session_result(
	const struct sippymodem_runtime *runtime);
enum sippymodem_terminal_result sippymodem_runtime_last_terminal_result(
	const struct sippymodem_runtime *runtime);
enum slmodem_calls_result sippymodem_runtime_last_calls_result(
	const struct sippymodem_runtime *runtime);
enum slmodem_terminal_result
sippymodem_runtime_last_modem_terminal_result(
	const struct sippymodem_runtime *runtime);
const char *sippymodem_runtime_result_name(
	enum sippymodem_runtime_result result);

struct modem *sippymodem_runtime_modem(struct sippymodem_runtime *runtime);
const struct recording_files *sippymodem_runtime_recording_files(
	const struct sippymodem_runtime *runtime);

#endif
