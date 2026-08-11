// SPDX-License-Identifier: BSD-3-Clause

#ifndef PTY_TERMINAL_H
#define PTY_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

enum pty_terminal_result {
	PTY_TERMINAL_OK,
	PTY_TERMINAL_IDLE,
	PTY_TERMINAL_CLIENT_DISCONNECTED,
	PTY_TERMINAL_INVALID_ARGUMENT,
	PTY_TERMINAL_ALREADY_OPEN,
	PTY_TERMINAL_NOT_OPEN,
	PTY_TERMINAL_OPEN_ERROR,
	PTY_TERMINAL_NAME_ERROR,
	PTY_TERMINAL_CONFIGURATION_ERROR,
	PTY_TERMINAL_ATTRIBUTE_ERROR,
	PTY_TERMINAL_READ_ERROR,
	PTY_TERMINAL_CLOSE_ERROR,
};

struct pty_terminal;

/* Own one nonblocking pseudoterminal master. The device path remains stable
 * until close and is borrowed by callers. */
struct pty_terminal *pty_terminal_create(void);
void pty_terminal_destroy(struct pty_terminal *terminal);
enum pty_terminal_result pty_terminal_open(
	struct pty_terminal *terminal);
enum pty_terminal_result pty_terminal_close(
	struct pty_terminal *terminal);

bool pty_terminal_is_open(
	const struct pty_terminal *terminal);
int pty_terminal_master_fd(
	const struct pty_terminal *terminal);
const char *pty_terminal_device_path(
	const struct pty_terminal *terminal);
enum pty_terminal_result pty_terminal_get_attributes(
	struct pty_terminal *terminal, struct termios *attributes);
enum pty_terminal_result pty_terminal_read(
	struct pty_terminal *terminal, void *buffer, size_t capacity,
	size_t *bytes_read);
int pty_terminal_system_error(
	const struct pty_terminal *terminal);
const char *pty_terminal_result_name(
	enum pty_terminal_result result);

#endif
