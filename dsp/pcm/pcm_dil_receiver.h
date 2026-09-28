// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DIL_RECEIVER_H
#define PCM_DIL_RECEIVER_H

#include "pcm_constellation.h"
#include "v90_dil_sequence.h"

/* The first supported impairment class retains a common set of G.711 levels
 * in all six intervals after linear equalization. Code substitutions (including
 * robbed bits) remove affected levels. We do not infer an unmeasured codec law
 * or use a nearest-level guess to turn a collision into a valid constellation. */
struct pcm_dil_result {
	struct pcm_constellation usable;
	uint32_t symbols;
	uint16_t gain_q3_13;
	uint8_t rejected_levels;
	bool measured;
};

struct pcm_dil_receiver {
	struct v90_dil_sequence expected;
	/* Two independent observations of each sign in each interval. Counters
	 * saturate at two; memory and arithmetic are bounded for any descriptor. */
	uint8_t observations[PCM_CONSTELLATION_UCODES][12];
	struct pcm_constellation rejected;
	uint64_t reference_energy;
	uint64_t received_cross;
	uint32_t symbols;
	uint32_t limit;
};

bool pcm_dil_receiver_init(struct pcm_dil_receiver *receiver,
	const struct v90_dil_parameters *parameters, enum g711_law law);
/* Call once per symbol, starting immediately after Jd-prime/Jp-prime. */
bool pcm_dil_receiver_receive(struct pcm_dil_receiver *receiver, uint8_t octet);
/* A deadline may end a traversal early. Unobserved levels remain unavailable. */
bool pcm_dil_receiver_finish(const struct pcm_dil_receiver *receiver,
	struct pcm_dil_result *result);
/* Preserve cardinality and bound every ordered level by its original value.
 * This cannot increase the shared mapper's exact average-power bound. */
bool pcm_dil_select_training(const struct pcm_dil_result *result,
	struct pcm_constellation *constellation);

#endif
