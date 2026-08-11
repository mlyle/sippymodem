// SPDX-License-Identifier: BSD-3-Clause

#ifndef MODEM_RUNTIME_H
#define MODEM_RUNTIME_H

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
	MODEM_RUNTIME_NAME_MAX = 64,
	MODEM_RUNTIME_MAX_CALL_TRANSITIONS = 16,
};

enum modem_runtime_result {
	MODEM_RUNTIME_OK,
	MODEM_RUNTIME_IDLE,
	MODEM_RUNTIME_MEDIA_BINDING_ERROR,
	MODEM_RUNTIME_INVALID_ARGUMENT,
	MODEM_RUNTIME_TERMINAL_ERROR,
	MODEM_RUNTIME_SESSION_ERROR,
	MODEM_RUNTIME_MODEM_STOP_ERROR,
	MODEM_RUNTIME_HOST_ERROR,
	MODEM_RUNTIME_CALL_ERROR,
	MODEM_RUNTIME_RECORDING_ERROR,
	MODEM_RUNTIME_MODEM_TERMINAL_ERROR,
};

struct modem;
struct modem_runtime;

/* Owns the modem, closed SIP host, optional recording files, and every adapter
 * between them. Construction does not open a transport. */
struct modem_runtime *modem_runtime_create(
	const struct app_config *config, const char *modem_name);
void modem_runtime_destroy(struct modem_runtime *runtime);

enum modem_runtime_result modem_runtime_open(
	struct modem_runtime *runtime);
enum modem_runtime_result modem_runtime_close(
	struct modem_runtime *runtime);
/* Advance bounded terminal, host, and queued call-transition work without
 * blocking. All counters are optional outputs. */
enum modem_runtime_result modem_runtime_step(
	struct modem_runtime *runtime,
	uint64_t now_ns,
	size_t *terminal_bytes_received,
	size_t *datagrams_received,
	size_t *frames_processed);
/* Return the next host deadline relative to now_ns; UINT64_MAX means none. */
uint64_t modem_runtime_wait_ns(
	const struct modem_runtime *runtime, uint64_t now_ns);
bool modem_runtime_is_open(const struct modem_runtime *runtime);
int modem_runtime_terminal_fd(
	const struct modem_runtime *runtime);
/* Return the terminal descriptor only when polling it can make progress. */
int modem_runtime_terminal_poll_fd(
	const struct modem_runtime *runtime);
const char *modem_runtime_device_path(
	const struct modem_runtime *runtime);
enum modem_host_result modem_runtime_last_host_result(
	const struct modem_runtime *runtime);
enum sip_session_result modem_runtime_last_session_result(
	const struct modem_runtime *runtime);
enum pty_terminal_result modem_runtime_last_terminal_result(
	const struct modem_runtime *runtime);
enum slmodem_calls_result modem_runtime_last_calls_result(
	const struct modem_runtime *runtime);
enum slmodem_terminal_result
modem_runtime_last_modem_terminal_result(
	const struct modem_runtime *runtime);
const char *modem_runtime_result_name(
	enum modem_runtime_result result);

struct modem *modem_runtime_modem(struct modem_runtime *runtime);
const struct recording_files *modem_runtime_recording_files(
	const struct modem_runtime *runtime);

#endif
