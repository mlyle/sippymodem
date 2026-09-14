// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SHORT_RANGING_H
#define V92_SHORT_RANGING_H

#include "phase2_ranging.h"

enum {
	V92_SHORT_RANGING_TIMEOUT_SAMPLES = V34_PHASE2_SAMPLE_RATE * 2500 / 1000,
};

enum v92_short_ranging_phase {
	V92_SHORT_RANGING_DIGITAL_GUARD,
	V92_SHORT_RANGING_ANALOG_WAIT,
	V92_SHORT_RANGING_ANALOG_DELAY,
	V92_SHORT_RANGING_TONE_TAIL,
	V92_SHORT_RANGING_DIGITAL_WAIT,
	V92_SHORT_RANGING_COMPLETE,
};

enum v92_short_ranging_event {
	V92_SHORT_RANGING_NO_EVENT,
	V92_SHORT_RANGING_TRANSMIT_INFO1A,
	V92_SHORT_RANGING_RECEIVE_INFO1A,
	V92_SHORT_RANGING_RECOVER_FULL_PHASE2,
	V92_SHORT_RANGING_REQUEST_FULL_RETRAIN,
};

struct v92_short_ranging {
	enum phase2_signal_role signal_role;
	enum v92_short_ranging_phase phase;
	uint16_t tone_samples;
	/* Analogue: from the end of local INFO0. Digital: from its B reversal. */
	uint16_t elapsed_samples;
	uint16_t response_samples;
	uint16_t tail_samples;
};

struct v92_short_ranging_step {
	bool transmit_tone;
	bool reverse_carrier;
	enum v92_short_ranging_event event;
	/* Only RECEIVE_INFO1A selects these fields. The INFO1a deadline is still
	 * relative to the original B reversal, not a fresh 2500 ms interval. */
	uint16_t round_trip_delay_samples;
	uint16_t info1a_remaining_samples;
};

/* V.92 9.4, after both INFO0 records select short Phase 2. The caller keeps
 * the phase-continuous tone transmitter, detector and INFO decoder. Pass the
 * actual completed local-tone duration and elapsed samples since INFO0a for
 * the analogue deadline; digital elapsed starts at its own B reversal.
 * Repeated INFO0 is handled by that owner, which reinitializes this controller
 * at the new agreement. This does not enable modem-level Quick Connect. */
bool v92_short_ranging_init(
	struct v92_short_ranging *ranging, enum phase2_signal_role signal_role,
	uint16_t completed_tone_samples, uint16_t since_info0_samples);
bool v92_short_ranging_is_valid(const struct v92_short_ranging *ranging);

/* Consume one 8 kHz detector result, then generate this sample according to
 * step. A terminal event hands the next sample to its owner; subsequent calls
 * fail unchanged. False never changes ranging or step. */
bool v92_short_ranging_sample(
	struct v92_short_ranging *ranging, bool peer_tone_present,
	const struct v34_phase2_tone_event *tone_event,
	struct v92_short_ranging_step *step);

#endif
