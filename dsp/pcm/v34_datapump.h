// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATAPUMP_H
#define V34_DATAPUMP_H

#include <stdbool.h>

enum {
	V34_MIN_BIT_RATE = 2400,
	V34_MAX_BIT_RATE = 14400,
	V34_RATE_STEP = 2400,
};

int v34_datapump_register(void);
int v34_datapump_unregister(void);
bool v34_datapump_is_busy(void);

#endif /* V34_DATAPUMP_H */
