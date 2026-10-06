// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_DOWNSTREAM_STATE_FIXTURE_H
#define PCM_DOWNSTREAM_STATE_FIXTURE_H

#include <pcm_downstream_tx.h>
#include <stddef.h>
#include <string.h>

/* Snapshot every state byte, replacing only queued symbols' C padding.
 * Do not normalize the live transmitter or omit unused queue entries. */
static inline void pcm_downstream_tx_snapshot(
	struct pcm_downstream_tx *snapshot,
	const struct pcm_downstream_tx *transmitter)
{
	memcpy(snapshot, transmitter, sizeof(*snapshot));
	for (size_t index = 0; index < V90_CP_FRAME_INTERVALS; ++index) {
		const struct pcm_downstream_symbol *symbol = &transmitter->queue[index];
		unsigned char bytes[sizeof(*symbol)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct pcm_downstream_symbol, field), \
	&symbol->field, sizeof(symbol->field))
		COPY(pcm.ucode);
		COPY(pcm.positive);
		COPY(pcm.octet);
		COPY(pcm.sample);
		COPY(shaping_rule);
		COPY(shaping_rule_valid);
		COPY(shaping_frame_start);
#undef COPY
		memcpy(&snapshot->queue[index], bytes, sizeof(bytes));
	}
}

static inline bool pcm_downstream_tx_states_equal(
	const struct pcm_downstream_tx *left,
	const struct pcm_downstream_tx *right)
{
	struct pcm_downstream_tx left_snapshot, right_snapshot;

	pcm_downstream_tx_snapshot(&left_snapshot, left);
	pcm_downstream_tx_snapshot(&right_snapshot, right);
	return memcmp(&left_snapshot, &right_snapshot, sizeof(left_snapshot)) == 0;
}

static inline bool pcm_downstream_tx_queues_equal(
	const struct pcm_downstream_tx *left,
	const struct pcm_downstream_tx *right)
{
	struct pcm_downstream_tx left_snapshot, right_snapshot;

	pcm_downstream_tx_snapshot(&left_snapshot, left);
	pcm_downstream_tx_snapshot(&right_snapshot, right);
	return memcmp(left_snapshot.queue, right_snapshot.queue,
		      sizeof(left_snapshot.queue)) == 0;
}

#endif
