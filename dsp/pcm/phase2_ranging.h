// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_RANGING_H
#define PHASE2_RANGING_H

#include "phase2_info0_exchange.h"

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

enum phase2_ranging_result {
	PHASE2_RANGING_OK,
	PHASE2_RANGING_INVALID_ARGUMENT,
};

enum phase2_ranging_event_kind {
	PHASE2_RANGING_NO_EVENT,
	PHASE2_ROUND_TRIP_DELAY_MEASURED,
	PHASE2_PROBE_RECEIVE_READY,
	PHASE2_PROBE_TRANSMIT_READY,
	PHASE2_RANGING_TIMEOUT,
};

struct phase2_ranging_event {
	enum phase2_ranging_event_kind kind;
	uint16_t round_trip_delay_samples;
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

enum phase2_ranging_result phase2_ranging_control_init(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange);
bool phase2_ranging_control_is_valid(
	const struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received);

/* The owning typed exchange applies these two operations to a candidate copy
 * around exactly one INFO0 exchange sample. `transmit_phase_before` is sampled
 * after prepare and before that exchange sample. This keeps record decoding in
 * the mode while sharing exact tone-reversal and ranging deadlines. */
enum phase2_ranging_result phase2_ranging_prepare_sample(
	struct phase2_ranging_control *control,
	struct phase2_info0_exchange *exchange,
	bool peer_info0_received);
enum phase2_ranging_result phase2_ranging_finish_sample(
	struct phase2_ranging_control *control,
	const struct phase2_info0_exchange *exchange,
	bool peer_info0_received,
	enum phase2_info0_transmit_phase transmit_phase_before,
	const struct v34_phase2_tone_event *tone_event,
	int16_t *transmit_sample, struct phase2_ranging_event *event);

#endif
