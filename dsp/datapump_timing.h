// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMP_TIMING_H
#define DATAPUMP_TIMING_H

#include "modem.h"

#include <stdint.h>

enum {
	DATAPUMP_DEFAULT_CARRIER_WAIT_SECONDS = 60,
	/* Shared permissive line-power floor for QAM carrier gates. */
	DATAPUMP_MIN_QAM_LINE_POWER = 1000000,
};

/* Accumulate media samples without wrapping past a training or loss limit. */
static inline void datapump_add_samples_bounded(
	uint32_t *value, uint32_t increment, uint32_t limit)
{
	*value = *value >= limit || increment >= limit - *value ?
		       limit : *value + increment;
}

/* Mean square of one bounded 8 kHz media fragment. Use the same power floor
 * for V.22, V.32, and V.34; timing and protocol qualification stay local. */
static inline bool datapump_qam_line_power_present(
	const int16_t *samples, int count)
{
	uint64_t power = 0;

	if (samples == NULL || count <= 0 || count > MODEM_FRAG)
		return false;
	for (int index = 0; index < count; ++index) {
		int32_t sample = samples[index];

		power += (uint64_t)(sample * sample);
	}
	return power / (unsigned int)count >= DATAPUMP_MIN_QAM_LINE_POWER;
}

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
