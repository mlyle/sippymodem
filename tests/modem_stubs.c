// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>

void modem_dp_connected(struct modem *modem)
{
	(void)modem;
}

void modem_dp_reconnected(struct modem *modem)
{
	(void)modem;
}

void modem_update_status(struct modem *modem, unsigned int status)
{
	(void)modem;
	(void)status;
}

__attribute__((weak)) void modem_note_disconnect(struct modem *modem, bool local, const char *reason)
{
	(void)modem;
	(void)local;
	(void)reason;
}

/* Bare DSP fixtures supply bits directly and have no LAPM owner. */
bool modem_ec_get_tx_recovery(const struct modem *modem,
	struct modem_tx_recovery *observation)
{
	(void)modem;
	*observation = (struct modem_tx_recovery){0};
	return false;
}

bool modem_ec_probe_after_tx_recovery(struct modem *modem)
{
	(void)modem;
	return false;
}
