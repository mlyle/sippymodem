/*
 * Copyright (c) 2002, Smart Link Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Smart Link Ltd. nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SIPPYMODEM_TERMINAL_PATH_MAX = 128 };

struct sippymodem_terminal {
	int master_fd;
	int system_error;
	char device_path[SIPPYMODEM_TERMINAL_PATH_MAX];
};

static int set_close_on_exec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0 ? 0 : -1;
}

static int make_raw_115200(struct termios *attributes)
{
	attributes->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
				       IGNCR | ICRNL | IXON);
	attributes->c_oflag &= ~OPOST;
	attributes->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	attributes->c_cflag &= ~(CSIZE | PARENB);
	attributes->c_cflag |= CS8 | CLOCAL | CREAD;
	attributes->c_cc[VMIN] = 1;
	attributes->c_cc[VTIME] = 0;
	return cfsetispeed(attributes, B115200) == 0 &&
		       cfsetospeed(attributes, B115200) == 0 ?
		       0 :
		       -1;
}

static enum sippymodem_terminal_result fail_open(
	struct sippymodem_terminal *terminal, int master_fd, int device_fd,
	enum sippymodem_terminal_result result, int system_error)
{
	if (device_fd >= 0)
		(void)close(device_fd);
	if (master_fd >= 0)
		(void)close(master_fd);
	terminal->device_path[0] = '\0';
	terminal->system_error = system_error;
	errno = system_error;
	return result;
}

struct sippymodem_terminal *sippymodem_terminal_create(void)
{
	struct sippymodem_terminal *terminal = calloc(1, sizeof(*terminal));

	if (terminal != NULL)
		terminal->master_fd = -1;
	return terminal;
}

enum sippymodem_terminal_result sippymodem_terminal_open(
	struct sippymodem_terminal *terminal)
{
	struct termios attributes;
	int master_fd;
	int device_fd = -1;
	int name_error;

	if (terminal == NULL)
		return SIPPYMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->master_fd >= 0)
		return SIPPYMODEM_TERMINAL_ALREADY_OPEN;
	terminal->system_error = 0;
	master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (master_fd < 0)
		return fail_open(terminal, -1, -1,
			SIPPYMODEM_TERMINAL_OPEN_ERROR, errno);
	if (set_close_on_exec(master_fd) != 0 || grantpt(master_fd) != 0 ||
	    unlockpt(master_fd) != 0)
		return fail_open(terminal, master_fd, -1,
			SIPPYMODEM_TERMINAL_OPEN_ERROR, errno);
	name_error = ptsname_r(master_fd, terminal->device_path,
			       sizeof(terminal->device_path));
	if (name_error != 0)
		return fail_open(terminal, master_fd, -1,
			SIPPYMODEM_TERMINAL_NAME_ERROR, name_error);
	device_fd = open(terminal->device_path, O_RDWR | O_NOCTTY);
	if (device_fd < 0 || set_close_on_exec(device_fd) != 0 ||
	    tcgetattr(device_fd, &attributes) != 0)
		return fail_open(terminal, master_fd, device_fd,
			SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR, errno);
	if (make_raw_115200(&attributes) != 0 ||
	    tcsetattr(device_fd, TCSANOW, &attributes) != 0)
		return fail_open(terminal, master_fd, device_fd,
			SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR, errno);
	if (close(device_fd) != 0)
		return fail_open(terminal, master_fd, -1,
			SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR, errno);
	terminal->master_fd = master_fd;
	return SIPPYMODEM_TERMINAL_OK;
}

enum sippymodem_terminal_result sippymodem_terminal_close(
	struct sippymodem_terminal *terminal)
{
	int master_fd;

	if (terminal == NULL)
		return SIPPYMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->master_fd < 0)
		return SIPPYMODEM_TERMINAL_OK;
	master_fd = terminal->master_fd;
	terminal->master_fd = -1;
	terminal->device_path[0] = '\0';
	if (close(master_fd) != 0) {
		terminal->system_error = errno;
		return SIPPYMODEM_TERMINAL_CLOSE_ERROR;
	}
	terminal->system_error = 0;
	return SIPPYMODEM_TERMINAL_OK;
}

void sippymodem_terminal_destroy(struct sippymodem_terminal *terminal)
{
	if (terminal == NULL)
		return;
	(void)sippymodem_terminal_close(terminal);
	free(terminal);
}

bool sippymodem_terminal_is_open(
	const struct sippymodem_terminal *terminal)
{
	return terminal != NULL && terminal->master_fd >= 0;
}

int sippymodem_terminal_master_fd(
	const struct sippymodem_terminal *terminal)
{
	return terminal != NULL ? terminal->master_fd : -1;
}

const char *sippymodem_terminal_device_path(
	const struct sippymodem_terminal *terminal)
{
	return terminal != NULL && terminal->master_fd >= 0 ?
		       terminal->device_path :
		       NULL;
}

enum sippymodem_terminal_result sippymodem_terminal_get_attributes(
	struct sippymodem_terminal *terminal, struct termios *attributes)
{
	if (terminal == NULL || attributes == NULL)
		return SIPPYMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->master_fd < 0)
		return SIPPYMODEM_TERMINAL_NOT_OPEN;
	if (tcgetattr(terminal->master_fd, attributes) != 0) {
		terminal->system_error = errno;
		return SIPPYMODEM_TERMINAL_ATTRIBUTE_ERROR;
	}
	terminal->system_error = 0;
	return SIPPYMODEM_TERMINAL_OK;
}

enum sippymodem_terminal_result sippymodem_terminal_read(
	struct sippymodem_terminal *terminal, void *buffer, size_t capacity,
	size_t *bytes_read)
{
	ssize_t count;

	if (bytes_read != NULL)
		*bytes_read = 0;
	if (terminal == NULL || (buffer == NULL && capacity != 0) ||
	    capacity > (size_t)SSIZE_MAX)
		return SIPPYMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->master_fd < 0)
		return SIPPYMODEM_TERMINAL_NOT_OPEN;
	if (capacity == 0)
		return SIPPYMODEM_TERMINAL_IDLE;
	count = read(terminal->master_fd, buffer, capacity);
	if (count > 0) {
		if (bytes_read != NULL)
			*bytes_read = (size_t)count;
		terminal->system_error = 0;
		return SIPPYMODEM_TERMINAL_OK;
	}
	if (count == 0 || errno == EIO) {
		terminal->system_error = 0;
		return SIPPYMODEM_TERMINAL_CLIENT_DISCONNECTED;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		terminal->system_error = 0;
		return SIPPYMODEM_TERMINAL_IDLE;
	}
	terminal->system_error = errno;
	return SIPPYMODEM_TERMINAL_READ_ERROR;
}

int sippymodem_terminal_system_error(
	const struct sippymodem_terminal *terminal)
{
	return terminal != NULL ? terminal->system_error : EINVAL;
}

const char *sippymodem_terminal_result_name(
	enum sippymodem_terminal_result result)
{
	switch (result) {
	case SIPPYMODEM_TERMINAL_OK:
		return "ok";
	case SIPPYMODEM_TERMINAL_IDLE:
		return "idle";
	case SIPPYMODEM_TERMINAL_CLIENT_DISCONNECTED:
		return "terminal client disconnected";
	case SIPPYMODEM_TERMINAL_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_TERMINAL_ALREADY_OPEN:
		return "terminal is already open";
	case SIPPYMODEM_TERMINAL_NOT_OPEN:
		return "terminal is not open";
	case SIPPYMODEM_TERMINAL_OPEN_ERROR:
		return "could not open pseudoterminal";
	case SIPPYMODEM_TERMINAL_NAME_ERROR:
		return "could not resolve terminal device path";
	case SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR:
		return "could not configure terminal";
	case SIPPYMODEM_TERMINAL_ATTRIBUTE_ERROR:
		return "could not read terminal attributes";
	case SIPPYMODEM_TERMINAL_READ_ERROR:
		return "could not read terminal input";
	case SIPPYMODEM_TERMINAL_CLOSE_ERROR:
		return "could not close terminal";
	}
	return "unknown terminal result";
}
