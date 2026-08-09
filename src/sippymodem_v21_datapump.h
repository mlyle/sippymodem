// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V21_DATAPUMP_H
#define SIPPYMODEM_V21_DATAPUMP_H

#include <stdbool.h>

int sippymodem_v21_datapump_register(void);
int sippymodem_v21_datapump_unregister(void);
bool sippymodem_v21_datapump_is_busy(void);

#endif
