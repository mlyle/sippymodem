// SPDX-License-Identifier: BSD-3-Clause

#include "slmodem_adapter.h"

#include "modem.h"
#include "media.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static enum sippymodem_modem_core_result process_modem(
	void *context, int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	struct sippymodem_slmodem_adapter *adapter = context;
	struct modem *modem;

	if (adapter == NULL || adapter->modem == NULL ||
	    receive_samples == NULL || transmit_samples == NULL || count == 0 ||
	    count > INT_MAX)
		return SIPPYMODEM_MODEM_CORE_ERROR;
	modem = adapter->modem;
	if (!modem->started)
		return SIPPYMODEM_MODEM_CORE_STOPPED;

	modem_process(modem, receive_samples, transmit_samples, (int)count);
	return modem->started ? SIPPYMODEM_MODEM_CORE_OK :
			      SIPPYMODEM_MODEM_CORE_STOPPED;
}

bool sippymodem_slmodem_adapter_init(
	struct sippymodem_slmodem_adapter *adapter, struct modem *modem)
{
	struct sippymodem_slmodem_adapter initialized = {
		.modem = modem,
		.core = {
			.context = adapter,
			.process = process_modem,
		},
	};

	if (adapter == NULL || modem == NULL ||
	    modem->srate != SIPPYMODEM_MEDIA_SAMPLE_RATE ||
	    modem->format != MODEM_FORMAT || modem->frag != MODEM_FRAG)
		return false;
	*adapter = initialized;
	return true;
}

const struct sippymodem_modem_core *sippymodem_slmodem_adapter_core(
	const struct sippymodem_slmodem_adapter *adapter)
{
	return adapter != NULL && adapter->core.process != NULL ?
		       &adapter->core :
		       NULL;
}
