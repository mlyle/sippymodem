// SPDX-License-Identifier: BSD-3-Clause

#ifndef FSK300_DATAPUMP_H
#define FSK300_DATAPUMP_H

#include <stdbool.h>

enum {
	FSK300_BIT_RATE = 300,
};

int fsk300_datapump_register(void);
int fsk300_datapump_unregister(void);
bool fsk300_datapump_is_busy(void);

#endif /* FSK300_DATAPUMP_H */
