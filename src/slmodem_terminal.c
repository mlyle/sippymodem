// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_terminal.h"

#include "modem.h"

#include <limits.h>
#include <string.h>

static bool attributes_equal(
	const struct termios *left, const struct termios *right)
{
	return left->c_iflag == right->c_iflag &&
	       left->c_oflag == right->c_oflag &&
	       left->c_cflag == right->c_cflag &&
	       left->c_lflag == right->c_lflag &&
	       memcmp(left->c_cc, right->c_cc, sizeof(left->c_cc)) == 0 &&
	       cfgetispeed(left) == cfgetispeed(right) &&
	       cfgetospeed(left) == cfgetospeed(right);
}

bool slmodem_terminal_init(
	struct slmodem_terminal *terminal, struct modem *modem)
{
	if (terminal == NULL)
		return false;
	terminal->modem = NULL;
	if (modem == NULL)
		return false;
	terminal->modem = modem;
	modem->pty = -1;
	modem->pty_name = NULL;
	return true;
}

enum slmodem_terminal_result
slmodem_terminal_bind(
	struct slmodem_terminal *terminal, int file_descriptor,
	const char *device_path)
{
	if (terminal == NULL || terminal->modem == NULL || file_descriptor < 0 ||
	    device_path == NULL || device_path[0] == '\0')
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->modem->pty >= 0 || terminal->modem->pty_name != NULL)
		return SLMODEM_TERMINAL_INVALID_STATE;
	terminal->modem->pty = file_descriptor;
	terminal->modem->pty_name = device_path;
	return SLMODEM_TERMINAL_OK;
}

enum slmodem_terminal_result
slmodem_terminal_unbind(
	struct slmodem_terminal *terminal)
{
	if (terminal == NULL || terminal->modem == NULL)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->modem->pty < 0 && terminal->modem->pty_name == NULL)
		return SLMODEM_TERMINAL_IDLE;
	terminal->modem->pty = -1;
	terminal->modem->pty_name = NULL;
	return SLMODEM_TERMINAL_OK;
}

enum slmodem_terminal_result
slmodem_terminal_apply_attributes(
	struct slmodem_terminal *terminal,
	const struct termios *attributes)
{
	if (terminal == NULL || terminal->modem == NULL || attributes == NULL)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (attributes_equal(attributes, &terminal->modem->termios))
		return SLMODEM_TERMINAL_IDLE;
	modem_update_termios(terminal->modem, attributes);
	return SLMODEM_TERMINAL_OK;
}

enum slmodem_terminal_result
slmodem_terminal_service_event(
	struct slmodem_terminal *terminal)
{
	if (terminal == NULL || terminal->modem == NULL)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->modem->event == 0)
		return SLMODEM_TERMINAL_IDLE;
	modem_event(terminal->modem);
	return SLMODEM_TERMINAL_OK;
}

enum slmodem_terminal_result
slmodem_terminal_input_capacity(
	const struct slmodem_terminal *terminal, size_t *capacity)
{
	if (capacity != NULL)
		*capacity = 0;
	if (terminal == NULL || terminal->modem == NULL || capacity == NULL)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (terminal->modem->xmit.count < 0 ||
	    (unsigned int)terminal->modem->xmit.count >
		    terminal->modem->xmit.size)
		return SLMODEM_TERMINAL_INVALID_STATE;
	*capacity = terminal->modem->xmit.size -
		    (unsigned int)terminal->modem->xmit.count;
	return SLMODEM_TERMINAL_OK;
}

enum slmodem_terminal_result
slmodem_terminal_receive(
	struct slmodem_terminal *terminal, const void *bytes,
	size_t count)
{
	size_t capacity;
	int accepted;

	if (terminal == NULL || terminal->modem == NULL ||
	    (bytes == NULL && count != 0) || count > (size_t)INT_MAX)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if (count == 0)
		return SLMODEM_TERMINAL_IDLE;
	if (slmodem_terminal_input_capacity(terminal, &capacity) !=
		    SLMODEM_TERMINAL_OK ||
	    count > capacity)
		return SLMODEM_TERMINAL_INVALID_STATE;
	accepted = modem_write(terminal->modem, bytes, (int)count);
	return accepted >= 0 && (size_t)accepted == count ?
		       SLMODEM_TERMINAL_OK :
		       SLMODEM_TERMINAL_INPUT_ERROR;
}

enum slmodem_terminal_result
slmodem_terminal_disconnect(
	struct slmodem_terminal *terminal)
{
	if (terminal == NULL || terminal->modem == NULL)
		return SLMODEM_TERMINAL_INVALID_ARGUMENT;
	if ((terminal->modem->termios.c_cflag & HUPCL) == 0)
		return SLMODEM_TERMINAL_IDLE;
	modem_hangup(terminal->modem);
	return SLMODEM_TERMINAL_OK;
}

const char *slmodem_terminal_result_name(
	enum slmodem_terminal_result result)
{
	switch (result) {
	case SLMODEM_TERMINAL_OK:
		return "ok";
	case SLMODEM_TERMINAL_IDLE:
		return "idle";
	case SLMODEM_TERMINAL_INVALID_ARGUMENT:
		return "invalid argument";
	case SLMODEM_TERMINAL_INVALID_STATE:
		return "invalid modem terminal state";
	case SLMODEM_TERMINAL_INPUT_ERROR:
		return "modem rejected terminal input";
	}
	return "unknown modem terminal result";
}
