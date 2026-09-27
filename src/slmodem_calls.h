// SPDX-License-Identifier: BSD-3-Clause

#ifndef SLMODEM_CALLS_H
#define SLMODEM_CALLS_H

#include "sip_signaling.h"

#include <stdbool.h>

struct modem;

enum slmodem_calls_result {
	SLMODEM_CALLS_OK,
	SLMODEM_CALLS_IDLE,
	SLMODEM_CALLS_INVALID_ARGUMENT,
	SLMODEM_CALLS_INVALID_EVENT,
	SLMODEM_CALLS_EVENT_OVERFLOW,
	SLMODEM_CALLS_MODEM_ERROR,
};

enum {
	SLMODEM_CALL_EVENT_CAPACITY = 8,
};

struct slmodem_call_transition {
	enum sip_call_direction direction;
	enum sip_call_state state;
	unsigned int status_code;
};

struct slmodem_calls {
	struct modem *modem;
	struct sip_signaling_callbacks callbacks;
	struct slmodem_call_transition
		pending[SLMODEM_CALL_EVENT_CAPACITY];
	unsigned int pending_head;
	unsigned int pending_count;
	enum sip_call_state state;
	bool overflowed;
};

/* modem is borrowed and must outlive calls and its callbacks. */
bool slmodem_calls_init(
	struct slmodem_calls *calls, struct modem *modem);
const struct sip_signaling_callbacks *
slmodem_calls_callbacks(
	const struct slmodem_calls *calls);

/* Apply one queued event outside the signaling callback that supplied it. */
enum slmodem_calls_result slmodem_calls_drain(
	struct slmodem_calls *calls);
const char *slmodem_calls_result_name(
	enum slmodem_calls_result result);

#endif
