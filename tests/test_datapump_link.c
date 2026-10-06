// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_link.h>
#include <datapump_transmit.h>
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
static unsigned int initial_links, resumed_links;

void modem_dp_connected(struct modem *modem)
{
	(void)modem;
	++initial_links;
}

void modem_dp_reconnected(struct modem *modem)
{
	(void)modem;
	++resumed_links;
}

static bool test_training_completion_ownership(void)
{
	struct modem modem = { 0 };
	struct dp parent = { .id = DP_V8, .modem = &modem };
	struct dp child = { .id = DP_V32, .modem = &modem };
	struct training_session training;

	modem.dp = &parent;
	CHECK(training_session_init(&training, MODEM_RATE) == TRAINING_SESSION_OK &&
	      datapump_complete_training(&child, &training) &&
	      training_session_is_online(&training) && child.status == DPSTAT_CONNECT &&
	      initial_links == 0 && resumed_links == 0,
	      "unadopted legacy child published its parent's connection");
	modem.dp = &child;
	CHECK(training_session_start_retrain(&training, TRAINING_REMOTE_RETRAIN) ==
		      TRAINING_SESSION_OK &&
	      datapump_complete_training(&child, &training) &&
	      resumed_links == 1 && initial_links == 0,
	      "adopted physical retrain did not resume the retained host link");
	CHECK(training_session_init(&training, MODEM_RATE) == TRAINING_SESSION_OK &&
	      datapump_complete_training(&child, &training) &&
	      initial_links == 1 && resumed_links == 1 &&
	      !datapump_complete_training(&child, &training) && initial_links == 1 &&
	      resumed_links == 1,
	      "initial training resumed an old link or announced completion twice");
	return true;
}

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

/* Keep host publication failures visible without a complete modem. */
int modem_set_link_rates(struct modem *modem, uint32_t transmit_rate, uint32_t receive_rate)
{
	if (modem == NULL || transmit_rate > INT_MAX || receive_rate > INT_MAX)
		return -1;
	unsigned int *setter_calls = modem->dev_data;
	(*setter_calls)++;
	if (transmit_rate == 111 || receive_rate == 111)
		return -1;
	modem->tx_rate = transmit_rate;
	modem->rx_rate = receive_rate;
	return 0;
}

static bool test_link_rates(void)
{
	unsigned int setter_calls = 0;
	struct modem modem = { .dev_data = &setter_calls };
	struct dp dp = { .modem = &modem };

	CHECK(!datapump_set_link_rates(NULL, 1, 1),
	      "missing datapump accepted link rates");
	dp.modem = NULL;
	CHECK(!datapump_set_link_rates(&dp, 1, 1),
	      "missing modem accepted link rates");
	dp.modem = &modem;
	CHECK(!datapump_set_link_rates(&dp, (uint32_t)INT_MAX + 1u, 1) &&
	      !datapump_set_link_rates(&dp, 1, (uint32_t)INT_MAX + 1u) &&
	      setter_calls == 0,
	      "out-of-range rate changed the modem");
	CHECK(datapump_set_link_rates(&dp, 9600, 2400) &&
	      modem.tx_rate == 9600 && modem.rx_rate == 2400 &&
	      setter_calls == 1,
	      "asymmetric link rates were not published together");
	setter_calls = 0;
	CHECK(!datapump_set_link_rates(&dp, 111, 1200) &&
	      setter_calls == 1 && modem.tx_rate == 9600 && modem.rx_rate == 2400,
	      "failed publication changed either directional rate");
	setter_calls = 0;
	CHECK(!datapump_set_link_rates(&dp, 1200, 111) &&
	      setter_calls == 1 && modem.tx_rate == 9600 &&
	      modem.rx_rate == 2400,
	      "failed receive publication changed either directional rate");
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

static struct dp_link_rates negotiated_rates(const struct dp *dp)
{
	(void)dp;
	return (struct dp_link_rates){ 45333, 26400, 8000, 3200 };
}

static bool test_rate_diagnostics(void)
{
	struct modem modem = { .tx_rate = 1200, .rx_rate = 75 };
	struct dp_operations operations = { 0 };
	struct dp dp = { .id = DP_V23, .modem = &modem, .op = &operations };
	struct dp_link_rates rates = datapump_get_link_rates(&dp);

	CHECK(rates.transmit_bps == 0 && rates.receive_bps == 0 &&
	      rates.transmit_baud == 0 && rates.receive_baud == 0 &&
	      datapump_get_link_rates(NULL).transmit_bps == 0,
	      "training diagnostics exposed stale host rates");
	dp.status = DPSTAT_CONNECT;
	rates = datapump_get_link_rates(&dp);
	CHECK(rates.transmit_bps == 1200 && rates.receive_bps == 75 &&
	      rates.transmit_baud == 1200 && rates.receive_baud == 75,
	      "V.23 diagnostics lost their asymmetric directions");
	dp.id = DP_V22BIS;
	modem.tx_rate = modem.rx_rate = 2400;
	rates = datapump_get_link_rates(&dp);
	CHECK(rates.transmit_bps == 2400 && rates.transmit_baud == 600 &&
	      rates.receive_baud == 600, "V.22bis confused bits and symbols");
	dp.id = DP_V32BIS;
	modem.tx_rate = modem.rx_rate = 14400;
	rates = datapump_get_link_rates(&dp);
	CHECK(rates.transmit_bps == 14400 && rates.transmit_baud == 2400 &&
	      rates.receive_baud == 2400, "V.32bis confused bits and symbols");
	dp.status = DPSTAT_OK;
	operations.link_rates = negotiated_rates;
	rates = datapump_get_link_rates(&dp);
	CHECK(rates.transmit_bps == 45333 && rates.receive_bps == 26400 &&
	      rates.transmit_baud == 8000 && rates.receive_baud == 3200,
	      "negotiated rates were replaced by stale host rates");
	return true;
}

static bool test_transmit_tail(void)
{
	CHECK(datapump_symbol_tail(8000, 300, 1, 1) == 27 &&
	      datapump_symbol_tail(8000, 75, 1, 1) == 107 &&
	      datapump_symbol_tail(8000, 24000, 7, 40) == 94,
	      "tail bound rounded down or lost the fractional symbol clock");
	CHECK(datapump_symbol_tail(8000, 300, 1, 0) == 0 &&
	      datapump_symbol_tail(0, 300, 1, 1) == -1 &&
	      datapump_symbol_tail(8000, 0, 1, 1) == -1 &&
	      datapump_symbol_tail(8000, 300, 0, 1) == -1 &&
	      datapump_symbol_tail(UINT32_MAX, 1, UINT32_MAX, UINT_MAX) == -1 &&
	      datapump_symbol_tail(UINT32_MAX, 1, 1, 1) == -1,
	      "invalid or overflowing tail bound was accepted");
	struct qam_modulator modulator = { .channel = {
		.sample_rate = 8000, .symbol_rate_numerator = 600,
		.symbol_rate_denominator = 1, .pulse_span_symbols = 6,
	} };
	CHECK(datapump_qam_tail(&modulator) == 94 && datapump_qam_tail(NULL) == -1,
	      "QAM tail omitted the current symbol or finite pulse history");
	modulator.channel.pulse_span_symbols = UINT_MAX;
	CHECK(datapump_qam_tail(&modulator) == -1, "QAM pulse span wrapped");
	return true;
}

int main(void)
{
	return test_transmit_tail() && test_rate_diagnostics() && test_training_completion_ownership() && test_link_rates() && test_source_bits() &&
	       test_source_split_bits() && test_deliver_bits() ? 0 : 1;
}
