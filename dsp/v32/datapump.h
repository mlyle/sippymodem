// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_DATAPUMP_H
#define SIPPYMODEM_V32_DATAPUMP_H

#include <stdbool.h>

enum {
	SIPPYMODEM_V32_4800_BIT_RATE = 4800,
	SIPPYMODEM_V32_7200_BIT_RATE = 7200,
	SIPPYMODEM_V32_9600_BIT_RATE = 9600,
	SIPPYMODEM_V32_12000_BIT_RATE = 12000,
	SIPPYMODEM_V32_14400_BIT_RATE = 14400,
};

int sippymodem_v32_datapump_register(void);
int sippymodem_v32_datapump_unregister(void);
bool sippymodem_v32_datapump_is_busy(void);

#endif
