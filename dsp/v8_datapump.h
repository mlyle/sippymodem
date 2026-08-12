// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_DATAPUMP_H
#define V8_DATAPUMP_H

#include "modem_defs.h"
#include "v8_menu.h"

#include <stdbool.h>

/* Returns NONE when the requested datapump has no V.8 handoff yet. */
static inline enum v8_modulation
v8_datapump_modulation(enum DP_ID id)
{
	switch (id) {
	case DP_V21:
		return V8_MODULATION_V21;
	case DP_V22:
	case DP_V22BIS:
		return V8_MODULATION_V22;
	case DP_V23:
		return V8_MODULATION_V23_DUPLEX;
	case DP_V32:
		return V8_MODULATION_V32;
	case DP_V34:
		return V8_MODULATION_V34_DUPLEX;
	default:
		return V8_MODULATION_NONE;
	}
}

int v8_datapump_register(void);
int v8_datapump_unregister(void);
bool v8_datapump_is_busy(void);

#endif
