// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <qam.h>
#include <qam_gain.h>
#include <single_tone.h>
#include <conditioning.h>
#include <conditioning_receiver.h>
#include <constellation.h>
#include <datapump.h>
#include <framing.h>
#include <line_startup.h>
#include <rate_signal.h>

#include "test.h"

#include <stdint.h>

enum {
	TEST_TONE_AMPLITUDE = 12000,
	TEST_TURNAROUND_PRIMARY_AMPLITUDE = 9000,
	TEST_TURNAROUND_SECONDARY_AMPLITUDE = 6000,
	TEST_INITIAL_SILENCE_SAMPLES = 400,
	TEST_ANSWER_TONE_SAMPLES =
		V32_ANSWER_GUARD_SAMPLES + 500,
	TEST_TURNAROUND_SETTLE_SAMPLES = 400,
	TEST_REVERSAL_SAMPLES = 800,
	TEST_SILENCE_RUN = 24,
	TEST_RATE_WORDS_BEFORE_RESPONSE = 2,
	TEST_SECOND_CONDITIONING_TAIL_SAMPLES = 1200,
	TEST_SECOND_CONDITIONING_TAIL_AMPLITUDE = 200,
	TEST_SECOND_CONDITIONING_SILENCE = 8000,
	TEST_QAM_SAMPLE_LIMIT = 14000,
};

static const struct qam_channel test_v32_channel = {
	.sample_rate = MODEM_RATE,
	.symbol_rate_numerator = V32_STARTUP_SYMBOL_RATE,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = V32_CALLING_CARRIER_HZ,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = 6,
};

struct test_turnaround_signal {
	struct single_tone_transmitter primary;
	struct single_tone_transmitter secondary;
};

struct test_peer_qam {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter conditioning;
	struct v32_symbol_encoder encoder;
	struct qam_point point;
	uint16_t rate_word;
	uint16_t end_word;
	unsigned int rate_dibit;
	unsigned int rate_symbols;
	unsigned int end_dibit;
	bool rate_started;
	bool selection;
	bool end_requested;
};

struct test_caller_qam_receiver {
	struct qam_demodulator demodulator;
	struct v32_conditioning_receiver conditioning;
	struct v32_symbol_decoder decoder;
	struct v32_rate_signal_detector rate_detector;
	struct qam_gain gain;
	struct qam_point previous_point;
	bool conditioning_complete;
	bool rate_detected;
	bool end_detected;
};

static int unused_start(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_stop(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_control(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	(void)modem;
	(void)command;
	(void)argument;
	return 0;
}

static bool process_sample(
	struct dp_operations *operations, struct dp *datapump, int16_t receive,
	int16_t *transmit)
{
	return operations->process(datapump, &receive, transmit, 1) == DPSTAT_OK;
}

static bool process_sample_until_link(
	struct dp_operations *operations, struct dp *datapump, int16_t receive,
	int16_t *transmit, bool *connected)
{
	int status = operations->process(datapump, &receive, transmit, 1);

	*connected = status == DPSTAT_CONNECT;
	return status == DPSTAT_OK || *connected;
}

static bool turnaround_signal_init(struct test_turnaround_signal *signal)
{
	return single_tone_transmitter_init(
		       &signal->primary,
		       V32_LOW_TURNAROUND_TONE_HZ,
		       TEST_TURNAROUND_PRIMARY_AMPLITUDE) ==
		       SINGLE_TONE_OK &&
	       single_tone_transmitter_init(
		       &signal->secondary,
		       V32_HIGH_TURNAROUND_TONE_HZ,
		       TEST_TURNAROUND_SECONDARY_AMPLITUDE) ==
		       SINGLE_TONE_OK;
}

static bool turnaround_signal_sample(
	struct test_turnaround_signal *signal, int16_t *sample)
{
	int16_t primary;
	int16_t secondary;

	if (single_tone_transmitter_sample(
		    &signal->primary, &primary) != SINGLE_TONE_OK ||
	    single_tone_transmitter_sample(
		    &signal->secondary, &secondary) !=
		    SINGLE_TONE_OK)
		return false;
	*sample = (int16_t)((int32_t)primary + secondary);
	return true;
}

static bool turnaround_signal_reverse(struct test_turnaround_signal *signal)
{
	return single_tone_transmitter_reverse(&signal->primary) ==
		       SINGLE_TONE_OK &&
	       single_tone_transmitter_reverse(&signal->secondary) ==
		       SINGLE_TONE_OK;
}

static bool drive_line_startup(
	struct dp_operations *operations, struct dp *datapump)
{
	struct single_tone_transmitter answer_tone;
	struct test_turnaround_signal turnaround;
	uint64_t calling_power = 0;
	unsigned int silent_samples = 0;
	int16_t transmit;

	CHECK(single_tone_transmitter_init(
		      &answer_tone, V32_ANSWER_TONE_HZ,
		      TEST_TONE_AMPLITUDE) == SINGLE_TONE_OK &&
		      turnaround_signal_init(&turnaround),
	      "V.32 line fixture did not initialize");
	for (unsigned int count = 0; count < TEST_INITIAL_SILENCE_SAMPLES;
	     ++count)
		CHECK(process_sample(operations, datapump, 0, &transmit) &&
			      transmit == 0,
		      "V.32 caller left silence before the answer tone");
	for (unsigned int count = 0; count < TEST_ANSWER_TONE_SAMPLES; ++count) {
		int16_t receive;

		CHECK(single_tone_transmitter_sample(
			      &answer_tone, &receive) ==
			      SINGLE_TONE_OK &&
			      process_sample(
				      operations, datapump, receive, &transmit),
		      "V.32 caller rejected the answer tone");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	for (unsigned int count = 0; count < TEST_TURNAROUND_SETTLE_SAMPLES;
	     ++count) {
		int16_t receive;

		CHECK(turnaround_signal_sample(&turnaround, &receive) &&
			      process_sample(
				      operations, datapump, receive, &transmit),
		      "V.32 caller rejected the initial turnaround signal");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(calling_power > UINT64_C(1000000000),
	      "V.32 caller omitted its response carrier");
	CHECK(turnaround_signal_reverse(&turnaround),
	      "V.32 turnaround signal did not reverse");
	for (unsigned int count = 0; count < TEST_REVERSAL_SAMPLES; ++count) {
		int16_t receive;

		CHECK(turnaround_signal_sample(&turnaround, &receive) &&
			      process_sample(
				      operations, datapump, receive, &transmit),
		      "V.32 caller rejected the first turnaround reversal");
	}
	CHECK(turnaround_signal_reverse(&turnaround),
	      "V.32 turnaround signal did not reverse a second time");
	for (unsigned int count = 0;
	     count < TEST_REVERSAL_SAMPLES &&
	     silent_samples < TEST_SILENCE_RUN;
	     ++count) {
		int16_t receive;

		CHECK(turnaround_signal_sample(&turnaround, &receive) &&
			      process_sample(
				      operations, datapump, receive, &transmit),
		      "V.32 caller rejected the second turnaround reversal");
		silent_samples = transmit == 0 ? silent_samples + 1 : 0;
	}
	CHECK(silent_samples == TEST_SILENCE_RUN,
	      "V.32 caller did not become silent for peer conditioning");
	for (unsigned int count = 0;
	     count < V32_ANSWERER_DROP_SILENCE_SAMPLES; ++count)
		CHECK(process_sample(operations, datapump, 0, &transmit) &&
			      transmit == 0,
		      "V.32 caller rejected the post-turnaround gap");
	return true;
}

static bool peer_qam_init(struct test_peer_qam *peer)
{
	const struct v32_rate_offer offer = {
		.rates = V32_RATE_CAPABILITY_4800,
		.trellis_at_highest_rate = false,
	};

	*peer = (struct test_peer_qam){ 0 };
	return qam_modulator_init(
		       &peer->modulator, &test_v32_channel,
		       TEST_TONE_AMPLITUDE) == QAM_OK &&
	       v32_conditioning_transmitter_init(
		       &peer->conditioning,
		       V32_ANSWERING_TRANSMITTER,
		       V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		       V32_CONDITIONING_OK &&
	       v32_rate_signal_encode(
		       V32_RATE_SIGNAL, &offer, &peer->rate_word) ==
		       V32_RATE_SIGNAL_OK &&
	       v32_rate_signal_encode(
		       V32_END_SIGNAL, &offer, &peer->end_word) ==
		       V32_RATE_SIGNAL_OK;
}

static bool peer_next_quadrant(
	struct test_peer_qam *peer, uint8_t *quadrant)
{
	uint8_t dibit;

	if (!v32_conditioning_transmitter_is_complete(
		    &peer->conditioning))
		return v32_conditioning_transmitter_next(
			       &peer->conditioning, quadrant) ==
		       V32_CONDITIONING_OK;
	if (!peer->rate_started) {
		if (v32_conditioning_transmitter_framing_state(
			    &peer->conditioning, &peer->encoder) !=
		    V32_CONDITIONING_OK)
			return false;
		peer->rate_started = true;
	}
	if (peer->selection &&
	    (peer->end_dibit != 0 ||
	     (peer->end_requested && peer->rate_dibit == 0))) {
		if (peer->end_dibit < V32_RATE_SIGNAL_DIBITS) {
			if (v32_rate_signal_dibit(
				    peer->end_word, peer->end_dibit, &dibit) !=
			    V32_RATE_SIGNAL_OK)
				return false;
			peer->end_dibit++;
		} else {
			dibit = 3;
		}
	} else {
		if (v32_rate_signal_dibit(
			    peer->rate_word, peer->rate_dibit, &dibit) !=
			    V32_RATE_SIGNAL_OK)
			return false;
		peer->rate_dibit = (peer->rate_dibit + 1u) %
				   V32_RATE_SIGNAL_DIBITS;
		peer->rate_symbols++;
	}
	if (v32_4800_symbol_encode(
		    &peer->encoder, dibit, quadrant) !=
		    V32_FRAMING_OK)
		return false;
	return true;
}

static bool peer_qam_sample(struct test_peer_qam *peer, int16_t *sample)
{
	bool can_accept =
		qam_modulator_can_accept_symbol(&peer->modulator);
	bool accepted;

	if (can_accept) {
		uint8_t quadrant;

		if (!peer_next_quadrant(peer, &quadrant) ||
		    v32_4800_constellation_point(
			    quadrant, &peer->point) !=
			    V32_CONSTELLATION_OK)
			return false;
	}
	return qam_modulator_sample(
		       &peer->modulator, &peer->point, sample, &accepted) ==
		       QAM_OK &&
	       accepted == can_accept;
}

static bool caller_qam_receiver_init(
	struct test_caller_qam_receiver *receiver)
{
	*receiver = (struct test_caller_qam_receiver){ 0 };
	return qam_demodulator_init(
		       &receiver->demodulator, &test_v32_channel) ==
		       QAM_OK &&
	       v32_conditioning_receiver_init(
		       &receiver->conditioning,
		       V32_CALLING_TRANSMITTER) ==
		       V32_CONDITIONING_RECEIVER_OK &&
	       v32_rate_signal_detector_init(
		       &receiver->rate_detector) ==
		       V32_RATE_SIGNAL_OK;
}

static bool caller_qam_receiver_observe(
	struct test_caller_qam_receiver *receiver, int16_t sample)
{
	struct qam_point point;
	bool ready;

	if (qam_demodulator_sample(
		    &receiver->demodulator, sample, &point, &ready) !=
	    QAM_OK)
		return false;
	if (!ready || receiver->end_detected)
		return true;
	if (!receiver->conditioning_complete) {
		if (v32_conditioning_receiver_observe(
			    &receiver->conditioning, &point, &ready) !=
		    V32_CONDITIONING_RECEIVER_OK)
			return false;
		if (!ready)
			return true;
		if (v32_conditioning_receiver_framing_state(
			    &receiver->conditioning, &receiver->decoder,
			    &receiver->gain) !=
		    V32_CONDITIONING_RECEIVER_OK)
			return false;
		receiver->previous_point = point;
		receiver->conditioning_complete = true;
		return true;
	}
	{
		struct v32_rate_offer offer;
		enum v32_rate_signal_event event;
		uint8_t quarter_turns;
		uint8_t quadrant;
		uint8_t dibit;

		if (qam_nearest_quarter_turn(
			    &receiver->previous_point, &point,
			    &quarter_turns) != QAM_OK)
			return false;
		quadrant = (uint8_t)((receiver->decoder.phase_quadrant +
				      quarter_turns) &
				     3u);
		if (v32_4800_symbol_decode(
			    &receiver->decoder, quadrant, &dibit) !=
			    V32_FRAMING_OK ||
		    v32_rate_signal_detector_observe(
			    &receiver->rate_detector, dibit, &event, &offer) !=
			    V32_RATE_SIGNAL_OK)
			return false;
		if (event == V32_RATE_SIGNAL_DETECTED) {
			if (offer.rates != V32_RATE_CAPABILITY_4800 ||
			    offer.trellis_at_highest_rate)
				return false;
			receiver->rate_detected = true;
		} else if (event == V32_END_SIGNAL_DETECTED) {
			if (!receiver->rate_detected ||
			    offer.rates != V32_RATE_CAPABILITY_4800 ||
			    offer.trellis_at_highest_rate)
				return false;
			receiver->end_detected = true;
		}
	}
	receiver->previous_point = point;
	return true;
}

static bool test_bidirectional_conditioning_rate_handoff(void)
{
	struct modem_driver driver = {
		.name = "V.32 conditioning handoff test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct test_peer_qam peer;
	struct test_caller_qam_receiver caller_receiver;
	struct single_tone_transmitter residual_carrier;
	struct dp_operations *operations;
	struct modem *modem;
	struct dp *datapump;
	uint64_t response_power = 0;
	bool connected = false;

	CHECK(v32_datapump_register() == 0,
	      "V.32 datapump registration failed");
	operations = modem_dp_get_registered_ops(DP_V32);
	modem = modem_create(&driver, "V.32 conditioning handoff test");
	CHECK(operations != NULL && modem != NULL,
	      "V.32 conditioning handoff fixture did not initialize");
	modem->min_rate = modem->max_rate = V32_4800_BIT_RATE;
	datapump = operations->create(
		modem, DP_V32, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(datapump != NULL && drive_line_startup(operations, datapump) &&
		      peer_qam_init(&peer) &&
		      caller_qam_receiver_init(&caller_receiver),
	      "V.32 caller did not reach receiver conditioning");
	for (unsigned int count = 0;
	     count < TEST_QAM_SAMPLE_LIMIT &&
	     !caller_receiver.rate_detected;
	     ++count) {
		int16_t receive;
		int16_t transmit;

		CHECK(peer_qam_sample(&peer, &receive) &&
			      process_sample(
				      operations, datapump, receive, &transmit) &&
			      caller_qam_receiver_observe(
				      &caller_receiver, transmit),
		      "V.32 conditioning-to-rate processing stopped");
		if (peer.rate_symbols <
		    TEST_RATE_WORDS_BEFORE_RESPONSE *
			    V32_RATE_SIGNAL_DIBITS)
			CHECK(transmit == 0,
			      "V.32 caller responded before a repeated rate word");
		response_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(peer.rate_symbols >=
		      TEST_RATE_WORDS_BEFORE_RESPONSE *
			      V32_RATE_SIGNAL_DIBITS &&
		      response_power >= UINT64_C(1000000000) &&
		      caller_receiver.conditioning_complete &&
		      caller_receiver.rate_detected,
	      "V.32 caller did not return conditioning and a rate response");
	CHECK(v32_conditioning_transmitter_init(
		      &peer.conditioning,
		      V32_ANSWERING_TRANSMITTER,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_OK,
	      "V.32 peer receiver-training restart failed");
	CHECK(single_tone_transmitter_init(
		      &residual_carrier, V32_CALLING_CARRIER_HZ,
		      TEST_SECOND_CONDITIONING_TAIL_AMPLITUDE) == SINGLE_TONE_OK,
	      "V.32 residual carrier fixture did not initialize");
	peer.rate_started = false;
	peer.rate_dibit = 0;
	peer.rate_symbols = 0;
	peer.end_dibit = 0;
	peer.selection = true;
	caller_receiver.rate_detected = false;
	/* A weak tail can lock the QAM clock after the old carrier has been
	 * declared absent.  The second acquisition must wait through it and the
	 * following gap before accepting the restarted conditioning carrier. */
	for (unsigned int count = 0;
	     count < TEST_SECOND_CONDITIONING_SILENCE; ++count) {
		int16_t receive = 0;
		int16_t transmit;

		if (count < TEST_SECOND_CONDITIONING_TAIL_SAMPLES)
			CHECK(single_tone_transmitter_sample(
				      &residual_carrier, &receive) == SINGLE_TONE_OK,
			      "V.32 residual carrier fixture stopped");
		bool processed = process_sample(
			operations, datapump, receive, &transmit);
		bool observed = processed && caller_qam_receiver_observe(
			&caller_receiver, transmit);

		CHECK(processed && observed,
		      "V.32 second-conditioning gap failed at sample %u "
		      "(process=%d, observe=%d)",
		      count, processed, observed);
	}
	CHECK(caller_receiver.rate_detected && !caller_receiver.end_detected,
	      "V.32 caller did not continue R2 before second conditioning");
	for (unsigned int count = 0;
	     count < TEST_QAM_SAMPLE_LIMIT && !connected;
	     ++count) {
		int16_t receive;
		int16_t transmit;

		peer.end_requested = caller_receiver.end_detected;
		CHECK(peer_qam_sample(&peer, &receive) &&
			      process_sample_until_link(
				      operations, datapump, receive, &transmit,
				      &connected) &&
			      caller_qam_receiver_observe(
				      &caller_receiver, transmit),
		      "V.32 second-conditioning handoff stopped");
	}
	CHECK(v32_conditioning_transmitter_is_complete(
		      &peer.conditioning) &&
		      peer.rate_symbols >=
			      2 * V32_RATE_SIGNAL_DIBITS &&
		      peer.end_requested && caller_receiver.end_detected &&
		      peer.end_dibit == V32_RATE_SIGNAL_DIBITS &&
		      connected,
	      "V.32 caller did not reacquire second conditioning before R3 "
	      "(conditioning=%d, rate symbols=%u, requested=%d, "
	      "received=%d, E dibits=%u, link=%d)",
	      v32_conditioning_transmitter_is_complete(
		      &peer.conditioning),
	      peer.rate_symbols, peer.end_requested, caller_receiver.end_detected,
	      peer.end_dibit, connected);
	CHECK(operations->delete(datapump) == 0,
	      "V.32 datapump cleanup failed");
	modem_delete(modem);
	CHECK(v32_datapump_unregister() == 0,
	      "V.32 datapump deregistration failed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] bidirectional conditioning and rate handoff\n");
	if (!test_bidirectional_conditioning_rate_handoff()) {
		fprintf(stderr,
			"[  FAILED  ] bidirectional conditioning and rate handoff\n");
		return 1;
	}
	fprintf(stderr,
		"[       OK ] bidirectional conditioning and rate handoff\n");
	return 0;
}
