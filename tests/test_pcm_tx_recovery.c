// SPDX-License-Identifier: BSD-3-Clause
#include "pcm_tx_recovery.h"
#include <assert.h>
#include <stdio.h>

static void test_reply_guard(void)
{
	const uint16_t delays[] = { 0, 1, 1903, UINT16_MAX };

	for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
		struct pcm_tx_recovery recovery = { .phase = 1 };
		uint64_t start = 16000;
		uint64_t end = start + MODEM_RATE / 10 + delays[i];

		pcm_tx_recovery_arm(&recovery, start);
		assert(!pcm_tx_recovery_ready(&recovery, start - 1, delays[i]));
		assert(!pcm_tx_recovery_ready(&recovery, end - 1, delays[i]));
		assert(pcm_tx_recovery_ready(&recovery, end, delays[i]));
		/* A Final arriving inside the round-trip guard retires the timing
		 * hypothesis. It must not start a deferred physical exchange. */
		pcm_tx_recovery_progress(&recovery, 1);
		assert(!recovery.pending && !recovery.used && !recovery.phase);
		assert(!pcm_tx_recovery_ready(&recovery, end, delays[i]));
	}
}

int main(void)
{
	struct pcm_tx_recovery recovery = {0}, other = {0};
	struct modem_tx_recovery evidence = {
		.first_retry = true, .peer_noack = true, .receive_healthy = true,
		.receive_sample = 16000, .peer_poll_sample = 16000,
	};
	test_reply_guard();
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
	struct modem_tx_recovery second = evidence;
	second.first_retry = second.peer_noack = false;
	second.second_information_retry = true;
	assert(!pcm_tx_recovery_eligible(&recovery, 16000, &second));
	assert(pcm_tx_recovery_second_eligible(&recovery, 16000, &second));
	second.flow_controlled = true;
	assert(!pcm_tx_recovery_second_eligible(&recovery, 16000, &second));
	pcm_tx_recovery_arm(&recovery, 16000);
	assert(recovery.used && recovery.pending && !pcm_tx_recovery_ready(&recovery, 15999, 0));
	assert(!pcm_tx_recovery_ready(&recovery, 16000 + MODEM_RATE / 10 - 1, 0));
	assert(pcm_tx_recovery_ready(&recovery, 16000 + MODEM_RATE / 10, 0));
	recovery.applied = true;
	pcm_tx_recovery_note(&recovery, 16000, 1);
	assert(recovery.phase == 4);
	pcm_tx_recovery_progress(&recovery, 128);
	assert(recovery.ack_generation == 128 && !recovery.phase && !recovery.used &&
	       !recovery.pending && !recovery.applied && !pcm_tx_recovery_ready(&recovery, 20000, 0));
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
