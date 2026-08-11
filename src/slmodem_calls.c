// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_calls.h"

#include "modem.h"

#include <string.h>

static void capture_call(
	void *context, const struct sippymodem_sip_call_event *event)
{
	struct sippymodem_slmodem_calls *calls = context;
	unsigned int tail;

	if (calls == NULL || event == NULL)
		return;
	if (calls->pending_count == SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY) {
		calls->overflowed = true;
		calls->pending_head = 0;
		calls->pending_count = 0;
		/* Never let overflow hide the terminal state. */
		if (event->state != SIPPYMODEM_SIP_CALL_IDLE)
			return;
	}
	tail = (calls->pending_head + calls->pending_count) %
	       SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY;
	calls->pending[tail].direction = event->direction;
	calls->pending[tail].state = event->state;
	calls->pending_count++;
}

bool sippymodem_slmodem_calls_init(
	struct sippymodem_slmodem_calls *calls, struct modem *modem)
{
	struct sippymodem_slmodem_calls initialized;

	if (calls == NULL || modem == NULL)
		return false;
	memset(&initialized, 0, sizeof(initialized));
	initialized.modem = modem;
	initialized.callbacks.context = calls;
	initialized.callbacks.call_changed = capture_call;
	*calls = initialized;
	return true;
}

const struct sippymodem_sip_signaling_callbacks *
sippymodem_slmodem_calls_callbacks(
	const struct sippymodem_slmodem_calls *calls)
{
	return calls != NULL && calls->callbacks.call_changed == capture_call ?
		       &calls->callbacks :
		       NULL;
}

enum sippymodem_slmodem_calls_result sippymodem_slmodem_calls_drain(
	struct sippymodem_slmodem_calls *calls)
{
	struct sippymodem_slmodem_call_transition transition;
	int result = 0;

	if (calls == NULL || calls->modem == NULL)
		return SIPPYMODEM_SLMODEM_CALLS_INVALID_ARGUMENT;
	if (calls->overflowed) {
		calls->overflowed = false;
		return SIPPYMODEM_SLMODEM_CALLS_EVENT_OVERFLOW;
	}
	if (calls->pending_count == 0)
		return SIPPYMODEM_SLMODEM_CALLS_IDLE;
	transition = calls->pending[calls->pending_head];
	calls->pending_head = (calls->pending_head + 1) %
			      SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY;
	calls->pending_count--;

	if (transition.direction != SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING &&
	    transition.direction != SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING)
		return SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT;
	switch (transition.state) {
	case SIPPYMODEM_SIP_CALL_IDLE:
		result = modem_report_remote_call_end(calls->modem);
		break;
	case SIPPYMODEM_SIP_CALL_INCOMING_PENDING:
		if (transition.direction !=
		    SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING)
			return SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT;
		result = modem_report_incoming_call(calls->modem);
		break;
	case SIPPYMODEM_SIP_CALL_CONNECTED:
		if (transition.direction ==
		    SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING)
			result = modem_report_outgoing_call_connected(calls->modem);
		else if (!calls->modem->started)
			result = modem_answer(calls->modem);
		break;
	case SIPPYMODEM_SIP_CALL_DIALING:
	case SIPPYMODEM_SIP_CALL_EARLY:
		break;
	default:
		return SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT;
	}
	return result == 0 ? SIPPYMODEM_SLMODEM_CALLS_OK :
			     SIPPYMODEM_SLMODEM_CALLS_MODEM_ERROR;
}

const char *sippymodem_slmodem_calls_result_name(
	enum sippymodem_slmodem_calls_result result)
{
	switch (result) {
	case SIPPYMODEM_SLMODEM_CALLS_OK:
		return "ok";
	case SIPPYMODEM_SLMODEM_CALLS_IDLE:
		return "idle";
	case SIPPYMODEM_SLMODEM_CALLS_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT:
		return "invalid signaling event";
	case SIPPYMODEM_SLMODEM_CALLS_EVENT_OVERFLOW:
		return "signaling event queue overflow";
	case SIPPYMODEM_SLMODEM_CALLS_MODEM_ERROR:
		return "modem call transition failed";
	}
	return "unknown modem call result";
}
