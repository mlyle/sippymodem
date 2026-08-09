// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SLMODEM_TERMINAL_H
#define SIPPYMODEM_SLMODEM_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

struct modem;

enum sippymodem_slmodem_terminal_result {
	SIPPYMODEM_SLMODEM_TERMINAL_OK,
	SIPPYMODEM_SLMODEM_TERMINAL_IDLE,
	SIPPYMODEM_SLMODEM_TERMINAL_INVALID_ARGUMENT,
	SIPPYMODEM_SLMODEM_TERMINAL_INVALID_STATE,
	SIPPYMODEM_SLMODEM_TERMINAL_INPUT_ERROR,
};

struct sippymodem_slmodem_terminal {
	struct modem *modem;
};

/* modem is borrowed and must outlive this adapter. */
bool sippymodem_slmodem_terminal_init(
	struct sippymodem_slmodem_terminal *terminal, struct modem *modem);
enum sippymodem_slmodem_terminal_result
sippymodem_slmodem_terminal_apply_attributes(
	struct sippymodem_slmodem_terminal *terminal,
	const struct termios *attributes);
enum sippymodem_slmodem_terminal_result
sippymodem_slmodem_terminal_service_event(
	struct sippymodem_slmodem_terminal *terminal);
enum sippymodem_slmodem_terminal_result
sippymodem_slmodem_terminal_input_capacity(
	const struct sippymodem_slmodem_terminal *terminal, size_t *capacity);
enum sippymodem_slmodem_terminal_result
sippymodem_slmodem_terminal_receive(
	struct sippymodem_slmodem_terminal *terminal,
	const void *bytes,
	size_t count);
enum sippymodem_slmodem_terminal_result
sippymodem_slmodem_terminal_disconnect(
	struct sippymodem_slmodem_terminal *terminal);
const char *sippymodem_slmodem_terminal_result_name(
	enum sippymodem_slmodem_terminal_result result);

#endif
