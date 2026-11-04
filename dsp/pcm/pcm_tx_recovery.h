// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_TX_RECOVERY_H
#define PCM_TX_RECOVERY_H

#include "modem.h"
#include "v90_cp.h"

/* Owned by one host datapump. Timing is a hypothesis until both the peer's
 * feedback and ordinary LAPM supervision qualify an action. */
struct pcm_tx_recovery {
	uint64_t ack_generation, event_sample, poll_sample;
	unsigned phase;
	bool used, pending, applied;
};

static inline void pcm_tx_recovery_progress(struct pcm_tx_recovery *recovery,
	uint64_t ack_generation)
{
	if (recovery->ack_generation != ack_generation)
		*recovery = (struct pcm_tx_recovery){ .ack_generation = ack_generation };
}

/* First allow an ordinary final to return after the peer's retry poll. This
 * bounded media guard prevents colliding/lost replies from authorizing an
 * immediate phase change. LAPM's T401 deadline continues unchanged. */
static inline void pcm_tx_recovery_arm(struct pcm_tx_recovery *recovery, uint64_t now)
{
	recovery->used = recovery->pending = true;
	recovery->poll_sample = now;
}

static inline bool pcm_tx_recovery_ready(const struct pcm_tx_recovery *recovery, uint64_t now)
{
	return recovery->pending && now >= recovery->poll_sample &&
		now - recovery->poll_sample >= MODEM_RATE / 10;
}

static inline void pcm_tx_recovery_note(struct pcm_tx_recovery *recovery,
	uint64_t now, int shift)
{
	if (recovery->used)
		return;
	if (now < recovery->event_sample || now - recovery->event_sample > 15u * MODEM_RATE)
		recovery->phase = 0;
	int phase = shift % V90_CP_FRAME_INTERVALS;
	if (phase < 0)
		phase += V90_CP_FRAME_INTERVALS;
	recovery->phase = (recovery->phase + (unsigned)phase) % V90_CP_FRAME_INTERVALS;
	recovery->event_sample = now;
}

static inline bool pcm_tx_recovery_eligible(const struct pcm_tx_recovery *recovery,
	uint64_t now, const struct modem_tx_recovery *observation)
{
	return recovery->ack_generation == observation->ack_generation &&
		recovery->phase && observation->first_retry && observation->peer_noack &&
		observation->receive_healthy && observation->receive_sample >= recovery->event_sample &&
		observation->peer_poll_sample >= recovery->event_sample && now >= recovery->event_sample &&
		now - recovery->event_sample <= 15u * MODEM_RATE;
}

#endif
