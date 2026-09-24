// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_link.h>
#include <modem.h>

#include <string.h>

#include "test.h"

static int source_calls;
static int source_wrapper_calls;
static int source_result;
static int sink_calls;
static int sink_wrapper_calls;
static int sink_result;
static u8 sink_bits[DATAPUMP_LINK_MAX_BITS];

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
	source_wrapper_calls++;
	if (modem->get_bits != NULL)
		return modem->get_bits(modem, nbits, bits, count);
	memset(bits, 0xff, (size_t)count);
	return count;
}

static int test_put_bits(struct modem *modem, int nbits, u8 *bits, int count)
{
	(void)modem;
	sink_calls++;
	if (nbits != 1 || count > (int)sizeof(sink_bits))
		return -1;
	memcpy(sink_bits, bits, (size_t)count);
	if (count != 0)
		bits[0] = 0xff; /* A host callback may reuse its input buffer. */
	return sink_result;
}

int modem_put_bits(struct modem *modem, int nbits, u8 *bits, int count)
{
	sink_wrapper_calls++;
	if (modem->put_bits != NULL)
		return modem->put_bits(modem, nbits, bits, count);
	return count;
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
	source_wrapper_calls = 0;
	CHECK(datapump_source_bits(&dp, true, bits, sizeof(bits)) &&
	      memcmp(bits, (u8[]){ 1, 1, 1, 1 }, sizeof(bits)) == 0 &&
	      source_wrapper_calls == 1,
	      "missing host source bypassed the modem wrapper");
	CHECK(!datapump_source_bits(NULL, true, bits, sizeof(bits)) &&
	      !datapump_source_bits(&dp, true, NULL, sizeof(bits)) &&
	      !datapump_source_bits(&dp, true, bits, (size_t)INT_MAX + 1u),
	      "invalid source request accepted");
	return true;
}

static bool test_source_split_bits(void)
{
	struct modem modem = { .get_bits = test_get_bits };
	struct dp dp = { .modem = &modem };
	u8 primary[DATAPUMP_LINK_MAX_BITS];
	u8 auxiliary = 0;

	source_result = 2;
	source_calls = 0;
	CHECK(datapump_source_split_bits(&dp, true, primary, 3, &auxiliary) &&
	      memcmp(primary, (u8[]){ 0, 1, 1 }, 3) == 0 && auxiliary == 1 &&
	      source_calls == 1,
	      "primary and auxiliary bits were not split from one host read");
	source_result = DATAPUMP_LINK_MAX_BITS;
	CHECK(datapump_source_split_bits(&dp, true, primary,
					   DATAPUMP_LINK_MAX_BITS - 1,
					   &auxiliary) &&
	      primary[0] == 0 && primary[DATAPUMP_LINK_MAX_BITS - 2] == 0 &&
	      auxiliary == 1 && source_calls == 2,
	      "maximum mapping frame was not split safely");
	source_result = 3;
	CHECK(datapump_source_split_bits(&dp, true, primary, 3, NULL) &&
	      memcmp(primary, (u8[]){ 0, 1, 0 }, 3) == 0 &&
	      source_calls == 3,
	      "primary-only mapping frame changed");
	source_calls = 0;
	CHECK(datapump_source_split_bits(&dp, false, primary, 3, &auxiliary) &&
	      memcmp(primary, (u8[]){ 1, 1, 1 }, 3) == 0 &&
	      auxiliary == 1 && source_calls == 0,
	      "offline split frame read the host");
	CHECK(!datapump_source_split_bits(&dp, true, NULL, 3, &auxiliary) &&
	      !datapump_source_split_bits(&dp, true, primary,
					    DATAPUMP_LINK_MAX_BITS, &auxiliary) &&
	      !datapump_source_split_bits(&dp, true, primary,
					    DATAPUMP_LINK_MAX_BITS + 1, NULL) &&
	      source_calls == 0,
	      "invalid split frame reached the host");
	return true;
}

static bool test_deliver_bits(void)
{
	struct modem modem = { .put_bits = test_put_bits };
	struct dp dp = { .modem = &modem };
	u8 bits[DATAPUMP_LINK_MAX_BITS + 1];

	memset(bits, 1, sizeof(bits));
	sink_calls = 0;
	sink_result = DATAPUMP_LINK_MAX_BITS;
	CHECK(datapump_deliver_bits(&dp, true, bits,
				    DATAPUMP_LINK_MAX_BITS) &&
	      sink_calls == 1 && bits[0] == 1 &&
	      memcmp(sink_bits, bits, DATAPUMP_LINK_MAX_BITS) == 0,
	      "valid frame was not delivered through a private copy");
	sink_calls = 0;
	CHECK(datapump_deliver_bits(&dp, false, bits, 4) && sink_calls == 0,
	      "offline frame called the host sink");
	modem.put_bits = NULL;
	sink_wrapper_calls = 0;
	CHECK(datapump_deliver_bits(&dp, true, bits, 4) && sink_calls == 0 &&
	      sink_wrapper_calls == 1,
	      "missing host sink bypassed the modem wrapper");
	modem.put_bits = test_put_bits;
	bits[3] = 2;
	CHECK(!datapump_deliver_bits(&dp, false, bits, 4) &&
	      !datapump_deliver_bits(&dp, true, bits, 4) && sink_calls == 0,
	      "nonbinary frame escaped validation");
	bits[3] = 1;
	CHECK(!datapump_deliver_bits(&dp, true, bits, sizeof(bits)) &&
	      !datapump_deliver_bits(&dp, true, NULL, 4) &&
	      !datapump_deliver_bits(NULL, true, bits, 4) && sink_calls == 0,
	      "invalid delivery request reached the host sink");
	sink_result = 3;
	CHECK(!datapump_deliver_bits(&dp, true, bits, 4) && sink_calls == 1,
	      "short host write was accepted");
	return true;
}

int main(void)
{
	return test_link_rates() && test_source_bits() &&
	       test_source_split_bits() && test_deliver_bits() ? 0 : 1;
}
