// SPDX-License-Identifier: BSD-3-Clause

#include <v90_datapump.h>
#include <v90_profile.h>

#include <g711.h>
#include <modem.h>
#include <modem_param.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	CALL_OCTET_LIMIT = 120000,
	MINIMUM_RECEIVED_BITS = 128,
};

struct bit_endpoint {
	uint64_t supplied_bits;
	uint64_t received_bits;
	uint8_t transmit_seed;
	uint8_t receive_seed;
	bool enabled;
	bool receive_started;
	bool mismatch;
};

static struct dp_operations *registered_v90;

int modem_dp_register(enum DP_ID id, struct dp_operations *operations)
{
	if (id != DP_V90 || operations == NULL || registered_v90 != NULL)
		return -1;
	registered_v90 = operations;
	return 0;
}

int modem_dp_deregister(enum DP_ID id, struct dp_operations *operations)
{
	if (id != DP_V90 || operations == NULL ||
	    registered_v90 != operations || operations->use_count != 0)
		return -1;
	registered_v90 = NULL;
	return 0;
}

struct dp_operations *modem_dp_get_registered_ops(enum DP_ID id)
{
	return id == DP_V90 ? registered_v90 : NULL;
}

int modem_get_sreg(struct modem *modem, unsigned int number)
{
	if (modem == NULL || number >= sizeof(modem->sregs))
		return -1;
	return modem->sregs[number];
}

long modem_set_param(struct modem *modem, unsigned int name, int value)
{
	if (modem == NULL || value < 0)
		return -1;
	if (name == MDMPRM_TX_RATE)
		modem->tx_rate = (unsigned int)value;
	else if (name == MDMPRM_RX_RATE)
		modem->rx_rate = (unsigned int)value;
	else
		return -1;
	return 0;
}

int modem_get_bits(
	struct modem *modem, int bits_per_value, uint8_t *bits, int count)
{
	if (modem == NULL || bits == NULL || count < 0 ||
	    bits_per_value != 1)
		return -1;
	if (modem->get_bits == NULL) {
		memset(bits, 1, (size_t)count);
		return count;
	}
	return modem->get_bits(modem, bits_per_value, bits, count);
}

int modem_put_bits(
	struct modem *modem, int bits_per_value, uint8_t *bits, int count)
{
	if (modem == NULL || bits == NULL || count < 0 ||
	    bits_per_value != 1)
		return -1;
	return modem->put_bits == NULL ? count :
		       modem->put_bits(modem, bits_per_value, bits, count);
}

static uint8_t pattern_bit(uint8_t seed, uint64_t index)
{
	uint64_t mixed = index * UINT64_C(0x9e3779b97f4a7c15) + seed;

	if (index == 0)
		return 0;
	mixed ^= mixed >> 29;
	mixed ^= mixed >> 17;
	return (uint8_t)(mixed & 1u);
}

static int source_bits(
	struct modem *modem, int bits_per_value, uint8_t *bits, int count)
{
	struct bit_endpoint *endpoint = modem->dev_data;

	if (endpoint == NULL || bits_per_value != 1 || bits == NULL || count < 0)
		return -1;
	if (!endpoint->enabled)
		return 0;
	for (int index = 0; index < count; ++index) {
		bits[index] = pattern_bit(
			endpoint->transmit_seed, endpoint->supplied_bits);
		endpoint->supplied_bits++;
	}
	return count;
}

static int sink_bits(
	struct modem *modem, int bits_per_value, uint8_t *bits, int count)
{
	struct bit_endpoint *endpoint = modem->dev_data;

	if (endpoint == NULL || bits_per_value != 1 || bits == NULL || count < 0)
		return -1;
	if (!endpoint->enabled)
		return count;
	for (int index = 0; index < count; ++index) {
		if (!endpoint->receive_started) {
			if (bits[index] != 0)
				continue;
			endpoint->receive_started = true;
		}
		uint8_t expected = pattern_bit(
			endpoint->receive_seed, endpoint->received_bits);

		if (bits[index] != expected)
			endpoint->mismatch = true;
		endpoint->received_bits++;
	}
	return count;
}

static struct modem endpoint_modem(
	enum modem_pcm_role role, struct bit_endpoint *bits)
{
	struct modem modem = {
		.dev_data = bits,
		.min_rate = MODEM_MIN_RATE,
		.max_rate = MODEM_MAX_RATE,
		.get_bits = source_bits,
		.put_bits = sink_bits,
	};

	modem.dsp_info.pcm_role = role;
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 60;
	return modem;
}

static int16_t decode(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static uint8_t encode(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
}

static uint8_t transcode(
	enum g711_law source, enum g711_law destination, uint8_t octet)
{
	uint8_t best;
	int32_t best_error = INT32_MAX;
	int16_t target;

	if (source == destination)
		return octet;
	if (source == G711_LAW_PCMU)
		return encode(destination, decode(source, octet));
	if (octet == g711_encode_pcma(0))
		return g711_encode_pcmu(0);
	if (octet == g711_encode_pcma(-1))
		return g711_encode_pcmu(-1);
	target = g711_decode_pcma(octet);
	best = g711_encode_pcmu(target);
	/* Independently invert the finite tandem code space.  This models a
	 * codec bridge without assuming the implementation's neighboring-code
	 * shortcut. */
	for (unsigned int candidate = 0; candidate <= UINT8_MAX; ++candidate) {
		int16_t sample = g711_decode_pcmu((uint8_t)candidate);
		int32_t error;

		if (g711_encode_pcma(sample) != octet)
			continue;
		error = (int32_t)sample - target;
		if (error < 0)
			error = -error;
		if (error < best_error) {
			best = (uint8_t)candidate;
			best_error = error;
		}
	}
	return best;
}

static bool run_pair(
	enum g711_law analog_law, enum g711_law digital_law,
	uint32_t downstream_rate,
	uint32_t upstream_rate, uint32_t upstream_symbol_rate,
	bool require_online)
{
	struct bit_endpoint analog_bits = {
		.transmit_seed = 0x35,
		.receive_seed = 0xa7,
	};
	struct bit_endpoint digital_bits = {
		.transmit_seed = 0xa7,
		.receive_seed = 0x35,
	};
	struct modem analog = endpoint_modem(
		MODEM_PCM_ROLE_ANALOG, &analog_bits);
	struct modem digital = endpoint_modem(
		MODEM_PCM_ROLE_DIGITAL, &digital_bits);
	struct dp_operations *operations =
		modem_dp_get_registered_ops(DP_V90);
	struct dp *analog_dp;
	struct dp *digital_dp;
	uint8_t analog_to_digital = digital_law == G711_LAW_PCMA ?
					     g711_encode_pcma(0) :
					     g711_encode_pcmu(0);
	bool data_enabled = false;

	analog.min_rate = downstream_rate;
	analog.max_rate = downstream_rate;
	digital.min_rate = downstream_rate;
	digital.max_rate = downstream_rate;
	analog.v90_upstream_min_rate = upstream_rate;
	analog.v90_upstream_max_rate = upstream_rate;
	analog.v90_upstream_max_baud = upstream_symbol_rate;
	digital.v90_upstream_min_rate = upstream_rate;
	digital.v90_upstream_max_rate = upstream_rate;
	digital.v90_upstream_max_baud = upstream_symbol_rate;

	CHECK(operations != NULL && operations->process_g711 != NULL,
	      "V.90 exact-codeword operation was not registered");
	analog_dp = operations->create(
		&analog, DP_V90, 1, MODEM_RATE, MODEM_FRAG, operations);
	digital_dp = operations->create(
		&digital, DP_V90, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && digital_dp != NULL &&
	      operations->use_count == 2 && v90_datapump_is_busy(),
	      "V.90 peers did not own independent data pumps");
	for (size_t octet = 0;
	     octet < (require_online ? CALL_OCTET_LIMIT : 30000u); ++octet) {
		uint8_t next_digital;
		uint8_t next_analog;
		int16_t digital_receive = decode(digital_law, analog_to_digital);
		int16_t analog_receive;
		int16_t digital_transmit = 1;
		int16_t analog_transmit = 1;
		bool digital_exact = false;
		bool analog_exact = false;
		int digital_result = operations->process_g711(
			digital_dp, digital_law,
			&analog_to_digital, &digital_receive,
			&next_digital, &digital_transmit, 1, &digital_exact);

		CHECK((digital_result == DPSTAT_OK ||
		       digital_result == DPSTAT_CONNECT) && digital_exact &&
		      digital_transmit == decode(digital_law, next_digital),
		      "digital V.90 path failed at octet %zu", octet);
		next_digital = transcode(
			digital_law, analog_law, next_digital);
		analog_receive = decode(analog_law, next_digital);
		int analog_result = operations->process_g711(
			analog_dp, analog_law, &next_digital, &analog_receive,
			&next_analog, &analog_transmit, 1, &analog_exact);

		CHECK((analog_result == DPSTAT_OK ||
		       analog_result == DPSTAT_CONNECT) && analog_exact &&
		      analog_transmit == decode(analog_law, next_analog),
		      "analogue V.90 path failed at octet %zu", octet);
		analog_to_digital = transcode(
			analog_law, digital_law, next_analog);
		if (!data_enabled && analog_dp->status == DPSTAT_CONNECT &&
		    digital_dp->status == DPSTAT_CONNECT) {
			analog_bits.enabled = true;
			digital_bits.enabled = true;
			data_enabled = true;
		}
		if (data_enabled &&
		    analog_bits.received_bits >= MINIMUM_RECEIVED_BITS &&
		    digital_bits.received_bits >= MINIMUM_RECEIVED_BITS)
			break;
	}
	if (!require_online) {
		CHECK((analog_dp->status == DPSTAT_OK ||
		       analog_dp->status == DPSTAT_CONNECT) &&
		      (digital_dp->status == DPSTAT_OK ||
		       digital_dp->status == DPSTAT_CONNECT) &&
		      operations->delete(analog_dp) == 0 &&
		      operations->delete(digital_dp) == 0 &&
		      operations->use_count == 0,
		      "V.90 media-law boundary did not remain live through Phase 2");
		return true;
	}
	CHECK(data_enabled && analog_dp->status == DPSTAT_CONNECT &&
	      digital_dp->status == DPSTAT_CONNECT &&
	      analog.rx_rate == downstream_rate &&
	      digital.tx_rate == downstream_rate &&
	      analog.tx_rate != 0 && analog.tx_rate == digital.rx_rate &&
	      (upstream_rate == 0 || analog.tx_rate == upstream_rate),
	      "V.90 pair did not report complementary online rates "
	      "(expected down/up %u/%u, analogue %u/%u, digital %u/%u)",
	      downstream_rate, upstream_rate,
	      analog.rx_rate, analog.tx_rate,
	      digital.tx_rate, digital.rx_rate);
	CHECK(analog_bits.supplied_bits >= MINIMUM_RECEIVED_BITS &&
	      digital_bits.supplied_bits >= MINIMUM_RECEIVED_BITS &&
	      analog_bits.received_bits >= MINIMUM_RECEIVED_BITS &&
	      digital_bits.received_bits >= MINIMUM_RECEIVED_BITS &&
	      !analog_bits.mismatch && !digital_bits.mismatch,
	      "V.90 host bits did not pass exactly in both directions "
	      "(supplied %llu/%llu received %llu/%llu mismatch %d/%d)",
	      (unsigned long long)analog_bits.supplied_bits,
	      (unsigned long long)digital_bits.supplied_bits,
	      (unsigned long long)analog_bits.received_bits,
	      (unsigned long long)digital_bits.received_bits,
	      analog_bits.mismatch, digital_bits.mismatch);
	CHECK(operations->delete(analog_dp) == 0 &&
	      operations->delete(digital_dp) == 0 &&
	      operations->use_count == 0 && !v90_datapump_is_busy(),
	      "V.90 data pumps did not release their call state");
	return true;
}

static bool test_generated_pairs(void)
{
	CHECK(v90_datapump_register() == 0 &&
	      v90_datapump_register() != 0,
	      "V.90 registration did not reject a duplicate owner");
	for (uint8_t rate_index = PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER;
	     rate_index <= PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER; ++rate_index) {
		uint32_t bit_rate =
			pcm_downstream_bit_rate(rate_index);

		CHECK(run_pair(
			      G711_LAW_PCMU, G711_LAW_PCMU,
			      bit_rate, 0, 0, true) &&
		      run_pair(
			      G711_LAW_PCMA, G711_LAW_PCMA,
			      bit_rate, 0, 0, true),
		      "generated V.90 pair failed at %u bit/s", bit_rate);
	}
	CHECK(v90_datapump_unregister() == 0 &&
	      modem_dp_get_registered_ops(DP_V90) == NULL,
	      "V.90 registration did not stop cleanly");
	return true;
}

static bool test_forced_upstream_pairs(void)
{
	CHECK(v90_datapump_register() == 0,
	      "forced-upstream V.90 registration failed");
	CHECK(run_pair(
		      G711_LAW_PCMU, G711_LAW_PCMU,
		      42667, 26400, 3200, true) &&
		      run_pair(
			      G711_LAW_PCMA, G711_LAW_PCMA,
			      42667, 26400, 3200, true),
	      "V.90 data pump did not honor a forced upstream profile");
	CHECK(v90_datapump_unregister() == 0,
	      "forced-upstream V.90 registration remained busy");
	return true;
}

static bool test_independent_media_laws(void)
{
	CHECK(v90_datapump_register() == 0,
	      "mixed-media V.90 registration failed");
	CHECK(run_pair(
		      G711_LAW_PCMU, G711_LAW_PCMA,
		      28000, 14400, 3000, false) &&
	      run_pair(
		      G711_LAW_PCMA, G711_LAW_PCMU,
		      28000, 14400, 3000, false),
	      "V.90 PCM codebook changed with endpoint media companding");
	CHECK(v90_datapump_unregister() == 0,
	      "mixed-media V.90 registration remained busy");
	return true;
}

static bool test_training_deadline(void)
{
	struct bit_endpoint bits = { 0 };
	struct modem modem = endpoint_modem(MODEM_PCM_ROLE_DIGITAL, &bits);
	struct dp_operations *operations;
	struct dp *datapump;
	uint8_t receive_octets[MODEM_FRAG];
	uint8_t transmit_octets[MODEM_FRAG];
	int16_t receive_samples[MODEM_FRAG];
	int16_t transmit_samples[MODEM_FRAG];
	uint8_t silence = g711_encode_pcmu(0);

	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 1;
	memset(receive_octets, silence, sizeof(receive_octets));
	for (size_t index = 0; index < ARRAY_LENGTH(receive_samples); ++index)
		receive_samples[index] = g711_decode_pcmu(silence);
	CHECK(v90_datapump_register() == 0,
	      "V.90 deadline registration failed");
	operations = modem_dp_get_registered_ops(DP_V90);
	datapump = operations != NULL ? operations->create(
		&modem, DP_V90, 0, MODEM_RATE, MODEM_FRAG, operations) : NULL;
	CHECK(datapump != NULL,
	      "V.90 deadline data pump did not initialize");
	for (unsigned int frame = 0;
	     frame < MODEM_RATE / MODEM_FRAG; ++frame) {
		bool exact = false;

		CHECK(operations->process_g711(
			      datapump, G711_LAW_PCMU,
			      receive_octets, receive_samples,
			      transmit_octets, transmit_samples,
			      MODEM_FRAG, &exact) == DPSTAT_OK && exact,
		      "V.90 training expired before its inclusive deadline");
	}
	{
		bool exact = true;

		CHECK(operations->process_g711(
			      datapump, G711_LAW_PCMU,
			      receive_octets, receive_samples,
			      transmit_octets, transmit_samples,
			      1, &exact) == DPSTAT_ERROR && !exact &&
		      transmit_octets[0] == silence && transmit_samples[0] == 0,
		      "V.90 training did not fail closed after its deadline");
		exact = true;
		CHECK(operations->process_g711(
			      datapump, G711_LAW_PCMU,
			      receive_octets, receive_samples,
			      transmit_octets, transmit_samples,
			      1, &exact) == DPSTAT_ERROR && !exact,
		      "V.90 training failure was not latched");
	}
	CHECK(operations->delete(datapump) == 0 &&
	      v90_datapump_unregister() == 0,
	      "failed V.90 training retained ownership");
	return true;
}

static bool test_validation(void)
{
	struct bit_endpoint bits = { 0 };
	struct modem modem = endpoint_modem(MODEM_PCM_ROLE_ANALOG, &bits);
	struct dp_operations *operations;
	struct dp *datapump;
	uint8_t receive_octet = g711_encode_pcmu(0);
	uint8_t transmit_octet = 0xa5;
	int16_t receive_sample = g711_decode_pcmu(receive_octet);
	int16_t transmit_sample = INT16_MAX;
	bool exact = true;

	CHECK(v90_datapump_register() == 0,
	      "V.90 validation registration failed");
	operations = modem_dp_get_registered_ops(DP_V90);
	CHECK(operations != NULL && operations->create != NULL &&
	      operations->delete != NULL && operations->process != NULL &&
	      operations->process_g711 != NULL && operations->hangup == NULL,
	      "V.90 operation surface is incomplete");
	modem.dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
	CHECK(operations->create(
		      &modem, DP_V90, 1, MODEM_RATE, MODEM_FRAG,
		      operations) == NULL,
	      "V.90 accepted a missing PCM role");
	modem.dsp_info.pcm_role = MODEM_PCM_ROLE_ANALOG;
	modem.min_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE + 1u;
	modem.max_rate = 29332;
	CHECK(operations->create(
		      &modem, DP_V90, 1, MODEM_RATE, MODEM_FRAG,
		      operations) == NULL,
	      "V.90 accepted a range containing no downstream rate");
	modem.min_rate = MODEM_MIN_RATE;
	modem.max_rate = MODEM_MAX_RATE;
	datapump = operations->create(
		&modem, DP_V90, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(datapump != NULL && operations->use_count == 1 &&
	      operations->process(
		      datapump, &receive_sample, &transmit_sample, 1) ==
		      DPSTAT_ERROR && transmit_sample == 0,
	      "V.90 accepted lossy linear-PCM processing");
	CHECK(operations->process_g711(
		      datapump, (enum g711_law)-1,
		      &receive_octet, &receive_sample,
		      &transmit_octet, &transmit_sample, 1, &exact) ==
		      DPSTAT_ERROR && !exact && transmit_sample == 0 &&
	      operations->delete(datapump) == 0,
	      "V.90 invalid media input changed ownership or exactness");
	CHECK(v90_datapump_unregister() == 0,
	      "V.90 validation registration remained busy");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "forced upstream pairs", test_forced_upstream_pairs },
		{ "media-independent Phase 2 codebook",
		  test_independent_media_laws },
		{ "generated PCMU and PCMA data-pump pairs",
		  test_generated_pairs },
		{ "training deadline", test_training_deadline },
		{ "registration and media validation", test_validation },
	};
	int failed = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failed++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failed == 0 ? 0 : 1;
}
