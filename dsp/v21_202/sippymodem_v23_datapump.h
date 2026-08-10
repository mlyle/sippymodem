// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V23_DATAPUMP_H
#define SIPPYMODEM_V23_DATAPUMP_H

#include <stdbool.h>

enum {
	SIPPYMODEM_V23_FORWARD_BIT_RATE = 1200,
	SIPPYMODEM_V23_BACKWARD_BIT_RATE = 75,
};

int sippymodem_v23_datapump_register(void);
int sippymodem_v23_datapump_unregister(void);
bool sippymodem_v23_datapump_is_busy(void);

#endif
