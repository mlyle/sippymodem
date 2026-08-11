// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_adapter.h"

#include "modem.h"
#include "media.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static enum modem_core_result process_modem(
	void *context, int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	struct slmodem_adapter *adapter = context;
	struct modem *modem;

	if (adapter == NULL || adapter->modem == NULL ||
	    receive_samples == NULL || transmit_samples == NULL || count == 0 ||
	    count > INT_MAX)
		return MODEM_CORE_ERROR;
	modem = adapter->modem;
	if (!modem->started)
		return MODEM_CORE_STOPPED;

	modem_process(modem, receive_samples, transmit_samples, (int)count);
	return modem->started ? MODEM_CORE_OK :
			      MODEM_CORE_STOPPED;
}

bool slmodem_adapter_init(
	struct slmodem_adapter *adapter, struct modem *modem)
{
	struct slmodem_adapter initialized = {
		.modem = modem,
		.core = {
			.context = adapter,
			.process = process_modem,
		},
	};

	if (adapter == NULL || modem == NULL ||
	    modem->srate != MEDIA_SAMPLE_RATE ||
	    modem->format != MODEM_FORMAT || modem->frag != MODEM_FRAG)
		return false;
	*adapter = initialized;
	return true;
}

const struct modem_core *slmodem_adapter_core(
	const struct slmodem_adapter *adapter)
{
	return adapter != NULL && adapter->core.process != NULL ?
		       &adapter->core :
		       NULL;
}
