// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_INFO0_EXCHANGE_H
#define PCM_PHASE2_INFO0_EXCHANGE_H

#include "pcm_info0.h"
#include "phase2_info0_exchange.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase2_info0_exchange_result {
	PCM_PHASE2_INFO0_EXCHANGE_OK,
	PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
};

enum pcm_peer_info0_event {
	PCM_PEER_INFO0_NO_EVENT,
	PCM_PEER_INFO0_RECEIVED,
	PCM_PEER_INFO0_REJECTED,
};

enum pcm_info0_recovery_event {
	PCM_INFO0_RECOVERY_NO_EVENT,
	PCM_INFO0_RECOVERY_STARTED,
	PCM_INFO0_RECOVERY_ACCEPTED,
	PCM_INFO0_RECOVERY_REJECTED,
	/* The first acknowledgment arrived after completion on a peer tone. */
	PCM_INFO0_LATE_ACK_RECEIVED,
};

enum pcm_phase2_info0_exchange_state {
	PCM_PHASE2_INFO0_INITIAL,
	/* Opposite tone arrived before a valid record; repeat without ACK. */
	PCM_PHASE2_INFO0_REPEAT,
	/* Validate another peer record before deciding whether it needs a reply
	 * and a new ranging exchange, or only acknowledges the preceding one. */
	PCM_PHASE2_INFO0_VERIFY_REPEAT,
	/* A peer record is retained; its local ACK is pending or in flight. */
	PCM_PHASE2_INFO0_ACKNOWLEDGING,
	/* An unacknowledged peer repeat queued one more local ACK. */
	PCM_PHASE2_INFO0_ACK_REPEAT,
	/* The local ACK finished; wait for a new peer ACK or following tone. */
	PCM_PHASE2_INFO0_WAIT_COMPLETION,
	PCM_PHASE2_INFO0_COMPLETE,
	/* Full retrain reuses the previously completed INFO0 agreement. */
	PCM_PHASE2_INFO0_RETRAIN,
};

struct pcm_phase2_info0_exchange_event {
	enum pcm_peer_info0_event peer_info0_event;
	union pcm_info0_record peer_info0;
	struct v34_phase2_tone_event tone_event;
	enum pcm_protocol protocol;
	/* Exact start and validation boundaries of a repeated peer INFO0. */
	enum pcm_info0_recovery_event recovery_event;
};

struct pcm_phase2_info0_exchange {
	struct phase2_info0_exchange transport;
	union pcm_info0_record peer_info0;
	enum v90_modem_role local_role;
	enum pcm_protocol protocol;
	enum pcm_phase2_info0_exchange_state state;
	/* Qualified after the last retained peer INFO0 record. */
	bool fresh_peer_tone_seen;
};

/* Initial INFO0 clears its acknowledgment bit. V.90 emits reserved extension
 * bits as zero; V.92 carries its capability and short-Phase-2 request. The
 * protocol tag is immutable for the exchange. A valid peer record is retained
 * while a phase-continuous local repetition acknowledges it. The record is
 * released to ranging only after that repetition finishes and either the peer
 * acknowledgment or its following opposite tone has been received. */
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v34_info0 *local_info0a);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v90_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_analog(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0a *local_info0a);
enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_init_v92_digital(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t nominal_amplitude,
	const struct v92_info0d *local_info0d);
bool pcm_phase2_info0_exchange_is_valid(
	const struct pcm_phase2_info0_exchange *exchange);
/* Lightweight predicate for an exchange already established as valid. */
bool pcm_phase2_info0_exchange_complete(
	const struct pcm_phase2_info0_exchange *exchange);

enum pcm_phase2_info0_exchange_result
pcm_phase2_info0_exchange_sample(
	struct pcm_phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct pcm_phase2_info0_exchange_event *event);

const char *pcm_phase2_info0_exchange_result_name(
	enum pcm_phase2_info0_exchange_result result);

#endif
