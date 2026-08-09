// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SLMODEM_CALLS_H
#define SIPPYMODEM_SLMODEM_CALLS_H

#include "sippymodem_sip_signaling.h"

#include <stdbool.h>

struct modem;

enum sippymodem_slmodem_calls_result {
	SIPPYMODEM_SLMODEM_CALLS_OK,
	SIPPYMODEM_SLMODEM_CALLS_IDLE,
	SIPPYMODEM_SLMODEM_CALLS_INVALID_ARGUMENT,
	SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT,
	SIPPYMODEM_SLMODEM_CALLS_EVENT_OVERFLOW,
	SIPPYMODEM_SLMODEM_CALLS_MODEM_ERROR,
};

enum {
	SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY = 8,
};

struct sippymodem_slmodem_call_transition {
	enum sippymodem_sip_call_direction direction;
	enum sippymodem_sip_call_state state;
};

struct sippymodem_slmodem_calls {
	struct modem *modem;
	struct sippymodem_sip_signaling_callbacks callbacks;
	struct sippymodem_slmodem_call_transition
		pending[SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY];
	unsigned int pending_head;
	unsigned int pending_count;
	bool overflowed;
};

/* modem is borrowed and must outlive calls and its callbacks. */
bool sippymodem_slmodem_calls_init(
	struct sippymodem_slmodem_calls *calls, struct modem *modem);
const struct sippymodem_sip_signaling_callbacks *
sippymodem_slmodem_calls_callbacks(
	const struct sippymodem_slmodem_calls *calls);

/* Apply one queued event outside the signaling callback that supplied it. */
enum sippymodem_slmodem_calls_result sippymodem_slmodem_calls_drain(
	struct sippymodem_slmodem_calls *calls);
const char *sippymodem_slmodem_calls_result_name(
	enum sippymodem_slmodem_calls_result result);

#endif
