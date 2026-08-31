// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_DATAPUMP_H
#define V8_DATAPUMP_H

#include "modem_defs.h"
#include "v8_menu.h"

#include <stdbool.h>

struct dp;

/* Returns the classic V.8 mode carried with the requested datapump. */
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
	case DP_V32BIS:
		return V8_MODULATION_V32;
	case DP_V34:
	case DP_V90:
	case DP_V90_NO_V8BIS:
	case DP_V92:
		return V8_MODULATION_V34_DUPLEX;
	default:
		return V8_MODULATION_NONE;
	}
}

static inline bool v8_datapump_uses_pcm(enum DP_ID id)
{
	return id == DP_V90 || id == DP_V90_NO_V8BIS || id == DP_V92;
}

int v8_datapump_register(void);
int v8_datapump_unregister(void);
bool v8_datapump_is_busy(void);
/* True only while a completed PCM-mode V.8 exchange is handing this modem
 * to conventional V.34 because the joint menu selected no PCM roles. */
bool v8_datapump_selected_v34_fallback(const struct dp *dp);

#endif
