// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_link.h>
#include <modem.h>

#include "test.h"

/* Keep setter effects visible without constructing a complete host modem. */
long modem_set_param(struct modem *modem, unsigned int name, int value)
{
	modem->update_delay++;
	if (value == 111)
		return -1;
	if (name == MDMPRM_TX_RATE)
		modem->tx_rate = (unsigned int)value;
	else if (name == MDMPRM_RX_RATE)
		modem->rx_rate = (unsigned int)value;
	else
		return -1;
	return 0;
}

static bool test_link_rates(void)
{
	struct modem modem = { 0 };
	struct dp dp = { .modem = &modem };

	CHECK(!datapump_set_link_rates(NULL, 1, 1),
	      "missing datapump accepted link rates");
	dp.modem = NULL;
	CHECK(!datapump_set_link_rates(&dp, 1, 1),
	      "missing modem accepted link rates");
	dp.modem = &modem;
	CHECK(!datapump_set_link_rates(&dp, (uint32_t)INT_MAX + 1u, 1) &&
	      !datapump_set_link_rates(&dp, 1, (uint32_t)INT_MAX + 1u) &&
	      modem.update_delay == 0,
	      "out-of-range rate changed the modem");
	CHECK(datapump_set_link_rates(&dp, 9600, 2400) &&
	      modem.tx_rate == 9600 && modem.rx_rate == 2400 &&
	      modem.update_delay == 2,
	      "asymmetric link rates were not set TX first");
	modem.update_delay = 0;
	CHECK(!datapump_set_link_rates(&dp, 111, 1200) &&
	      modem.update_delay == 1 && modem.rx_rate == 2400,
	      "failed transmit rate still set receive rate");
	modem.update_delay = 0;
	CHECK(!datapump_set_link_rates(&dp, 1200, 111) &&
	      modem.update_delay == 2 && modem.tx_rate == 1200 &&
	      modem.rx_rate == 2400,
	      "failed receive rate changed its value or skipped transmit");
	CHECK(datapump_set_link_rates(&dp, 0, INT_MAX) &&
	      modem.tx_rate == 0 && modem.rx_rate == INT_MAX,
	      "full-width setter values changed");
	return true;
}

int main(void)
{
	return test_link_rates() ? 0 : 1;
}
