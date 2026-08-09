// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_slmodem_calls.h>

#include <modem.h>

#include "test.h"

#include <string.h>

struct transition_capture {
	unsigned int incoming_calls;
	unsigned int outgoing_connected_calls;
	unsigned int remote_end_calls;
	unsigned int answers;
	int incoming_result;
	int outgoing_connected_result;
	int remote_end_result;
	int answer_result;
	struct sippymodem_slmodem_calls *emit_during_incoming;
};

static struct transition_capture capture;

static void emit_call(
	struct sippymodem_slmodem_calls *calls,
	enum sippymodem_sip_call_direction direction,
	enum sippymodem_sip_call_state state)
{
	const struct sippymodem_sip_call_event event = {
		.direction = direction,
		.state = state,
	};
	const struct sippymodem_sip_signaling_callbacks *callbacks =
		sippymodem_slmodem_calls_callbacks(calls);

	callbacks->call_changed(callbacks->context, &event);
}

int modem_report_incoming_call(struct modem *modem)
{
	(void)modem;
	capture.incoming_calls++;
	if (capture.emit_during_incoming != NULL)
		emit_call(capture.emit_during_incoming,
			  SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
			  SIPPYMODEM_SIP_CALL_CONNECTED);
	return capture.incoming_result;
}

int modem_report_outgoing_call_connected(struct modem *modem)
{
	(void)modem;
	capture.outgoing_connected_calls++;
	return capture.outgoing_connected_result;
}

int modem_report_remote_call_end(struct modem *modem)
{
	(void)modem;
	capture.remote_end_calls++;
	return capture.remote_end_result;
}

int modem_answer(struct modem *modem)
{
	(void)modem;
	capture.answers++;
	return capture.answer_result;
}

static bool test_preserves_event_order(void)
{
	struct modem modem;
	struct sippymodem_slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_slmodem_calls_init(&calls, &modem),
	      "call latch initialization failed");
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_EARLY);
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	CHECK(capture.outgoing_connected_calls == 0,
	      "call transition ran inside its signaling callback");
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 0 &&
		      sippymodem_slmodem_calls_drain(&calls) ==
			      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 1 &&
		      sippymodem_slmodem_calls_drain(&calls) ==
			      SIPPYMODEM_SLMODEM_CALLS_IDLE,
	      "outgoing call states were not drained in order");

	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_IDLE);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 2 &&
		      capture.remote_end_calls == 0 &&
		      sippymodem_slmodem_calls_drain(&calls) ==
			      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.remote_end_calls == 1,
	      "terminal call state overtook an earlier transition");
	return true;
}

static bool test_maps_incoming_lifecycle(void)
{
	struct modem modem;
	struct sippymodem_slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_slmodem_calls_init(&calls, &modem),
	      "incoming call fixture failed");
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
		  SIPPYMODEM_SIP_CALL_INCOMING_PENDING);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.incoming_calls == 1,
	      "incoming offer was not reported to the modem");

	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "signaling-side automatic answer did not start the modem");
	modem.started = 1;
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "connected modem was answered twice");
	return true;
}

static bool test_preserves_events_raised_while_draining(void)
{
	struct modem modem;
	struct sippymodem_slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_slmodem_calls_init(&calls, &modem),
	      "reentrant call fixture failed");
	capture.emit_during_incoming = &calls;
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING,
		  SIPPYMODEM_SIP_CALL_INCOMING_PENDING);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.incoming_calls == 1 && capture.answers == 0,
	      "incoming offer drain failed");
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "call event raised during a transition was lost");
	return true;
}

static bool test_rejects_invalid_or_failed_transitions(void)
{
	struct modem modem;
	struct sippymodem_slmodem_calls calls = { 0 };
	struct sippymodem_slmodem_calls original = calls;
	const struct sippymodem_sip_signaling_callbacks *callbacks;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(!sippymodem_slmodem_calls_init(NULL, &modem) &&
		      !sippymodem_slmodem_calls_init(&calls, NULL) &&
		      calls.modem == original.modem &&
		      calls.callbacks.call_changed ==
			      original.callbacks.call_changed &&
		      sippymodem_slmodem_calls_callbacks(NULL) == NULL &&
		      sippymodem_slmodem_calls_drain(NULL) ==
			      SIPPYMODEM_SLMODEM_CALLS_INVALID_ARGUMENT,
	      "call latch accepted incomplete ownership");
	CHECK(sippymodem_slmodem_calls_init(&calls, &modem),
	      "invalid transition fixture failed");
	callbacks = sippymodem_slmodem_calls_callbacks(&calls);
	callbacks->call_changed(callbacks->context, NULL);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_IDLE,
	      "null signaling event changed call state");

	emit_call(&calls, (enum sippymodem_sip_call_direction)99,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT,
	      "invalid call direction reached the modem");
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_INCOMING_PENDING);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT,
	      "outgoing call was accepted as an incoming offer");

	capture.outgoing_connected_result = -1;
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_CONNECTED);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_MODEM_ERROR,
	      "failed modem transition was hidden");
	CHECK(strcmp(sippymodem_slmodem_calls_result_name(
			     SIPPYMODEM_SLMODEM_CALLS_INVALID_EVENT),
		     "invalid signaling event") == 0 &&
		      strcmp(sippymodem_slmodem_calls_result_name(
				     (enum sippymodem_slmodem_calls_result)99),
			     "unknown modem call result") == 0,
	      "call transition diagnostics were incomplete");
	return true;
}

static bool test_reports_bounded_queue_overflow(void)
{
	struct modem modem;
	struct sippymodem_slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_slmodem_calls_init(&calls, &modem),
	      "overflow fixture failed");
	for (unsigned int index = 0;
	     index < SIPPYMODEM_SLMODEM_CALL_EVENT_CAPACITY; ++index)
		emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
			  SIPPYMODEM_SIP_CALL_EARLY);
	emit_call(&calls, SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING,
		  SIPPYMODEM_SIP_CALL_IDLE);
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_EVENT_OVERFLOW &&
		      capture.remote_end_calls == 0,
	      "event queue overflow was silent");
	CHECK(sippymodem_slmodem_calls_drain(&calls) ==
		      SIPPYMODEM_SLMODEM_CALLS_OK &&
		      capture.remote_end_calls == 1 &&
		      sippymodem_slmodem_calls_drain(&calls) ==
			      SIPPYMODEM_SLMODEM_CALLS_IDLE,
	      "event overflow hid the terminal call state");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "event order", test_preserves_event_order },
		{ "incoming lifecycle", test_maps_incoming_lifecycle },
		{ "drain event preservation",
		  test_preserves_events_raised_while_draining },
		{ "invalid transitions",
		  test_rejects_invalid_or_failed_transitions },
		{ "bounded queue overflow", test_reports_bounded_queue_overflow },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
