// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DATAPUMP_H
#define V90_DATAPUMP_H

#include <stdbool.h>

int v90_datapump_register(void);
int v90_datapump_unregister(void);
bool v90_datapump_is_busy(void);

#endif /* V90_DATAPUMP_H */
