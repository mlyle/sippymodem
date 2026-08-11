// SPDX-License-Identifier: BSD-3-Clause

#ifndef SLMODEM_ADAPTER_H
#define SLMODEM_ADAPTER_H

#include "audio_path.h"

#include <stdbool.h>

struct modem;

struct slmodem_adapter {
	struct modem *modem;
	struct modem_core core;
};

/* modem must outlive adapter; both must outlive calls through the core. */
bool slmodem_adapter_init(
	struct slmodem_adapter *adapter, struct modem *modem);
const struct modem_core *slmodem_adapter_core(
	const struct slmodem_adapter *adapter);

#endif
