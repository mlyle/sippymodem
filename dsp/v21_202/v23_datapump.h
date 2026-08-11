// SPDX-License-Identifier: BSD-3-Clause

#ifndef V23_DATAPUMP_H
#define V23_DATAPUMP_H

#include <stdbool.h>

enum {
	V23_FORWARD_BIT_RATE = 1200,
	V23_BACKWARD_BIT_RATE = 75,
};

int v23_datapump_register(void);
int v23_datapump_unregister(void);
bool v23_datapump_is_busy(void);

#endif /* V23_DATAPUMP_H */
