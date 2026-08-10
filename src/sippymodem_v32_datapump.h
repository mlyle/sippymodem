// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_DATAPUMP_H
#define SIPPYMODEM_V32_DATAPUMP_H

#include <stdbool.h>

enum {
	SIPPYMODEM_V32_BIT_RATE = 4800,
};

int sippymodem_v32_datapump_register(void);
int sippymodem_v32_datapump_unregister(void);
bool sippymodem_v32_datapump_is_busy(void);

#endif
