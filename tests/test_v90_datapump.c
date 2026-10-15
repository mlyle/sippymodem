// SPDX-License-Identifier: BSD-3-Clause

#include <v90_datapump.h>
#include <v90_profile.h>
#include <v92_profile.h>
#include <v34_datapump.h>
#include <v34_call.h>
#include <datapump_link.h>

#include <dc_remover.h>
#include <g711.h>
#include <modem.h>
#include <v8_datapump.h>

#include "test.h"

#include <stdint.h>
#include <stdlib.h>
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
	SPREAD_ECHO_DELAY = 2967,
	SPREAD_LINK_DELAY = 2136,
	/* At 3429 symbols/s, 49 samples contain exactly 21 symbols. With the
	 * fixed 16-symbol decision delay, this echo begins just beyond the
	 * conventional 37-tap span. */
	UPSTREAM_ECHO_DELAY = 49,
	UPSTREAM_ECHO_HISTORY = UPSTREAM_ECHO_DELAY,
	UPSTREAM_ECHO_GAIN_Q15 = 8000,
	PCM_UPSTREAM_NOISE_SAMPLES = 16000,
	V34_UPSTREAM_NOISE_SAMPLES = 24000,
	RETRAIN_DROPOUT_START = V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES +
		V34_RETRAIN_TONE_SAMPLES + 2u * V34_PHASE2_TONE_WINDOW_SAMPLES,
	RETRAIN_DROPOUT_SAMPLES = 3u * V34_PHASE2_TONE_LOSS_SAMPLES,
};

enum returned_echo_mode {
	RETURNED_ECHO_NONE,
	RETURNED_ECHO_STABLE,
	RETURNED_ECHO_DC_BIASED,
	RETURNED_ECHO_DC_BIASED_G711,
	RETURNED_ECHO_WEAK_G711,
	RETURNED_ECHO_SPREAD_G711,
};

enum upstream_channel_mode {
	UPSTREAM_CHANNEL_CLEAN,
	UPSTREAM_CHANNEL_DELAYED_ECHO,
	UPSTREAM_CHANNEL_NOISE_BURST,
	UPSTREAM_CHANNEL_RETRAIN_DROPOUT,
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

int modem_get_sreg(struct modem *modem, unsigned int number)
{
	if (modem == NULL || number >= sizeof(modem->sregs))
		return -1;
	if (number == SREG_V90_DIGITAL_INDICATION)
		return modem->dsp_info.pcm_role == MODEM_PCM_ROLE_DIGITAL;
	return modem->sregs[number];
}

int modem_set_link_rates(struct modem *modem, uint32_t transmit_rate, uint32_t receive_rate)
{
	if (modem == NULL || transmit_rate > INT_MAX || receive_rate > INT_MAX)
		return -1;
	modem->tx_rate = transmit_rate;
	modem->rx_rate = receive_rate;
	return 0;
}

/* This fixture supplies host bits directly; it has no async byte framer. */
void modem_note_disconnect(struct modem *modem, bool local, const char *reason)
{
	if (modem->disconnect_reason == NULL) {
		modem->disconnect_reason = reason;
		modem->disconnect_local = local;
	}
}

void modem_async_retrain(struct modem *modem)
{
	(void)modem;
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
		.pcm_codec_grid = true,
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

static bool qc_adapter_step(struct modem *modem, enum g711_law law,
	const uint8_t *input, uint8_t *output, unsigned count, int *status)
{
	struct dp *old = modem->dp;
	int16_t receive[MODEM_FRAG], transmit[MODEM_FRAG];
	bool exact = false;

	for (unsigned i = 0; i < count; ++i)
		receive[i] = decode(law, input[i]);
	if (old->op->process_g711 != NULL)
		*status = old->op->process_g711(old, law, input, receive, NULL,
			output, transmit, (int)count, &exact);
	else
		*status = old->op->process(old, receive, transmit, (int)count);
	if (!exact)
		for (unsigned i = 0; i < count; ++i)
			output[i] = encode(law, transmit[i]);
	if (*status == DPSTAT_CHANGEDP) {
		enum DP_ID target = modem->dp_requested;
		const struct dp_operations *ops = modem_dp_get_registered_ops(target == 0 ? DP_V8 : target);
		struct dp *next = ops == NULL ? NULL : ops->create(modem,
			target == 0 ? DP_V92 : target, modem->caller, MODEM_RATE, MODEM_FRAG, ops);

		CHECK(next != NULL, "QC owner transfer failed from %u to %u", old->id, target);
		modem->dp = next;
		CHECK(old->op->delete(old) == 0, "QC transfer retained its old owner");
	}
	return true;
}

static bool check_qc_adapter(enum g711_law law, bool digital_caller, bool pcm_upstream,
	bool decline, unsigned block)
{
	struct bit_endpoint bits[2] = {
		{ .transmit_seed = 19, .receive_seed = 73 },
		{ .transmit_seed = 73, .receive_seed = 19 },
	};
	struct modem modems[2] = {
		endpoint_modem(MODEM_PCM_ROLE_ANALOG, &bits[0]),
		endpoint_modem(MODEM_PCM_ROLE_DIGITAL, &bits[1]),
	};
	uint8_t channel[2][MODEM_FRAG];
	bool linked[2] = { false, false };

	for (unsigned side = 0; side < 2; ++side) {
		struct modem *m = &modems[side];
		const struct dp_operations *v8 = modem_dp_get_registered_ops(DP_V8);

		m->caller = (side == 1) == digital_caller;
		m->min_rate = m->max_rate = 52000;
		m->sregs[SREG_V92_PCM_UPSTREAM] = pcm_upstream;
		m->sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 13; /* 40000 bit/s. */
		m->sregs[SREG_V92_QUICK_CONNECT] = !decline || m->caller;
		/* Single-sample cases also exercise asymmetric LAPM capability. */
		m->sregs[SREG_EC] = pcm_upstream && (side == 1 || block != 1);
		m->dp = v8->create(m, DP_V92, m->caller,
			MODEM_RATE, MODEM_FRAG, v8);
		CHECK(m->dp != NULL, "QC V.8 owner creation failed");
		memset(channel[side], encode(law, 0), sizeof(channel[side]));
	}
	for (unsigned clock = 0; clock < 30u * MODEM_RATE; clock += block) {
		for (unsigned side = 0; side < 2; ++side) {
			int status;

			CHECK(qc_adapter_step(&modems[side], law, channel[1 - side], channel[side], block, &status) &&
			      (status == DPSTAT_OK || status == DPSTAT_CHANGEDP || status == DPSTAT_CONNECT),
			      "QC adapter failed: law %u caller %u pcm %u decline %u block %u clock %u side %u status %d",
			      law, digital_caller, pcm_upstream, decline, block, clock, side, status);
			linked[side] |= status == DPSTAT_CONNECT;
		}
		if (linked[0] && linked[1])
			bits[0].enabled = bits[1].enabled = true;
		if (bits[0].received_bits >= SUSTAINED_RECEIVED_BITS && bits[1].received_bits >= SUSTAINED_RECEIVED_BITS)
			break;
	}
	for (unsigned side = 0; side < 2; ++side) {
		CHECK((side == 0 ? modems[side].rx_rate : modems[side].tx_rate) == 52000 &&
		      (!pcm_upstream || (side == 0 ? modems[side].tx_rate : modems[side].rx_rate) == 40000),
		      "QC adapter rates: law %u caller %u pcm %u decline %u side %u tx/rx %u/%u",
		      law, digital_caller, pcm_upstream, decline, side, modems[side].tx_rate, modems[side].rx_rate);
		CHECK(bits[side].received_bits >= SUSTAINED_RECEIVED_BITS && !bits[side].mismatch &&
		      modems[side].dsp_info.startup == (decline ? MODEM_STARTUP_V8 : MODEM_STARTUP_QC) &&
		      modems[side].dsp_info.v8_lapm == (pcm_upstream && block != 1) &&
		      modems[side].dsp_info.upstream_mode == (pcm_upstream ? MODEM_UPSTREAM_PCM : MODEM_UPSTREAM_V34),
		      "QC adapter lacked exact data or observed selection: law %u caller %u pcm %u decline %u side %u bits %llu mismatch %u startup %u",
		      law, digital_caller, pcm_upstream, decline, side,
		      (unsigned long long)bits[side].received_bits, bits[side].mismatch, modems[side].dsp_info.startup);
		CHECK(modems[side].dp->op->delete(modems[side].dp) == 0, "QC owner deletion failed");
	}
	return true;
}

static bool test_qc_adapter(void)
{
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned caller = 0; caller < 2; ++caller) {
			for (unsigned pcm = 0; pcm < 2; ++pcm)
				CHECK(check_qc_adapter(law, caller, pcm, false, MODEM_FRAG),
				      "QC fragment handoff failed");
			CHECK(check_qc_adapter(law, caller, true, false, 1) &&
			      check_qc_adapter(law, caller, true, true, MODEM_FRAG),
			      "QC exact/declined opening failed");
		}
	return true;
}

static bool check_qc_adapter_timeout(enum g711_law law, bool digital_answerer)
{
	struct bit_endpoint bits[2] = { 0 };
	struct modem modems[2] = {
		endpoint_modem(digital_answerer ? MODEM_PCM_ROLE_ANALOG : MODEM_PCM_ROLE_DIGITAL, &bits[0]),
		endpoint_modem(digital_answerer ? MODEM_PCM_ROLE_DIGITAL : MODEM_PCM_ROLE_ANALOG, &bits[1]),
	};
	uint8_t channel[2][MODEM_FRAG];
	bool accepted = false, restarted = false, failed = false;
	unsigned clock;

	const struct dp_operations *v8 = modem_dp_get_registered_ops(DP_V8);
	for (unsigned side = 0; side < 2; ++side) {
		modems[side].caller = side == 0;
		modems[side].sregs[SREG_V92_QUICK_CONNECT] = 1;
		modems[side].sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 8;
		modems[side].dp = v8->create(&modems[side], DP_V92, side == 0,
			MODEM_RATE, MODEM_FRAG, v8);
		CHECK(modems[side].dp != NULL, "QC timeout owner creation failed");
		memset(channel[side], encode(law, 0), sizeof(channel[side]));
	}
	for (clock = 0; clock < 9u * MODEM_RATE; clock += MODEM_FRAG) {
		int status;

		if (!accepted)
			CHECK(qc_adapter_step(&modems[0], law, channel[1], channel[0], MODEM_FRAG, &status),
			      "QC timeout caller failed");
		else
			memset(channel[0], encode(law, 0), sizeof(channel[0]));
		CHECK(qc_adapter_step(&modems[1], law, channel[0], channel[1], MODEM_FRAG, &status),
		      "QC timeout answerer failed");
		accepted |= modems[1].dp->id == DP_V92;
		if (accepted && modems[1].dp->id == DP_V8)
			restarted = true;
		if (status == DPSTAT_ERROR) {
			failed = true;
			break;
		}
	}
	CHECK(accepted && restarted && failed && clock == 8u * MODEM_RATE &&
	      modems[1].dsp_info.startup != MODEM_STARTUP_QC,
	      "QC fallback reset the deadline or claimed startup: law %u digital answer %u clock %u states %u/%u/%u",
	      law, digital_answerer, clock, accepted, restarted, failed);
	for (unsigned side = 0; side < 2; ++side)
		CHECK(modems[side].dp->op->delete(modems[side].dp) == 0, "QC timeout retained owner");
	return true;
}

static bool test_qc_adapter_timeout(void)
{
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned digital = 0; digital < 2; ++digital)
			CHECK(check_qc_adapter_timeout(law, digital), "QC fallback deadline failed");
	return true;
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
	if (mode == RETURNED_ECHO_SPREAD_G711) {
		int32_t sample = 0;
		for (unsigned int tap = 0; tap < 8u; ++tap)
			sample += scale_q15(history_sample(history, RETURNED_ECHO_HISTORY,
				write, fill, SPREAD_ECHO_DELAY + 17u * tap),
				tap % 3u == 0u ? -1024 : 1024);
		return add_s16(sample);
	}
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
	if (mode == RETURNED_ECHO_DC_BIASED ||
	    mode == RETURNED_ECHO_DC_BIASED_G711)
		sample += 1400;
	if (mode == RETURNED_ECHO_WEAK_G711)
		sample /= 8;
	return add_s16(sample);
}

static bool check_cleardown(struct dp *dp, enum g711_law law,
	unsigned int timeout_seconds)
{
	uint8_t input[MODEM_FRAG];
	int16_t samples[MODEM_FRAG] = { 0 };
	bool complete = false;
	bool signaling = false;
	unsigned int elapsed = 0u;

	memset(input, encode(law, 0), sizeof(input));
	CHECK(dp->op->hangup != NULL && dp->op->hangup(dp) == 0 &&
	      dp->op->hangup(dp) == 0,
	      "PCM hangup did not start idempotently");
	for (; elapsed < timeout_seconds * MODEM_RATE; elapsed += MODEM_FRAG) {
		uint8_t output[MODEM_FRAG];
		int16_t generated[MODEM_FRAG];
		bool exact = false;
		int result = dp->op->process_g711(
			dp, law, input, samples, NULL, output, generated, MODEM_FRAG,
			&exact);

		CHECK((result == DPSTAT_OK || result == DPSTAT_DISCONNECT) &&
		      exact,
		      "PCM cleardown failed after %u samples, status %d",
		      elapsed, result);
		for (unsigned int index = 0u; index < MODEM_FRAG; ++index) {
			CHECK(generated[index] == decode(law, output[index]),
			      "PCM cleardown lost exact network character");
			signaling |= output[index] != encode(law, 0);
		}
		if (result == DPSTAT_DISCONNECT) {
			complete = true;
			break;
		}
	}
	CHECK(complete && signaling,
	      "cleardown did not emit a bounded protocol sequence (complete=%u elapsed=%u)",
	      complete, elapsed);
	{
		uint8_t output[MODEM_FRAG];
		int16_t generated[MODEM_FRAG];
		bool exact = false;

		CHECK(dp->status == DPSTAT_DISCONNECT && dp->op->retrain(dp) != 0 &&
		      dp->op->hangup(dp) == 0 &&
		      dp->op->process_g711(dp, law, input, samples, NULL, output, generated,
					  MODEM_FRAG, &exact) == DPSTAT_DISCONNECT && exact,
		      "closed datapump lost ownership or attempted to restart");
		for (unsigned int index = 0u; index < MODEM_FRAG; ++index)
			CHECK(output[index] == encode(law, 0) &&
			      generated[index] == decode(law, output[index]),
			      "closed datapump generated further output");
	}
	return true;
}

static bool run_training_cleardown(
	enum DP_ID protocol, enum g711_law law, unsigned int initiator,
	size_t *control_clock)
{
	struct bit_endpoint bits[2] = { 0 };
	struct modem modems[2] = {
		endpoint_modem(MODEM_PCM_ROLE_ANALOG, &bits[0]),
		endpoint_modem(MODEM_PCM_ROLE_DIGITAL, &bits[1]),
	};
	const struct dp_operations *operations = modem_dp_get_registered_ops(protocol);
	struct dp *pumps[2];
	struct dc_remover frontend;
	uint8_t received[2] = { encode(law, 0), encode(law, 0) };
	bool started = false;
	bool near_deadline = *control_clock != 0u;
	unsigned int carrier_seconds = near_deadline ?
		(unsigned int)(*control_clock / MODEM_RATE) + 2u : 60u;

	CHECK(operations != NULL && dc_remover_init(&frontend) == DC_REMOVER_OK,
	      "training cleardown fixture did not initialize");
	for (unsigned int side = 0; side < 2u; ++side) {
		modems[side].min_rate = modems[side].max_rate = 28000;
		modems[side].sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = carrier_seconds;
		modems[side].sregs[SREG_V92_PCM_UPSTREAM] = protocol == DP_V92;
		modems[side].sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 4u;
		pumps[side] = operations->create(&modems[side], protocol, side == 0u,
			MODEM_RATE, MODEM_FRAG, operations);
		CHECK(pumps[side] != NULL, "training cleardown datapump did not initialize");
	}
	/* Rejected early hangups must not disturb training. The first accepted
	 * request must finish through the datapump API without ever CONNECTing. */
	for (size_t clock = 0; clock < CALL_OCTET_LIMIT; ++clock) {
		uint8_t transmitted[2];

		if (clock % MODEM_FRAG == 0u && (near_deadline ?
			clock == *control_clock : operations->hangup(pumps[initiator]) == 0)) {
			*control_clock = clock;
			started = true;
			break;
		}
		for (unsigned int side = 0; side < 2u; ++side) {
			int16_t sample = decode(law, received[side]);
			int16_t generated;
			bool exact = false;

			if (side == 1u)
				CHECK(dc_remover_process(&frontend, &sample, 1) == DC_REMOVER_OK,
				      "training cleardown receive conditioning failed");
			CHECK(operations->process_g711(pumps[side], law, &received[side],
				      &sample, NULL, &transmitted[side], &generated, 1, &exact) ==
				      DPSTAT_OK && exact && pumps[side]->status != DPSTAT_CONNECT,
			      "training cleardown missed control at %zu side %u", clock, side);
		}
		received[0] = transmitted[1];
		received[1] = transmitted[0];
	}
	CHECK(started, "training cleardown never reached control");
	if (near_deadline) {
		/* Repeat the same handshake up to its first cancellable control
		 * interval. Withhold the peer response until only one sample of
		 * the host CONNECT budget remains, then request hangup. Shutdown
		 * must use its own bounded protocol timer, not the old S7 budget. */
		size_t remaining = (size_t)carrier_seconds * MODEM_RATE - *control_clock - 1u;
		uint8_t input[MODEM_FRAG];
		int16_t samples[MODEM_FRAG] = { 0 };

		memset(input, encode(law, 0), sizeof(input));
		while (remaining != 0u) {
			int count = remaining > MODEM_FRAG ? MODEM_FRAG : (int)remaining;
			uint8_t output[MODEM_FRAG];
			int16_t generated[MODEM_FRAG];
			bool exact = false;

			CHECK(operations->process_g711(pumps[initiator], law, input, samples, NULL,
				      output, generated, count, &exact) == DPSTAT_OK && exact &&
			      pumps[initiator]->status != DPSTAT_CONNECT,
			      "control did not survive to the CONNECT deadline");
			remaining -= (size_t)count;
		}
	}
	CHECK(check_cleardown(pumps[initiator], law, 2u) &&
	      bits[0].supplied_bits == 0u && bits[1].supplied_bits == 0u,
	      "training cleardown did not close before host data (near deadline=%u)",
	      near_deadline);
	CHECK(operations->delete(pumps[0]) == 0 && operations->delete(pumps[1]) == 0,
	      "training cleardown retained a datapump owner");
	return true;
}

static bool test_training_cleardown(void)
{
	const enum DP_ID protocols[] = { DP_V90, DP_V92 };

	for (size_t p = 0u; p < ARRAY_LENGTH(protocols); ++p)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned int side = 0; side < 2u; ++side) {
				size_t control_clock = 0u;

				CHECK(run_training_cleardown(protocols[p], law, side, &control_clock) &&
				      run_training_cleardown(protocols[p], law, side, &control_clock),
				      "protocol %u law %u side %u training cleardown failed",
				      protocols[p], law, side);
			}
	return true;
}

static bool run_pcm_pair(
	enum DP_ID protocol, uint8_t pcm_upstream_max_drn,
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
	uint8_t downstream_delay[SPREAD_LINK_DELAY];
	size_t downstream_delay_write = 0;
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
	const struct dp_operations *operations =
		modem_dp_get_registered_ops(protocol);
	struct dp *analog_dp;
	struct dp *digital_dp;
	uint8_t analog_to_digital = law == G711_LAW_PCMA ?
					     g711_encode_pcma(0) :
					     g711_encode_pcmu(0);
	uint8_t digital_to_analog = law == G711_LAW_PCMA ?
					     g711_encode_pcma(0) :
					     g711_encode_pcmu(0);
	bool data_enabled = false;
	bool training_rates_seen = false;
	bool retrain_requested = false;
	bool analog_reconnected = false;
	bool digital_reconnected = false;
	bool post_retrain_data_enabled = false;
	bool automatic_retrain = upstream_channel == UPSTREAM_CHANNEL_NOISE_BURST;
	bool automatic_downshift = automatic_retrain && pcm_upstream_max_drn == 0 &&
		upstream_min_rate < upstream_max_rate;
	bool exercise_retrain = retrain_role != MODEM_PCM_ROLE_NONE || automatic_retrain;
	/* Reserve payload time at the slowest supported upstream rate separately
	 * from each training budget, which now includes nonzero DIL. */
	size_t sample_limit = (CALL_OCTET_LIMIT +
		(minimum_received_bits * MODEM_RATE + 4799u) / 4800u) *
		(exercise_retrain ? 2u : 1u);
	uint32_t noise_state = UINT32_C(0x197bc485);
	/* V.34 recovery now needs at least 1.8 s of complete bad windows. */
	const unsigned int noise_limit = pcm_upstream_max_drn == 0 ?
		V34_UPSTREAM_NOISE_SAMPLES : PCM_UPSTREAM_NOISE_SAMPLES;
	unsigned int noise_samples = 0;
	unsigned int dropout_samples = 0;
	size_t retrain_start = 0;
	bool sample_codeword_separated = false;
	size_t echo_history_write = 0;
	size_t echo_history_fill = 0;
	size_t upstream_history_write = 0;
	size_t upstream_history_fill = 0;
	memset(downstream_delay, encode(law, 0), sizeof(downstream_delay));

	if (protocol == DP_V92) {
		analog.sregs[SREG_V92_PCM_UPSTREAM] = pcm_upstream_max_drn != 0;
		digital.sregs[SREG_V92_PCM_UPSTREAM] = pcm_upstream_max_drn != 0;
		analog.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = pcm_upstream_max_drn;
		digital.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = pcm_upstream_max_drn;
	}
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
		&analog, protocol, 1, MODEM_RATE, MODEM_FRAG, operations);
	digital_dp = operations->create(
		&digital, protocol, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && digital_dp != NULL,
	      "V.90 peers did not own independent data pumps");
	CHECK(datapump_get_link_rates(analog_dp).transmit_bps == 0 &&
	      datapump_get_link_rates(digital_dp).receive_baud == 0,
	      "untrained PCM diagnostics published selected rates");
	for (size_t octet = 0;
	     octet < (require_online ? sample_limit : 30000u);
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
		if (echo_mode != RETURNED_ECHO_WEAK_G711)
			digital_receive = scale_uq15(
				digital_receive, digital_receive_gain_q15);
		if (upstream_channel == UPSTREAM_CHANNEL_RETRAIN_DROPOUT &&
		    retrain_requested && octet - retrain_start >= RETRAIN_DROPOUT_START &&
		    dropout_samples < RETRAIN_DROPOUT_SAMPLES) {
			/* Lose Tone A after the digital modem qualifies the request,
			 * during its response silence. The peer keeps running normally. */
			digital_received_octet = encode(law, 0);
			digital_receive = decode(law, digital_received_octet);
			dropout_samples++;
		}
		if (automatic_retrain && retrain_requested &&
		    noise_samples < noise_limit) {
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
		if (echo_mode == RETURNED_ECHO_DC_BIASED_G711 ||
		    echo_mode == RETURNED_ECHO_WEAK_G711 ||
		    echo_mode == RETURNED_ECHO_SPREAD_G711) {
			/* The analogue return, echo and bias share one codec input. */
			digital_received_octet = encode(law, digital_receive);
			digital_receive = decode(law, digital_received_octet);
		}
		if (echo_mode == RETURNED_ECHO_WEAK_G711)
			digital_receive = scale_uq15(digital_receive, digital_receive_gain_q15);
		/* Match modem_preserve_pcm_input: PCM upstream needs the original
		 * codec cells, not an additional time-varying DC-removal transform. */
		if (protocol != DP_V92 || !pcm_upstream_max_drn)
			CHECK(dc_remover_process(
				      &digital_frontend, &digital_receive, 1) == DC_REMOVER_OK,
			      "V.90 digital receive front end failed at octet %zu", octet);
		digital_result = operations->process_g711(
			digital_dp, law,
			&digital_received_octet, &digital_receive, NULL,
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
			if (echo_mode == RETURNED_ECHO_SPREAD_G711) {
				digital_to_analog = downstream_delay[downstream_delay_write];
				downstream_delay[downstream_delay_write] = next_digital;
				downstream_delay_write = (downstream_delay_write + 1u) % SPREAD_LINK_DELAY;
			}
			analog_receive = decode(law, delivered);
			analog_result = operations->process_g711(
				analog_dp, law, &delivered,
				&analog_receive, NULL, &next_analog,
				&analog_transmit, 1, &analog_exact);
		}
		CHECK((analog_result == DPSTAT_OK ||
		       analog_result == DPSTAT_CONNECT) && analog_exact &&
		      analog_transmit == decode(law, next_analog),
		      "analogue V.90 path failed at octet %zu", octet);
		analog_to_digital = next_analog;
		if (!training_rates_seen && octet % MODEM_FRAG == 0) {
			struct dp_link_rates rates = datapump_get_link_rates(digital_dp);

			training_rates_seen = rates.transmit_baud == 8000 &&
				rates.receive_baud > 0 && rates.transmit_bps == 0 && rates.receive_bps == 0;
		}
		if (retrain_requested) {
			if (automatic_downshift) {
				CHECK(analog_dp->status == DPSTAT_CONNECT &&
				      digital_dp->status == DPSTAT_CONNECT,
				      "quality downshift entered full retrain at %zu", octet);
				analog_reconnected |= analog.tx_rate == expected_upstream_rate;
				digital_reconnected |= digital.rx_rate == expected_upstream_rate;
			}
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
			struct dp_link_rates a = datapump_get_link_rates(analog_dp);
			struct dp_link_rates d = datapump_get_link_rates(digital_dp);
			double baud = pcm_upstream_max_drn != 0 ? 8000 :
				upstream_symbol_rate == 3429 ? 24000.0 / 7.0 : upstream_symbol_rate;

			CHECK(a.transmit_bps == analog.tx_rate && a.receive_bps == analog.rx_rate &&
			      d.transmit_bps == digital.tx_rate && d.receive_bps == digital.rx_rate &&
			      a.receive_baud == 8000 && d.transmit_baud == 8000 &&
			      a.transmit_baud > 0 && a.transmit_baud == d.receive_baud &&
			      (baud == 0 || a.transmit_baud == baud),
			      "PCM diagnostics lost negotiated directional rates: %g/%g expected %g",
			      a.transmit_baud, d.receive_baud, baud);
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
			retrain_start = octet;
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
		      operations->delete(digital_dp) == 0,
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
	{
		struct dp_link_rates a = datapump_get_link_rates(analog_dp);
		struct dp_link_rates d = datapump_get_link_rates(digital_dp);

		CHECK(training_rates_seen && a.transmit_bps == analog.tx_rate &&
		      a.receive_bps == analog.rx_rate && d.transmit_bps == digital.tx_rate &&
		      d.receive_bps == digital.rx_rate,
		      "PCM diagnostics missed training clocks or retained pre-recovery bit rates");
	}
	CHECK(analog.dsp_info.upstream_mode ==
		      (pcm_upstream_max_drn != 0 ? MODEM_UPSTREAM_PCM : MODEM_UPSTREAM_V34) &&
	      digital.dsp_info.upstream_mode == analog.dsp_info.upstream_mode,
	      "PCM pair reported the configured rather than negotiated upstream mode");
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
	CHECK(!automatic_retrain || noise_samples == noise_limit,
	      "quality recovery did not consume the complete noise burst");
	CHECK(upstream_channel != UPSTREAM_CHANNEL_RETRAIN_DROPOUT ||
	      dropout_samples == RETRAIN_DROPOUT_SAMPLES,
	      "retrain response did not exercise the complete tone dropout");
	CHECK(!separate_upstream_sample || sample_codeword_separated,
	      "PCMA upstream sample never diverged from its test codeword");
	if (protocol == DP_V90 || pcm_upstream_max_drn == 0) {
		CHECK(!v90_datapump_start_rate_renegotiation(analog_dp, false) &&
		      !v90_datapump_start_rate_renegotiation(digital_dp, true),
		      "non-PCM-upstream call accepted a V.92 rate exchange");
	}
	{
		uint64_t analog_supplied = analog_bits.supplied_bits;
		uint64_t digital_supplied = digital_bits.supplied_bits;
		/* Withhold the peer after hangup. PCM upstream must first bound
		 * an unanswered Ru/Rd entry; conventional upstream sends CP/MP
		 * directly. Neither path may consume more host payload. */
		unsigned int timeout = pcm_upstream_max_drn != 0 ? 6u : 2u;
		CHECK(check_cleardown(analog_dp, law, timeout) &&
		      check_cleardown(digital_dp, law, timeout) &&
		      analog_supplied == analog_bits.supplied_bits &&
		      digital_supplied == digital_bits.supplied_bits,
		      "PCM cleardown failed or consumed new host payload");
	}
	CHECK(operations->delete(analog_dp) == 0 &&
	      operations->delete(digital_dp) == 0,
	      "V.90 data pumps did not release their call state");
	return true;
}

static bool run_pair_with_rate_window(
	enum g711_law law, uint32_t downstream_rate,
	uint32_t upstream_min_rate, uint32_t upstream_max_rate,
	uint32_t expected_upstream_rate, uint32_t upstream_symbol_rate,
	uint64_t minimum_received_bits,
	bool require_online, enum modem_pcm_role retrain_role,
	bool separate_upstream_sample, uint16_t digital_receive_gain_q15,
	enum returned_echo_mode echo_mode,
	enum upstream_channel_mode upstream_channel)
{
	return run_pcm_pair(DP_V90, 0, law, downstream_rate,
		upstream_min_rate, upstream_max_rate, expected_upstream_rate,
		upstream_symbol_rate, minimum_received_bits, require_online, retrain_role,
		separate_upstream_sample, digital_receive_gain_q15, echo_mode,
		upstream_channel);
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
	return true;
}

static bool test_upstream_quality_downshift(void)
{
	static const enum DP_ID protocols[] = { DP_V90, DP_V92 };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t index = 0; index < ARRAY_LENGTH(protocols); ++index)
			CHECK(run_pcm_pair(protocols[index], 0, law, 45333,
				26400, 28800, 26400, 3200, SUSTAINED_RECEIVED_BITS,
				true, MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
				RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_NOISE_BURST),
			      "law %u protocol %u did not downshift after noise", law, protocols[index]);
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
	return true;
}

static bool test_forced_upstream_pairs(void)
{
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
	return true;
}

static bool test_sustained_maximum_rate_pairs(void)
{
	static const uint32_t SYMBOL_RATES[] = { 3200, 3429 };

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
	return true;
}

static bool test_attenuated_upstream(void)
{
	static const uint16_t GAINS_Q15[] = {
		WEAK_UPSTREAM_GAIN_Q15,
		/* Decodable QAM below the fixed raw retrain-quiet ceiling. */
		1600u,
	};

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
	return true;
}

static bool test_delayed_upstream_channel(void)
{
	CHECK(run_pair_with_rate_window(
		      G711_LAW_PCMU,
		      56000, 31200, 31200, 31200, 3429,
		      SUSTAINED_RECEIVED_BITS, true,
		      MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
		      RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_DELAYED_ECHO),
	      "V.90 upstream did not sustain data through a delayed channel");
	return true;
}

static bool test_upstream_uses_media_samples(void)
{
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMA,
		      42667, 26400, 3200, true,
		      MODEM_PCM_ROLE_NONE, true, false),
	      "digital upstream receiver used the PCMA codeword instead of "
	      "the media sample");
	return true;
}

static bool test_delayed_host_echo(void)
{
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      28000, 14400, 3000, true,
		      MODEM_PCM_ROLE_NONE, false, true) &&
	      run_pair_with_input_contract(
		      G711_LAW_PCMA,
		      28000, 14400, 3000, true,
		      MODEM_PCM_ROLE_NONE, false, true),
	      "V.90 digital answerer did not cancel delayed host echo");
	return true;
}

static bool test_dc_biased_host_echo(void)
{
	static const uint32_t MINIMUM_RATES[] = { 28800, 33600 };

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
	return true;
}

static bool test_quantized_host_echo(void)
{
	/* PCMA quantization of the biased return produces a false S candidate
	 * during Sd/TRN1d. It cannot acknowledge the not-yet-transmitted Jd.
	 * The calibrated -12 dBm0 probe selects 31.2 kbit/s. Require exact
	 * sustained data at that rate, not just survival of the prefix. */
	CHECK(run_pair_with_rate_window(
		      G711_LAW_PCMA, 56000, 4800, 33600, 31200, 3429,
		      2u * 33600u, true, MODEM_PCM_ROLE_NONE, false,
		      UNITY_GAIN_Q15, RETURNED_ECHO_DC_BIASED_G711,
		      UPSTREAM_CHANNEL_CLEAN),
	      "quantized PCMA host echo lost training or sustained data");
	return true;
}

static bool test_full_retrain_pairs(void)
{
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
	return true;
}

static bool test_retrain_tone_dropout(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		CHECK(run_pair_with_rate_window(
			      law, 28000, 14400, 14400, 14400, 3000,
			      SUSTAINED_RECEIVED_BITS, true, MODEM_PCM_ROLE_ANALOG,
			      false, UNITY_GAIN_Q15, RETURNED_ECHO_NONE,
			      UPSTREAM_CHANNEL_RETRAIN_DROPOUT),
		      "law %u did not recover from retrain Tone A loss", (unsigned int)law);
	return true;
}

static bool test_host_echo_retrain_pairs(void)
{
	CHECK(run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      56000, 31200, 3429, true,
		      MODEM_PCM_ROLE_ANALOG, false, true) &&
	      run_pair_with_input_contract(
		      G711_LAW_PCMU,
		      56000, 31200, 3429, true,
		      MODEM_PCM_ROLE_DIGITAL, false, true),
	      "V.90 retrain did not recover through returned host echo");
	return true;
}

static bool test_sustained_maximum_rate_retrains(void)
{
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
	return true;
}

static bool test_training_deadline(void)
{
	struct bit_endpoint bits = { 0 };
	struct modem modem = endpoint_modem(MODEM_PCM_ROLE_DIGITAL, &bits);
	const struct dp_operations *operations;
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
			      receive_octets, receive_samples, NULL,
			      transmit_octets, transmit_samples,
			      MODEM_FRAG, &exact) == DPSTAT_OK && exact,
		      "V.90 training expired before its inclusive deadline");
	}
	{
		bool exact = true;

		CHECK(operations->process_g711(
			      datapump, G711_LAW_PCMU,
			      receive_octets, receive_samples, NULL,
			      transmit_octets, transmit_samples,
			      1, &exact) == DPSTAT_ERROR && !exact &&
		      transmit_octets[0] == silence && transmit_samples[0] == 0,
		      "V.90 training did not fail closed after its deadline");
		exact = true;
		CHECK(operations->process_g711(
			      datapump, G711_LAW_PCMU,
			      receive_octets, receive_samples, NULL,
			      transmit_octets, transmit_samples,
			      1, &exact) == DPSTAT_ERROR && !exact,
		      "V.90 training failure was not latched");
	}
	CHECK(operations->delete(datapump) == 0,
	      "failed V.90 training retained ownership");
	return true;
}

static bool test_v92_pcm_upstream_selection(void)
{
	struct bit_endpoint analog_bits = { 0 };
	struct bit_endpoint digital_bits = { 0 };
	struct modem analog = endpoint_modem(
		MODEM_PCM_ROLE_ANALOG, &analog_bits);
	struct modem digital = endpoint_modem(
		MODEM_PCM_ROLE_DIGITAL, &digital_bits);
	const struct dp_operations *operations;
	struct dp *analog_dp;
	struct dp *digital_dp;
	uint8_t analog_to_digital = g711_encode_pcmu(0);
	uint8_t digital_to_analog = g711_encode_pcmu(0);

	CHECK(modem_dp_get_registered_ops(DP_V90) != NULL &&
	      modem_dp_get_registered_ops(DP_V90) ==
		      modem_dp_get_registered_ops(DP_V92),
	      "V.90 and V.92 did not share one operation owner");
	operations = modem_dp_get_registered_ops(DP_V92);
	analog.min_rate = analog.max_rate = 28000;
	digital.min_rate = digital.max_rate = 28000;
	CHECK(operations != NULL, "missing V.92 operations");
	analog_dp = operations->create(
		&analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && operations->delete(analog_dp) == 0,
	      "V.92 rejected explicit conventional upstream");
	analog.sregs[SREG_V92_PCM_UPSTREAM] = 1;
	digital.sregs[SREG_V92_PCM_UPSTREAM] = 1;
	analog.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 4;
	digital.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 4;
	analog.sregs[SREG_V92_QUICK_CONNECT] = 1;
	CHECK(operations->create(
		      &analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG,
		      operations) == NULL,
	      "V.92 accepted quick connect without a V.8 handoff");
	analog.sregs[SREG_V92_QUICK_CONNECT] = 0;
	analog_dp = operations->create(
		&analog, DP_V92, 1, MODEM_RATE, MODEM_FRAG, operations);
	digital_dp = operations->create(
		&digital, DP_V92, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(analog_dp != NULL && digital_dp != NULL &&
	      analog_dp->id == DP_V92 && digital_dp->id == DP_V92,
	      "V.92 PCM-upstream peers did not share the PCM call owner");
	CHECK(!v90_datapump_start_rate_renegotiation(analog_dp, true) &&
	      !v90_datapump_start_rate_renegotiation(digital_dp, false),
	      "untrained PCM-upstream peers accepted a rate exchange");
	for (size_t index = 0; index < MODEM_FRAG * 2u; ++index) {
		uint8_t next_analog;
		uint8_t next_digital;
		int16_t analog_received = g711_decode_pcmu(digital_to_analog);
		int16_t digital_received = g711_decode_pcmu(analog_to_digital);
		int16_t analog_transmitted;
		int16_t digital_transmitted;
		bool analog_exact = false;
		bool digital_exact = false;
		int digital_result;
		int analog_result;

		digital_result = operations->process_g711(
			      digital_dp, G711_LAW_PCMU,
			      &analog_to_digital, &digital_received, NULL,
			      &next_digital, &digital_transmitted, 1,
			      &digital_exact);
		analog_result = operations->process_g711(
			      analog_dp, G711_LAW_PCMU,
			      &digital_to_analog, &analog_received, NULL,
			      &next_analog, &analog_transmitted, 1,
			      &analog_exact);
		CHECK(digital_result == DPSTAT_OK && digital_exact &&
		      analog_result == DPSTAT_OK && analog_exact,
		      "V.92 PCM-upstream selection stopped at sample %zu", index);
		analog_to_digital = next_analog;
		digital_to_analog = next_digital;
	}
	CHECK(operations->delete(analog_dp) == 0 &&
	      operations->delete(digital_dp) == 0,
	      "V.92 PCM-upstream selection retained shared ownership");
	return true;
}

static bool test_v92_pcm_host_data(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		/* PCM upstream receives the physical sample, not an accidentally
		 * parallel pristine octet. Cover sustained twelve-symbol frames. */
		CHECK(run_pcm_pair(DP_V92, 14, law, 41333, 0, 0, 41333, 0,
			100000, true, MODEM_PCM_ROLE_NONE, true, UNITY_GAIN_Q15,
			RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
		      "law %u V.92 sustained PCM host bits failed", law);
		for (enum modem_pcm_role role = MODEM_PCM_ROLE_ANALOG;
		     role <= MODEM_PCM_ROLE_DIGITAL; ++role) {
			CHECK(run_pcm_pair(DP_V92, 4, law, 28000, 0, 0, 28000, 0,
				MAXIMUM_RATE_RECEIVED_BITS, true, role, false, UNITY_GAIN_Q15,
				RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u V.92 host data did not resume after retrain",
			      law, role);
		}
	}
	return true;
}

static bool test_v92_pcm_host_echo(void)
{
	/* The 700 ms, three-tap returned downstream is independent of the
	 * upstream source. Require exact PCM host bits, not a V.34 fallback or
	 * CONNECT alone. Exercise both fresh training and each retrain origin. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		CHECK(run_pcm_pair(DP_V92, 1, law, 41333, 0, 0, 24000, 0,
			100000, true, MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
			RETURNED_ECHO_STABLE, UPSTREAM_CHANNEL_CLEAN),
		      "law %u V.92 low-rate data failed through returned echo", law);
		for (enum modem_pcm_role role = MODEM_PCM_ROLE_ANALOG;
		     role <= MODEM_PCM_ROLE_DIGITAL; ++role)
			CHECK(run_pcm_pair(DP_V92, 4, law, 41333, 0, 0, 28000, 0,
				100000, true, role, false, UNITY_GAIN_Q15,
				RETURNED_ECHO_STABLE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u V.92 echo retrain lost PCM host data", law, role);
	}
	return true;
}

static bool test_v92_quantized_host_echo(void)
{
	/* Quantize the combined upstream and independent returned echo, then
	 * apply host gain. Re-quantizing the echo-cleaned sample cannot recover
	 * these original wire cells. Keep exact bits and B1 quality unchanged. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		CHECK(run_pcm_pair(DP_V92, 1, law, 41333, 0, 0, 24000, 0,
			100000, true, MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
			RETURNED_ECHO_WEAK_G711, UPSTREAM_CHANNEL_CLEAN),
		      "law %u quantized echo lost PCM host bits", law);
		for (enum modem_pcm_role role = MODEM_PCM_ROLE_ANALOG;
		     role <= MODEM_PCM_ROLE_DIGITAL; ++role)
			CHECK(run_pcm_pair(DP_V92, 4, law, 41333, 0, 0, 28000, 0,
				100000, true, role, false,
				role == MODEM_PCM_ROLE_ANALOG ? 24576 : 40960,
				RETURNED_ECHO_WEAK_G711, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u scaled quantized echo lost retrained PCM data", law, role);
	}
	return true;
}

static bool test_v92_spread_host_echo(void)
{
	/* Ranging and the echo return have different delays. The strongest
	 * correlation can be a later tap: both reacting peers must negotiate
	 * usable PCM parameters and pass arbitrary bits after quantization. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		CHECK(run_pcm_pair(DP_V92, 1, law, 41333, 0, 0, 24000, 0,
			MAXIMUM_RATE_RECEIVED_BITS, true, MODEM_PCM_ROLE_NONE, false, UNITY_GAIN_Q15,
			RETURNED_ECHO_SPREAD_G711, UPSTREAM_CHANNEL_CLEAN),
		      "law %u spread echo lost low-rate PCM data", law);
		for (enum modem_pcm_role role = MODEM_PCM_ROLE_ANALOG;
		     role <= MODEM_PCM_ROLE_DIGITAL; ++role)
			CHECK(run_pcm_pair(DP_V92, 4, law, 41333, 0, 0, 28000, 0,
				MAXIMUM_RATE_RECEIVED_BITS, true, role, false, UNITY_GAIN_Q15,
				RETURNED_ECHO_SPREAD_G711, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u spread echo lost retrained PCM data", law, role);
	}
	return true;
}

static bool test_v92_conventional_host_data(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum modem_pcm_role role = MODEM_PCM_ROLE_ANALOG;
		     role <= MODEM_PCM_ROLE_DIGITAL; ++role)
			CHECK(run_pcm_pair(DP_V92, 0, law, 28000, 14400, 14400, 14400, 3000,
				MAXIMUM_RATE_RECEIVED_BITS, true, role, true, UNITY_GAIN_Q15,
				RETURNED_ECHO_NONE, UPSTREAM_CHANNEL_CLEAN),
			      "law %u role %u conventional V.92 data/retrain/cleardown failed",
			      law, role);
	return true;
}

static bool test_v92_upstream_quality_recovery(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		/* A qualified upstream data failure lowers the automatic selection
		 * by one rate; the downstream selection and configured cap stay put. */
		CHECK(run_pcm_pair(DP_V92, 14, law, 41333, 0, 0, 40000, 0,
			SUSTAINED_RECEIVED_BITS, true, MODEM_PCM_ROLE_NONE,
			false, UNITY_GAIN_Q15, RETURNED_ECHO_NONE,
			UPSTREAM_CHANNEL_NOISE_BURST),
		      "law %u V.92 did not recover from sustained upstream noise", law);
	}
	return true;
}

static bool test_validation(void)
{
	struct bit_endpoint bits = { 0 };
	struct modem modem = endpoint_modem(MODEM_PCM_ROLE_ANALOG, &bits);
	const struct dp_operations *operations;
	struct dp *datapump;
	uint8_t receive_octet = g711_encode_pcmu(0);
	uint8_t transmit_octet = 0xa5;
	int16_t receive_sample = g711_decode_pcmu(receive_octet);
	int16_t transmit_sample = INT16_MAX;
	bool exact = true;

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
	CHECK(!v90_datapump_start_rate_renegotiation(NULL, true) &&
	      !v90_datapump_start_rate_renegotiation(datapump, false),
	      "invalid or unstarted datapump accepted a rate exchange");
	CHECK(datapump != NULL && operations->process(
		      datapump, &receive_sample, &transmit_sample, 1) ==
		      DPSTAT_ERROR && transmit_sample == 0,
	      "V.90 accepted lossy linear-PCM processing");
	CHECK(operations->process_g711(
		      datapump, (enum g711_law)-1,
		      &receive_octet, &receive_sample, NULL,
		      &transmit_octet, &transmit_sample, 1, &exact) ==
		      DPSTAT_ERROR && !exact && transmit_sample == 0 &&
	      operations->delete(datapump) == 0,
	      "V.90 invalid media input changed ownership or exactness");
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

static bool fallback_pcm_source(void *context, uint8_t *bits, size_t bit_count)
{
	memset(bits, 1, bit_count);
	return source_bits(context, 1, bits, (int)bit_count) >= 0;
}

static bool check_fallback_phase3_retrain(
	struct modem *modem, enum g711_law law, bool retraining)
{
	const struct dp_operations *operations = v34_datapump_operations(DP_V34);
	struct dp *dp = operations->create(
		modem, DP_V34, 0, MODEM_RATE, MODEM_FRAG, operations);
	struct v34_phase2_tone_transmitter tone;
	int16_t input[MODEM_FRAG];
	int16_t output[MODEM_FRAG];
	int status = DPSTAT_OK;

	CHECK(dp != NULL &&
	      v34_phase2_tone_transmitter_init(
		      &tone, V34_ANSWERING_STATION, 6000) == V34_PHASE2_TONE_OK,
	      "fallback retrain probe did not initialize");
	/* Repeat the shared request before either direction reaches data: 70 ms
	 * quiet followed by guarded Tone A. Initial Phase 3 must stay unarmed. */
	for (unsigned int clock = 0; clock < 2400u; clock += MODEM_FRAG) {
		for (unsigned int i = 0; i < MODEM_FRAG; ++i) {
			int16_t sample = 0;

			if (clock + i >= V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES)
				CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
				      V34_PHASE2_TONE_OK, "fallback peer tone failed");
			input[i] = decode(law, encode(law, sample));
		}
		status = dp->op->process(dp, input, output, MODEM_FRAG);
		if (status == DPSTAT_CHANGEDP)
			break;
		CHECK(status == DPSTAT_OK, "fallback Phase 3 probe failed");
	}
	CHECK((status == DPSTAT_CHANGEDP) == retraining,
	      "fallback Phase 3 lost its retrain context (active=%d status=%d)",
	      retraining, status);
	if (retraining) {
		struct pcm_call *call = malloc(sizeof(*call));
		struct training_session training;

		CHECK(call != NULL &&
		      v34_datapump_pcm_retrain_handoff(dp, call, &training) &&
		      call->retrain_active && call->stage == PCM_CALL_RETRAIN_SILENCE &&
		      call->retrain.peer_tone_retained &&
		      call->retrain.response_silence_samples_remaining > 0 &&
		      call->retrain.response_silence_samples_remaining <=
			      V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES,
		      "fallback repeated request lost its PCM response epoch");
		free(call);
	}
	CHECK(dp->op->delete(dp) == 0, "fallback probe owner was not released");
	modem->dp_requested = DP_V34;
	return true;
}

static bool run_fallback_restart(
	enum g711_law law, bool remote, enum DP_ID protocol, uint8_t pcm_up_max_drn,
	bool after_online)
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
	unsigned int expected_phases = after_online ? 3u : 2u;
	int16_t peer_gain = 0;

	digital.min_rate = 28000;
	digital.max_rate = 56000;
	digital.v90_upstream_min_rate = 4800;
	digital.v90_upstream_max_rate = 28800;
	digital.v90_upstream_max_baud = 3000;
	digital.sregs[SREG_V92_PCM_UPSTREAM] = pcm_up_max_drn != 0;
	digital.sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = pcm_up_max_drn;
	if (protocol == DP_V92) {
		CHECK(v92_profile_init(&configuration, V90_ANALOG_MODEM, law,
				       28000, 56000, pcm_up_max_drn, &upstream) == V92_PROFILE_OK,
		      "V.92 fallback peer profile did not initialize");
		configuration.phase3.v92_analog.restore_codec_grid = true;
		configuration.local_info0.v92_analog.v90.symbol_rate_2800_supported = true;
	} else {
		CHECK(v90_profile_init(&configuration, V90_ANALOG_MODEM, law,
				       28000, 56000, &upstream) == V90_PROFILE_OK,
		      "V.90 fallback peer profile did not initialize");
		configuration.local_info0.analog.symbol_rate_2800_supported = true;
	}
	CHECK(pcm_call_init(&peer, &configuration) == PCM_CALL_OK &&
	      dc_remover_init(&frontend) == DC_REMOVER_OK,
	      "fallback peer did not initialize");
	digital.dp = modem_dp_get_registered_ops(protocol)->create(
		&digital, protocol, 0, MODEM_RATE, MODEM_FRAG,
		modem_dp_get_registered_ops(protocol));
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
		if (dp->id != DP_V34)
			result = dp->op->process_g711(dp, law, to_digital, input, NULL,
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
				struct pcm_v34_fallback_handoff handoff;
				struct v34_call_configuration v34_configuration;
				struct v34_info0 digital_info0;
				uint16_t mask;

				CHECK(digital.dp_requested == DP_V34 &&
				      v90_datapump_v34_fallback_handoff(dp, &handoff) &&
				      training_session_is_retraining(&handoff.training) == after_online &&
				      (!after_online || handoff.training.elapsed_ticks != 0) &&
				      handoff.restart.protocol == configuration.protocol &&
				      (protocol != DP_V92 ||
				       handoff.restart.local_info1.v92_digital.pcm_upstream_supported ==
					       (pcm_up_max_drn != 0)),
				      "PCM did not export its fallback agreement");
				CHECK(check_fallback_phase3_retrain(&digital, law, after_online),
				      "fallback Phase 3 repeated request failed");
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
				CHECK(retrain && digital.dp_requested == protocol,
				      "fallback retrain did not return to the agreed PCM protocol");
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
					      "fallback peer could not resume PCM");
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
						.pcm_upstream = fallback_pcm_source, .pcm_upstream_context = &analog,
					};
					struct pcm_call_data_event event;

					CHECK(pcm_call_data_octet(&peer, to_analog[index], sample, true,
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
					if (!substituted && (!after_online || phases != 0u) &&
					    observation.phase2.kind ==
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
		if (digital.dp->status == DPSTAT_CONNECT &&
		    ((!fallback || returned) ? pcm_call_is_online(&peer) :
					       v34_call_is_online(&conventional))) {
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
			CHECK(digital.dp->id == (fallback && !returned ? DP_V34 : protocol),
			      "fallback data used the wrong protocol in phase %u", phases);
			if (!fallback || returned)
				CHECK(digital.tx_rate > 33600 && digital.dsp_info.upstream_mode ==
					      (pcm_up_max_drn != 0 ? MODEM_UPSTREAM_PCM : MODEM_UPSTREAM_V34),
				      "fallback PCM data lost the upstream mode in phase %u", phases);
			if (++phases == expected_phases)
				break;
			digital_bits = (struct bit_endpoint){
				.transmit_seed = (uint8_t)(31u + phases),
				.receive_seed = (uint8_t)(43u + phases),
			};
			analog_bits = (struct bit_endpoint){
				.transmit_seed = digital_bits.receive_seed,
				.receive_seed = digital_bits.transmit_seed,
			};
			warmup = 0;
			retrain = true;
			CHECK(remote ? (fallback ?
					v34_call_start_retrain(&conventional) == V34_CALL_OK :
					pcm_call_start_retrain(&peer) == PCM_CALL_OK) :
				       digital.dp->op->retrain(digital.dp) == 0,
			      "fallback ATO1 did not start");
			CHECK(remote || digital.dp->status == DPSTAT_OK,
			      "fallback retrain retained online datapump status");
		}
	}
	CHECK(phases == expected_phases && returned && peer_returned &&
	      digital.dp->id == protocol && digital.tx_rate > 33600 &&
	      digital.dsp_info.upstream_mode ==
		      (pcm_up_max_drn != 0 ? MODEM_UPSTREAM_PCM : MODEM_UPSTREAM_V34),
	      "fallback did not restore PCM data (phases=%u returned=%d/%d)",
	      phases, returned, peer_returned);
	CHECK(digital.dp->op->delete(digital.dp) == 0,
	      "fallback final owner was not released");
	return true;
}

static bool run_fallback_restarts(bool after_online)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int mode = 0; mode < 3u; ++mode) {
			enum DP_ID protocol = mode == 0 ? DP_V90 : DP_V92;
			uint8_t pcm_up_max_drn = mode == 2u ? 4u : 0;

			CHECK(run_fallback_restart(law, false, protocol, pcm_up_max_drn, after_online) &&
			      run_fallback_restart(law, true, protocol, pcm_up_max_drn, after_online),
			      "fallback retrain failed for law %u mode %u online %u",
			      law, mode, after_online);
		}
	return true;
}

static bool test_fallback_restart(void)
{
	return run_fallback_restarts(false);
}

static bool test_online_fallback_restart(void)
{
	/* An established PCM call can select V.34 in a later INFO1 exchange.
	 * Require data in all three epochs: PCM, V.34, and PCM again, with the
	 * retraining origin retained when the datapump owner changes. */
	return run_fallback_restarts(true);
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "V.92 Quick Connect adapter data", test_qc_adapter },
		{ "V.92 Quick Connect fallback deadline", test_qc_adapter_timeout },
		{ "PCM retrain after V.34 fallback", test_fallback_restart },
		{ "online PCM retrain through V.34 fallback", test_online_fallback_restart },
		{ "cleardown before CONNECT", test_training_cleardown },
		{ "automatic upstream quality recovery", test_upstream_quality_recovery },
		{ "automatic upstream quality downshift", test_upstream_quality_downshift },
		{ "long-delay upstream channel",
		  test_delayed_upstream_channel },
		{ "attenuated maximum-rate upstream",
		  test_attenuated_upstream },
		{ "V.92 PCM-upstream selection",
		  test_v92_pcm_upstream_selection },
		{ "V.92 PCM-upstream data and retrain through host echo",
		  test_v92_pcm_host_echo },
		{ "V.92 PCM-upstream data and retrain through quantized host echo",
		  test_v92_quantized_host_echo },
		{ "V.92 PCM-upstream data and retrain through spread host echo",
		  test_v92_spread_host_echo },
		{ "V.92 PCM host data and retrain", test_v92_pcm_host_data },
		{ "V.92 conventional host data and retrain", test_v92_conventional_host_data },
		{ "V.92 automatic upstream quality recovery", test_v92_upstream_quality_recovery },
		{ "forced upstream pairs", test_forced_upstream_pairs },
		{ "sustained maximum-rate pairs",
		  test_sustained_maximum_rate_pairs },
		{ "sample-owned PCMA upstream", test_upstream_uses_media_samples },
		{ "delayed digital-answerer host echo", test_delayed_host_echo },
		{ "DC-biased digital-answerer host echo",
		  test_dc_biased_host_echo },
		{ "quantized DC-biased digital-answerer host echo",
		  test_quantized_host_echo },
		{ "local and remote full retrain", test_full_retrain_pairs },
		{ "retrain response with tone loss", test_retrain_tone_dropout },
		{ "sustained maximum-rate retrains",
		  test_sustained_maximum_rate_retrains },
		{ "full retrain through returned host echo",
		  test_host_echo_retrain_pairs },
		{ "generated PCMU and PCMA data-pump pairs",
		  test_generated_pairs },
		{ "training deadline", test_training_deadline },
		{ "factory and media validation", test_validation },
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
