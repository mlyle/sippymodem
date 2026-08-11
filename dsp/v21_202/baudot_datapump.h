// SPDX-License-Identifier: BSD-3-Clause

#ifndef BAUDOT_DATAPUMP_H
#define BAUDOT_DATAPUMP_H

#include <stdbool.h>

int baudot_datapump_register(void);
int baudot_datapump_unregister(void);
bool baudot_datapump_is_busy(void);

#endif /* BAUDOT_DATAPUMP_H */
