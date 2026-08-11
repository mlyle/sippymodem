// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_DATAPUMP_H
#define SIPPYMODEM_V8_DATAPUMP_H

#include "modem_defs.h"
#include "v8_menu.h"

#include <stdbool.h>

/* Returns NONE when the requested datapump has no V.8 handoff yet. */
static inline enum sippymodem_v8_modulation
sippymodem_v8_datapump_modulation(enum DP_ID id)
{
	switch (id) {
	case DP_V21:
		return SIPPYMODEM_V8_MODULATION_V21;
	case DP_V22:
	case DP_V22BIS:
		return SIPPYMODEM_V8_MODULATION_V22;
	case DP_V23:
		return SIPPYMODEM_V8_MODULATION_V23_DUPLEX;
	case DP_V32:
		return SIPPYMODEM_V8_MODULATION_V32;
	default:
		return SIPPYMODEM_V8_MODULATION_NONE;
	}
}

int sippymodem_v8_datapump_register(void);
int sippymodem_v8_datapump_unregister(void);
bool sippymodem_v8_datapump_is_busy(void);

#endif
