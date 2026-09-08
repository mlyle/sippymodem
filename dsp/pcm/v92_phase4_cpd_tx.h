// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_CPD_TX_H
#define V92_PHASE4_CPD_TX_H

#include "pcm_downstream_tx.h"
#include "v92_cpd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Own the one complete initial CPd record without enlarging the continuous
 * downstream transmitter's compact status-record storage. */
struct v92_phase4_cpd_tx {
	uint8_t sequence[V92_CPD_MAX_FRAME_BYTES];
	size_t sequence_bits;
	size_t bit_index;
	uint32_t sequence_fingerprint;
	uint8_t frame_bits;
	enum g711_law law;
	bool complete;
};

bool v92_phase4_cpd_tx_init(
	struct v92_phase4_cpd_tx *transmitter,
	const struct pcm_downstream_tx *downstream,
	const struct v92_cpd_parameters *parameters);
bool v92_phase4_cpd_tx_is_valid(
	const struct v92_phase4_cpd_tx *transmitter);

/* Feed the record through the inherited PCM coder. Source bits, cursor, line
 * state, and output commit together. The line resumes its stored SUVd only
 * after the complete CPd source record has entered the coder. */
bool v92_phase4_cpd_tx_next(
	struct v92_phase4_cpd_tx *transmitter,
	struct pcm_downstream_tx *downstream,
	struct pcm_downstream_symbol *symbol);

#endif /* V92_PHASE4_CPD_TX_H */
