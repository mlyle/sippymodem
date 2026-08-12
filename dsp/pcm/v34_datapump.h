// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATAPUMP_H
#define V34_DATAPUMP_H

#include <stdbool.h>

enum {
	V34_INITIAL_MAX_BIT_RATE = 9600,
};

int v34_datapump_register(void);
int v34_datapump_unregister(void);
bool v34_datapump_is_busy(void);

#endif /* V34_DATAPUMP_H */
