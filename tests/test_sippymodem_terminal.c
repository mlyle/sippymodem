// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_terminal.h>

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
	struct sippymodem_terminal *terminal;
	struct termios attributes;
	char buffer[16];
	const char *device_path;
	int master_fd;
	int device_fd;
	int flags;
	int descriptor_flags;
	ssize_t count;

	terminal = sippymodem_terminal_create();
	CHECK(terminal != NULL && !sippymodem_terminal_is_open(terminal) &&
		      sippymodem_terminal_master_fd(terminal) == -1 &&
		      sippymodem_terminal_device_path(terminal) == NULL,
	      "new terminal was not closed");
	CHECK(sippymodem_terminal_open(terminal) == SIPPYMODEM_TERMINAL_OK &&
		      sippymodem_terminal_is_open(terminal),
	      "terminal did not open: %s",
	      strerror(sippymodem_terminal_system_error(terminal)));
	master_fd = sippymodem_terminal_master_fd(terminal);
	device_path = sippymodem_terminal_device_path(terminal);
	flags = fcntl(master_fd, F_GETFL);
	descriptor_flags = fcntl(master_fd, F_GETFD);
	CHECK(master_fd >= 0 && device_path != NULL && device_path[0] == '/' &&
		      flags >= 0 && (flags & O_NONBLOCK) != 0 &&
		      descriptor_flags >= 0 &&
		      (descriptor_flags & FD_CLOEXEC) != 0 &&
		      sippymodem_terminal_get_attributes(terminal, &attributes) ==
			      SIPPYMODEM_TERMINAL_OK &&
		      cfgetispeed(&attributes) == B115200 &&
		      cfgetospeed(&attributes) == B115200 &&
		      (attributes.c_lflag & (ECHO | ICANON | ISIG)) == 0,
	      "terminal attributes or descriptor flags were incorrect");

	device_fd = open(device_path, O_RDWR | O_NOCTTY);
	CHECK(device_fd >= 0, "terminal device did not open: %s", strerror(errno));
	count = write(device_fd, from_client, sizeof(from_client) - 1);
	CHECK(count == (ssize_t)sizeof(from_client) - 1 &&
		      wait_readable(master_fd),
	      "client bytes did not reach the modem endpoint");
	count = read(master_fd, buffer, sizeof(buffer));
	CHECK(count == (ssize_t)sizeof(from_client) - 1 &&
		      memcmp(buffer, from_client, (size_t)count) == 0,
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
	CHECK(sippymodem_terminal_open(terminal) ==
			      SIPPYMODEM_TERMINAL_ALREADY_OPEN &&
		      sippymodem_terminal_close(terminal) ==
			      SIPPYMODEM_TERMINAL_OK &&
		      sippymodem_terminal_close(terminal) ==
			      SIPPYMODEM_TERMINAL_OK &&
		      !sippymodem_terminal_is_open(terminal),
	      "terminal open and close were not idempotent");
	sippymodem_terminal_destroy(terminal);
	return true;
}

static bool test_rejects_invalid_operations(void)
{
	struct sippymodem_terminal *terminal = sippymodem_terminal_create();
	struct termios attributes;

	CHECK(terminal != NULL &&
		      sippymodem_terminal_open(NULL) ==
			      SIPPYMODEM_TERMINAL_INVALID_ARGUMENT &&
		      sippymodem_terminal_close(NULL) ==
			      SIPPYMODEM_TERMINAL_INVALID_ARGUMENT &&
		      sippymodem_terminal_get_attributes(NULL, &attributes) ==
			      SIPPYMODEM_TERMINAL_INVALID_ARGUMENT &&
		      sippymodem_terminal_get_attributes(terminal, NULL) ==
			      SIPPYMODEM_TERMINAL_INVALID_ARGUMENT &&
		      sippymodem_terminal_get_attributes(terminal, &attributes) ==
			      SIPPYMODEM_TERMINAL_NOT_OPEN &&
		      sippymodem_terminal_system_error(NULL) == EINVAL &&
		      strcmp(sippymodem_terminal_result_name(
			     SIPPYMODEM_TERMINAL_CONFIGURATION_ERROR),
			     "could not configure terminal") == 0,
	      "terminal invalid-operation diagnostics were incomplete");
	sippymodem_terminal_destroy(terminal);
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
