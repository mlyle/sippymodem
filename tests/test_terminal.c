// SPDX-License-Identifier: BSD-3-Clause

#include <terminal.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static bool wait_readable(int fd)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };

	return poll(&descriptor, 1, 1000) == 1 &&
	       (descriptor.revents & POLLIN) != 0;
}

static bool test_opens_raw_terminal_and_moves_bytes(void)
{
	static const char from_client[] = "AT\r";
	static const char from_modem[] = "OK\r\n";
	struct pty_terminal *terminal;
	struct termios attributes;
	char buffer[16];
	const char *device_path;
	int master_fd;
	int device_fd;
	int flags;
	int descriptor_flags;
	ssize_t count;
	size_t bytes_read = 99;

	terminal = pty_terminal_create();
	CHECK(terminal != NULL && !pty_terminal_is_open(terminal) &&
		      pty_terminal_master_fd(terminal) == -1 &&
		      pty_terminal_device_path(terminal) == NULL,
	      "new terminal was not closed");
	CHECK(pty_terminal_open(terminal) == PTY_TERMINAL_OK &&
		      pty_terminal_is_open(terminal),
	      "terminal did not open: %s",
	      strerror(pty_terminal_system_error(terminal)));
	master_fd = pty_terminal_master_fd(terminal);
	device_path = pty_terminal_device_path(terminal);
	flags = fcntl(master_fd, F_GETFL);
	descriptor_flags = fcntl(master_fd, F_GETFD);
	CHECK(master_fd >= 0 && device_path != NULL && device_path[0] == '/' &&
		      flags >= 0 && (flags & O_NONBLOCK) != 0 &&
		      descriptor_flags >= 0 &&
		      (descriptor_flags & FD_CLOEXEC) != 0 &&
		      pty_terminal_get_attributes(terminal, &attributes) ==
			      PTY_TERMINAL_OK &&
		      cfgetispeed(&attributes) == B115200 &&
		      cfgetospeed(&attributes) == B115200 &&
		      (attributes.c_lflag & (ECHO | ICANON | ISIG)) == 0,
	      "terminal attributes or descriptor flags were incorrect");

	device_fd = open(device_path, O_RDWR | O_NOCTTY);
	CHECK(device_fd >= 0, "terminal device did not open: %s", strerror(errno));
	CHECK(pty_terminal_read(
		      terminal, buffer, sizeof(buffer), &bytes_read) ==
		      PTY_TERMINAL_IDLE &&
		      bytes_read == 0,
	      "empty terminal input did not report idle");
	count = write(device_fd, from_client, sizeof(from_client) - 1);
	CHECK(count == (ssize_t)sizeof(from_client) - 1 &&
		      wait_readable(master_fd),
	      "client bytes did not reach the modem endpoint");
	CHECK(pty_terminal_read(
		      terminal, buffer, sizeof(buffer), &bytes_read) ==
		      PTY_TERMINAL_OK &&
		      bytes_read == sizeof(from_client) - 1 &&
		      memcmp(buffer, from_client, bytes_read) == 0,
	      "client bytes changed in transit");
	count = write(master_fd, from_modem, sizeof(from_modem) - 1);
	CHECK(count == (ssize_t)sizeof(from_modem) - 1 &&
		      wait_readable(device_fd),
	      "modem bytes did not reach the client endpoint");
	count = read(device_fd, buffer, sizeof(buffer));
	CHECK(count == (ssize_t)sizeof(from_modem) - 1 &&
		      memcmp(buffer, from_modem, (size_t)count) == 0,
	      "modem bytes changed in transit");

	close(device_fd);
	CHECK(pty_terminal_read(
		      terminal, buffer, sizeof(buffer), &bytes_read) ==
			      PTY_TERMINAL_CLIENT_DISCONNECTED &&
		      bytes_read == 0 &&
		      pty_terminal_open(terminal) ==
			      PTY_TERMINAL_ALREADY_OPEN &&
		      pty_terminal_close(terminal) ==
			      PTY_TERMINAL_OK &&
		      pty_terminal_close(terminal) ==
			      PTY_TERMINAL_OK &&
		      !pty_terminal_is_open(terminal),
	      "terminal open and close were not idempotent");
	pty_terminal_destroy(terminal);
	return true;
}

static bool test_rejects_invalid_operations(void)
{
	struct pty_terminal *terminal = pty_terminal_create();
	struct termios attributes;

	CHECK(terminal != NULL &&
		      pty_terminal_open(NULL) ==
			      PTY_TERMINAL_INVALID_ARGUMENT &&
		      pty_terminal_close(NULL) ==
			      PTY_TERMINAL_INVALID_ARGUMENT &&
		      pty_terminal_get_attributes(NULL, &attributes) ==
			      PTY_TERMINAL_INVALID_ARGUMENT &&
		      pty_terminal_get_attributes(terminal, NULL) ==
			      PTY_TERMINAL_INVALID_ARGUMENT &&
		      pty_terminal_get_attributes(terminal, &attributes) ==
			      PTY_TERMINAL_NOT_OPEN &&
		      pty_terminal_read(terminal, NULL, 1, NULL) ==
			      PTY_TERMINAL_INVALID_ARGUMENT &&
		      pty_terminal_read(terminal, NULL, 0, NULL) ==
			      PTY_TERMINAL_NOT_OPEN &&
		      pty_terminal_system_error(NULL) == EINVAL &&
		      strcmp(pty_terminal_result_name(
			     PTY_TERMINAL_CONFIGURATION_ERROR),
			     "could not configure terminal") == 0,
	      "terminal invalid-operation diagnostics were incomplete");
	pty_terminal_destroy(terminal);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "raw byte path", test_opens_raw_terminal_and_moves_bytes },
		{ "invalid operations", test_rejects_invalid_operations },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
