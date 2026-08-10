// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_H
#define SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_H

#include "sippymodem_v34_info1.h"
#include "sippymodem_v34_phase2_info_audio.h"
#include "sippymodem_v34_phase2_reciprocal_probe.h"
#include "sippymodem_v34_phase2_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_CALLER_INFO1A_WAIT_BASE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 700 / 1000,
	SIPPYMODEM_V34_PHASE2_ANSWERER_INFO1C_WAIT_BASE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 2,
};

enum sippymodem_v34_phase2_info1_exchange_result {
	SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
	SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT,
};

enum sippymodem_v34_phase2_info1_exchange_phase {
	SIPPYMODEM_V34_PHASE2_INFO1_TRANSMITTING,
	SIPPYMODEM_V34_PHASE2_INFO1_CARRIER_TAIL,
	SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER,
	SIPPYMODEM_V34_PHASE2_INFO1_ANSWERER_REPLY_NEEDED,
	SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE,
	SIPPYMODEM_V34_PHASE2_INFO1_FAILED,
};

enum sippymodem_v34_phase2_info1_exchange_failure {
	SIPPYMODEM_V34_PHASE2_INFO1_NO_FAILURE,
	SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT,
};

enum sippymodem_v34_phase2_info1_exchange_event_kind {
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED,
	SIPPYMODEM_V34_PHASE2_INFO1_EVENT_FAILED,
};

union sippymodem_v34_phase2_info1_parameters {
	struct sippymodem_v34_info1c info1c;
	struct sippymodem_v34_info1a info1a;
};

struct sippymodem_v34_phase2_info1_exchange_event {
	struct sippymodem_v34_phase2_info_event frame_event;
	union sippymodem_v34_phase2_info1_parameters peer_parameters;
	enum sippymodem_v34_phase2_info1_exchange_event_kind kind;
	enum sippymodem_v34_phase2_info1_exchange_failure failure;
};

struct sippymodem_v34_phase2_info1_exchange {
	struct sippymodem_v34_phase2_info_audio_transmitter transmitter;
	struct sippymodem_v34_phase2_info_audio_receiver receiver;
	struct sippymodem_v34_phase2_tone_transmitter answerer_tone_a;
	union sippymodem_v34_phase2_info1_parameters peer_parameters;
	uint32_t peer_deadline_elapsed_samples;
	uint16_t round_trip_delay_samples;
	int16_t nominal_amplitude;
	uint8_t carrier_tail_symbols;
	enum sippymodem_v34_station_role local_role;
	enum sippymodem_v34_phase2_info1_exchange_phase phase;
	enum sippymodem_v34_phase2_info1_exchange_failure failure;
	bool peer_parameters_received;
};

/* The caller's reciprocal probe must have ended after Tone A acquisition.
 * INFO1c begins on the next line sample. */
enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_caller_init(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_phase2_reciprocal_probe *completed_probe,
	const struct sippymodem_v34_info1c *local_info1c);

/* The answerer's reciprocal probe must have ended as it began final Tone A.
 * Tone A remains active while INFO1c is received. */
enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_answerer_init(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_phase2_reciprocal_probe *completed_probe);

bool sippymodem_v34_phase2_info1_exchange_is_valid(
	const struct sippymodem_v34_phase2_info1_exchange *exchange);

/* Supply INFO1a after PEER_INFO1C_RECEIVED. This explicit boundary lets the
 * answerer select parameters from the peer frame and retained probe results.
 * Until the reply is supplied, sample() continues phase-coherent Tone A. */
enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_answerer_reply(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	const struct sippymodem_v34_info1a *local_info1a);

/* One call consumes and produces one 8 kHz line sample. A valid peer frame
 * wins over a deadline reached on the same sample. Sent events include the
 * causal pulse-filter tail, so the following sample belongs to the next
 * waveform owner. Terminal states are silent and stable. */
enum sippymodem_v34_phase2_info1_exchange_result
sippymodem_v34_phase2_info1_exchange_sample(
	struct sippymodem_v34_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_info1_exchange_event *event);

const char *sippymodem_v34_phase2_info1_exchange_result_name(
	enum sippymodem_v34_phase2_info1_exchange_result result);

#endif
