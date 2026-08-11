// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_RANGING_H
#define V34_PHASE2_RANGING_H

#include "v34_phase2_info0_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_RANGING_TURNAROUND_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 40 / 1000,
	V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 50 / 1000,
	V34_PHASE2_RANGING_TONE_TAIL_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 10 / 1000,
	V34_PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
};

enum v34_phase2_ranging_result {
	V34_PHASE2_RANGING_OK,
	V34_PHASE2_RANGING_INVALID_ARGUMENT,
};

enum v34_phase2_ranging_phase {
	V34_PHASE2_RANGING_INFO0_EXCHANGE,
	V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE,
	V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL,
	V34_PHASE2_RESPONSE_DELAY,
	V34_PHASE2_RESPONSE_TONE_TAIL,
	V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL,
	V34_PHASE2_CALLER_WAIT_PROBE_START,
	V34_PHASE2_PROBE_READY,
	V34_PHASE2_RANGING_TIMED_OUT,
};

enum v34_phase2_ranging_event_kind {
	V34_PHASE2_RANGING_NO_EVENT,
	V34_PHASE2_ROUND_TRIP_DELAY_MEASURED,
	V34_PHASE2_PROBE_RECEIVE_READY,
	V34_PHASE2_PROBE_TRANSMIT_READY,
	V34_PHASE2_RANGING_TIMEOUT,
};

struct v34_phase2_ranging_event {
	struct v34_phase2_info0_exchange_event exchange_event;
	enum v34_phase2_ranging_event_kind kind;
	uint16_t round_trip_delay_samples;
};

struct v34_phase2_ranging {
	struct v34_phase2_info0_exchange info0_exchange;
	enum v34_phase2_ranging_phase phase;
	uint16_t local_tone_samples;
	uint16_t response_delay_samples;
	uint16_t post_reversal_samples;
	uint16_t probe_start_delay_samples;
	uint16_t measurement_elapsed_samples;
	uint16_t round_trip_delay_samples;
	bool round_trip_delay_valid;
};

/* This controller implements the error-free INFO0 and first Tone A/B ranging
 * turn. It stops where the answerer must begin L1 and at the caller's
 * latency-compensated estimate of that boundary. Probe reception owns final
 * waveform synchronization. INFO0 repetition and probing are later owners. */
enum v34_phase2_ranging_result
v34_phase2_ranging_init(
	struct v34_phase2_ranging *ranging,
	enum v34_station_role local_role,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0);
bool v34_phase2_ranging_is_valid(
	const struct v34_phase2_ranging *ranging);

/* One call consumes and produces one sample at the 8 kHz line terminal.
 * Deadlines compensate the tone detector's reported edge latency. A readiness
 * event hands the following sample to the probe controller; the transmit
 * boundary is exact and the receive boundary retains the detector's bounded
 * edge-estimation error. */
enum v34_phase2_ranging_result
v34_phase2_ranging_sample(
	struct v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_ranging_event *event);

const char *v34_phase2_ranging_result_name(
	enum v34_phase2_ranging_result result);

#endif
