// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_calls.h"

#include "modem.h"

#include <string.h>

enum {
	SIP_STATUS_BUSY_HERE = 486,
	SIP_STATUS_BUSY_EVERYWHERE = 600,
};

static void capture_call(
	void *context, const struct sip_call_event *event)
{
	struct slmodem_calls *calls = context;
	unsigned int tail;

	if (calls == NULL || event == NULL)
		return;
	if (calls->pending_count == SLMODEM_CALL_EVENT_CAPACITY) {
		calls->overflowed = true;
		calls->pending_head = 0;
		calls->pending_count = 0;
		/* Never let overflow hide the terminal state. */
		if (event->state != SIP_CALL_IDLE)
			return;
	}
	tail = (calls->pending_head + calls->pending_count) %
	       SLMODEM_CALL_EVENT_CAPACITY;
	calls->pending[tail].direction = event->direction;
	calls->pending[tail].state = event->state;
	calls->pending[tail].status_code = event->status_code;
	calls->pending_count++;
}

static bool is_outgoing_busy(const struct slmodem_call_transition *transition)
{
	return transition->direction == SIP_CALL_DIRECTION_OUTGOING &&
	       transition->state == SIP_CALL_IDLE &&
	       (transition->status_code == SIP_STATUS_BUSY_HERE ||
		transition->status_code == SIP_STATUS_BUSY_EVERYWHERE);
}

bool slmodem_calls_init(
	struct slmodem_calls *calls, struct modem *modem)
{
	struct slmodem_calls initialized;

	if (calls == NULL || modem == NULL)
		return false;
	memset(&initialized, 0, sizeof(initialized));
	initialized.modem = modem;
	initialized.callbacks.context = calls;
	initialized.callbacks.call_changed = capture_call;
	*calls = initialized;
	return true;
}

const struct sip_signaling_callbacks *
slmodem_calls_callbacks(
	const struct slmodem_calls *calls)
{
	return calls != NULL && calls->callbacks.call_changed == capture_call ?
		       &calls->callbacks :
		       NULL;
}

enum slmodem_calls_result slmodem_calls_drain(
	struct slmodem_calls *calls)
{
	struct slmodem_call_transition transition;
	int result = 0;

	if (calls == NULL || calls->modem == NULL)
		return SLMODEM_CALLS_INVALID_ARGUMENT;
	if (calls->overflowed) {
		calls->overflowed = false;
		return SLMODEM_CALLS_EVENT_OVERFLOW;
	}
	if (calls->pending_count == 0)
		return SLMODEM_CALLS_IDLE;
	transition = calls->pending[calls->pending_head];
	calls->pending_head = (calls->pending_head + 1) %
			      SLMODEM_CALL_EVENT_CAPACITY;
	calls->pending_count--;

	if (transition.direction != SIP_CALL_DIRECTION_INCOMING &&
	    transition.direction != SIP_CALL_DIRECTION_OUTGOING)
		return SLMODEM_CALLS_INVALID_EVENT;
	switch (transition.state) {
	case SIP_CALL_IDLE:
		if (is_outgoing_busy(&transition))
			modem_update_status(calls->modem, STATUS_BUSY);
		result = modem_report_remote_call_end(calls->modem);
		break;
	case SIP_CALL_INCOMING_PENDING:
		if (transition.direction !=
		    SIP_CALL_DIRECTION_INCOMING)
			return SLMODEM_CALLS_INVALID_EVENT;
		result = modem_report_incoming_call(calls->modem);
		break;
	case SIP_CALL_CONNECTED:
		if (transition.direction ==
		    SIP_CALL_DIRECTION_OUTGOING)
			result = modem_report_outgoing_call_connected(calls->modem);
		else if (!calls->modem->started)
			result = modem_answer(calls->modem);
		break;
	case SIP_CALL_DIALING:
	case SIP_CALL_EARLY:
		break;
	default:
		return SLMODEM_CALLS_INVALID_EVENT;
	}
	return result == 0 ? SLMODEM_CALLS_OK :
			     SLMODEM_CALLS_MODEM_ERROR;
}

const char *slmodem_calls_result_name(
	enum slmodem_calls_result result)
{
	switch (result) {
	case SLMODEM_CALLS_OK:
		return "ok";
	case SLMODEM_CALLS_IDLE:
		return "idle";
	case SLMODEM_CALLS_INVALID_ARGUMENT:
		return "invalid argument";
	case SLMODEM_CALLS_INVALID_EVENT:
		return "invalid signaling event";
	case SLMODEM_CALLS_EVENT_OVERFLOW:
		return "signaling event queue overflow";
	case SLMODEM_CALLS_MODEM_ERROR:
		return "modem call transition failed";
	}
	return "unknown modem call result";
}
