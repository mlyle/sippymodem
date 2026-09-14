// SPDX-License-Identifier: BSD-3-Clause

#include "v92_short_ranging.h"

#include "memory_region.h"

#include <stddef.h>

bool v92_short_ranging_is_valid(const struct v92_short_ranging *ranging)
{
	if (ranging == NULL ||
	    (ranging->signal_role != PHASE2_SIGNAL_A &&
	     ranging->signal_role != PHASE2_SIGNAL_B) ||
	    ranging->tone_samples > PHASE2_RANGING_INITIAL_TONE_SAMPLES ||
	    ranging->elapsed_samples > V92_SHORT_RANGING_TIMEOUT_SAMPLES ||
	    ranging->response_samples > PHASE2_RANGING_TURNAROUND_SAMPLES ||
	    ranging->tail_samples > PHASE2_RANGING_TONE_TAIL_SAMPLES)
		return false;
	if (ranging->phase != V92_SHORT_RANGING_ANALOG_DELAY &&
	    ranging->response_samples != 0)
		return false;
	if (ranging->signal_role == PHASE2_SIGNAL_A && ranging->tone_samples != 0)
		return false;
	if (ranging->signal_role == PHASE2_SIGNAL_B &&
	    ranging->phase != V92_SHORT_RANGING_DIGITAL_GUARD &&
	    ranging->tone_samples != PHASE2_RANGING_INITIAL_TONE_SAMPLES)
		return false;
	switch (ranging->phase) {
	case V92_SHORT_RANGING_DIGITAL_GUARD:
		return ranging->signal_role == PHASE2_SIGNAL_B &&
		       ranging->elapsed_samples == 0 && ranging->tail_samples == 0;
	case V92_SHORT_RANGING_ANALOG_WAIT:
		return ranging->signal_role == PHASE2_SIGNAL_A && ranging->tail_samples == 0;
	case V92_SHORT_RANGING_ANALOG_DELAY:
		return ranging->signal_role == PHASE2_SIGNAL_A &&
		       ranging->response_samples != 0 && ranging->tail_samples == 0;
	case V92_SHORT_RANGING_TONE_TAIL:
		return ranging->tail_samples != 0 &&
		       ranging->tail_samples < PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       (ranging->signal_role == PHASE2_SIGNAL_A ||
			ranging->elapsed_samples + 1 >= ranging->tail_samples);
	case V92_SHORT_RANGING_DIGITAL_WAIT:
		return ranging->signal_role == PHASE2_SIGNAL_B &&
		       ranging->tail_samples == PHASE2_RANGING_TONE_TAIL_SAMPLES &&
		       ranging->elapsed_samples + 1 >= ranging->tail_samples;
	case V92_SHORT_RANGING_COMPLETE:
		return true;
	}
	return false;
}

bool v92_short_ranging_init(
	struct v92_short_ranging *ranging, enum phase2_signal_role signal_role,
	uint16_t completed_tone_samples, uint16_t since_info0_samples)
{
	struct v92_short_ranging initialized = {
		.signal_role = signal_role,
		.phase = signal_role == PHASE2_SIGNAL_B ?
			V92_SHORT_RANGING_DIGITAL_GUARD : V92_SHORT_RANGING_ANALOG_WAIT,
		.tone_samples = signal_role != PHASE2_SIGNAL_B ? 0 :
			completed_tone_samples < PHASE2_RANGING_INITIAL_TONE_SAMPLES ?
				completed_tone_samples : PHASE2_RANGING_INITIAL_TONE_SAMPLES,
		.elapsed_samples = signal_role == PHASE2_SIGNAL_A ? since_info0_samples : 0,
	};

	if (ranging == NULL || since_info0_samples > V92_SHORT_RANGING_TIMEOUT_SAMPLES ||
	    !v92_short_ranging_is_valid(&initialized))
		return false;
	*ranging = initialized;
	return true;
}

static void finish(
	struct v92_short_ranging *ranging, struct v92_short_ranging_step *step,
	enum v92_short_ranging_event event)
{
	ranging->phase = V92_SHORT_RANGING_COMPLETE;
	ranging->response_samples = 0;
	step->event = event;
}

static void accept_reversal(
	struct v92_short_ranging *ranging, uint16_t delay,
	struct v92_short_ranging_step *step)
{
	if (delay == 0)
		return;
	if (ranging->phase == V92_SHORT_RANGING_ANALOG_WAIT &&
	    delay < PHASE2_RANGING_TURNAROUND_SAMPLES) {
		/* Use the dated line edge, not the detector's later notification. */
		ranging->response_samples = PHASE2_RANGING_TURNAROUND_SAMPLES - delay;
		ranging->phase = V92_SHORT_RANGING_ANALOG_DELAY;
	} else if (ranging->phase == V92_SHORT_RANGING_DIGITAL_WAIT &&
		   ranging->elapsed_samples >= delay) {
		uint16_t edge = ranging->elapsed_samples - delay;

		if (edge + PHASE2_RANGING_TURNAROUND_TOLERANCE_SAMPLES <
		    PHASE2_RANGING_TURNAROUND_SAMPLES)
			return;
		step->round_trip_delay_samples = edge > PHASE2_RANGING_TURNAROUND_SAMPLES ?
			edge - PHASE2_RANGING_TURNAROUND_SAMPLES : 0;
		step->info1a_remaining_samples = V92_SHORT_RANGING_TIMEOUT_SAMPLES -
			ranging->elapsed_samples;
		finish(ranging, step, V92_SHORT_RANGING_RECEIVE_INFO1A);
	}
}

bool v92_short_ranging_sample(
	struct v92_short_ranging *ranging, bool peer_tone_present,
	const struct v34_phase2_tone_event *tone_event,
	struct v92_short_ranging_step *step)
{
	struct v92_short_ranging next;
	struct v92_short_ranging_step emitted = { 0 };
	bool digital;

	if (!v92_short_ranging_is_valid(ranging) ||
	    ranging->phase == V92_SHORT_RANGING_COMPLETE || tone_event == NULL ||
	    (unsigned)tone_event->kind > V34_PHASE2_TONE_ENDED || step == NULL ||
	    memory_regions_overlap(ranging, sizeof(*ranging), step, sizeof(*step)) ||
	    memory_regions_overlap(ranging, sizeof(*ranging), tone_event, sizeof(*tone_event)) ||
	    memory_regions_overlap(step, sizeof(*step), tone_event, sizeof(*tone_event)))
		return false;
	next = *ranging;
	digital = next.signal_role == PHASE2_SIGNAL_B;
	emitted.transmit_tone = next.phase != V92_SHORT_RANGING_DIGITAL_WAIT;
	if (next.phase != V92_SHORT_RANGING_DIGITAL_GUARD &&
	    next.elapsed_samples < V92_SHORT_RANGING_TIMEOUT_SAMPLES)
		++next.elapsed_samples;
	/* V.92 9.4.1.2.2 and 9.4.2.2.2 have different recovery actions. */
	if (next.elapsed_samples == V92_SHORT_RANGING_TIMEOUT_SAMPLES &&
	    (digital || next.phase == V92_SHORT_RANGING_ANALOG_WAIT)) {
		finish(&next, &emitted, digital ? V92_SHORT_RANGING_RECOVER_FULL_PHASE2 :
			V92_SHORT_RANGING_REQUEST_FULL_RETRAIN);
	} else {
		if (next.phase == V92_SHORT_RANGING_DIGITAL_GUARD) {
			if (next.tone_samples == PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
			    peer_tone_present) {
				emitted.reverse_carrier = true;
				next.phase = V92_SHORT_RANGING_TONE_TAIL;
			} else if (next.tone_samples < PHASE2_RANGING_INITIAL_TONE_SAMPLES) {
				++next.tone_samples;
			}
		} else if (next.phase == V92_SHORT_RANGING_ANALOG_DELAY) {
			if (peer_tone_present && tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED) {
				/* A second edge before our response invalidates the first
				 * turn. Retain the detector's restored carrier reference. */
				next.phase = V92_SHORT_RANGING_ANALOG_WAIT;
				next.response_samples = 0;
			} else if (--next.response_samples == 0) {
				emitted.reverse_carrier = true;
				next.phase = V92_SHORT_RANGING_TONE_TAIL;
			}
		}
		if (next.phase == V92_SHORT_RANGING_TONE_TAIL &&
		    ++next.tail_samples == PHASE2_RANGING_TONE_TAIL_SAMPLES) {
			if (digital)
				next.phase = V92_SHORT_RANGING_DIGITAL_WAIT;
			else
				finish(&next, &emitted, V92_SHORT_RANGING_TRANSMIT_INFO1A);
		}
		if (peer_tone_present && tone_event->kind == V34_PHASE2_TONE_PHASE_REVERSED &&
		    ranging->phase != V92_SHORT_RANGING_ANALOG_DELAY)
			accept_reversal(&next, tone_event->reversal_delay_samples, &emitted);
	}
	if (!v92_short_ranging_is_valid(&next))
		return false;
	*ranging = next;
	*step = emitted;
	return true;
}
