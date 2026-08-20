// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CARRIER_H
#define V32_CARRIER_H

#include "qam.h"

enum {
	V32_CARRIER_SAMPLE_RATE = 8000,
	V32_CARRIER_SYMBOL_RATE = 2400,
	V32_CARRIER_FREQUENCY_HZ = 1800,
	/* The V.32 pulse-shaper profile spans twelve symbol intervals. */
	V32_CARRIER_PULSE_SPAN_SYMBOLS = 12,
};

enum v32_carrier_result {
	V32_CARRIER_OK,
	V32_CARRIER_INVALID_ARGUMENT,
};

/* Return the one 8 kHz V.32 passband waveform shared by transmitter and
 * receiver.  Rejected calls leave the destination unchanged. */
enum v32_carrier_result v32_carrier_channel(
	struct qam_channel *channel);

#endif
