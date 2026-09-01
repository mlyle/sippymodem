// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_TIMING_H
#define DATAPUMP_TIMING_H

#include "modem.h"

#include <stdint.h>

enum {
	DATAPUMP_DEFAULT_CARRIER_WAIT_SECONDS = 60,
};

/* Return the S7 carrier-wait interval in 8 kHz media ticks. */
static inline uint64_t datapump_carrier_wait_ticks(struct modem *modem)
{
	int seconds;

	if (modem == NULL)
		return 0;
	seconds = modem_get_sreg(modem, SREG_WAIT_CARRIER_AFTER_DIAL);
	if (seconds <= 0)
		seconds = DATAPUMP_DEFAULT_CARRIER_WAIT_SECONDS;
	return (uint64_t)(unsigned int)seconds * MODEM_RATE;
}

/* Return the S10 carrier-loss interval in 8 kHz media ticks.  A zero setting
 * requests immediate disconnect; retain one tick so callers can make the
 * transition after observing the first missing sample. */
static inline uint32_t datapump_carrier_loss_ticks(struct modem *modem)
{
	int tenths;

	if (modem == NULL)
		return 0;
	tenths = modem_get_sreg(
		modem, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	return tenths > 0 ?
		(uint32_t)(unsigned int)tenths * MODEM_RATE / 10u : 1u;
}

#endif
