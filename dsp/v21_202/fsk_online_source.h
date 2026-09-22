// SPDX-License-Identifier: BSD-3-Clause

#ifndef FSK_ONLINE_SOURCE_H
#define FSK_ONLINE_SOURCE_H

#include "fsk.h"
#include "modem.h"

/* Keep the next host bit until its entire FSK symbol has been generated.
 * A training mark already in progress finishes before host data begins. */
struct fsk_online_source {
	uint8_t bit;
	bool pending;
};

static inline enum fsk_result fsk_online_source_sample(
	struct fsk_online_source *source,
	struct fsk_transmitter *transmitter,
	struct modem *modem,
	int16_t *sample)
{
	bool consumed;

	if (!source->pending && fsk_transmitter_can_accept_bit(transmitter)) {
		source->bit = 1;
		if (modem_get_bits(modem, 1, &source->bit, 1) != 1)
			source->bit = 1;
		source->bit &= 1u;
		source->pending = true;
	}
	if (fsk_transmitter_sample(
		    transmitter, source->pending ? source->bit : 1,
		    sample, &consumed) != FSK_OK)
		return FSK_INVALID_ARGUMENT;
	if (consumed)
		source->pending = false;
	return FSK_OK;
}

#endif
