// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INFO1_EXCHANGE_H
#define V34_PHASE2_INFO1_EXCHANGE_H

#include "phase2_info1_exchange.h"
#include "v34_info1.h"
#include "v34_phase2_reciprocal_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_info1_exchange_event_kind {
	V34_PHASE2_INFO1_EVENT_NONE,
	V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT,
	V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED,
	V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT,
	V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED,
	V34_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED,
	V34_PHASE2_INFO1_EVENT_FAILED,
};

union v34_phase2_info1_parameters {
	struct v34_info1c info1c;
	struct v34_info1a info1a;
};

struct v34_phase2_info1_exchange_event {
	struct v34_phase2_info_event frame_event;
	union v34_phase2_info1_parameters peer_parameters;
	enum v34_phase2_info1_exchange_event_kind kind;
	enum phase2_info1_failure failure;
};

struct v34_phase2_info1_exchange {
	struct phase2_info1_exchange control;
	union v34_phase2_info1_parameters peer_parameters;
	bool peer_parameters_received;
};

/* The caller's reciprocal probe must have ended after Tone A acquisition.
 * INFO1c begins on the next line sample. */
enum phase2_info1_result v34_phase2_info1_exchange_caller_init(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1c);

/* The answerer's reciprocal probe must have ended as it began final Tone A.
 * Tone A remains active while INFO1c is received. */
enum phase2_info1_result v34_phase2_info1_exchange_answerer_init(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_phase2_reciprocal_probe *completed_probe);

bool v34_phase2_info1_exchange_is_valid(
	const struct v34_phase2_info1_exchange *exchange);

/* Supply INFO1a after PEER_INFO1C_RECEIVED. This explicit boundary lets the
 * answerer select parameters from the peer frame and retained probe results.
 * Until the reply is supplied, sample() continues phase-coherent Tone A. */
enum phase2_info1_result v34_phase2_info1_exchange_answerer_reply(
	struct v34_phase2_info1_exchange *exchange,
	const struct v34_info1a *local_info1a);

/* One call consumes and produces one 8 kHz line sample. A valid peer frame
 * wins over a deadline reached on the same sample. Sent events include the
 * causal pulse-filter tail, so the following sample belongs to the next
 * waveform owner. Terminal states are silent and stable. */
enum phase2_info1_result v34_phase2_info1_exchange_sample(
	struct v34_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_info1_exchange_event *event);

#endif
