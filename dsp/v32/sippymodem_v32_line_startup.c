// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_line_startup.h"

#include <limits.h>

/* The alternating answerer states may divide their energy between the 600
 * and 3000 Hz projections. At least one projection still carries most of the
 * signal, but it is not a pure sinusoid. */
static const double V32_TURNAROUND_MINIMUM_CARRIER_SHARE = 0.60;

static bool state_is_valid(enum sippymodem_v32_line_startup_state state)
{
	return state >= SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE &&
	       state <= SIPPYMODEM_V32_LINE_STARTUP_FAILED;
}

static const struct sippymodem_single_tone_detector *selected_detector(
	const struct sippymodem_v32_line_startup *startup)
{
	if (startup->selected_turnaround_tone_hz ==
	    SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ)
		return &startup->low_turnaround_tone;
	if (startup->selected_turnaround_tone_hz ==
	    SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ)
		return &startup->high_turnaround_tone;
	return NULL;
}

static bool selected_turnaround_is_valid(
	const struct sippymodem_v32_line_startup *startup, bool required)
{
	const struct sippymodem_single_tone_detector *selected =
		selected_detector(startup);

	if (!required)
		return startup->selected_turnaround_tone_hz == 0;
	if (selected == NULL || !selected->tone_present)
		return false;
	return selected == &startup->low_turnaround_tone ?
		       !startup->high_turnaround_tone.tone_present :
		       !startup->low_turnaround_tone.tone_present;
}

static struct sippymodem_single_tone_detector *selected_detector_mutable(
	struct sippymodem_v32_line_startup *startup)
{
	if (startup->selected_turnaround_tone_hz ==
	    SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ)
		return &startup->low_turnaround_tone;
	if (startup->selected_turnaround_tone_hz ==
	    SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ)
		return &startup->high_turnaround_tone;
	return NULL;
}

bool sippymodem_v32_line_startup_is_valid(
	const struct sippymodem_v32_line_startup *startup)
{
	bool turnaround_selected;
	bool before_turnaround;
	bool terminal;

	if (startup == NULL || !state_is_valid(startup->state) ||
	    !sippymodem_single_tone_detector_is_valid(&startup->answer_tone) ||
	    startup->answer_tone.frequency_hz !=
		    SIPPYMODEM_V32_ANSWER_TONE_HZ ||
	    !sippymodem_single_tone_detector_is_valid(
		    &startup->low_turnaround_tone) ||
	    startup->low_turnaround_tone.frequency_hz !=
		    SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ ||
	    startup->low_turnaround_tone.minimum_carrier_share !=
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE ||
	    !sippymodem_single_tone_detector_is_valid(
		    &startup->high_turnaround_tone) ||
	    startup->high_turnaround_tone.frequency_hz !=
		    SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ ||
	    startup->high_turnaround_tone.minimum_carrier_share !=
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE ||
	    !sippymodem_single_tone_transmitter_is_valid(
		    &startup->calling_carrier) ||
	    startup->calling_carrier.frequency_hz !=
		    SIPPYMODEM_V32_CALLING_CARRIER_HZ ||
	    (startup->state == SIPPYMODEM_V32_LINE_ANSWER_GUARD &&
	     startup->state_samples >=
		     SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES) ||
	    (startup->state ==
		     SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL &&
	     (startup->state_samples == 0 ||
	      startup->state_samples >=
		      SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES)))
		return false;
	turnaround_selected =
		startup->state ==
			SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL ||
		startup->state ==
			SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER;
	before_turnaround =
		startup->state == SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE ||
		startup->state == SIPPYMODEM_V32_LINE_ANSWER_GUARD;
	terminal = startup->state == SIPPYMODEM_V32_LINE_STARTUP_COMPLETE ||
		   startup->state == SIPPYMODEM_V32_LINE_STARTUP_FAILED;
	return startup->answer_tone.tone_present ==
		       (startup->state == SIPPYMODEM_V32_LINE_ANSWER_GUARD) &&
	       selected_turnaround_is_valid(startup, turnaround_selected) &&
	       (!before_turnaround ||
		(!startup->low_turnaround_tone.tone_present &&
		 !startup->high_turnaround_tone.tone_present)) &&
	       (!terminal ||
		(startup->state_samples == 0 &&
		 !startup->low_turnaround_tone.tone_present &&
		 !startup->high_turnaround_tone.tone_present));
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
	    sippymodem_single_tone_detector_init_with_minimum_carrier_share(
		    &initialized.low_turnaround_tone,
		    SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ,
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE) !=
		    SIPPYMODEM_SINGLE_TONE_OK ||
	    sippymodem_single_tone_detector_init_with_minimum_carrier_share(
		    &initialized.high_turnaround_tone,
		    SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ,
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE) !=
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

static enum sippymodem_single_tone_result reset_detectors(
	struct sippymodem_v32_line_startup *startup)
{
	if (sippymodem_single_tone_detector_reset(&startup->answer_tone) !=
		    SIPPYMODEM_SINGLE_TONE_OK ||
	    sippymodem_single_tone_detector_reset(
		    &startup->low_turnaround_tone) !=
		    SIPPYMODEM_SINGLE_TONE_OK ||
	    sippymodem_single_tone_detector_reset(
		    &startup->high_turnaround_tone) !=
		    SIPPYMODEM_SINGLE_TONE_OK)
		return SIPPYMODEM_SINGLE_TONE_INVALID_ARGUMENT;
	startup->selected_turnaround_tone_hz = 0;
	return SIPPYMODEM_SINGLE_TONE_OK;
}

static enum sippymodem_v32_line_startup_result fail_startup(
	struct sippymodem_v32_line_startup *next,
	struct sippymodem_v32_line_startup *startup,
	int16_t *transmit_sample, bool *complete)
{
	if (reset_detectors(next) !=
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
	struct sippymodem_single_tone_detector *turnaround_detector;
	struct sippymodem_single_tone_event answer_event = { 0 };
	struct sippymodem_single_tone_event low_event = { 0 };
	struct sippymodem_single_tone_event high_event = { 0 };
	struct sippymodem_single_tone_event turnaround_event = { 0 };
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
	switch (next.state) {
	case SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE:
		if (sippymodem_single_tone_detector_sample(
			    &next.answer_tone, receive_sample, &answer_event) !=
		    SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
		advance_state_clock(&next);
		if (answer_event.kind == SIPPYMODEM_SINGLE_TONE_STARTED)
			enter_state(&next, SIPPYMODEM_V32_LINE_ANSWER_GUARD);
		break;
	case SIPPYMODEM_V32_LINE_ANSWER_GUARD:
		if (sippymodem_single_tone_detector_sample(
			    &next.answer_tone, receive_sample, &answer_event) !=
		    SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (answer_event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
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
		if (sippymodem_single_tone_detector_sample(
			    &next.low_turnaround_tone, receive_sample,
			    &low_event) != SIPPYMODEM_SINGLE_TONE_OK ||
		    sippymodem_single_tone_detector_sample(
			    &next.high_turnaround_tone, receive_sample,
			    &high_event) != SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
		advance_state_clock(&next);
		if (low_event.kind == SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED ||
		    high_event.kind == SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED) {
			bool low_reversed =
				low_event.kind ==
				SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED;

			if (low_reversed &&
			    high_event.kind ==
				    SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED)
				return fail_startup(
					&next, startup, transmit_sample, complete);
			next.selected_turnaround_tone_hz =
				low_reversed ?
					SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ :
					SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ;
			turnaround_event = low_reversed ? low_event : high_event;
			if (sippymodem_single_tone_detector_reset(
				    low_reversed ? &next.high_turnaround_tone :
						   &next.low_turnaround_tone) !=
			    SIPPYMODEM_SINGLE_TONE_OK)
				return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL);
			next.state_samples =
				turnaround_event.reversal_delay_samples;
		}
		break;
	case SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL:
		turnaround_detector = selected_detector_mutable(&next);
		if (turnaround_detector == NULL ||
		    sippymodem_single_tone_detector_sample(
			    turnaround_detector, receive_sample,
			    &turnaround_event) != SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (turnaround_event.kind == SIPPYMODEM_SINGLE_TONE_ENDED ||
		    turnaround_event.kind ==
			    SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (next.state_samples ==
		    SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES) {
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
		turnaround_detector = selected_detector_mutable(&next);
		if (turnaround_detector == NULL ||
		    sippymodem_single_tone_detector_sample(
			    turnaround_detector, receive_sample,
			    &turnaround_event) != SIPPYMODEM_SINGLE_TONE_OK)
			return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (turnaround_event.kind == SIPPYMODEM_SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (turnaround_event.kind ==
		    SIPPYMODEM_SINGLE_TONE_PHASE_REVERSED) {
			if (reset_detectors(&next) != SIPPYMODEM_SINGLE_TONE_OK)
				return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next, SIPPYMODEM_V32_LINE_STARTUP_COMPLETE);
		}
		break;
	case SIPPYMODEM_V32_LINE_STARTUP_COMPLETE:
	case SIPPYMODEM_V32_LINE_STARTUP_FAILED:
		return SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (next.state == SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER ||
	    next.state ==
		    SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL ||
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
	case SIPPYMODEM_V32_LINE_DELAY_CALLING_CARRIER_REVERSAL:
		return "carrier reversal delay";
	case SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER:
		return "reversed calling carrier";
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
