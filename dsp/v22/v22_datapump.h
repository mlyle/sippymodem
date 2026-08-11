// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22_DATAPUMP_H
#define V22_DATAPUMP_H

#include <stdbool.h>

enum {
	V22_BIT_RATE = 1200,
	V22BIS_BIT_RATE = 2400,
};

int v22_datapump_register(void);
int v22_datapump_unregister(void);
bool v22_datapump_is_busy(void);

#endif /* V22_DATAPUMP_H */
