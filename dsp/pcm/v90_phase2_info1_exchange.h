// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_INFO1_EXCHANGE_H
#define V90_PHASE2_INFO1_EXCHANGE_H

#include "phase2_info1_exchange.h"
#include "v90_info1.h"
#include "v90_phase2_reciprocal_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase2_info1_event_kind {
	V90_PHASE2_INFO1_EVENT_NONE,
	V90_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT,
	V90_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED,
	V90_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT,
	V90_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED,
	V90_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED,
	V90_PHASE2_INFO1_EVENT_FAILED,
};

union v90_info1_record {
	struct v34_info1c info1d;
	struct v90_info1a info1a;
};

struct v90_phase2_info1_event {
	struct v34_phase2_info_event frame_event;
	union v90_info1_record peer_record;
	enum v90_phase2_info1_event_kind kind;
	enum phase2_info1_failure failure;
};

struct v90_phase2_info1_exchange {
	struct phase2_info1_exchange control;
	union v90_info1_record peer_record;
	bool peer_record_received;
};

/* The digital modem sends INFO1d immediately after reciprocal probing. INFO1d
 * is the shared INFO1c wire record and therefore uses that codec directly. */
enum phase2_info1_result v90_phase2_info1_init_digital(
	struct v90_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1d);

/* The analog modem holds phase-coherent Tone A while receiving INFO1d. */
enum phase2_info1_result v90_phase2_info1_init_analog(
	struct v90_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe);

bool v90_phase2_info1_is_valid(
	const struct v90_phase2_info1_exchange *exchange);

/* Supply the analog modem's V.90 selection after INFO1d is received. */
enum phase2_info1_result v90_phase2_info1_reply_analog(
	struct v90_phase2_info1_exchange *exchange,
	const struct v90_info1a *local_info1a);

enum phase2_info1_result v90_phase2_info1_sample(
	struct v90_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct v90_phase2_info1_event *event);

#endif
