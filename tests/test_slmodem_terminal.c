// SPDX-License-Identifier: BSD-3-Clause

#include <slmodem_terminal.h>

#include <modem.h>

#include "test.h"

#include <string.h>

struct capture {
	unsigned int attribute_updates;
	unsigned int events;
	unsigned int hangups;
	bool short_write;
	char input[16];
	size_t input_count;
};

static struct capture capture;

void modem_update_termios(
	struct modem *modem, const struct termios *attributes)
{
	capture.attribute_updates++;
	modem->termios = *attributes;
}

void modem_event(struct modem *modem)
{
	capture.events++;
	modem->event = 0;
}

void modem_hangup(struct modem *modem)
{
	(void)modem;
	capture.hangups++;
}

int modem_write(struct modem *modem, const char *bytes, int count)
{
	(void)modem;
	if (count < 0 || (size_t)count > sizeof(capture.input))
		return -1;
	memcpy(capture.input, bytes, (size_t)count);
	capture.input_count = (size_t)count;
	return capture.short_write && count > 0 ? count - 1 : count;
}

static bool test_applies_control_state(void)
{
	struct sippymodem_slmodem_terminal terminal;
	struct modem modem;
	struct termios attributes;

	memset(&capture, 0, sizeof(capture));
	memset(&modem, 0, sizeof(modem));
	memset(&attributes, 0, sizeof(attributes));
	CHECK(sippymodem_slmodem_terminal_init(&terminal, &modem),
	      "terminal adapter did not initialize");
	CHECK(sippymodem_slmodem_terminal_bind(
		      &terminal, 17, "/dev/pts/test") ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      modem.pty == 17 &&
		      strcmp(modem.pty_name, "/dev/pts/test") == 0 &&
		      sippymodem_slmodem_terminal_bind(
			      &terminal, 18, "/dev/pts/other") ==
			      SIPPYMODEM_SLMODEM_TERMINAL_INVALID_STATE &&
		      sippymodem_slmodem_terminal_unbind(&terminal) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      modem.pty == -1 && modem.pty_name == NULL &&
		      sippymodem_slmodem_terminal_unbind(&terminal) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_IDLE,
	      "terminal endpoint binding was not guarded");
	CHECK(sippymodem_slmodem_terminal_apply_attributes(
		      &terminal, &attributes) ==
		      SIPPYMODEM_SLMODEM_TERMINAL_IDLE,
	      "unchanged attributes did not remain idle");
	attributes.c_lflag = ECHO;
	CHECK(sippymodem_slmodem_terminal_apply_attributes(
		      &terminal, &attributes) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      capture.attribute_updates == 1 && modem.termios.c_lflag == ECHO,
	      "changed attributes did not reach the modem");
	CHECK(sippymodem_slmodem_terminal_service_event(&terminal) ==
		      SIPPYMODEM_SLMODEM_TERMINAL_IDLE,
	      "empty modem event did not remain idle");
	modem.event = 1;
	CHECK(sippymodem_slmodem_terminal_service_event(&terminal) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      capture.events == 1 && modem.event == 0,
	      "pending modem event was not serviced once");
	CHECK(sippymodem_slmodem_terminal_disconnect(&terminal) ==
		      SIPPYMODEM_SLMODEM_TERMINAL_IDLE &&
		      capture.hangups == 0,
	      "disconnect ignored terminal HUPCL state");
	modem.termios.c_cflag |= HUPCL;
	CHECK(sippymodem_slmodem_terminal_disconnect(&terminal) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      capture.hangups == 1,
	      "HUPCL disconnect did not reach the modem");
	return true;
}

static bool test_bounds_terminal_input(void)
{
	static const char command[] = "AT\r";
	struct sippymodem_slmodem_terminal terminal;
	struct modem modem;
	size_t capacity = 99;

	memset(&capture, 0, sizeof(capture));
	memset(&modem, 0, sizeof(modem));
	modem.xmit.size = 8;
	modem.xmit.count = 3;
	CHECK(sippymodem_slmodem_terminal_init(&terminal, &modem) &&
		      sippymodem_slmodem_terminal_input_capacity(
			      &terminal, &capacity) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      capacity == 5 &&
		      sippymodem_slmodem_terminal_receive(
			      &terminal, command, sizeof(command) - 1) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_OK &&
		      capture.input_count == sizeof(command) - 1 &&
		      memcmp(capture.input, command, sizeof(command) - 1) == 0,
	      "bounded terminal input did not reach the modem exactly");
	capture.short_write = true;
	CHECK(sippymodem_slmodem_terminal_receive(
		      &terminal, command, sizeof(command) - 1) ==
		      SIPPYMODEM_SLMODEM_TERMINAL_INPUT_ERROR,
	      "partially accepted terminal input was hidden");
	modem.xmit.count = 6;
	CHECK(sippymodem_slmodem_terminal_receive(
		      &terminal, command, sizeof(command) - 1) ==
		      SIPPYMODEM_SLMODEM_TERMINAL_INVALID_STATE,
	      "input larger than remaining capacity was accepted");
	modem.xmit.count = 9;
	capacity = 99;
	CHECK(sippymodem_slmodem_terminal_input_capacity(&terminal, &capacity) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_INVALID_STATE &&
		      capacity == 0,
	      "corrupt modem input state was not rejected");
	return true;
}

static bool test_rejects_invalid_arguments(void)
{
	struct sippymodem_slmodem_terminal terminal = { 0 };
	struct modem modem;
	size_t capacity;

	memset(&modem, 0, sizeof(modem));
	CHECK(!sippymodem_slmodem_terminal_init(NULL, &modem) &&
		      !sippymodem_slmodem_terminal_init(&terminal, NULL) &&
		      sippymodem_slmodem_terminal_input_capacity(
			      &terminal, &capacity) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_INVALID_ARGUMENT &&
		      sippymodem_slmodem_terminal_receive(&terminal, NULL, 1) ==
			      SIPPYMODEM_SLMODEM_TERMINAL_INVALID_ARGUMENT &&
		      strcmp(sippymodem_slmodem_terminal_result_name(
			     SIPPYMODEM_SLMODEM_TERMINAL_INPUT_ERROR),
			     "modem rejected terminal input") == 0,
	      "terminal adapter invalid-argument diagnostics were incomplete");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "control state", test_applies_control_state },
		{ "bounded input", test_bounds_terminal_input },
		{ "invalid arguments", test_rejects_invalid_arguments },
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
