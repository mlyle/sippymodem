// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_RATE_H
#define PCM_RATE_H

#include <stdbool.h>
#include <stdint.h>

enum {
	PCM_DOWNSTREAM_BASE_FRAME_BITS = 20,
	PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER = 1,
	PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER = 22,
	PCM_DOWNSTREAM_RATE_CAPABILITY_MASK =
		(UINT32_C(1) << PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER) - 1u,
};

/* A configured rate set must be nonempty. A syntactically valid J record may
 * advertise no rates, so wire-record validation has a different contract. */
static inline bool pcm_downstream_rate_mask_is_usable(uint32_t mask)
{
	return mask != 0 &&
	       (mask & ~(uint32_t)PCM_DOWNSTREAM_RATE_CAPABILITY_MASK) == 0;
}

struct pcm_downstream_rate_selection {
	uint32_t enabled_mask;
	uint32_t selected_bit_rate;
	uint8_t selected_data_rate_number;
};

/* V.90 and V.92 use the same PCM-downstream data-rate numbering. Zero is
 * returned for cleardown and out-of-range values. */
uint32_t pcm_downstream_bit_rate(uint8_t data_rate_number);

/* Enable every standard rate in the inclusive range and select the highest.
 * Rejected calls leave selection unchanged. */
bool pcm_downstream_rates_select(
	struct pcm_downstream_rate_selection *selection,
	unsigned int minimum_rate, unsigned int maximum_rate);

#endif /* PCM_RATE_H */
