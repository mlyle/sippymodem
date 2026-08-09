// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_TERMINAL_H
#define SIPPYMODEM_TERMINAL_H

#include <stdbool.h>
#include <termios.h>

enum sippymodem_terminal_result {
	SIPPYMODEM_TERMINAL_OK,
	SIPPYMODEM_TERMINAL_INVALID_ARGUMENT,
	SIPPYMODEM_TERMINAL_ALREADY_OPEN,
	SIPPYMODEM_TERMINAL_NOT_OPEN,
	SIPPYMODEM_TERMINAL_OPEN_ERROR,
	SIPPYMODEM_TERMINAL_NAME_ERROR,
	SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR,
	SIPPYMODEM_TERMINAL_ATTRIBUTE_ERROR,
	SIPPYMODEM_TERMINAL_CLOSE_ERROR,
};

struct sippymodem_terminal;

/* Own one nonblocking pseudoterminal master. The device path remains stable
 * until close and is borrowed by callers. */
struct sippymodem_terminal *sippymodem_terminal_create(void);
void sippymodem_terminal_destroy(struct sippymodem_terminal *terminal);
enum sippymodem_terminal_result sippymodem_terminal_open(
	struct sippymodem_terminal *terminal);
enum sippymodem_terminal_result sippymodem_terminal_close(
	struct sippymodem_terminal *terminal);

bool sippymodem_terminal_is_open(
	const struct sippymodem_terminal *terminal);
int sippymodem_terminal_master_fd(
	const struct sippymodem_terminal *terminal);
const char *sippymodem_terminal_device_path(
	const struct sippymodem_terminal *terminal);
enum sippymodem_terminal_result sippymodem_terminal_get_attributes(
	struct sippymodem_terminal *terminal, struct termios *attributes);
int sippymodem_terminal_system_error(
	const struct sippymodem_terminal *terminal);
const char *sippymodem_terminal_result_name(
	enum sippymodem_terminal_result result);

#endif
