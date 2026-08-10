// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_RANGING_H
#define SIPPYMODEM_V34_PHASE2_RANGING_H

#include "sippymodem_v34_phase2_info0_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_RANGING_TURNAROUND_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 40 / 1000,
	SIPPYMODEM_V34_PHASE2_RANGING_INITIAL_TONE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 50 / 1000,
	SIPPYMODEM_V34_PHASE2_RANGING_TONE_TAIL_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 10 / 1000,
	SIPPYMODEM_V34_PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 2,
};

enum sippymodem_v34_phase2_ranging_result {
	SIPPYMODEM_V34_PHASE2_RANGING_OK,
	SIPPYMODEM_V34_PHASE2_RANGING_INVALID_ARGUMENT,
};

enum sippymodem_v34_phase2_ranging_phase {
	SIPPYMODEM_V34_PHASE2_RANGING_INFO0_EXCHANGE,
	SIPPYMODEM_V34_PHASE2_ANSWERER_WAIT_INITIAL_TONE,
	SIPPYMODEM_V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL,
	SIPPYMODEM_V34_PHASE2_RESPONSE_DELAY,
	SIPPYMODEM_V34_PHASE2_RESPONSE_TONE_TAIL,
	SIPPYMODEM_V34_PHASE2_CALLER_WAIT_SECOND_REMOTE_REVERSAL,
	SIPPYMODEM_V34_PHASE2_PROBE_READY,
	SIPPYMODEM_V34_PHASE2_RANGING_TIMED_OUT,
};

enum sippymodem_v34_phase2_ranging_event_kind {
	SIPPYMODEM_V34_PHASE2_RANGING_NO_EVENT,
	SIPPYMODEM_V34_PHASE2_ROUND_TRIP_DELAY_MEASURED,
	SIPPYMODEM_V34_PHASE2_PROBE_RECEIVE_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_TRANSMIT_READY,
	SIPPYMODEM_V34_PHASE2_RANGING_TIMEOUT,
};

struct sippymodem_v34_phase2_ranging_event {
	struct sippymodem_v34_phase2_info0_exchange_event exchange_event;
	enum sippymodem_v34_phase2_ranging_event_kind kind;
	uint16_t round_trip_delay_samples;
};

struct sippymodem_v34_phase2_ranging {
	struct sippymodem_v34_phase2_info0_exchange info0_exchange;
	enum sippymodem_v34_phase2_ranging_phase phase;
	uint16_t local_tone_samples;
	uint16_t response_delay_samples;
	uint16_t post_reversal_samples;
	uint16_t measurement_elapsed_samples;
	uint16_t round_trip_delay_samples;
	bool round_trip_delay_valid;
};

/* This controller implements the error-free INFO0 and first Tone A/B ranging
 * turn. It stops at the exact boundary where the answerer must begin L1 and
 * the caller must receive L1. INFO0 repetition and probing are later owners. */
enum sippymodem_v34_phase2_ranging_result
sippymodem_v34_phase2_ranging_init(
	struct sippymodem_v34_phase2_ranging *ranging,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude,
	const struct sippymodem_v34_info0 *local_info0);
bool sippymodem_v34_phase2_ranging_is_valid(
	const struct sippymodem_v34_phase2_ranging *ranging);

/* One call consumes and produces one sample at the 8 kHz line terminal.
 * Response deadlines compensate the tone detector's reported edge latency. */
enum sippymodem_v34_phase2_ranging_result
sippymodem_v34_phase2_ranging_sample(
	struct sippymodem_v34_phase2_ranging *ranging,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_ranging_event *event);

const char *sippymodem_v34_phase2_ranging_result_name(
	enum sippymodem_v34_phase2_ranging_result result);

#endif
