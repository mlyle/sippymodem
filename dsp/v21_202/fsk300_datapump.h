// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_FSK300_DATAPUMP_H
#define SIPPYMODEM_FSK300_DATAPUMP_H

#include <stdbool.h>

enum {
	SIPPYMODEM_FSK300_BIT_RATE = 300,
};

int sippymodem_fsk300_datapump_register(void);
int sippymodem_fsk300_datapump_unregister(void);
bool sippymodem_fsk300_datapump_is_busy(void);

#endif /* SIPPYMODEM_FSK300_DATAPUMP_H */
