// SPDX-License-Identifier: BSD-3-Clause

#include <slmodem_calls.h>

#include <modem.h>

#include "test.h"

#include <string.h>

struct transition_capture {
	unsigned int incoming_calls;
	unsigned int outgoing_connected_calls;
	unsigned int remote_end_calls;
	unsigned int answers;
	unsigned int status_updates;
	unsigned int last_status;
	int incoming_result;
	int outgoing_connected_result;
	int remote_end_result;
	int answer_result;
	struct slmodem_calls *emit_during_incoming;
};

static struct transition_capture capture;

static void emit_call(
	struct slmodem_calls *calls,
	enum sip_call_direction direction,
	enum sip_call_state state)
{
	const struct sip_call_event event = {
		.direction = direction,
		.state = state,
	};
	const struct sip_signaling_callbacks *callbacks =
		slmodem_calls_callbacks(calls);

	callbacks->call_changed(callbacks->context, &event);
}

static void emit_call_status(
	struct slmodem_calls *calls,
	enum sip_call_direction direction,
	enum sip_call_state state, unsigned int status_code)
{
	const struct sip_call_event event = {
		.direction = direction,
		.state = state,
		.status_code = status_code,
	};
	const struct sip_signaling_callbacks *callbacks =
		slmodem_calls_callbacks(calls);

	callbacks->call_changed(callbacks->context, &event);
}

int modem_report_incoming_call(struct modem *modem)
{
	(void)modem;
	capture.incoming_calls++;
	if (capture.emit_during_incoming != NULL)
		emit_call(capture.emit_during_incoming,
			  SIP_CALL_DIRECTION_INCOMING,
			  SIP_CALL_CONNECTED);
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

void modem_update_status(struct modem *modem, unsigned int status)
{
	(void)modem;
	capture.status_updates++;
	capture.last_status = status;
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
	struct slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(slmodem_calls_init(&calls, &modem),
	      "call latch initialization failed");
	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_EARLY);
	CHECK(calls.state == SIP_CALL_EARLY, "SIP early state was not retained");
	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_CONNECTED);
	CHECK(calls.state == SIP_CALL_CONNECTED, "SIP connected state was not retained");
	CHECK(capture.outgoing_connected_calls == 0,
	      "call transition ran inside its signaling callback");
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 0 &&
		      slmodem_calls_drain(&calls) ==
			      SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 1 &&
		      slmodem_calls_drain(&calls) ==
			      SLMODEM_CALLS_IDLE,
	      "outgoing call states were not drained in order");

	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_CONNECTED);
	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_IDLE);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.outgoing_connected_calls == 2 &&
		      capture.remote_end_calls == 0 &&
		      slmodem_calls_drain(&calls) ==
			      SLMODEM_CALLS_OK &&
		      capture.remote_end_calls == 1,
	      "terminal call state overtook an earlier transition");
	return true;
}

static bool test_maps_incoming_lifecycle(void)
{
	struct modem modem;
	struct slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(slmodem_calls_init(&calls, &modem),
	      "incoming call fixture failed");
	emit_call(&calls, SIP_CALL_DIRECTION_INCOMING,
		  SIP_CALL_INCOMING_PENDING);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.incoming_calls == 1,
	      "incoming offer was not reported to the modem");

	emit_call(&calls, SIP_CALL_DIRECTION_INCOMING,
		  SIP_CALL_CONNECTED);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "signaling-side automatic answer did not start the modem");
	modem.started = 1;
	emit_call(&calls, SIP_CALL_DIRECTION_INCOMING,
		  SIP_CALL_CONNECTED);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "connected modem was answered twice");
	return true;
}

static bool test_maps_outgoing_busy_responses(void)
{
	struct modem modem;
	struct slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(slmodem_calls_init(&calls, &modem),
	      "busy response fixture failed");
	emit_call_status(&calls, SIP_CALL_DIRECTION_OUTGOING,
			 SIP_CALL_IDLE, 486);
	CHECK(slmodem_calls_drain(&calls) == SLMODEM_CALLS_OK &&
		      capture.status_updates == 1 &&
		      capture.last_status == STATUS_BUSY &&
		      capture.remote_end_calls == 1,
	      "486 Busy Here did not reach the modem result state");
	emit_call_status(&calls, SIP_CALL_DIRECTION_OUTGOING,
			 SIP_CALL_IDLE, 600);
	CHECK(slmodem_calls_drain(&calls) == SLMODEM_CALLS_OK &&
		      capture.status_updates == 2 &&
		      capture.last_status == STATUS_BUSY &&
		      capture.remote_end_calls == 2,
	      "600 Busy Everywhere did not reach the modem result state");

	emit_call_status(&calls, SIP_CALL_DIRECTION_OUTGOING,
			 SIP_CALL_IDLE, 603);
	emit_call_status(&calls, SIP_CALL_DIRECTION_INCOMING,
			 SIP_CALL_IDLE, 486);
	CHECK(slmodem_calls_drain(&calls) == SLMODEM_CALLS_OK &&
		      slmodem_calls_drain(&calls) == SLMODEM_CALLS_OK &&
		      capture.status_updates == 2 &&
		      capture.remote_end_calls == 4,
	      "a non-busy or incoming response was reported as BUSY");
	return true;
}

static bool test_preserves_events_raised_while_draining(void)
{
	struct modem modem;
	struct slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(slmodem_calls_init(&calls, &modem),
	      "reentrant call fixture failed");
	capture.emit_during_incoming = &calls;
	emit_call(&calls, SIP_CALL_DIRECTION_INCOMING,
		  SIP_CALL_INCOMING_PENDING);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.incoming_calls == 1 && capture.answers == 0,
	      "incoming offer drain failed");
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.answers == 1,
	      "call event raised during a transition was lost");
	return true;
}

static bool test_rejects_invalid_or_failed_transitions(void)
{
	struct modem modem;
	struct slmodem_calls calls = { 0 };
	struct slmodem_calls original = calls;
	const struct sip_signaling_callbacks *callbacks;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(!slmodem_calls_init(NULL, &modem) &&
		      !slmodem_calls_init(&calls, NULL) &&
		      calls.modem == original.modem &&
		      calls.callbacks.call_changed ==
			      original.callbacks.call_changed &&
		      slmodem_calls_callbacks(NULL) == NULL &&
		      slmodem_calls_drain(NULL) ==
			      SLMODEM_CALLS_INVALID_ARGUMENT,
	      "call latch accepted incomplete ownership");
	CHECK(slmodem_calls_init(&calls, &modem),
	      "invalid transition fixture failed");
	callbacks = slmodem_calls_callbacks(&calls);
	callbacks->call_changed(callbacks->context, NULL);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_IDLE,
	      "null signaling event changed call state");

	emit_call(&calls, (enum sip_call_direction)99,
		  SIP_CALL_CONNECTED);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_INVALID_EVENT,
	      "invalid call direction reached the modem");
	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_INCOMING_PENDING);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_INVALID_EVENT,
	      "outgoing call was accepted as an incoming offer");

	capture.outgoing_connected_result = -1;
	emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
		  SIP_CALL_CONNECTED);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_MODEM_ERROR,
	      "failed modem transition was hidden");
	CHECK(strcmp(slmodem_calls_result_name(
			     SLMODEM_CALLS_INVALID_EVENT),
		     "invalid signaling event") == 0 &&
		      strcmp(slmodem_calls_result_name(
				     (enum slmodem_calls_result)99),
			     "unknown modem call result") == 0,
	      "call transition diagnostics were incomplete");
	return true;
}

static bool test_reports_bounded_queue_overflow(void)
{
	struct modem modem;
	struct slmodem_calls calls;

	memset(&modem, 0, sizeof(modem));
	memset(&capture, 0, sizeof(capture));
	CHECK(slmodem_calls_init(&calls, &modem),
	      "overflow fixture failed");
	for (unsigned int index = 0;
	     index < SLMODEM_CALL_EVENT_CAPACITY; ++index)
		emit_call(&calls, SIP_CALL_DIRECTION_OUTGOING,
			  SIP_CALL_EARLY);
	emit_call_status(&calls, SIP_CALL_DIRECTION_OUTGOING,
			 SIP_CALL_IDLE, 486);
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_EVENT_OVERFLOW &&
		      capture.remote_end_calls == 0 &&
		      capture.status_updates == 0,
	      "event queue overflow was silent");
	CHECK(slmodem_calls_drain(&calls) ==
		      SLMODEM_CALLS_OK &&
		      capture.remote_end_calls == 1 &&
		      capture.status_updates == 1 &&
		      capture.last_status == STATUS_BUSY &&
		      slmodem_calls_drain(&calls) ==
			      SLMODEM_CALLS_IDLE,
	      "event overflow hid the terminal call state or status");
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
		{ "outgoing busy responses", test_maps_outgoing_busy_responses },
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
