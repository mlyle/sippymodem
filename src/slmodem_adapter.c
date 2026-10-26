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

static enum modem_core_result process_modem_g711(
	void *context, enum g711_law law,
	const uint8_t *receive_codewords, int16_t *receive_samples, const bool *received_mask,
	uint8_t *transmit_codewords, int16_t *transmit_samples,
	size_t count, bool *exact_output)
{
	struct slmodem_adapter *adapter = context;
	struct modem *modem;

	if (exact_output != NULL)
		*exact_output = false;
	if (adapter == NULL || adapter->modem == NULL ||
	    !g711_law_is_valid(law) ||
	    receive_codewords == NULL || receive_samples == NULL ||
	    transmit_codewords == NULL || transmit_samples == NULL ||
	    exact_output == NULL || count == 0 || count > INT_MAX)
		return MODEM_CORE_ERROR;
	modem = adapter->modem;
	if (!modem->started)
		return MODEM_CORE_STOPPED;

	modem_process_g711(
		modem, law, receive_codewords, receive_samples, received_mask,
		transmit_codewords, transmit_samples, (int)count, exact_output);
	return modem->started ? MODEM_CORE_OK : MODEM_CORE_STOPPED;
}

static size_t receive_pending(void *context)
{
	const struct slmodem_adapter *adapter = context;
	return adapter ? modem_receive_pending(adapter->modem) : 0;
}

static enum modem_core_result drain_receive(void *context, size_t budget)
{
	struct slmodem_adapter *adapter = context;
	if (!adapter || !adapter->modem || !budget || budget > UINT_MAX)
		return MODEM_CORE_ERROR;
	if (!adapter->modem->started)
		return MODEM_CORE_STOPPED;
	if (modem_drain_receive(adapter->modem, (unsigned)budget) != 0)
		return MODEM_CORE_ERROR;
	return adapter->modem->started ? MODEM_CORE_OK : MODEM_CORE_STOPPED;
}

bool slmodem_adapter_init(
	struct slmodem_adapter *adapter, struct modem *modem)
{
	struct slmodem_adapter initialized = {
		.modem = modem,
		.core = {
			.context = adapter,
			.process = process_modem,
			.process_g711 = process_modem_g711,
			.receive_pending = receive_pending,
			.drain_receive = drain_receive,
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
