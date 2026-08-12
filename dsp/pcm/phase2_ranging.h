// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_RANGING_H
#define PHASE2_RANGING_H

#include "v34_phase2_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PHASE2_RANGING_TURNAROUND_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 40 / 1000,
	PHASE2_RANGING_INITIAL_TONE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 50 / 1000,
	PHASE2_RANGING_TONE_TAIL_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 10 / 1000,
	PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
};

enum phase2_ranging_phase {
	PHASE2_RANGING_INFO0_EXCHANGE,
	PHASE2_SIGNAL_A_WAIT_INITIAL_TONE,
	PHASE2_WAIT_FIRST_REMOTE_REVERSAL,
	PHASE2_RESPONSE_DELAY,
	PHASE2_RESPONSE_TONE_TAIL,
	PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL,
	PHASE2_SIGNAL_B_WAIT_PROBE_START,
	PHASE2_RANGING_PROBE_READY,
	PHASE2_RANGING_TIMED_OUT,
};

/* State shared by the V.34 and PCM first-ranging procedures. Signal A is the
 * 2400 Hz guarded side; signal B is the unguarded 1200 Hz side. The owning
 * mode retains its typed INFO0 records and transport. */
struct phase2_ranging_control {
	enum phase2_ranging_phase phase;
	uint16_t local_tone_samples;
	uint16_t response_delay_samples;
	uint16_t post_reversal_samples;
	uint16_t probe_start_delay_samples;
	uint16_t measurement_elapsed_samples;
	uint16_t round_trip_delay_samples;
	bool round_trip_delay_valid;
};

#endif
