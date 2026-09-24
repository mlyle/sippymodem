// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <fsk.h>
#include <fsk_online_source.h>
#include <v21.h>
#include <v23_datapump.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	FRAME_SAMPLES = MODEM_FRAG,
	PAIR_FRAME_LIMIT = 900,
	SHORT_GAP_FRAMES = 120,
	LOSS_FRAME_LIMIT = 150,
};

struct v23_pair {
	struct modem originator_modem;
	struct modem answerer_modem;
	struct dp *originator;
	struct dp *answerer;
	struct dp_operations *operations;
	int16_t answerer_to_originator[FRAME_SAMPLES];
	int16_t originator_output[FRAME_SAMPLES];
	int16_t answerer_output[FRAME_SAMPLES];
};

struct received_bits {
	unsigned int calls;
	unsigned int bits;
};

static int capture_received_bits(
	struct modem *modem, int nbits, u8 *bits, int count)
{
	struct received_bits *received = modem->dev_data;

	if (received == NULL || bits == NULL || nbits != 1 || count <= 0)
		return 0;
	received->calls++;
	received->bits += (unsigned int)count;
	return count;
}

static void initialize_modem(struct modem *modem)
{
	memset(modem, 0, sizeof(*modem));
	modem->format = MODEM_FORMAT;
	modem->srate = MODEM_RATE;
	modem->frag = MODEM_FRAG;
	modem->sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 10;
	modem->sregs[SREG_CARRIER_DETECT_RESPONSE_TIME] = 6;
	modem->sregs[SREG_CARRIER_LOSS_DISCONNECT_TIME] = 7;
}

static int online_bits(struct modem *modem, int nbits, u8 *bits, int count)
{
	unsigned int *requests = modem->dev_data;

	if (nbits != 1 || count != 1)
		return 0;
	++*requests;
	*bits = 0;
	return *requests == 1 ? 1 : 0;
}

static bool test_online_source_symbol_handoff(void)
{
	struct fsk_transmitter transmitter;
	struct fsk_online_source source = { 0 };
	struct modem modem;
	unsigned int requests = 0;
	unsigned int samples;
	int16_t sample;
	bool consumed;

	initialize_modem(&modem);
	modem.dev_data = &requests;
	modem.get_bits = online_bits;
	CHECK(fsk_transmitter_init(
		      &transmitter, &v21_channel_1, 8192) == FSK_OK,
	      "online FSK transmitter did not initialize");
	CHECK(fsk_transmitter_sample(
		      &transmitter, 1, &sample, &consumed) == FSK_OK &&
		      !consumed, "training mark ended unexpectedly");
	for (samples = 0; transmitter.holding_bit && samples < 32; ++samples) {
		CHECK(fsk_online_source_sample(
			      &source, &transmitter, &modem, &sample) == FSK_OK &&
			      requests == 0,
		      "host bit displaced a training mark");
	}
	CHECK(!transmitter.holding_bit,
	      "training mark never completed");
	CHECK(fsk_online_source_sample(
		      &source, &transmitter, &modem, &sample) == FSK_OK &&
		      requests == 1 && source.pending &&
		      transmitter.held_bit == 0,
	      "first host bit was not latched at the next symbol");
	for (samples = 0; source.pending && samples < 32; ++samples) {
		CHECK(fsk_online_source_sample(
			      &source, &transmitter, &modem, &sample) == FSK_OK &&
			      requests == 1,
		      "online source fetched a host bit mid-symbol");
	}
	CHECK(!source.pending, "online source never completed the host symbol");
	CHECK(fsk_online_source_sample(
		      &source, &transmitter, &modem, &sample) == FSK_OK &&
		      requests == 2 && transmitter.held_bit == 1,
	      "empty host source did not fall back to mark");
	return true;
}

static bool pair_init(struct v23_pair *pair)
{
	memset(pair, 0, sizeof(*pair));
	initialize_modem(&pair->originator_modem);
	initialize_modem(&pair->answerer_modem);
	CHECK(v23_datapump_register() == 0,
	      "V.23 data-pump registration failed");
	pair->operations = modem_dp_get_registered_ops(DP_V23);
	CHECK(pair->operations != NULL && pair->operations->create != NULL &&
		      pair->operations->delete != NULL &&
		      pair->operations->process != NULL,
	      "V.23 operations were incomplete");
	pair->originator = pair->operations->create(
		&pair->originator_modem, DP_V23, 1, MODEM_RATE, MODEM_FRAG,
		pair->operations);
	pair->answerer = pair->operations->create(
		&pair->answerer_modem, DP_V23, 0, MODEM_RATE, MODEM_FRAG,
		pair->operations);
	CHECK(pair->originator != NULL && pair->answerer != NULL,
	      "V.23 role pair did not initialize");
	return true;
}

static void pair_destroy(struct v23_pair *pair)
{
	if (pair->answerer != NULL)
		(void)pair->operations->delete(pair->answerer);
	if (pair->originator != NULL)
		(void)pair->operations->delete(pair->originator);
	pair->answerer = NULL;
	pair->originator = NULL;
}

static bool process_pair(
	struct v23_pair *pair, bool mute_originator_receive,
	int *originator_status, int *answerer_status)
{
	int16_t silence[FRAME_SAMPLES] = { 0 };
	int16_t *originator_input = mute_originator_receive ?
		silence : pair->answerer_to_originator;

	*originator_status = pair->operations->process(
		pair->originator, originator_input,
		pair->originator_output, FRAME_SAMPLES);
	*answerer_status = pair->operations->process(
		pair->answerer, pair->originator_output,
		pair->answerer_output, FRAME_SAMPLES);
	memcpy(
		pair->answerer_to_originator, pair->answerer_output,
		sizeof(pair->answerer_to_originator));
	return (*originator_status == DPSTAT_OK ||
		*originator_status == DPSTAT_CONNECT) &&
	       (*answerer_status == DPSTAT_OK ||
		*answerer_status == DPSTAT_CONNECT);
}

static uint64_t sample_energy(const int16_t *samples)
{
	uint64_t energy = 0;

	for (size_t index = 0; index < FRAME_SAMPLES; ++index) {
		int32_t sample = samples[index];

		energy += (uint64_t)(sample * sample);
	}
	return energy;
}

static bool test_training_roles_and_carrier_loss(void)
{
	struct v23_pair pair;
	struct received_bits received = { 0 };
	unsigned int originator_connect_frame = UINT_MAX;
	unsigned int answerer_connect_frame = UINT_MAX;
	int originator_status = DPSTAT_OK;
	int answerer_status = DPSTAT_OK;
	unsigned int loss_frame;

	CHECK(pair_init(&pair), "V.23 pair fixture failed");
	for (unsigned int frame = 0; frame < PAIR_FRAME_LIMIT; ++frame) {
		CHECK(process_pair(
		      &pair, false, &originator_status, &answerer_status),
		      "V.23 training failed at frame %u", frame);
		if (frame == 0)
			CHECK(sample_energy(pair.answerer_output) != 0,
			      "V.23 answerer did not start its answer tone");
		if (frame == 600)
			CHECK(sample_energy(pair.answerer_output) == 0,
			      "V.23 answerer omitted its post-tone silence");
		if (frame == 610)
			CHECK(sample_energy(pair.answerer_output) != 0,
			      "V.23 answerer did not start its forward carrier");
		if (originator_status == DPSTAT_CONNECT &&
		    originator_connect_frame == UINT_MAX)
			originator_connect_frame = frame;
		if (answerer_status == DPSTAT_CONNECT &&
		    answerer_connect_frame == UINT_MAX)
			answerer_connect_frame = frame;
		if (originator_connect_frame != UINT_MAX &&
		    answerer_connect_frame != UINT_MAX)
			break;
	}
	CHECK(originator_connect_frame >= 720 &&
		      originator_connect_frame <= 760 &&
		      answerer_connect_frame >= 640 &&
		      answerer_connect_frame <= 680,
	      "V.23 roles escaped their bounded carrier gates: %u/%u",
	      originator_connect_frame, answerer_connect_frame);
	CHECK(pair.originator_modem.tx_rate ==
		      V23_BACKWARD_BIT_RATE &&
		      pair.originator_modem.rx_rate ==
			      V23_FORWARD_BIT_RATE &&
		      pair.answerer_modem.tx_rate ==
			      V23_FORWARD_BIT_RATE &&
		      pair.answerer_modem.rx_rate ==
			      V23_BACKWARD_BIT_RATE,
	      "V.23 roles reported the wrong asymmetric rates");
	CHECK(v23_datapump_unregister() != 0,
	      "active V.23 pair did not protect its registration");
	pair.originator_modem.dev_data = &received;
	pair.originator_modem.put_bits = capture_received_bits;
	CHECK(process_pair(
		      &pair, false, &originator_status, &answerer_status),
	      "V.23 online data frame failed");
	received = (struct received_bits){ 0 };
	CHECK(process_pair(
		      &pair, false, &originator_status, &answerer_status) &&
		      received.calls == 1 && received.bits > 1,
	      "V.23 online bits were not delivered as one frame");

	for (unsigned int frame = 0; frame < SHORT_GAP_FRAMES; ++frame) {
		CHECK(process_pair(
		      &pair, true, &originator_status, &answerer_status),
		      "V.23 lost a short carrier gap at frame %u", frame);
		if (frame == 4)
			received = (struct received_bits){ 0 };
	}
	CHECK(received.bits == 0,
	      "V.23 delivered bits from a silent receive line");
	for (unsigned int frame = 0; frame < 4; ++frame)
		CHECK(process_pair(
		      &pair, false, &originator_status, &answerer_status),
		      "V.23 did not recover its carrier at frame %u", frame);
	for (loss_frame = 0; loss_frame < LOSS_FRAME_LIMIT; ++loss_frame) {
		(void)process_pair(
			&pair, true, &originator_status, &answerer_status);
		if (originator_status == DPSTAT_ERROR)
			break;
	}
	CHECK(originator_status == DPSTAT_ERROR && loss_frame >= 135 &&
		      loss_frame < LOSS_FRAME_LIMIT,
	      "V.23 sustained carrier loss was not debounced: frame %u",
	      loss_frame);
	pair_destroy(&pair);
	CHECK(v23_datapump_unregister() == 0 &&
		      modem_dp_get_registered_ops(DP_V23) == NULL,
	      "V.23 registration did not release cleanly");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] online FSK symbol handoff\n");
	if (!test_online_source_symbol_handoff()) {
		fprintf(stderr, "[  FAILED  ] online FSK symbol handoff\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] online FSK symbol handoff\n");
	fprintf(stderr, "[ RUN      ] V.23 training roles and carrier loss\n");
	if (!test_training_roles_and_carrier_loss()) {
		fprintf(stderr, "[  FAILED  ] V.23 training roles and carrier loss\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.23 training roles and carrier loss\n");
	return 0;
}
