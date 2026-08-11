// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V22_DATAPUMP_H
#define SIPPYMODEM_V22_DATAPUMP_H

#include <stdbool.h>

enum {
	SIPPYMODEM_V22_BIT_RATE = 1200,
	SIPPYMODEM_V22BIS_BIT_RATE = 2400,
};

int sippymodem_v22_datapump_register(void);
int sippymodem_v22_datapump_unregister(void);
bool sippymodem_v22_datapump_is_busy(void);

#endif /* SIPPYMODEM_V22_DATAPUMP_H */
