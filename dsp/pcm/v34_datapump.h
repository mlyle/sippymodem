// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATAPUMP_H
#define V34_DATAPUMP_H

#include <stdbool.h>

enum {
	V34_MIN_BIT_RATE = 2400,
	V34_MAX_BIT_RATE = 21600,
	V34_RATE_STEP = 2400,
};

/* Decision-directed tracking must follow the data waveform after Phase 4;
 * this step is deliberately stronger than the initial equalizer settling
 * step because the far-end transmitter continues to drift during a call. */
#define V34_DATA_EQUALIZER_TRACKING_STEP 0.05

int v34_datapump_register(void);
int v34_datapump_unregister(void);
bool v34_datapump_is_busy(void);

#endif /* V34_DATAPUMP_H */
