// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_BAUDOT_DATAPUMP_H
#define SIPPYMODEM_BAUDOT_DATAPUMP_H

#include <stdbool.h>

int sippymodem_baudot_datapump_register(void);
int sippymodem_baudot_datapump_unregister(void);
bool sippymodem_baudot_datapump_is_busy(void);

#endif
