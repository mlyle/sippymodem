// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>

void modem_dp_connected(struct modem *modem)
{
	(void)modem;
}

void modem_dp_reconnected(struct modem *modem,
	enum modem_reframe_direction direction)
{
	(void)modem;
	(void)direction;
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
bool modem_ec_xid_recovery_ready(const struct modem *modem,
	uint64_t timing_sample, uint16_t round_trip_samples)
{
	(void)modem;
	(void)timing_sample;
	(void)round_trip_samples;
	return false;
}

bool modem_ec_get_tx_recovery(const struct modem *modem,
	struct modem_tx_recovery *observation)
{
	(void)modem;
	*observation = (struct modem_tx_recovery){0};
	return false;
}

bool modem_ec_probe_after_tx_recovery(struct modem *modem, uint64_t grant)
{
	(void)grant;
	(void)modem;
	return false;
}

bool modem_ec_verify_tx(struct modem *modem)
{
	(void)modem;
	return false;
}

uint64_t modem_ec_take_retrain_request(struct modem *modem)
{
	(void)modem;
	return 0;
}

bool modem_ec_cancel_retrain_request(struct modem *modem, uint64_t grant)
{
	(void)modem;
	(void)grant;
	return false;
}

/* Bare DSP fixtures have no established HDLC/LAPM owner. */
bool modem_accepts_erased_bits(const struct modem *modem)
{
	(void)modem;
	return false;
}

bool modem_put_erasure_bits(struct modem *modem, int bits)
{
	(void)modem;
	(void)bits;
	return false;
}
