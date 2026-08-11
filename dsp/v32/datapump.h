// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_DATAPUMP_H
#define V32_DATAPUMP_H

#include <stdbool.h>

enum {
	V32_4800_BIT_RATE = 4800,
	V32_7200_BIT_RATE = 7200,
	V32_9600_BIT_RATE = 9600,
	V32_12000_BIT_RATE = 12000,
	V32_14400_BIT_RATE = 14400,
};

int v32_datapump_register(void);
int v32_datapump_unregister(void);
bool v32_datapump_is_busy(void);

#endif
