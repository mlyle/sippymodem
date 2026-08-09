// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SLMODEM_ADAPTER_H
#define SIPPYMODEM_SLMODEM_ADAPTER_H

#include "sippymodem_audio_path.h"

#include <stdbool.h>

struct modem;

struct sippymodem_slmodem_adapter {
	struct modem *modem;
	struct sippymodem_modem_core core;
};

/* modem must outlive adapter; both must outlive calls through the core. */
bool sippymodem_slmodem_adapter_init(
	struct sippymodem_slmodem_adapter *adapter, struct modem *modem);
const struct sippymodem_modem_core *sippymodem_slmodem_adapter_core(
	const struct sippymodem_slmodem_adapter *adapter);

#endif
