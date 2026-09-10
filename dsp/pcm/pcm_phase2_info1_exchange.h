// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_INFO1_EXCHANGE_H
#define PCM_PHASE2_INFO1_EXCHANGE_H

#include "pcm_info1.h"
#include "phase2_info1_exchange.h"
#include "v90_phase2_reciprocal_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase2_info1_event_kind {
	PCM_PHASE2_INFO1_EVENT_NONE,
	PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT,
	PCM_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED,
	PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT,
	PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED,
	PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED,
	PCM_PHASE2_INFO1_EVENT_RETRAIN_REQUESTED,
	PCM_PHASE2_INFO1_EVENT_FAILED,
};

struct pcm_phase2_info1a {
	union pcm_info1a_record record;
	enum pcm_info1a_kind kind;
};

union pcm_phase2_info1_peer_record {
	struct pcm_info1d digital;
	struct pcm_phase2_info1a analog;
};

struct pcm_phase2_info1_event {
	struct v34_phase2_info_event frame_event;
	union pcm_phase2_info1_peer_record peer_record;
	enum pcm_phase2_info1_event_kind kind;
	enum phase2_info1_failure failure;
	enum pcm_protocol protocol;
};

struct pcm_phase2_info1_exchange {
	struct phase2_info1_exchange control;
	struct pcm_info1d digital;
	struct pcm_phase2_info1a analog;
	enum pcm_protocol protocol;
	uint8_t info1d_resends;
	bool digital_record_stored;
	bool analog_record_stored;
};

/* INFO1 transport and deadlines are shared. These constructors nail the
 * record codecs to one immutable protocol before the digital modem sends
 * INFO1d or the analog modem begins listening for it. */
enum phase2_info1_result pcm_phase2_info1_init_v90_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v34_info1c *local_info1d);
enum phase2_info1_result pcm_phase2_info1_init_v92_digital(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe,
	const struct v92_info1d *local_info1d);

enum phase2_info1_result pcm_phase2_info1_init_v90_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe);
enum phase2_info1_result pcm_phase2_info1_init_v92_analog(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_phase2_reciprocal_probe *completed_probe);

bool pcm_phase2_info1_is_valid(
	const struct pcm_phase2_info1_exchange *exchange);

/* True while a digital modem is recovering from the first full-Phase-2
 * INFO1a deadline and still owns its one INFO1d resend before full retrain. */
bool pcm_phase2_info1_retry_pending(
	const struct pcm_phase2_info1_exchange *exchange);

/* V.90 and V.92 can select conventional V.34 upstream. PCM upstream is a
 * V.92-only branch and is accepted only when the received INFO1d permits it. */
enum phase2_info1_result pcm_phase2_info1_reply_conventional(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v90_info1a *local_info1a);
enum phase2_info1_result pcm_phase2_info1_reply_pcm(
	struct pcm_phase2_info1_exchange *exchange,
	const struct v92_pcm_info1a *local_info1a);

enum phase2_info1_result pcm_phase2_info1_sample(
	struct pcm_phase2_info1_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_info1_event *event);

#endif
