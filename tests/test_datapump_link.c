// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_link.h>
#include <modem.h>

#include <string.h>

#include "test.h"

static int source_calls;
static int source_result;

static int test_get_bits(struct modem *modem, int nbits, u8 *bits, int count)
{
	(void)modem;
	source_calls++;
	if (nbits != 1)
		return -1;
	for (int index = 0; index < count; ++index)
		bits[index] = (u8)(index + 2);
	return source_result;
}

int modem_get_bits(struct modem *modem, int nbits, u8 *bits, int count)
{
	return modem->get_bits(modem, nbits, bits, count);
}

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

static bool test_source_bits(void)
{
	struct modem modem = { .get_bits = test_get_bits };
	struct dp dp = { .modem = &modem };
	u8 bits[4];

	source_calls = 0;
	source_result = 2;
	memset(bits, 0xaa, sizeof(bits));
	CHECK(datapump_source_bits(&dp, true, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 0, 1, 1, 1 }, sizeof(bits)) == 0 &&
	      source_calls == 1,
	      "short host read did not preserve payload and mark remainder");
	source_result = -1;
	CHECK(datapump_source_bits(&dp, true, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 1, 1, 1, 1 }, sizeof(bits)) == 0,
	      "failed host read did not emit marks");
	source_result = 4;
	CHECK(datapump_source_bits(&dp, true, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 0, 1, 0, 1 }, sizeof(bits)) == 0,
	      "full host read was not normalized to bits");
	source_calls = 0;
	CHECK(datapump_source_bits(&dp, false, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 1, 1, 1, 1 }, sizeof(bits)) == 0 &&
	      source_calls == 0,
	      "offline frame read host bits");
	modem.get_bits = NULL;
	CHECK(datapump_source_bits(&dp, true, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 1, 1, 1, 1 }, sizeof(bits)) == 0,
	      "missing host source did not emit marks");
	CHECK(!datapump_source_bits(NULL, true, bits, sizeof(bits)) &&
	      !datapump_source_bits(&dp, true, NULL, sizeof(bits)) &&
	      !datapump_source_bits(&dp, true, bits, (size_t)INT_MAX + 1u),
	      "invalid source request accepted");
	return true;
}

int main(void)
{
	return test_link_rates() && test_source_bits() ? 0 : 1;
}
