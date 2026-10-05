// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_TX_FIXTURE_H
#define PCM_PHASE3_TX_FIXTURE_H

#include "pcm_phase3_owned.h"
#include "memory_region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the independent timeline checks below, and require each accepted
 * sample to produce the same complete state through the private continuation. */
static enum pcm_phase3_digital_tx_result pcm_phase3_checked_next(
	struct pcm_phase3_digital_tx *transmitter,
	struct v90_pcm_symbol *symbol)
{
	struct pcm_phase3_digital_tx owned;
	struct v90_pcm_symbol expected;
	bool compare = pcm_phase3_digital_tx_is_valid(transmitter) &&
		symbol != NULL && !memory_regions_overlap(
			transmitter, sizeof(*transmitter), symbol, sizeof(*symbol));
	enum pcm_phase3_digital_tx_result expected_result;
	enum pcm_phase3_digital_tx_result result;

	if (compare) {
		owned = *transmitter;
		memset(&expected, 0, sizeof(expected));
		expected_result = pcm_phase3_digital_tx_next_owned(&owned, &expected);
	}
	result = pcm_phase3_digital_tx_next(transmitter, symbol);
	if (compare && (result != expected_result ||
	    memcmp(transmitter, &owned, sizeof(owned)) != 0 ||
	    (result == PCM_PHASE3_DIGITAL_TX_OK &&
	     memcmp(symbol, &expected, sizeof(expected)) != 0))) {
		fprintf(stderr, "owned Phase 3 continuation changed output or state\n");
		abort();
	}
	return result;
}

#define pcm_phase3_digital_tx_next pcm_phase3_checked_next


#endif /* PCM_PHASE3_TX_FIXTURE_H */
