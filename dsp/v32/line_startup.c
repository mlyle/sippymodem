// SPDX-License-Identifier: BSD-3-Clause

#include "line_startup.h"

#include <limits.h>

/* The alternating answerer states divide their energy between the 600 and
 * 3000 Hz projections. Either projection may carry only half of the line
 * power on a clean channel. */
static const double V32_TURNAROUND_MINIMUM_CARRIER_SHARE = 0.40;

static bool state_is_valid(enum v32_line_startup_state state)
{
	return state >= V32_LINE_WAIT_ANSWER_TONE &&
	       state <= V32_LINE_STARTUP_FAILED;
}

static const struct single_tone_detector *selected_detector(
	const struct v32_line_startup *startup)
{
	if (startup->selected_turnaround_tone_hz ==
	    V32_LOW_TURNAROUND_TONE_HZ)
		return &startup->low_turnaround_tone;
	if (startup->selected_turnaround_tone_hz ==
	    V32_HIGH_TURNAROUND_TONE_HZ)
		return &startup->high_turnaround_tone;
	return NULL;
}

static bool selected_turnaround_is_valid(
	const struct v32_line_startup *startup, bool required)
{
	const struct single_tone_detector *selected =
		selected_detector(startup);

	if (!required)
		return startup->selected_turnaround_tone_hz == 0;
	if (selected == NULL || !selected->tone_present)
		return false;
	return selected == &startup->low_turnaround_tone ?
		       !startup->high_turnaround_tone.tone_present :
		       !startup->low_turnaround_tone.tone_present;
}

static struct single_tone_detector *selected_detector_mutable(
	struct v32_line_startup *startup)
{
	if (startup->selected_turnaround_tone_hz ==
	    V32_LOW_TURNAROUND_TONE_HZ)
		return &startup->low_turnaround_tone;
	if (startup->selected_turnaround_tone_hz ==
	    V32_HIGH_TURNAROUND_TONE_HZ)
		return &startup->high_turnaround_tone;
	return NULL;
}

static bool turnaround_marker(const struct single_tone_event *event)
{
	/* AC acquisition establishes the tone; only the later CA reversal starts
	 * the caller's round-trip measurement and 64-symbol response interval. */
	return event->kind == SINGLE_TONE_PHASE_REVERSED;
}

bool v32_line_startup_is_valid(
	const struct v32_line_startup *startup)
{
	bool turnaround_selected;
	bool before_turnaround;
	bool terminal;

	if (startup == NULL || !state_is_valid(startup->state) ||
	    !single_tone_detector_is_valid(&startup->answer_tone) ||
	    startup->answer_tone.frequency_hz !=
		    V32_ANSWER_TONE_HZ ||
	    !single_tone_detector_is_valid(
		    &startup->low_turnaround_tone) ||
	    startup->low_turnaround_tone.frequency_hz !=
		    V32_LOW_TURNAROUND_TONE_HZ ||
	    startup->low_turnaround_tone.minimum_carrier_share !=
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE ||
	    !single_tone_detector_is_valid(
		    &startup->high_turnaround_tone) ||
	    startup->high_turnaround_tone.frequency_hz !=
		    V32_HIGH_TURNAROUND_TONE_HZ ||
	    startup->high_turnaround_tone.minimum_carrier_share !=
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE ||
	    !single_tone_transmitter_is_valid(
		    &startup->calling_carrier) ||
	    startup->calling_carrier.frequency_hz !=
		    V32_CALLING_CARRIER_HZ ||
	    (startup->state == V32_LINE_ANSWER_GUARD &&
	     startup->state_samples >=
		     V32_ANSWER_GUARD_SAMPLES) ||
	    (startup->state ==
		     V32_LINE_DELAY_CALLING_CARRIER_REVERSAL &&
	     (startup->state_samples == 0 ||
	      startup->state_samples >=
		      V32_REVERSAL_RESPONSE_SAMPLES)))
		return false;
	turnaround_selected =
		startup->state ==
			V32_LINE_DELAY_CALLING_CARRIER_REVERSAL ||
		startup->state ==
			V32_LINE_SEND_REVERSED_CALLING_CARRIER;
	before_turnaround =
		startup->state == V32_LINE_WAIT_ANSWER_TONE ||
		startup->state == V32_LINE_ANSWER_GUARD;
	terminal = startup->state == V32_LINE_STARTUP_COMPLETE ||
		   startup->state == V32_LINE_STARTUP_FAILED;
	return startup->answer_tone.tone_present ==
		       (startup->state == V32_LINE_ANSWER_GUARD) &&
	       selected_turnaround_is_valid(startup, turnaround_selected) &&
	       (!before_turnaround ||
		(!startup->low_turnaround_tone.tone_present &&
		 !startup->high_turnaround_tone.tone_present)) &&
	       (!terminal ||
		(startup->state_samples == 0 &&
		 !startup->low_turnaround_tone.tone_present &&
		 !startup->high_turnaround_tone.tone_present)) &&
	       (startup->round_trip_samples != 0) ==
		       (startup->state == V32_LINE_STARTUP_COMPLETE);
}

enum v32_line_startup_result v32_line_startup_init(
	struct v32_line_startup *startup,
	int16_t transmit_amplitude)
{
	struct v32_line_startup initialized = { 0 };

	if (startup == NULL ||
	    single_tone_detector_init(
		    &initialized.answer_tone,
		    V32_ANSWER_TONE_HZ) !=
		    SINGLE_TONE_OK ||
	    single_tone_detector_init_with_minimum_carrier_share(
		    &initialized.low_turnaround_tone,
		    V32_LOW_TURNAROUND_TONE_HZ,
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE) !=
		    SINGLE_TONE_OK ||
	    single_tone_detector_init_with_minimum_carrier_share(
		    &initialized.high_turnaround_tone,
		    V32_HIGH_TURNAROUND_TONE_HZ,
		    V32_TURNAROUND_MINIMUM_CARRIER_SHARE) !=
		    SINGLE_TONE_OK ||
	    single_tone_transmitter_init(
		    &initialized.calling_carrier,
		    V32_CALLING_CARRIER_HZ,
		    transmit_amplitude) != SINGLE_TONE_OK)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	initialized.state = V32_LINE_WAIT_ANSWER_TONE;
	if (!v32_line_startup_is_valid(&initialized))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = initialized;
	return V32_LINE_STARTUP_OK;
}

bool v32_line_startup_is_complete(
	const struct v32_line_startup *startup)
{
	return v32_line_startup_is_valid(startup) &&
	       startup->state == V32_LINE_STARTUP_COMPLETE;
}

uint32_t v32_line_startup_round_trip_samples(
	const struct v32_line_startup *startup)
{
	return v32_line_startup_is_complete(startup) ?
		       startup->round_trip_samples : 0;
}

static bool answerer_state_is_valid(enum v32_answerer_line_state state)
{
	return state >= V32_ANSWERER_LINE_ANSWER_TONE &&
	       state <= V32_ANSWERER_LINE_FAILED;
}

bool v32_answerer_line_startup_is_valid(
	const struct v32_answerer_line_startup *startup)
{
	bool measured;

	if (startup == NULL || !answerer_state_is_valid(startup->state) ||
	    !single_tone_transmitter_is_valid(&startup->answer_tone) ||
	    startup->answer_tone.frequency_hz != V32_ANSWER_TONE_HZ ||
	    !single_tone_transmitter_is_valid(
		    &startup->low_turnaround_tone) ||
	    startup->low_turnaround_tone.frequency_hz !=
		    V32_LOW_TURNAROUND_TONE_HZ ||
	    !single_tone_transmitter_is_valid(
		    &startup->high_turnaround_tone) ||
	    startup->high_turnaround_tone.frequency_hz !=
		    V32_HIGH_TURNAROUND_TONE_HZ ||
	    startup->low_turnaround_tone.amplitude !=
		    startup->high_turnaround_tone.amplitude ||
	    !single_tone_detector_is_valid(&startup->calling_carrier) ||
	    startup->calling_carrier.frequency_hz !=
		    V32_CALLING_CARRIER_HZ)
		return false;
	measured = startup->round_trip_samples != 0;
	if (startup->state == V32_ANSWERER_LINE_ANSWER_TONE)
		return startup->state_samples < V32_ANSWERER_TONE_SAMPLES &&
		       !measured && !startup->caller_reversed_early;
	if (startup->state == V32_ANSWERER_LINE_SILENCE)
		return startup->state_samples < V32_ANSWERER_SILENCE_SAMPLES &&
		       !measured && !startup->caller_reversed_early;
	if (startup->state == V32_ANSWERER_LINE_SEND_AC)
		return startup->state_samples <
			       V32_ANSWERER_CARRIER_WAIT_SAMPLES &&
		       !measured && !startup->caller_reversed_early;
	if (startup->state == V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL)
		return startup->state_samples <
			       V32_ANSWERER_CARRIER_WAIT_SAMPLES &&
		       !measured && !startup->caller_reversed_early;
	if (startup->state == V32_ANSWERER_LINE_DELAY_RESPONSE)
		return startup->state_samples > 0 &&
		       startup->state_samples < V32_REVERSAL_RESPONSE_SAMPLES &&
		       (measured || startup->caller_reversed_early);
	if (startup->state == V32_ANSWERER_LINE_HOLD_RESPONSE)
		return startup->state_samples <
			       V32_ANSWERER_CARRIER_WAIT_SAMPLES &&
		       (measured || startup->caller_reversed_early);
	if (startup->state == V32_ANSWERER_LINE_POST_DROP_SILENCE)
		return startup->state_samples <
			       V32_ANSWERER_DROP_SILENCE_SAMPLES &&
		       (measured || startup->caller_reversed_early);
	return startup->state_samples == 0 &&
	       (startup->state != V32_ANSWERER_LINE_COMPLETE ||
		measured || startup->caller_reversed_early);
}

enum v32_line_startup_result v32_answerer_line_startup_init(
	struct v32_answerer_line_startup *startup,
	int16_t transmit_amplitude)
{
	struct v32_answerer_line_startup initialized = { 0 };
	int16_t turnaround_amplitude;

	if (startup == NULL || transmit_amplitude < 2)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	turnaround_amplitude = transmit_amplitude / 2;
	if (single_tone_transmitter_init(
		    &initialized.answer_tone, V32_ANSWER_TONE_HZ,
		    transmit_amplitude) != SINGLE_TONE_OK ||
	    single_tone_transmitter_init(
		    &initialized.low_turnaround_tone,
		    V32_LOW_TURNAROUND_TONE_HZ,
		    turnaround_amplitude) != SINGLE_TONE_OK ||
	    single_tone_transmitter_init(
		    &initialized.high_turnaround_tone,
		    V32_HIGH_TURNAROUND_TONE_HZ,
		    turnaround_amplitude) != SINGLE_TONE_OK ||
	    single_tone_detector_init(
		    &initialized.calling_carrier,
		    V32_CALLING_CARRIER_HZ) != SINGLE_TONE_OK)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	initialized.state = V32_ANSWERER_LINE_ANSWER_TONE;
	if (!v32_answerer_line_startup_is_valid(&initialized))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = initialized;
	return V32_LINE_STARTUP_OK;
}

bool v32_answerer_line_startup_is_complete(
	const struct v32_answerer_line_startup *startup)
{
	return v32_answerer_line_startup_is_valid(startup) &&
	       startup->state == V32_ANSWERER_LINE_COMPLETE;
}

uint32_t v32_answerer_line_startup_round_trip_samples(
	const struct v32_answerer_line_startup *startup)
{
	return v32_answerer_line_startup_is_complete(startup) ?
		       startup->round_trip_samples : 0;
}

static void answerer_enter_state(
	struct v32_answerer_line_startup *startup,
	enum v32_answerer_line_state state)
{
	startup->state = state;
	startup->state_samples = 0;
}

static bool answerer_reverse_turnaround(
	struct v32_answerer_line_startup *startup)
{
	return single_tone_transmitter_reverse(
		       &startup->low_turnaround_tone) == SINGLE_TONE_OK &&
	       single_tone_transmitter_reverse(
		       &startup->high_turnaround_tone) == SINGLE_TONE_OK;
}

static enum v32_line_startup_result answerer_fail(
	struct v32_answerer_line_startup *next,
	struct v32_answerer_line_startup *startup,
	int16_t *transmit_sample, bool *complete)
{
	answerer_enter_state(next, V32_ANSWERER_LINE_FAILED);
	next->round_trip_samples = 0;
	next->caller_reversed_early = false;
	if (!v32_answerer_line_startup_is_valid(next))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = *next;
	*transmit_sample = 0;
	*complete = false;
	return V32_LINE_STARTUP_PROTOCOL_ERROR;
}

static bool answerer_turnaround_sample(
	struct v32_answerer_line_startup *startup, int16_t *sample)
{
	int16_t low;
	int16_t high;

	if (single_tone_transmitter_sample(
		    &startup->low_turnaround_tone, &low) != SINGLE_TONE_OK ||
	    single_tone_transmitter_sample(
		    &startup->high_turnaround_tone, &high) != SINGLE_TONE_OK)
		return false;
	*sample = (int16_t)((int32_t)low + high);
	return true;
}

enum v32_line_startup_result v32_answerer_line_startup_process(
	struct v32_answerer_line_startup *startup,
	int16_t receive_sample, int16_t *transmit_sample,
	bool *complete)
{
	struct v32_answerer_line_startup next;
	struct single_tone_event carrier_event = { 0 };
	int16_t generated = 0;

	if (!v32_answerer_line_startup_is_valid(startup) ||
	    transmit_sample == NULL || complete == NULL)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	if (startup->state == V32_ANSWERER_LINE_FAILED) {
		*transmit_sample = 0;
		*complete = false;
		return V32_LINE_STARTUP_PROTOCOL_ERROR;
	}
	if (startup->state == V32_ANSWERER_LINE_COMPLETE) {
		*transmit_sample = 0;
		*complete = true;
		return V32_LINE_STARTUP_OK;
	}
	next = *startup;
	if (next.state >= V32_ANSWERER_LINE_SEND_AC &&
	    next.state <= V32_ANSWERER_LINE_HOLD_RESPONSE &&
	    single_tone_detector_sample(
		    &next.calling_carrier, receive_sample,
		    &carrier_event) != SINGLE_TONE_OK)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	switch (next.state) {
	case V32_ANSWERER_LINE_ANSWER_TONE:
		if (single_tone_transmitter_sample(
			    &next.answer_tone, &generated) != SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		next.state_samples++;
		if (next.state_samples == V32_ANSWERER_TONE_SAMPLES)
			answerer_enter_state(&next, V32_ANSWERER_LINE_SILENCE);
		break;
	case V32_ANSWERER_LINE_SILENCE:
		next.state_samples++;
		if (next.state_samples == V32_ANSWERER_SILENCE_SAMPLES)
			answerer_enter_state(&next, V32_ANSWERER_LINE_SEND_AC);
		break;
	case V32_ANSWERER_LINE_SEND_AC:
		next.state_samples++;
		if (carrier_event.kind == SINGLE_TONE_PHASE_REVERSED) {
			next.caller_reversed_early = true;
			next.state_samples = carrier_event.reversal_delay_samples;
			next.state = V32_ANSWERER_LINE_DELAY_RESPONSE;
		} else if (next.state_samples >= V32_ANSWERER_AC_SAMPLES &&
			   next.calling_carrier.tone_present) {
			if (!answerer_reverse_turnaround(&next))
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			answerer_enter_state(
				&next,
				V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL);
		} else if (next.state_samples ==
			   V32_ANSWERER_CARRIER_WAIT_SAMPLES) {
			return answerer_fail(
				&next, startup, transmit_sample, complete);
		}
		break;
	case V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL:
		next.state_samples++;
		if (carrier_event.kind == SINGLE_TONE_PHASE_REVERSED) {
			uint32_t measured = next.state_samples;

			if (measured <= carrier_event.reversal_delay_samples)
				return answerer_fail(
					&next, startup, transmit_sample, complete);
			next.round_trip_samples =
				measured - carrier_event.reversal_delay_samples;
			next.state_samples = carrier_event.reversal_delay_samples;
			next.state = V32_ANSWERER_LINE_DELAY_RESPONSE;
		} else if (carrier_event.kind == SINGLE_TONE_ENDED ||
			   next.state_samples ==
				   V32_ANSWERER_CARRIER_WAIT_SAMPLES) {
			return answerer_fail(
				&next, startup, transmit_sample, complete);
		}
		break;
	case V32_ANSWERER_LINE_DELAY_RESPONSE:
		if (carrier_event.kind == SINGLE_TONE_ENDED)
			return answerer_fail(
				&next, startup, transmit_sample, complete);
		next.state_samples++;
		if (next.state_samples == V32_REVERSAL_RESPONSE_SAMPLES) {
			if (!answerer_reverse_turnaround(&next))
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			answerer_enter_state(
				&next, V32_ANSWERER_LINE_HOLD_RESPONSE);
		}
		break;
	case V32_ANSWERER_LINE_HOLD_RESPONSE:
		next.state_samples++;
		if (carrier_event.kind == SINGLE_TONE_ENDED)
			answerer_enter_state(
				&next,
				V32_ANSWERER_LINE_POST_DROP_SILENCE);
		else if (next.state_samples ==
			 V32_ANSWERER_CARRIER_WAIT_SAMPLES)
			return answerer_fail(
				&next, startup, transmit_sample, complete);
		break;
	case V32_ANSWERER_LINE_POST_DROP_SILENCE:
		next.state_samples++;
		if (next.state_samples ==
		    V32_ANSWERER_DROP_SILENCE_SAMPLES)
			answerer_enter_state(
				&next, V32_ANSWERER_LINE_COMPLETE);
		break;
	case V32_ANSWERER_LINE_COMPLETE:
	case V32_ANSWERER_LINE_FAILED:
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (startup->state >= V32_ANSWERER_LINE_SEND_AC &&
	    startup->state <= V32_ANSWERER_LINE_HOLD_RESPONSE &&
	    !answerer_turnaround_sample(&next, &generated))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	if (!v32_answerer_line_startup_is_valid(&next))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	*transmit_sample = generated;
	*complete = next.state == V32_ANSWERER_LINE_COMPLETE;
	return V32_LINE_STARTUP_OK;
}

static void enter_state(
	struct v32_line_startup *startup,
	enum v32_line_startup_state state)
{
	startup->state = state;
	startup->state_samples = 0;
}

static void advance_state_clock(struct v32_line_startup *startup)
{
	if (startup->state_samples < UINT32_MAX)
		startup->state_samples++;
}

static enum single_tone_result reset_detectors(
	struct v32_line_startup *startup)
{
	if (single_tone_detector_reset(&startup->answer_tone) !=
		    SINGLE_TONE_OK ||
	    single_tone_detector_reset(
		    &startup->low_turnaround_tone) !=
		    SINGLE_TONE_OK ||
	    single_tone_detector_reset(
		    &startup->high_turnaround_tone) !=
		    SINGLE_TONE_OK)
		return SINGLE_TONE_INVALID_ARGUMENT;
	startup->selected_turnaround_tone_hz = 0;
	return SINGLE_TONE_OK;
}

static enum v32_line_startup_result fail_startup(
	struct v32_line_startup *next,
	struct v32_line_startup *startup,
	int16_t *transmit_sample, bool *complete)
{
	if (reset_detectors(next) !=
	    SINGLE_TONE_OK)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	next->round_trip_samples = 0;
	enter_state(next, V32_LINE_STARTUP_FAILED);
	if (!v32_line_startup_is_valid(next))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = *next;
	*transmit_sample = 0;
	*complete = false;
	return V32_LINE_STARTUP_PROTOCOL_ERROR;
}

enum v32_line_startup_result
v32_line_startup_process(
	struct v32_line_startup *startup, int16_t receive_sample,
	int16_t *transmit_sample, bool *complete)
{
	struct v32_line_startup next;
	struct single_tone_detector *turnaround_detector;
	struct single_tone_event answer_event = { 0 };
	struct single_tone_event low_event = { 0 };
	struct single_tone_event high_event = { 0 };
	struct single_tone_event turnaround_event = { 0 };
	int16_t generated = 0;

	if (!v32_line_startup_is_valid(startup) ||
	    transmit_sample == NULL || complete == NULL)
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	if (startup->state == V32_LINE_STARTUP_FAILED) {
		*transmit_sample = 0;
		*complete = false;
		return V32_LINE_STARTUP_PROTOCOL_ERROR;
	}
	if (startup->state == V32_LINE_STARTUP_COMPLETE) {
		*transmit_sample = 0;
		*complete = true;
		return V32_LINE_STARTUP_OK;
	}
	next = *startup;
	switch (next.state) {
	case V32_LINE_WAIT_ANSWER_TONE:
		if (single_tone_detector_sample(
			    &next.answer_tone, receive_sample, &answer_event) !=
		    SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		advance_state_clock(&next);
		if (answer_event.kind == SINGLE_TONE_STARTED)
			enter_state(&next, V32_LINE_ANSWER_GUARD);
		break;
	case V32_LINE_ANSWER_GUARD:
		if (single_tone_detector_sample(
			    &next.answer_tone, receive_sample, &answer_event) !=
		    SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (answer_event.kind == SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (next.state_samples ==
		    V32_ANSWER_GUARD_SAMPLES) {
			if (single_tone_detector_reset(
				    &next.answer_tone) != SINGLE_TONE_OK)
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				V32_LINE_SEND_CALLING_CARRIER);
		}
		break;
	case V32_LINE_SEND_CALLING_CARRIER:
		if (single_tone_detector_sample(
			    &next.low_turnaround_tone, receive_sample,
			    &low_event) != SINGLE_TONE_OK ||
		    single_tone_detector_sample(
			    &next.high_turnaround_tone, receive_sample,
			    &high_event) != SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		advance_state_clock(&next);
		if (turnaround_marker(&low_event) ||
		    turnaround_marker(&high_event)) {
			bool low_marked = turnaround_marker(&low_event);
			bool high_marked = turnaround_marker(&high_event);
			bool low_selected = low_marked;

			if (low_marked && high_marked) {
				struct single_tone_measurement low =
					single_tone_detector_measure(
						&next.low_turnaround_tone);
				struct single_tone_measurement high =
					single_tone_detector_measure(
						&next.high_turnaround_tone);

				low_selected =
					low.carrier_power >= high.carrier_power;
			}
			next.selected_turnaround_tone_hz =
				low_selected ?
					V32_LOW_TURNAROUND_TONE_HZ :
					V32_HIGH_TURNAROUND_TONE_HZ;
			turnaround_event =
				low_selected ? low_event : high_event;
			if (single_tone_detector_reset(
				    low_selected ? &next.high_turnaround_tone :
						   &next.low_turnaround_tone) !=
			    SINGLE_TONE_OK)
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				V32_LINE_DELAY_CALLING_CARRIER_REVERSAL);
			next.state_samples =
				turnaround_event.reversal_delay_samples != 0 ?
					turnaround_event.reversal_delay_samples : 1;
		}
		break;
	case V32_LINE_DELAY_CALLING_CARRIER_REVERSAL:
		turnaround_detector = selected_detector_mutable(&next);
		if (turnaround_detector == NULL ||
		    single_tone_detector_sample(
			    turnaround_detector, receive_sample,
			    &turnaround_event) != SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (turnaround_event.kind == SINGLE_TONE_ENDED ||
		    turnaround_event.kind ==
			    SINGLE_TONE_PHASE_REVERSED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (next.state_samples ==
		    V32_REVERSAL_RESPONSE_SAMPLES) {
			if (single_tone_transmitter_reverse(
				    &next.calling_carrier) !=
			    SINGLE_TONE_OK)
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next,
				V32_LINE_SEND_REVERSED_CALLING_CARRIER);
		}
		break;
	case V32_LINE_SEND_REVERSED_CALLING_CARRIER:
		turnaround_detector = selected_detector_mutable(&next);
		if (turnaround_detector == NULL ||
		    single_tone_detector_sample(
			    turnaround_detector, receive_sample,
			    &turnaround_event) != SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
		if (turnaround_event.kind == SINGLE_TONE_ENDED)
			return fail_startup(
				&next, startup, transmit_sample, complete);
		advance_state_clock(&next);
		if (turnaround_event.kind ==
		    SINGLE_TONE_PHASE_REVERSED) {
			uint64_t measured =
				(uint64_t)V32_REVERSAL_RESPONSE_SAMPLES +
				next.state_samples;

			if (measured <= turnaround_event.reversal_delay_samples)
				return fail_startup(
					&next, startup, transmit_sample, complete);
			measured -= turnaround_event.reversal_delay_samples;
			if (measured > UINT32_MAX)
				return fail_startup(
					&next, startup, transmit_sample, complete);
			next.round_trip_samples = (uint32_t)measured;
			if (reset_detectors(&next) != SINGLE_TONE_OK)
				return V32_LINE_STARTUP_INVALID_ARGUMENT;
			enter_state(
				&next, V32_LINE_STARTUP_COMPLETE);
		}
		break;
	case V32_LINE_STARTUP_COMPLETE:
	case V32_LINE_STARTUP_FAILED:
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (next.state == V32_LINE_SEND_CALLING_CARRIER ||
	    next.state ==
		    V32_LINE_DELAY_CALLING_CARRIER_REVERSAL ||
	    next.state ==
		    V32_LINE_SEND_REVERSED_CALLING_CARRIER) {
		if (single_tone_transmitter_sample(
			    &next.calling_carrier, &generated) !=
		    SINGLE_TONE_OK)
			return V32_LINE_STARTUP_INVALID_ARGUMENT;
	}
	if (!v32_line_startup_is_valid(&next))
		return V32_LINE_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	*transmit_sample = generated;
	*complete = next.state == V32_LINE_STARTUP_COMPLETE;
	return V32_LINE_STARTUP_OK;
}

const char *v32_line_startup_state_name(
	enum v32_line_startup_state state)
{
	switch (state) {
	case V32_LINE_WAIT_ANSWER_TONE:
		return "answer tone";
	case V32_LINE_ANSWER_GUARD:
		return "answer guard";
	case V32_LINE_SEND_CALLING_CARRIER:
		return "calling carrier";
	case V32_LINE_DELAY_CALLING_CARRIER_REVERSAL:
		return "carrier reversal delay";
	case V32_LINE_SEND_REVERSED_CALLING_CARRIER:
		return "reversed calling carrier";
	case V32_LINE_STARTUP_COMPLETE:
		return "complete";
	case V32_LINE_STARTUP_FAILED:
		return "failed";
	}
	return "unknown";
}

const char *v32_line_startup_result_name(
	enum v32_line_startup_result result)
{
	switch (result) {
	case V32_LINE_STARTUP_OK:
		return "ok";
	case V32_LINE_STARTUP_INVALID_ARGUMENT:
		return "invalid argument";
	case V32_LINE_STARTUP_PROTOCOL_ERROR:
		return "protocol error";
	}
	return "unknown";
}
