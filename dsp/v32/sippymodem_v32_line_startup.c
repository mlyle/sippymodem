// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_line_startup.h"

#include <limits.h>

static bool state_is_valid(enum sippymodem_v32_line_startup_state state)
{
	return state >= SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE &&
	       state <= SIPPYMODEM_V32_LINE_STARTUP_FAILED;
}

bool sippymodem_v32_line_startup_is_valid(
	const struct sippymodem_v32_line_startup *startup)
{
	bool tone_must_be_present;
	bool tone_must_be_absent;

	if (startup == NULL || !state_is_valid(startup->state) ||
	    !sippymodem_single_tone_detector_is_valid(&startup->answer_tone) ||
	    startup->answer_tone.frequency_hz !=
		    SIPPYMODEM_V32_ANSWER_TONE_HZ ||
	    !sippymodem_single_tone_transmitter_is_valid(
		    &startup->calling_carrier) ||
	    startup->calling_carrier.frequency_hz !=
		    SIPPYMODEM_V32_CALLING_CARRIER_HZ ||
	    (startup->state == SIPPYMODEM_V32_LINE_ANSWER_GUARD &&
	     startup->state_samples >=
		     SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES) ||
	    ((startup->state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE ||
	      startup->state == SIPPYMODEM_V32_LINE_STARTUP_FAILED) &&
	     startup->state_samples != 0))
		return false;
	tone_must_be_present =
		startup->state == SIPPYMODEM_V32_LINE_ANSWER_GUARD ||
		startup->state ==
			SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER ||
		startup->state == SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END;
	tone_must_be_absent =
		startup->state == SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE ||
		startup->state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE ||
		startup->state == SIPPYMODEM_V32_LINE_STARTUP_FAILED;
	return (!tone_must_be_present || startup->answer_tone.tone_present) &&
	       (!tone_must_be_absent || !startup->answer_tone.tone_present);
}

enum sippymodem_v32_line_startup_result sippymodem_v32_line_startup_init(
	struct sippymodem_v32_line_startup *startup,
	int16_t transmit_amplitude)
{
	struct sippymodem_v32_line_startup initialized = { 0 };

	if (startup == NULL ||
	    sippymodem_single_tone_detector_init(
		    &initialized.answer_tone,
		    SIPPYMODEM_V32_ANSWER_TONE_HZ) !=
		    SIPPYMODEM_SINGLE_TONE_OK ||
	    sippymodem_single_tone_transmitter_init(
		    &initialized.calling_carrier,
		    SIPPYMODEM_V32_CALLING_CARRIER_HZ,
		    transmit_amplitude) != SIPPYMODEM_SINGLE_TONE_OK)
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	initialized.state = SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE;
	if (!sippymodem_v32_line_startup_is_valid(&initialized))
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = initialized;
	return SIPPYMODEM_V32_LINE_STARTUP_OK;
}

bool sippymodem_v32_line_startup_is_complete(
	const struct sippymodem_v32_line_startup *startup)
{
	return sippymodem_v32_line_startup_is_valid(startup) &&
	       startup->state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE;
}

static void enter_state(
	struct sippymodem_v32_line_startup *startup,
	enum sippymodem_v32_line_startup_state state)
{
	startup->state = state;
	startup->state_samples = 0;
}

static void advance_state_clock(struct sippymodem_v32_line_startup *startup)
{
	if (startup->state_samples < UINT32_MAX)
		startup->state_samples++;
}

static enum sippymodem_v32_line_startup_result fail_startup(
	struct sippymodem_v32_line_startup *next,
	struct sippymodem_v32_line_startup *startup,
	int16_t *transmit_sample, bool *complete)
{
	if (sippymodem_single_tone_detector_reset(&next->answer_tone) !=
	    SIPPYMODEM_SINGLE_TONE_OK)
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	enter_state(next, SIPPYMODEM_V32_LINE_STARTUP_FAILED);
	if (!sippymodem_v32_line_startup_is_valid(next))
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = *next;
	*transmit_sample = 0;
	*complete = false;
	return SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR;
}

enum sippymodem_v32_line_startup_result
sippymodem_v32_line_startup_process(
	struct sippymodem_v32_line_startup *startup, int16_t receive_sample,
	int16_t *transmit_sample, bool *complete)
{
	struct sippymodem_v32_line_startup next;
	struct sippymodem_single_tone_event event;
	int16_t generated = 0;

	if (!sippymodem_v32_line_startup_is_valid(startup) ||
	    transmit_sample == NULL || complete == NULL)
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	if (startup->state == SIPPYMODEM_V32_LINE_STARTUP_FAILED) {
		*transmit_sample = 0;
		*complete = false;
		return SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR;
	}
	if (startup->state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE) {
		*transmit_sample = 0;
		*complete = true;
		return SIPPYMODEM_V32_LINE_STARTUP_OK;
	}
	next = *startup;
	if (sippymodem_single_tone_detector_sample(
		    &next.answer_tone, receive_sample, &event) !=
	    SIPPYMODEM_SINGLE_TONE_OK)
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	switch (next.state) {
	case SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE:
		advance_state_clock(&next);
		if (event.kind == SIPPYMODEM_SINGLE_TONE_STARTED)
			enter_state(&next, SIPPYMODEM_V32_LINE_ANSWER_GUARD);
		break;
	case SIPPYMODEM_V32_LINE_ANSWER_GUARD:
		if (event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (next.state_samples ==
		    SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES) {
			if (sippymodem_single_tone_detector_reset(
				    &next.answer_tone) != SIPPYMODEM_SINGLE_TONE_OK)
				return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER);
		}
		break;
	case SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER:
		if (event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (event.kind == SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED) {
			if (sippymodem_single_tone_transmitter_reverse(
				    &next.calling_carrier) !=
			    SIPPYMODEM_SINGLE_TONE_OK)
				return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER);
		}
		break;
	case SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER:
		if (event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (event.kind == SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED) {
			if (next.state_samples <
			    SIPPYMODEM_V32_MIN_REVERSAL_SPACING_SAMPLES)
				return fail_startup(
					&next, startup, transmit_sample, complete);
			enter_state(
				&next,
				SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END);
		}
		break;
	case SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END:
		advance_state_clock(&next);
		if (event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
			enter_state(
				&next, SIPPYMODEM_V32_LINE_STARTUP_COMPLETE);
		break;
	case SIPPYMODEM_V32_LINE_STARTUP_COMPLETE:
	case SIPPYMODEM_V32_LINE_STARTUP_FAILED:
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (next.state == SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER ||
	    next.state ==
		    SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER) {
		if (sippymodem_single_tone_transmitter_sample(
			    &next.calling_carrier, &generated) !=
		    SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (!sippymodem_v32_line_startup_is_valid(&next))
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	*transmit_sample = generated;
	*complete = next.state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE;
	return SIPPYMODEM_V32_LINE_STARTUP_OK;
}

const char *sippymodem_v32_line_startup_state_name(
	enum sippymodem_v32_line_startup_state state)
{
	switch (state) {
	case SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE:
		return "answer tone";
	case SIPPYMODEM_V32_LINE_ANSWER_GUARD:
		return "answer guard";
	case SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER:
		return "calling carrier";
	case SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER:
		return "reversed calling carrier";
	case SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END:
		return "peer turnaround";
	case SIPPYMODEM_V32_LINE_STARTUP_COMPLETE:
		return "complete";
	case SIPPYMODEM_V32_LINE_STARTUP_FAILED:
		return "failed";
	}
	return "unknown";
}

const char *sippymodem_v32_line_startup_result_name(
	enum sippymodem_v32_line_startup_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_LINE_STARTUP_OK:
		return "ok";
	case SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR:
		return "protocol error";
	}
	return "unknown";
}
