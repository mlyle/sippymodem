// SPDX-License-Identifier: BSD-3-Clause

#ifndef SLMODEM_TERMINAL_H
#define SLMODEM_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

struct modem;

enum slmodem_terminal_result {
	SLMODEM_TERMINAL_OK,
	SLMODEM_TERMINAL_IDLE,
	SLMODEM_TERMINAL_INVALID_ARGUMENT,
	SLMODEM_TERMINAL_INVALID_STATE,
	SLMODEM_TERMINAL_INPUT_ERROR,
};

struct slmodem_terminal {
	struct modem *modem;
};

/* modem is borrowed, must outlive this adapter, and begins unbound. */
bool slmodem_terminal_init(
	struct slmodem_terminal *terminal, struct modem *modem);
enum slmodem_terminal_result
slmodem_terminal_bind(
	struct slmodem_terminal *terminal,
	int file_descriptor,
	const char *device_path);
/* The descriptor and path are borrowed until unbind. */
enum slmodem_terminal_result
slmodem_terminal_unbind(
	struct slmodem_terminal *terminal);
enum slmodem_terminal_result
slmodem_terminal_apply_attributes(
	struct slmodem_terminal *terminal,
	const struct termios *attributes);
enum slmodem_terminal_result
slmodem_terminal_service_event(
	struct slmodem_terminal *terminal);
enum slmodem_terminal_result
slmodem_terminal_input_capacity(
	const struct slmodem_terminal *terminal, size_t *capacity);
enum slmodem_terminal_result
slmodem_terminal_receive(
	struct slmodem_terminal *terminal,
	const void *bytes,
	size_t count);
enum slmodem_terminal_result
slmodem_terminal_disconnect(
	struct slmodem_terminal *terminal);
const char *slmodem_terminal_result_name(
	enum slmodem_terminal_result result);

#endif
