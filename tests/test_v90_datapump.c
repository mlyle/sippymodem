// SPDX-License-Identifier: BSD-3-Clause

#include <v90_datapump.h>
#include <v90_profile.h>
#include <v34_datapump.h>
#include <v34_call.h>

#include <dc_remover.h>
#include <g711.h>
#include <modem.h>
#include <modem_param.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	CALL_OCTET_LIMIT = 120000,
	MINIMUM_RECEIVED_BITS = 128,
	SUSTAINED_RECEIVED_BITS = 4096,
	MAXIMUM_RATE_RECEIVED_BITS = 32768,
	UNITY_GAIN_Q15 = 32768,
	/* Keep generated QAM between definite quiet and the detector's broader
	 * retrain-silence ceiling. */
	WEAK_UPSTREAM_GAIN_Q15 = 2400,
	RETURNED_ECHO_DELAY = 5600,
	RETURNED_ECHO_TAP_1 = RETURNED_ECHO_DELAY + 7,
	RETURNED_ECHO_TAP_2 = RETURNED_ECHO_DELAY + 23,
	RETURNED_ECHO_HISTORY = RETURNED_ECHO_TAP_2 + 1,
	/* At 3429 symbols/s, 49 samples contain exactly 21 symbols. With the
	 * fixed 16-symbol decision delay, this echo begins just beyond the
	 * conventional 37-tap span. */
	UPSTREAM_ECHO_DELAY = 49,
	UPSTREAM_ECHO_HISTORY = UPSTREAM_ECHO_DELAY,
	UPSTREAM_ECHO_GAIN_Q15 = 8000,
	UPSTREAM_NOISE_SAMPLES = 16000,
};

enum returned_echo_mode {
	RETURNED_ECHO_NONE,
	RETURNED_ECHO_STABLE,
	RETURNED_ECHO_DC_BIASED,
};

enum upstream_channel_mode {
	UPSTREAM_CHANNEL_CLEAN,
	UPSTREAM_CHANNEL_DELAYED_ECHO,
	UPSTREAM_CHANNEL_NOISE_BURST,
};

struct bit_endpoint {
	uint64_t supplied_bits;
	uint64_t received_bits;
	uint8_t transmit_seed;
	uint8_t receive_seed;
	bool enabled;
	bool receive_started;
	bool mismatch;
	uint64_t mismatched_bits;
	uint64_t first_mismatch_bit_plus_one;
};

static struct dp_operations *registered_v90;
static struct dp_operations *registered_v92;
static struct dp_operations *registered_v34;

/* This direct datapump bench bypasses V.8 entirely. */
bool v8_datapump_selected_v34_fallback(struct dp *dp)
{
	(void)dp;
	return false;
}

int modem_dp_register(enum DP_ID id, struct dp_operations *operations)
{
	struct dp_operations **registered;

	if (id == DP_V90)
		registered = &registered_v90;
	else if (id == DP_V92)
		registered = &registered_v92;
	else if (id == DP_V34)
		registered = &registered_v34;
	else
		return -1;
	if (operations == NULL || *registered != NULL)
		return -1;
	*registered = operations;
	return 0;
}

int modem_dp_deregister(enum DP_ID id, struct dp_operations *operations)
{
	struct dp_operations **registered;

	if (id == DP_V90)
		registered = &registered_v90;
	else if (id == DP_V92)
		registered = &registered_v92;
	else if (id == DP_V34)
		registered = &registered_v34;
	else
		return -1;
	if (operations == NULL || *registered != operations ||
	    operations->use_count != 0)
		return -1;
	*registered = NULL;
	return 0;
}

struct dp_operations *modem_dp_get_registered_ops(enum DP_ID id)
{
	if (id == DP_V90)
		return registered_v90;
	if (id == DP_V92)
		return registered_v92;
	if (id == DP_V34)
		return registered_v34;
	return NULL;
}

int modem_get_sreg(struct modem *modem, unsigned int number)
{
	if (modem == NULL || number >= sizeof(modem->sregs))
		return -1;
	return modem->sregs[number];
}

long modem_get_param(struct modem *modem, unsigned int name)
{
	if (modem == NULL)
		return -1;
	if (name == MDMPRM_V92_QUICK_CONNECT)
		return modem->sregs[SREG_V92_QUICK_CONNECT] != 0;
	if (name == MDMPRM_V92_PCM_UPSTREAM)
		return modem->sregs[SREG_V92_PCM_UPSTREAM] != 0;
	return -1;
}

long modem_set_param(struct modem *modem, unsigned int name, int value)
{
	if (modem == NULL || value < 0)
		return -1;
	if (name == MDMPRM_TX_RATE)
		modem->tx_rate = (unsigned int)value;
	else if (name == MDMPRM_RX_RATE)
		modem->rx_rate = (unsigned int)value;
	else if (name == MDMPRM_DP_REQUESTED)
		modem->dp_requested = value;
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

		if (bits[index] != expected) {
			endpoint->mismatch = true;
			endpoint->mismatched_bits++;
			if (endpoint->first_mismatch_bit_plus_one == 0)
				endpoint->first_mismatch_bit_plus_one =
					endpoint->received_bits + 1u;
		}
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

static int16_t add_s16(int32_t sample)
{
	return sample > INT16_MAX ? INT16_MAX :
	       sample < INT16_MIN ? INT16_MIN : (int16_t)sample;
}

static int16_t scale_q15(int16_t sample, int16_t gain)
{
	return (int16_t)((int32_t)sample * gain / 32768);
}

static int16_t scale_uq15(int16_t sample, uint16_t gain)
{
	return (int16_t)((int32_t)sample * gain / 32768);
}

static int16_t history_sample(
	const int16_t *history, size_t capacity, size_t write,
	size_t fill, size_t age)
{
	size_t index;

	if (history == NULL || capacity == 0 || age == 0 ||
	    age > fill || age > capacity)
		return 0;
	index = write + capacity - age;
	if (index >= capacity)
		index -= capacity;
	return history[index];
}

static int16_t returned_echo(
	const int16_t history[RETURNED_ECHO_HISTORY], size_t write,
	size_t fill, enum returned_echo_mode mode)
{
	int32_t sample = scale_q15(
		history_sample(
			history, RETURNED_ECHO_HISTORY, write, fill,
			RETURNED_ECHO_DELAY),
		11000);

	sample += scale_q15(
		history_sample(
			history, RETURNED_ECHO_HISTORY, write, fill,
			RETURNED_ECHO_TAP_1),
		4500);
	sample -= scale_q15(
		history_sample(
			history, RETURNED_ECHO_HISTORY, write, fill,
			RETURNED_ECHO_TAP_2),
		2200);
	if (mode == RETURNED_ECHO_DC_BIASED)
		sample += 1400;
	return add_s16(sample);
}

static bool check_cleardown(struct dp *dp, enum g711_law law)
{
	uint8_t input[MODEM_FRAG];
	int16_t samples[MODEM_FRAG] = { 0 };
	bool complete = false;
	unsigned int elapsed = 0u;

	memset(input, encode(law, 0), sizeof(input));
	CHECK(dp->op->hangup != NULL && dp->op->hangup(dp) == 0 &&
	      dp->op->hangup(dp) == 0,
	      "V.90 hangup did not start idempotently");
	for (; elapsed < 2u * MODEM_RATE; elapsed += MODEM_FRAG) {
		uint8_t output[MODEM_FRAG];
		int16_t generated[MODEM_FRAG];
		bool exact = false;
		int result = dp->op->process_g711(
			dp, law, input, samples, output, generated, MODEM_FRAG,
			&exact);

		CHECK((result == DPSTAT_OK || result == DPSTAT_DISCONNECT) &&
		      exact,
		      "V.90 cleardown failed after %u samples, status %d",
		      elapsed, result);
		for (unsigned int index = 0u; index < MODEM_FRAG; ++index)
			CHECK(generated[index] == decode(law, output[index]) ||
			      (result == DPSTAT_DISCONNECT &&
			       generated[index] == 0 &&
			       output[index] == encode(law, 0)),
			      "V.90 cleardown lost exact network character");
		if (result == DPSTAT_DISCONNECT) {
			complete = true;
			break;
		}
	}
	CHECK(complete && elapsed > MODEM_RATE / 20u,
	      "V.90 cleardown did not emit a bounded protocol sequence");
	return true;
}

static bool run_pair_with_rate_window(
	enum g711_law law,
	uint32_t downstream_rate,
	uint32_t upstream_min_rate, uint32_t upstream_max_rate,
	uint32_t expected_upstream_rate, uint32_t upstream_symbol_rate,
	uint64_t minimum_received_bits,
	bool require_online, enum modem_pcm_role retrain_role,
	bool separate_upstream_sample, uint16_t digital_receive_gain_q15,
	enum returned_echo_mode echo_mode,
	enum upstream_channel_mode upstream_channel)
{
	int16_t echo_history[RETURNED_ECHO_HISTORY] = { 0 };
	int16_t upstream_history[UPSTREAM_ECHO_HISTORY] = { 0 };
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
	struct dc_remover digital_frontend;
	struct dp_operations *operations =
		modem_dp_get_registered_ops(DP_V90);
	struct dp *analog_dp;
	struct dp *digital_dp;
	uint8_t analog_to_digital = law == G711_LAW_PCMA ?
					     g711_encode_pcma(0) :
					     g711_encode_pcmu(0);
	uint8_t digital_to_analog = law == G711_LAW_PCMA ?
					     g711_encode_pcma(0) :
					     g711_encode_pcmu(0);
	bool data_enabled = false;
	bool retrain_requested = false;
	bool analog_reconnected = false;
	bool digital_reconnected = false;
	bool post_retrain_data_enabled = false;
	bool automatic_retrain = upstream_channel == UPSTREAM_CHANNEL_NOISE_BURST;
	bool exercise_retrain = retrain_role != MODEM_PCM_ROLE_NONE || automatic_retrain;
	uint32_t noise_state = UINT32_C(0x197bc485);
	unsigned int noise_samples = 0;
	bool sample_codeword_separated = false;
	size_t echo_history_write = 0;
	size_t echo_history_fill = 0;
	size_t upstream_history_write = 0;
	size_t upstream_history_fill = 0;

	analog.min_rate = downstream_rate;
	analog.max_rate = downstream_rate;
	digital.min_rate = downstream_rate;
	digital.max_rate = downstream_rate;
	analog.v90_upstream_min_rate = upstream_min_rate;
	analog.v90_upstream_max_rate = upstream_max_rate;
	analog.v90_upstream_max_baud = upstream_symbol_rate;
	digital.v90_upstream_min_rate = upstream_min_rate;
	digital.v90_upstream_max_rate = upstream_max_rate;
	digital.v90_upstream_max_baud = upstream_symbol_rate;

	CHECK(operations != NULL && operations->process_g711 != NULL,
	      "V.90 exact-codeword operation was not registered");
	/* This direct pair bypasses the modem-owned conventional receive path. */
	CHECK(dc_remover_init(&digital_frontend) == DC_REMOVER_OK,
	      "V.90 digital receive front end did not initialize");
	analog_dp = operations->create(
		&analog, DP_V90, 1, MODEM_RATE, MODEM_FRAG, operations);
	digital_dp = operations->create(
		&digital, DP_V90, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && digital_dp != NULL &&
	      operations->use_count == 2 && v90_datapump_is_busy(),
	      "V.90 peers did not own independent data pumps");
	for (size_t octet = 0;
	     octet < (require_online ?
			     CALL_OCTET_LIMIT * (exercise_retrain ? 2u : 1u) :
			     30000u);
	     ++octet) {
		uint8_t next_digital;
		uint8_t next_analog;
		uint8_t digital_received_octet = analog_to_digital;
		int16_t digital_receive =
			decode(law, digital_received_octet);
		int16_t analog_receive;
		int16_t digital_transmit = 1;
		int16_t analog_transmit = 1;
		bool digital_exact = false;
		bool analog_exact = false;
		int analog_result;
		int digital_result;

		if (separate_upstream_sample && data_enabled) {
			digital_received_octet = encode(law, 0);
			sample_codeword_separated |=
				digital_received_octet != analog_to_digital;
		}
		if (upstream_channel == UPSTREAM_CHANNEL_DELAYED_ECHO) {
			int16_t source = digital_receive;

			digital_receive = add_s16(
				(int32_t)source +
				scale_q15(
					history_sample(
						upstream_history,
						UPSTREAM_ECHO_HISTORY,
						upstream_history_write,
						upstream_history_fill,
						UPSTREAM_ECHO_DELAY),
					UPSTREAM_ECHO_GAIN_Q15));
			upstream_history[upstream_history_write] = source;
			upstream_history_write++;
			if (upstream_history_write == UPSTREAM_ECHO_HISTORY)
				upstream_history_write = 0;
			if (upstream_history_fill < UPSTREAM_ECHO_HISTORY)
				upstream_history_fill++;
		}
		digital_receive = scale_uq15(
			digital_receive, digital_receive_gain_q15);
		if (automatic_retrain && retrain_requested &&
		    noise_samples < UPSTREAM_NOISE_SAMPLES) {
			/* A bounded physical disturbance after verified initial data.
			 * Preserve network sample time and require automatic recovery;
			 * neither peer's retrain operation is called by this case. */
			noise_state ^= noise_state << 13;
			noise_state ^= noise_state >> 17;
			noise_state ^= noise_state << 5;
			digital_received_octet = encode(law, add_s16(
				(int32_t)digital_receive +
				(int32_t)(noise_state % 2001u) - 1000));
			digital_receive = decode(law, digital_received_octet);
			noise_samples++;
		}
		if (echo_mode != RETURNED_ECHO_NONE)
			digital_receive = add_s16(
				(int32_t)digital_receive +
				returned_echo(
					echo_history, echo_history_write,
					echo_history_fill, echo_mode));
		CHECK(dc_remover_process(
			      &digital_frontend, &digital_receive, 1) ==
			      DC_REMOVER_OK,
		      "V.90 digital receive front end failed at octet %zu",
		      octet);
		digital_result = operations->process_g711(
			digital_dp, law,
			&digital_received_octet, &digital_receive,
			&next_digital, &digital_transmit, 1, &digital_exact);

		CHECK((digital_result == DPSTAT_OK ||
		       digital_result == DPSTAT_CONNECT) && digital_exact &&
		      digital_transmit == decode(law, next_digital),
		      "digital V.90 path failed at octet %zu (status %d, exact %u)",
		      octet, digital_result, (unsigned int)digital_exact);
		if (echo_mode != RETURNED_ECHO_NONE) {
			echo_history[echo_history_write] = digital_transmit;
			echo_history_write++;
			if (echo_history_write == RETURNED_ECHO_HISTORY)
				echo_history_write = 0;
			if (echo_history_fill < RETURNED_ECHO_HISTORY)
				echo_history_fill++;
		}
		{
			uint8_t delivered = digital_to_analog;

			digital_to_analog = next_digital;
			analog_receive = decode(law, delivered);
			analog_result = operations->process_g711(
				analog_dp, law, &delivered,
				&analog_receive, &next_analog,
				&analog_transmit, 1, &analog_exact);
		}
		CHECK((analog_result == DPSTAT_OK ||
		       analog_result == DPSTAT_CONNECT) && analog_exact &&
		      analog_transmit == decode(law, next_analog),
		      "analogue V.90 path failed at octet %zu", octet);
		analog_to_digital = next_analog;
		if (retrain_requested) {
			analog_reconnected |= analog_result == DPSTAT_CONNECT;
			digital_reconnected |= digital_result == DPSTAT_CONNECT;
			if (analog_reconnected && digital_reconnected &&
			    !post_retrain_data_enabled) {
				analog_bits = (struct bit_endpoint){
					.transmit_seed = 0x6b,
					.receive_seed = 0xd3,
					.enabled = true,
				};
				digital_bits = (struct bit_endpoint){
					.transmit_seed = 0xd3,
					.receive_seed = 0x6b,
					.enabled = true,
				};
				post_retrain_data_enabled = true;
			}
		}
		if (!data_enabled && analog_dp->status == DPSTAT_CONNECT &&
		    digital_dp->status == DPSTAT_CONNECT) {
			analog_bits.enabled = true;
			digital_bits.enabled = true;
			data_enabled = true;
		}
		if (!data_enabled ||
		    analog_bits.received_bits < minimum_received_bits ||
		    digital_bits.received_bits < minimum_received_bits)
			continue;
		if (!exercise_retrain)
			break;
		if (!retrain_requested) {
			struct dp *selected =
				retrain_role == MODEM_PCM_ROLE_ANALOG ?
					analog_dp : digital_dp;

			CHECK(!analog_bits.mismatch && !digital_bits.mismatch &&
			      (automatic_retrain ||
			       (operations->retrain != NULL &&
			        operations->retrain(selected) == 0)),
			      "V.90 role %u local retrain request failed",
			      (unsigned int)retrain_role);
			analog_bits.enabled = false;
			digital_bits.enabled = false;
			retrain_requested = true;
			continue;
		}
		if (post_retrain_data_enabled &&
		    analog_bits.received_bits >= minimum_received_bits &&
		    digital_bits.received_bits >= minimum_received_bits)
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
	      (expected_upstream_rate == 0 ||
	       analog.tx_rate == expected_upstream_rate),
	      "V.90 pair did not report complementary online rates "
	      "(expected down/up %u/%u, analogue %u/%u, digital %u/%u)",
	      downstream_rate, expected_upstream_rate,
	      analog.rx_rate, analog.tx_rate,
	      digital.tx_rate, digital.rx_rate);
	CHECK(analog_bits.supplied_bits >= minimum_received_bits &&
	      digital_bits.supplied_bits >= minimum_received_bits &&
	      analog_bits.received_bits >= minimum_received_bits &&
	      digital_bits.received_bits >= minimum_received_bits &&
	      !analog_bits.mismatch && !digital_bits.mismatch,
	      "V.90 host bits did not pass exactly in both directions "
	      "(supplied %llu/%llu received %llu/%llu mismatch %llu/%llu "
	      "first %llu/%llu)",
	      (unsigned long long)analog_bits.supplied_bits,
	      (unsigned long long)digital_bits.supplied_bits,
	      (unsigned long long)analog_bits.received_bits,
	      (unsigned long long)digital_bits.received_bits,
	      (unsigned long long)analog_bits.mismatched_bits,
	      (unsigned long long)digital_bits.mismatched_bits,
	      (unsigned long long)analog_bits.first_mismatch_bit_plus_one,
	      (unsigned long long)digital_bits.first_mismatch_bit_plus_one);
	CHECK(!exercise_retrain ||
	      (retrain_requested && post_retrain_data_enabled &&
	       analog_bits.received_bits >= minimum_received_bits &&
	       digital_bits.received_bits >= minimum_received_bits),
	      "V.90 role %u did not complete local and remote retrain",
	      (unsigned int)retrain_role);
	CHECK(!automatic_retrain || noise_samples == UPSTREAM_NOISE_SAMPLES,
	      "quality recovery did not consume the complete noise burst");
	CHECK(!separate_upstream_sample || sample_codeword_separated,
	      "PCMA upstream sample never diverged from its test codeword");
	{
		uint64_t analog_supplied = analog_bits.supplied_bits;
		uint64_t digital_supplied = digital_bits.supplied_bits;

		CHECK(check_cleardown(analog_dp, law) && check_cleardown(digital_dp, law) &&
		      analog_supplied == analog_bits.supplied_bits &&
		      digital_supplied == digital_bits.supplied_bits,
		      "V.90 cleardown consumed new host payload");
	}
	CHECK(operations->delete(analog_dp) == 0 &&
	      operations->delete(digital_dp) == 0 &&
	      operations->use_count == 0 && !v90_datapump_is_busy(),
	      "V.90 data pumps did not release their call state");
	return true;
}

static bool test_upstream_quality_recovery(void)
{
	static const struct {
		uint32_t downstream_rate;
		uint32_t upstream_rate;
		uint32_t symbol_rate;
	} profiles[] = {
		{ 28000, 19200, 3000 },
		{ 42667, 26400, 3200 },
		{ 56000, 31200, 3200 },
		{ 56000, 31200, 3429 },
	};

	CHECK(v90_datapump_register() == 0,
	      "quality-recovery V.90 registration failed");
	/* Quality thresholds depend on constellation density, not just baud. */
	for (size_t index = 0; index < ARRAY_LENGTH(profiles); ++index) {
		uint32_t upstream_rate = profiles[index].upstream_rate;

		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			CHECK(run_pair_with_rate_window(
				      law, profiles[index].downstream_rate,
				      upstream_rate, upstream_rate, upstream_rate,
				      profiles[index].symbol_rate,
				      SUSTAINED_RECEIVED_BITS, true, MODEM_PCM_ROLE_NONE,
				      false, UNITY_GAIN_Q15, RETURNED_ECHO_NONE,
				      UPSTREAM_CHANNEL_NOISE_BURST),
			      "law %u did not recover %u bit/s upstream at %u symbols/s after noise",
			      (unsigned int)law, upstream_rate, profiles[index].symbol_rate);
	}
	CHECK(v90_datapump_unregister() == 0,
	      "quality-recovery V.90 registration remained busy");
	return true;
}

static bool run_pair_with_input_contract(
	enum g711_law law,
	uint32_t downstream_rate,
	uint32_t upstream_rate, uint32_t upstream_symbol_rate,
	bool require_online, enum modem_pcm_role retrain_role,
	bool separate_upstream_sample,
	bool add_returned_echo)
{
	return run_pair_with_rate_window(
		law, downstream_rate,
		upstream_rate, upstream_rate, upstream_rate,
		upstream_symbol_rate, MINIMUM_RECEIVED_BITS,
		require_online, retrain_role,
		separate_upstream_sample, UNITY_GAIN_Q15,
		add_returned_echo ? RETURNED_ECHO_STABLE : RETURNED_ECHO_NONE,
		UPSTREAM_CHANNEL_CLEAN);
}

static bool run_pair_for_bits(
	enum g711_law law, uint32_t downstream_rate,
	uint32_t upstream_rate, uint32_t upstream_symbol_rate,
	uint64_t minimum_received_bits)
{
	return run_pair_with_rate_window(
		law, downstream_rate,
		upstream_rate, upstream_rate, upstream_rate,
		upstream_symbol_rate, minimum_received_bits,
		true, MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
		RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN);
}

static bool run_pair(
	enum g711_law law,
	uint32_t downstream_rate,
	uint32_t upstream_rate, uint32_t upstream_symbol_rate,
	bool require_online, enum modem_pcm_role retrain_role)
{
	return run_pair_with_input_contract(
		law, downstream_rate,
		upstream_rate, upstream_symbol_rate,
		require_online, retrain_role, false, false);
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
			      G711_LAW_PCMU,
			      bit_rate, 0, 0, true, MODEM_PCM_ROLE_NONE) &&
		      run_pair(
			      G711_LAW_PCMA,
			      bit_rate, 0, 0, true, MODEM_PCM_ROLE_NONE),
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
		      G711_LAW_PCMU,
		      28000, 19200, 3000, true, MODEM_PCM_ROLE_NONE) &&
		      run_pair(
			      G711_LAW_PCMA,
			      28000, 19200, 3000, true,
			      MODEM_PCM_ROLE_NONE) &&
	      run_pair(
		      G711_LAW_PCMU,
		      42667, 26400, 3200, true, MODEM_PCM_ROLE_NONE) &&
		      run_pair(
			      G711_LAW_PCMA,
			      42667, 26400, 3200, true,
			      MODEM_PCM_ROLE_NONE),
	      "V.90 data pump did not honor a forced upstream profile");
	CHECK(v90_datapump_unregister() == 0,
	      "forced-upstream V.90 registration remained busy");
	return true;
}

static bool test_sustained_maximum_rate_pairs(void)
{
	static const uint32_t SYMBOL_RATES[] = { 3200, 3429 };

	CHECK(v90_datapump_register() == 0,
	      "sustained maximum-rate V.90 registration failed");
	/* 31200 at 3200 symbols/s uses a denser upstream constellation than
	 * at 3429. A short payload can miss errors after an intact prefix. */
	for (size_t index = 0; index < ARRAY_LENGTH(SYMBOL_RATES); ++index) {
		for (enum g711_law law = G711_LAW_PCMU;
		     law <= G711_LAW_PCMA; ++law)
			CHECK(run_pair_for_bits(
				      law, 56000, 31200, SYMBOL_RATES[index],
				      MAXIMUM_RATE_RECEIVED_BITS),
			      "law %u maximum-rate V.90 payload failed at %u symbols/s",
			      (unsigned int)law, SYMBOL_RATES[index]);
	}
	CHECK(v90_datapump_unregister() == 0,
	      "sustained maximum-rate V.90 registration remained busy");
	return true;
}

static bool test_attenuated_upstream(void)
{
	static const uint16_t GAINS_Q15[] = {
		WEAK_UPSTREAM_GAIN_Q15,
		/* Decodable QAM below the fixed raw retrain-quiet ceiling. */
		1600u,
	};

	CHECK(v90_datapump_register() == 0,
	      "attenuated-upstream V.90 registration failed");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0; index < ARRAY_LENGTH(GAINS_Q15); ++index) {
			CHECK(run_pair_with_rate_window(
				      law, 56000, 31200, 31200, 31200, 3429,
				      SUSTAINED_RECEIVED_BITS, true,
				      MODEM_PCM_ROLE_NONE, false, GAINS_Q15[index],
				      RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u gain %u attenuated V.90 upstream lost exact data",
			      (unsigned int)law, GAINS_Q15[index]);
		}
		for (enum modem_pcm_role initiator = MODEM_PCM_ROLE_ANALOG;
		     initiator <= MODEM_PCM_ROLE_DIGITAL; ++initiator)
			CHECK(run_pair_with_rate_window(
				      law, 56000, 31200, 31200, 31200, 3429,
				      SUSTAINED_RECEIVED_BITS, true,
				      initiator, false, GAINS_Q15[1],
				      RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u weak upstream lost retraining",
			      (unsigned int)law, (unsigned int)initiator);
	}
	CHECK(v90_datapump_unregister() == 0,
	      "attenuated-upstream V.90 registration remained busy");
	return true;
}

static bool test_delayed_upstream_channel(void)
{
	CHECK(v90_datapump_register() == 0,
	      "delayed-channel V.90 registration failed");
	CHECK(run_pair_with_rate_window(
		      G711_LAW_PCMU,
		      56000, 31200, 31200, 31200, 3429,
		      SUSTAINED_RECEIVED_BITS, true,
		      MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
		      RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_DELAYED_ECHO),
	      "V.90 upstream did not sustain data through a delayed channel");
	CHECK(v90_datapump_unregister() == 0,
	      "delayed-channel V.90 registration remained busy");
	return true;
}

static bool test_upstream_uses_media_samples(void)
{
	CHECK(v90_datapump_register() == 0,
	      "sample-owned V.90 registration failed");
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMA,
		      42667, 26400, 3200, true,
		      MODEM_PCM_ROLE_NONE, true, false),
	      "digital upstream receiver used the PCMA codeword instead of "
	      "the media sample");
	CHECK(v90_datapump_unregister() == 0,
	      "sample-owned V.90 registration remained busy");
	return true;
}

static bool test_delayed_host_echo(void)
{
	CHECK(v90_datapump_register() == 0,
	      "host-echo V.90 registration failed");
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      28000, 14400, 3000, true,
		      MODEM_PCM_ROLE_NONE, false, true) &&
	      run_pair_with_input_contract(
		      G711_LAW_PCMA,
		      28000, 14400, 3000, true,
		      MODEM_PCM_ROLE_NONE, false, true),
	      "V.90 digital answerer did not cancel delayed host echo");
	CHECK(v90_datapump_unregister() == 0,
	      "host-echo V.90 registration remained busy");
	return true;
}

static bool test_dc_biased_host_echo(void)
{
	static const uint32_t MINIMUM_RATES[] = { 28800, 33600 };

	CHECK(v90_datapump_register() == 0,
	      "DC-biased V.90 registration failed");
	/* The conditioned receiver supports 33.6 despite the earlier Ri-era
	 * residual error. Verify both an open and fixed rate window, with enough
	 * exact data to cross several online quality windows after B1. */
	for (size_t index = 0; index < ARRAY_LENGTH(MINIMUM_RATES); ++index)
		CHECK(run_pair_with_rate_window(
			      G711_LAW_PCMU,
			      56000, MINIMUM_RATES[index], 33600,
			      33600, 3429,
			      2u * 33600u, true,
			      MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
			      RETURNED_ECHO_DC_BIASED, UPSTREAM_CHANNEL_CLEAN),
		      "DC-biased digital host echo lost exact data with minimum %u",
		      MINIMUM_RATES[index]);
	CHECK(v90_datapump_unregister() == 0,
	      "DC-biased V.90 registration remained busy");
	return true;
}

static bool test_full_retrain_pairs(void)
{
	CHECK(v90_datapump_register() == 0,
	      "full-retrain V.90 registration failed");
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(run_pair(
			      law, 28000, 14400, 3000, true,
			      MODEM_PCM_ROLE_ANALOG) &&
		      run_pair(
			      law, 28000, 14400, 3000, true,
			      MODEM_PCM_ROLE_DIGITAL),
		      "law %u V.90 retrain roles failed", (unsigned int)law);
	}
	CHECK(v90_datapump_unregister() == 0,
	      "full-retrain V.90 registration remained busy");
	return true;
}

static bool test_host_echo_retrain_pairs(void)
{
	CHECK(v90_datapump_register() == 0,
	      "host-echo retrain V.90 registration failed");
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      56000, 31200, 3429, true,
		      MODEM_PCM_ROLE_ANALOG, false, true) &&
	      run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      56000, 31200, 3429, true,
		      MODEM_PCM_ROLE_DIGITAL, false, true),
	      "V.90 retrain did not recover through returned host echo");
	CHECK(v90_datapump_unregister() == 0,
	      "host-echo retrain V.90 registration remained busy");
	return true;
}

static bool test_sustained_maximum_rate_retrains(void)
{
	CHECK(v90_datapump_register() == 0,
	      "sustained-retrain V.90 registration failed");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (enum modem_pcm_role initiator = MODEM_PCM_ROLE_ANALOG;
		     initiator <= MODEM_PCM_ROLE_DIGITAL; ++initiator)
			CHECK(run_pair_with_rate_window(
				      law, 56000, 31200, 31200, 31200, 3429,
				      MAXIMUM_RATE_RECEIVED_BITS, true,
				      initiator, false, UNITY_GAIN_Q15,
				      RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u lost sustained data through retrain",
			      (unsigned int)law, (unsigned int)initiator);
	}
	CHECK(v90_datapump_unregister() == 0,
	      "sustained-retrain V.90 registration remained busy");
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

static bool test_v92_pcm_upstream_selection(void)
{
	struct dp_operations conflict = { 0 };
	struct bit_endpoint analog_bits = { 0 };
	struct bit_endpoint digital_bits = { 0 };
	struct modem analog = endpoint_modem(
		MODEM_PCM_ROLE_ANALOG, &analog_bits);
	struct modem digital = endpoint_modem(
		MODEM_PCM_ROLE_DIGITAL, &digital_bits);
	struct dp_operations *operations;
	struct dp *analog_dp;
	struct dp *digital_dp;
	uint8_t analog_to_digital = g711_encode_pcmu(0);
	uint8_t digital_to_analog = g711_encode_pcmu(0);

	CHECK(modem_dp_register(DP_V92, &conflict) == 0 &&
	      v90_datapump_register() != 0 &&
	      modem_dp_get_registered_ops(DP_V90) == NULL &&
	      modem_dp_get_registered_ops(DP_V92) == &conflict &&
	      modem_dp_deregister(DP_V92, &conflict) == 0,
	      "PCM family registration did not roll back transactionally");
	CHECK(v90_datapump_register() == 0 &&
	      modem_dp_get_registered_ops(DP_V90) != NULL &&
	      modem_dp_get_registered_ops(DP_V90) ==
		      modem_dp_get_registered_ops(DP_V92),
	      "V.90 and V.92 did not share one operation owner");
	operations = modem_dp_get_registered_ops(DP_V92);
	analog.min_rate = analog.max_rate = 28000;
	digital.min_rate = digital.max_rate = 28000;
	CHECK(operations != NULL &&
	      operations->create(
		      &analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG,
		      operations) == NULL,
	      "V.92 accepted conventional upstream without an implementation");
	analog.sregs[SREG_V92_PCM_UPSTREAM] = 1;
	digital.sregs[SREG_V92_PCM_UPSTREAM] = 1;
	analog.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 4;
	digital.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 4;
	analog.sregs[SREG_V92_QUICK_CONNECT] = 1;
	CHECK(operations->create(
		      &analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG,
		      operations) == NULL,
	      "V.92 accepted quick connect without retained history");
	analog.sregs[SREG_V92_QUICK_CONNECT] = 0;
	analog_dp = operations->create(
		&analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG, operations);
	digital_dp = operations->create(
		&digital, DP_V92, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && digital_dp != NULL &&
	      analog_dp->id == DP_V92 && digital_dp->id == DP_V92 &&
	      operations->use_count == 2,
	      "V.92 PCM-upstream peers did not share the PCM call owner");
	for (size_t index = 0; index < MODEM_FRAG * 2u; ++index) {
		uint8_t next_analog;
		uint8_t next_digital;
		int16_t analog_received = g711_decode_pcmu(digital_to_analog);
		int16_t digital_received = g711_decode_pcmu(analog_to_digital);
		int16_t analog_transmitted;
		int16_t digital_transmitted;
		bool analog_exact = false;
		bool digital_exact = false;

		CHECK(operations->process_g711(
			      digital_dp, G711_LAW_PCMU,
			      &analog_to_digital, &digital_received,
			      &next_digital, &digital_transmitted, 1,
			      &digital_exact) == DPSTAT_OK && digital_exact &&
		      operations->process_g711(
			      analog_dp, G711_LAW_PCMU,
			      &digital_to_analog, &analog_received,
			      &next_analog, &analog_transmitted, 1,
			      &analog_exact) == DPSTAT_OK && analog_exact,
		      "V.92 PCM-upstream selection stopped at sample %zu", index);
		analog_to_digital = next_analog;
		digital_to_analog = next_digital;
	}
	CHECK(operations->delete(analog_dp) == 0 &&
	      operations->delete(digital_dp) == 0 &&
	      v90_datapump_unregister() == 0 &&
	      modem_dp_get_registered_ops(DP_V90) == NULL &&
	      modem_dp_get_registered_ops(DP_V92) == NULL,
	      "V.92 PCM-upstream selection retained shared ownership");
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
	      operations->process_g711 != NULL && operations->retrain != NULL &&
	      operations->hangup != NULL,
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

static bool fallback_source(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct modem *modem = context;

	memset(primary_bits, 1, request->primary_bits);
	if (source_bits(modem, 1, primary_bits, (int)request->primary_bits) < 0)
		return false;
	if (auxiliary_bit != NULL)
		*auxiliary_bit = 1;
	return true;
}

static bool run_fallback_restart(enum g711_law law, bool remote)
{
	enum { BLOCK = MODEM_FRAG };
	struct bit_endpoint digital_bits = { .transmit_seed = 7, .receive_seed = 19 };
	struct bit_endpoint analog_bits = { .transmit_seed = 19, .receive_seed = 7 };
	struct modem digital = endpoint_modem(MODEM_PCM_ROLE_DIGITAL, &digital_bits);
	struct modem analog = endpoint_modem(MODEM_PCM_ROLE_ANALOG, &analog_bits);
	struct pcm_call_configuration configuration;
	struct v90_upstream_policy upstream = {
		.minimum_bit_rate = 4800, .maximum_bit_rate = 28800,
		.maximum_symbol_rate = 3000,
	};
	struct pcm_call peer;
	struct v34_call conventional;
	struct v34_phase2_info_audio_transmitter info1a;
	struct dc_remover frontend;
	uint8_t to_digital[BLOCK];
	uint8_t to_analog[BLOCK];
	int16_t input[BLOCK];
	int16_t output[BLOCK];
	bool substituted = false;
	bool info1a_complete = false;
	bool fallback = false;
	bool retrain = false;
	bool returned = false;
	bool peer_returned = false;
	unsigned int warmup = 0;
	unsigned int phases = 0;
	int16_t peer_gain = 0;

	digital.min_rate = 28000;
	digital.max_rate = 56000;
	digital.v90_upstream_min_rate = 4800;
	digital.v90_upstream_max_rate = 28800;
	digital.v90_upstream_max_baud = 3000;
	CHECK(v90_profile_init(&configuration, V90_ANALOG_MODEM, law,
			       28000, 56000, &upstream) == V90_PROFILE_OK,
	      "fallback peer profile did not initialize");
	/* This peer also implements the conventional rates offered in INFO1d. */
	configuration.local_info0.analog.symbol_rate_2800_supported = true;
	CHECK(pcm_call_init(&peer, &configuration) == PCM_CALL_OK &&
	      dc_remover_init(&frontend) == DC_REMOVER_OK,
	      "fallback peer did not initialize");
	digital.dp = registered_v90->create(
		&digital, DP_V90, 0, MODEM_RATE, MODEM_FRAG, registered_v90);
	CHECK(digital.dp != NULL, "fallback digital datapump did not initialize");
	memset(to_digital, encode(law, 0), sizeof(to_digital));
	for (size_t clock = 0; clock < 4u * CALL_OCTET_LIMIT; clock += BLOCK) {
		struct dp *dp = digital.dp;
		bool exact = false;
		int result;

		for (size_t index = 0; index < BLOCK; ++index)
			input[index] = decode(law, to_digital[index]);
		CHECK(dc_remover_process(&frontend, input, BLOCK) == DC_REMOVER_OK,
		      "fallback receive conditioning failed");
		if (dp->id == DP_V90)
			result = dp->op->process_g711(dp, law, to_digital, input,
						   to_analog, output, BLOCK, &exact);
		else {
			result = dp->op->process(dp, input, output, BLOCK);
			for (size_t index = 0; index < BLOCK; ++index)
				to_analog[index] = encode(law, output[index]);
		}
		CHECK(result == DPSTAT_OK || result == DPSTAT_CONNECT ||
		      result == DPSTAT_CHANGEDP,
		      "fallback datapump failed at %zu (id=%d status=%d)",
		      clock, dp->id, result);
		if (result == DPSTAT_CHANGEDP) {
			struct dp *next;

			if (!fallback) {
				struct v90_v34_fallback_handoff handoff;
				struct v34_call_configuration v34_configuration;
				struct v34_info0 digital_info0;
				uint16_t mask;

				CHECK(digital.dp_requested == DP_V34 &&
				      v90_datapump_v34_fallback_handoff(dp, &handoff),
				      "V.90 did not export its fallback agreement");
				/* The peer uses the same INFO0 capabilities retained in
				 * the digital handoff, with the station roles reversed. */
				digital_info0 = handoff.phase2.local_info0;
				handoff.phase2.local_info0 = handoff.phase2.peer_info0;
				handoff.phase2.peer_info0 = digital_info0;
				handoff.phase2.local_role = V34_ANSWERING_STATION;
				handoff.phase2.receive_gain = peer_gain;
				CHECK(v34_profile_init(&v34_configuration, &mask,
						       V34_ANSWERING_STATION, 2400, 28800, 0) ==
					      V34_PROFILE_OK &&
				      v34_call_init_from_phase2(&conventional,
							 &v34_configuration, &handoff.phase2) ==
					      V34_CALL_OK,
				      "fallback conventional peer did not initialize");
				fallback = true;
			} else {
				CHECK(retrain && digital.dp_requested == DP_V90,
				      "fallback retrain did not return to V.90");
				returned = true;
			}
			next = modem_dp_get_registered_ops(digital.dp_requested)->create(
				&digital, digital.dp_requested, 0, MODEM_RATE, MODEM_FRAG,
				modem_dp_get_registered_ops(digital.dp_requested));
			CHECK(next != NULL, "fallback owner transfer failed");
			digital.dp = next;
			CHECK(dp->op->delete(dp) == 0, "fallback owner was not released");
		}
		for (size_t index = 0; index < BLOCK; ++index) {
			int16_t sample = decode(law, to_analog[index]);
			uint8_t generated = encode(law, 0);

			if (fallback && !peer_returned) {
				struct v34_call_observation observation;
				struct v34_received_mapping_frame frame;
				int16_t transmit;

				CHECK(v34_call_sample(&conventional, sample, fallback_source,
						     &analog, &transmit, &observation, &frame) ==
					      V34_CALL_OK,
				      "conventional fallback peer failed at %zu", clock + index);
				generated = encode(law, transmit);
				if (observation.processed == V34_CALL_PHASE4 &&
				    (observation.events.phase4 &
				     V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED))
					CHECK(sink_bits(&analog, 1, frame.primary_bits,
							(int)frame.primary_bit_count) >= 0,
					      "fallback peer data failed");
				if (conventional.stage == V34_CALL_RETRAIN_SILENCE) {
					CHECK(pcm_call_start_retrain(&peer) == PCM_CALL_OK,
					      "fallback peer could not resume V.90");
					/* Test peer follows V.90 9.2.2.1.9: the accepted
					 * shared response enters PCM, not V.34, Phase 2. */
					peer.retrain = conventional.retrain;
					peer_returned = true;
				}
			} else {
				struct pcm_call_observation observation;

				if (pcm_call_phase4_is_ready(&peer))
					CHECK(pcm_call_start_data_path(&peer) == PCM_CALL_OK,
					      "fallback peer PCM data did not start");
				if (peer.stage == PCM_CALL_DATA_PATH) {
					struct pcm_call_data_sources sources = {
						.upstream = fallback_source, .upstream_context = &analog,
					};
					struct pcm_call_data_event event;

					CHECK(pcm_call_data_octet(&peer, to_analog[index], sample,
								 &sources, &generated, &event) == PCM_CALL_OK,
					      "fallback peer PCM data failed");
					if (event.kind == PCM_CALL_DOWNSTREAM_FRAME)
						CHECK(sink_bits(&analog, 1, event.frame.downstream.bits,
								(int)event.frame.downstream.bit_count) >= 0,
						      "fallback peer PCM payload failed");
				} else {
					CHECK(pcm_call_octet(&peer, to_analog[index], sample,
							    &generated, &observation) == PCM_CALL_OK,
					      "fallback peer PCM training failed at %zu", clock + index);
					if (!substituted && observation.phase2.kind ==
						    PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED) {
						const struct v34_info1a selected = {
							.call_transmit = { .high_carrier = true,
								.projected_data_rate_2400_bps_units = 12 },
							.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3000,
							.call_to_answer_symbol_rate = V34_SYMBOL_RATE_3000,
						};
						uint8_t payload[V34_INFO1A_PAYLOAD_BYTES];

						CHECK(pcm_phase2_receive_gain(&peer.state.phase2, &peer_gain) ==
							      PCM_PHASE2_OK &&
						      v34_info1a_encode(&selected, payload, sizeof(payload)) ==
							      V34_INFO1_OK &&
						      v34_phase2_info_audio_transmitter_init(&info1a,
							      V34_ANSWERING_STATION,
							      configuration.phase2_nominal_amplitude,
							      payload, V34_INFO1A_PAYLOAD_BITS) ==
							      V34_PHASE2_INFO_AUDIO_OK,
						      "fallback INFO1a fixture did not initialize");
						substituted = true;
					}
				}
				if (substituted && !fallback) {
					int16_t transmit = 0;

					if (!info1a_complete)
						CHECK(v34_phase2_info_audio_transmitter_sample(
							      &info1a, &transmit, &info1a_complete) ==
							      V34_PHASE2_INFO_AUDIO_OK,
						      "fallback INFO1a transmitter failed");
					generated = encode(law, transmit);
				}
			}
			to_digital[index] = generated;
		}
		if (fallback && digital.dp->status == DPSTAT_CONNECT &&
		    (returned ? pcm_call_is_online(&peer) : v34_call_is_online(&conventional))) {
			warmup += BLOCK;
			if (warmup >= 1600u)
				digital_bits.enabled = analog_bits.enabled = true;
		}
		CHECK(!digital_bits.mismatch && !analog_bits.mismatch,
		      "fallback payload mismatch at %zu (phase=%u bits=%llu/%llu)",
		      clock, phases, (unsigned long long)digital_bits.received_bits,
		      (unsigned long long)analog_bits.received_bits);
		if (digital_bits.received_bits >= SUSTAINED_RECEIVED_BITS &&
		    analog_bits.received_bits >= SUSTAINED_RECEIVED_BITS) {
			if (++phases == 2u)
				break;
			digital_bits = (struct bit_endpoint){ .transmit_seed = 31, .receive_seed = 43 };
			analog_bits = (struct bit_endpoint){ .transmit_seed = 43, .receive_seed = 31 };
			warmup = 0;
			retrain = true;
			CHECK(remote ? v34_call_start_retrain(&conventional) == V34_CALL_OK :
				       digital.dp->op->retrain(digital.dp) == 0,
			      "fallback ATO1 did not start");
		}
	}
	CHECK(phases == 2u && returned && peer_returned &&
	      digital.dp->id == DP_V90 && digital.tx_rate > 33600,
	      "fallback did not restore PCM data (phases=%u returned=%d/%d)",
	      phases, returned, peer_returned);
	CHECK(digital.dp->op->delete(digital.dp) == 0,
	      "fallback final owner was not released");
	return true;
}

static bool test_fallback_restart(void)
{
	CHECK(v90_datapump_register() == 0 && v34_datapump_register() == 0,
	      "fallback datapumps did not register");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		CHECK(run_fallback_restart(law, false) && run_fallback_restart(law, true),
		      "fallback retrain failed for law %u", (unsigned int)law);
	CHECK(v90_datapump_unregister() == 0 && v34_datapump_unregister() == 0,
	      "fallback datapumps leaked ownership");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "V.90 retrain after V.34 fallback", test_fallback_restart },
		{ "automatic upstream quality recovery", test_upstream_quality_recovery },
		{ "long-delay upstream channel",
		  test_delayed_upstream_channel },
		{ "attenuated maximum-rate upstream",
		  test_attenuated_upstream },
		{ "V.92 PCM-upstream selection",
		  test_v92_pcm_upstream_selection },
		{ "forced upstream pairs", test_forced_upstream_pairs },
		{ "sustained maximum-rate pairs",
		  test_sustained_maximum_rate_pairs },
		{ "sample-owned PCMA upstream", test_upstream_uses_media_samples },
		{ "delayed digital-answerer host echo", test_delayed_host_echo },
		{ "DC-biased digital-answerer host echo",
		  test_dc_biased_host_echo },
		{ "local and remote full retrain", test_full_retrain_pairs },
		{ "sustained maximum-rate retrains",
		  test_sustained_maximum_rate_retrains },
		{ "full retrain through returned host echo",
		  test_host_echo_retrain_pairs },
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
