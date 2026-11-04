// SPDX-License-Identifier: BSD-3-Clause
#include "pcm_tx_recovery.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct pcm_tx_recovery recovery = {0}, other = {0};
	struct modem_tx_recovery evidence = {
		.first_retry = true, .peer_noack = true, .receive_healthy = true,
		.receive_sample = 16000, .peer_poll_sample = 16000,
	};
	for (int shift = -12; shift <= 12; ++shift) {
		recovery = (struct pcm_tx_recovery){0};
		pcm_tx_recovery_note(&recovery, 8000, shift);
		assert(recovery.phase == (unsigned)((shift % 6 + 6) % 6));
		assert(pcm_tx_recovery_eligible(&recovery, 16000, &evidence) == (shift % 6 != 0));
	}
	pcm_tx_recovery_note(&recovery, 8000, 2);
	pcm_tx_recovery_note(&recovery, 8080, 2);
	assert(recovery.phase == 4);
	assert(pcm_tx_recovery_eligible(&recovery, 16000, &evidence));
	assert(!pcm_tx_recovery_eligible(&other, 16000, &evidence));
	for (unsigned excluded = 0; excluded < 7; ++excluded) {
		struct modem_tx_recovery rejected = evidence;
		if (excluded == 0) rejected.first_retry = false;
		if (excluded == 1) rejected.peer_noack = false;
		if (excluded == 2) rejected.receive_healthy = false;
		if (excluded == 3) rejected.receive_sample = 8000;
		if (excluded == 4) rejected.peer_poll_sample = 8000;
		uint64_t now = excluded == 5 ? 8000 : excluded == 6 ? 200000 : 16000;
		assert(!pcm_tx_recovery_eligible(&recovery, now, &rejected));
	}
	recovery.used = recovery.pending = recovery.applied = true;
	pcm_tx_recovery_note(&recovery, 16000, 1);
	assert(recovery.phase == 4);
	pcm_tx_recovery_progress(&recovery, 128);
	assert(recovery.ack_generation == 128 && !recovery.phase && !recovery.used &&
	       !recovery.pending && !recovery.applied);
	assert(!pcm_tx_recovery_eligible(&recovery, 16000, &evidence));
	pcm_tx_recovery_note(&recovery, 16000, 1);
	pcm_tx_recovery_progress(&recovery, 128);
	assert(recovery.phase == 1);
	assert(!pcm_tx_recovery_eligible(&recovery, 16000, &evidence));
	evidence.ack_generation = 128;
	assert(pcm_tx_recovery_eligible(&recovery, 16000, &evidence));
	pcm_tx_recovery_note(&recovery, 200000, 2);
	assert(recovery.phase == 2);
	pcm_tx_recovery_note(&recovery, 1000, 3);
	assert(recovery.phase == 3);
	puts("PCM transmit recovery evidence tests passed");
	return 0;
}
