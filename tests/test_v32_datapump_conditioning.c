// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <qam.h>
#include <qam_gain.h>
#include <single_tone.h>
#include <conditioning.h>
#include <conditioning_qam.h>
#include <conditioning_receiver.h>
#include <constellation.h>
#include <datapump.h>
#include <framing.h>
#include <g711.h>
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
	TEST_ANSWERER_LINE_LIMIT = MODEM_RATE * 8,
	TEST_ANSWERER_TRAIN_ECHO_SAMPLES = 4500,
	TEST_ANSWERER_OFFER_SAMPLES = 2000,
	TEST_ANSWERER_OFFER_POWER_SAMPLES = 400,
	TEST_FRESH_CALLER_SAMPLES = 4000,
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

struct test_conditioning_signal {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter transmitter;
	struct qam_point point;
};

struct test_caller_qam_receiver {
	struct qam_demodulator demodulator;
	struct v32_conditioning_receiver conditioning;
	struct v32_conditioning_qam conditioning_qam;
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

static bool is_quiet_sample(int16_t sample)
{
	return sample == 0 || sample == -1;
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

static bool conditioning_signal_init(
	struct test_conditioning_signal *signal,
	enum v32_transmitter_role role)
{
	*signal = (struct test_conditioning_signal){ 0 };
	return qam_modulator_init(
		       &signal->modulator, &test_v32_channel,
		       TEST_TONE_AMPLITUDE) == QAM_OK &&
	       v32_conditioning_transmitter_init(
		       &signal->transmitter, role,
		       V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		       V32_CONDITIONING_OK;
}

static bool conditioning_signal_sample(
	struct test_conditioning_signal *signal, int16_t *sample)
{
	bool can_accept = qam_modulator_can_accept_symbol(
		&signal->modulator);
	bool accepted;

	if (can_accept) {
		uint8_t quadrant;

		if (v32_conditioning_transmitter_next(
			    &signal->transmitter, &quadrant) !=
			    V32_CONDITIONING_OK ||
		    v32_4800_constellation_point(
			    quadrant, &signal->point) !=
			    V32_CONSTELLATION_OK)
			return false;
	}
	return qam_modulator_sample(
		       &signal->modulator, &signal->point, sample,
		       &accepted) == QAM_OK &&
	       accepted == can_accept;
}

static bool drive_answerer_line_startup(
	struct dp_operations *operations, struct dp *datapump,
	int16_t *answerer_transmit)
{
	struct v32_line_startup caller;
	int16_t caller_transmit = 0;
	int16_t answerer = 0;
	bool caller_complete = false;

	CHECK(v32_line_startup_init(
		      &caller, TEST_TONE_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK,
	      "V.32 answerer peer line startup did not initialize");
	for (unsigned int sample = 0;
	     sample < TEST_ANSWERER_LINE_LIMIT && !caller_complete;
	     ++sample) {
		int16_t next_caller;

		CHECK(v32_line_startup_process(
			      &caller, answerer, &next_caller,
			      &caller_complete) == V32_LINE_STARTUP_OK &&
		      process_sample(
			      operations, datapump, caller_transmit,
			      &answerer),
		      "V.32 answerer line exchange stopped at sample %u",
		      sample);
		caller_transmit = next_caller;
	}
	CHECK(caller_complete,
	      "V.32 answerer line exchange did not complete");
	for (unsigned int sample = 0;
	     sample < V32_ANSWERER_DROP_SILENCE_SAMPLES; ++sample)
		CHECK(process_sample(
			      operations, datapump, 0, &answerer),
		      "V.32 answerer rejected its final line-startup gap");
	*answerer_transmit = answerer;
	return true;
}

static bool drive_line_startup(
	struct dp_operations *operations, struct dp *datapump)
{
	struct single_tone_transmitter answer_tone;
	struct test_turnaround_signal turnaround;
	uint64_t calling_power = 0;
	int32_t pcma_silence_sum = 0;
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
		silent_samples = is_quiet_sample(transmit) ?
				 silent_samples + 1 : 0;
	}
	CHECK(silent_samples == TEST_SILENCE_RUN,
	      "V.32 caller did not become silent for peer conditioning");
	for (unsigned int count = 0;
	     count < V32_ANSWERER_DROP_SILENCE_SAMPLES; ++count) {
		CHECK(process_sample(operations, datapump, 0, &transmit) &&
			      is_quiet_sample(transmit),
		      "V.32 caller rejected the post-turnaround gap");
		pcma_silence_sum += g711_decode_pcma(
			g711_encode_pcma(transmit));
	}
	CHECK(pcma_silence_sum == 0,
	      "V.32 caller silence has an A-law DC component");
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
	       v32_conditioning_qam_init(
		       &receiver->conditioning_qam) ==
		       V32_CONDITIONING_QAM_OK &&
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
		if (v32_conditioning_qam_observe(
			    &receiver->conditioning_qam,
			    &receiver->conditioning, &point) !=
			    V32_CONDITIONING_QAM_OK)
			return false;
		if (!v32_conditioning_receiver_is_complete(
			    &receiver->conditioning))
			return true;
		if (v32_conditioning_receiver_framing_state(
			    &receiver->conditioning, &receiver->decoder) !=
			    V32_CONDITIONING_RECEIVER_OK ||
		    v32_conditioning_qam_handoff(
			    &receiver->conditioning_qam,
			    &receiver->conditioning,
			    &receiver->previous_point,
			    &receiver->gain) != V32_CONDITIONING_QAM_OK)
			return false;
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

static struct qam_point apply_test_channel(
	const struct qam_point *point, const struct qam_point *channel)
{
	return (struct qam_point){
		.in_phase = point->in_phase * channel->in_phase -
			    point->quadrature * channel->quadrature,
		.quadrature = point->in_phase * channel->quadrature +
			      point->quadrature * channel->in_phase,
	};
}

static bool test_conditioning_qam_adapter(void)
{
	const struct qam_point channel = { 0.8, -0.25 };
	const struct qam_point zero = { 0 };
	struct v32_conditioning_transmitter transmitter;
	struct v32_conditioning_receiver receiver;
	struct v32_conditioning_receiver fresh_receiver;
	struct v32_conditioning_qam adapter;
	struct v32_conditioning_receiver saved_receiver;
	struct v32_conditioning_qam saved_adapter;
	struct qam_point final_point = { 0 };
	struct qam_point saved_point;
	struct qam_point last_received = { 0 };
	struct qam_gain gain = { 0 };
	union {
		struct qam_point point;
		struct qam_gain gain;
	} overlapping_output = { 0 };
	unsigned char saved_output[sizeof(overlapping_output)];
	struct qam_gain saved_gain;

	CHECK(v32_conditioning_qam_init(&adapter) ==
		      V32_CONDITIONING_QAM_OK &&
	      v32_conditioning_receiver_init(
		      &receiver, V32_ANSWERING_TRANSMITTER) ==
		      V32_CONDITIONING_RECEIVER_OK &&
	      v32_conditioning_transmitter_init(
		      &transmitter, V32_ANSWERING_TRANSMITTER,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_OK,
	      "V.32 conditioning QAM bridge did not initialize");
	saved_receiver = receiver;
	saved_adapter = adapter;
	CHECK(v32_conditioning_qam_observe(
		      &adapter, &receiver, &zero) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      v32_conditioning_qam_observe(
		      NULL, &receiver, &channel) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      v32_conditioning_qam_observe(
		      &adapter, NULL, &channel) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      v32_conditioning_qam_observe(
		      &adapter, &receiver, NULL) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      memcmp(&adapter, &saved_adapter, sizeof(adapter)) == 0 &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "V.32 conditioning QAM bridge rejection was not transactional");
	for (uint16_t symbol = 0;
	     symbol < V32_CONDITIONING_S_SYMBOLS +
			      V32_CONDITIONING_S_BAR_SYMBOLS +
			      V32_CONDITIONING_TRN_MIN_SYMBOLS;
	     ++symbol) {
		struct qam_point transmitted;
		uint8_t quadrant;

		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) == V32_CONDITIONING_OK &&
		      v32_4800_constellation_point(
			      quadrant, &transmitted) ==
			      V32_CONSTELLATION_OK,
		      "V.32 conditioning QAM fixture stopped at %u", symbol);
		last_received = apply_test_channel(&transmitted, &channel);
		CHECK(v32_conditioning_qam_observe(
			      &adapter, &receiver, &last_received) ==
			      V32_CONDITIONING_QAM_OK,
		      "V.32 conditioning QAM bridge stopped at %u", symbol);
		if (symbol == 0) {
			saved_adapter = adapter;
			saved_receiver = receiver;
			CHECK(v32_conditioning_qam_observe(
				      &adapter, &receiver,
				      &adapter.previous_point) ==
				      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
			      memcmp(&adapter, &saved_adapter,
				     sizeof(adapter)) == 0 &&
			      memcmp(&receiver, &saved_receiver,
				     sizeof(receiver)) == 0,
			      "V.32 conditioning QAM bridge accepted owner aliasing");
			CHECK(v32_conditioning_receiver_init(
				      &fresh_receiver,
				      V32_ANSWERING_TRANSMITTER) ==
				      V32_CONDITIONING_RECEIVER_OK,
			      "V.32 mismatched QAM receiver did not initialize");
			saved_receiver = fresh_receiver;
			CHECK(v32_conditioning_qam_observe(
				      &adapter, &fresh_receiver,
				      &last_received) ==
				      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
			      memcmp(&adapter, &saved_adapter,
				     sizeof(adapter)) == 0 &&
			      memcmp(&fresh_receiver, &saved_receiver,
				     sizeof(fresh_receiver)) == 0,
			      "V.32 conditioning QAM bridge spliced unrelated owners");
			final_point = (struct qam_point){ 7.0, -9.0 };
			memset(&gain, 0x5a, sizeof(gain));
			saved_point = final_point;
			saved_gain = gain;
			CHECK(v32_conditioning_qam_handoff(
				      &adapter, &fresh_receiver,
				      &final_point, &gain) ==
				      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
			      memcmp(&final_point, &saved_point,
				     sizeof(final_point)) == 0 &&
			      memcmp(&gain, &saved_gain, sizeof(gain)) == 0,
			      "V.32 QAM handoff spliced owners or changed output");
		}
	}
	CHECK(v32_conditioning_receiver_is_complete(&receiver) &&
	      v32_conditioning_qam_handoff(
		      &adapter, &receiver, &final_point, &gain) ==
		      V32_CONDITIONING_QAM_OK &&
	      fabs(final_point.in_phase - last_received.in_phase) < 1e-12 &&
	      fabs(final_point.quadrature - last_received.quadrature) < 1e-12 &&
	      fabs(gain.channel_gain.in_phase - channel.in_phase) < 1e-12 &&
	      fabs(gain.channel_gain.quadrature - channel.quadrature) < 1e-12,
	      "V.32 conditioning QAM handoff changed the point or channel");
	saved_adapter = adapter;
	saved_receiver = receiver;
	CHECK(v32_conditioning_qam_observe(
		      &adapter, &receiver, &last_received) ==
		      V32_CONDITIONING_QAM_COMPLETE &&
	      memcmp(&adapter, &saved_adapter, sizeof(adapter)) == 0 &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "completed V.32 conditioning QAM bridge changed ownership");
	memcpy(saved_output, &overlapping_output, sizeof(saved_output));
	saved_gain = gain;
	CHECK(v32_conditioning_qam_handoff(
		      &adapter, &receiver, &overlapping_output.point,
		      &overlapping_output.gain) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      v32_conditioning_qam_handoff(
		      &adapter, &receiver, &adapter.previous_point, &gain) ==
		      V32_CONDITIONING_QAM_INVALID_ARGUMENT &&
	      memcmp(&overlapping_output, saved_output,
		     sizeof(saved_output)) == 0 &&
	      memcmp(&gain, &saved_gain, sizeof(gain)) == 0 &&
	      memcmp(&adapter, &saved_adapter, sizeof(adapter)) == 0,
	      "V.32 conditioning QAM handoff accepted output aliasing");
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
			CHECK(is_quiet_sample(transmit),
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

static bool test_answerer_requires_fresh_conditioning(void)
{
	struct modem_driver driver = {
		.name = "V.32 fresh answerer conditioning test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct test_conditioning_signal caller;
	struct single_tone_transmitter residual_carrier;
	struct dp_operations *operations;
	struct modem *modem;
	struct dp *datapump;
	uint64_t offer_power = 0;
	unsigned int quiet_samples = 0;
	int16_t answerer_transmit = 0;

	CHECK(v32_datapump_register() == 0,
	      "V.32 answerer conditioning registration failed");
	operations = modem_dp_get_registered_ops(DP_V32);
	modem = modem_create(
		&driver, "V.32 fresh answerer conditioning test");
	CHECK(operations != NULL && modem != NULL,
	      "V.32 answerer conditioning fixture did not initialize");
	modem->min_rate = modem->max_rate = V32_4800_BIT_RATE;
	datapump = operations->create(
		modem, DP_V32, 0, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(datapump != NULL &&
	      drive_answerer_line_startup(
		      operations, datapump, &answerer_transmit),
	      "V.32 answerer did not finish line startup");

	/* Loop back enough initial answerer conditioning to qualify S-bar, but
	 * remove the echo before the local training interval ends.  That stale
	 * receive state must not suppress the following rate offer. */
	for (unsigned int sample = 0;
	     sample < TEST_ANSWERER_TRAIN_ECHO_SAMPLES; ++sample) {
		int16_t echoed = answerer_transmit;

		CHECK(process_sample(
			      operations, datapump, echoed,
			      &answerer_transmit),
		      "V.32 answerer rejected training echo at sample %u",
		      sample);
	}
	CHECK(single_tone_transmitter_init(
		      &residual_carrier, V32_CALLING_CARRIER_HZ,
		      TEST_TONE_AMPLITUDE / 4) == SINGLE_TONE_OK,
	      "V.32 answerer residual carrier did not initialize");
	for (unsigned int sample = 0;
	     sample < TEST_ANSWERER_OFFER_SAMPLES; ++sample) {
		int16_t residual;

		CHECK(single_tone_transmitter_sample(
			      &residual_carrier, &residual) == SINGLE_TONE_OK &&
		      process_sample(
			      operations, datapump, residual,
			      &answerer_transmit),
		      "V.32 answerer stopped before its fresh rate offer");
		if (sample >= TEST_ANSWERER_OFFER_SAMPLES -
				      TEST_ANSWERER_OFFER_POWER_SAMPLES)
			offer_power += (uint64_t)(
				(int32_t)answerer_transmit * answerer_transmit);
	}
	CHECK(offer_power > UINT64_C(1000000000),
	      "V.32 answerer suppressed its offer on stale training evidence");

	CHECK(conditioning_signal_init(
		      &caller, V32_CALLING_TRANSMITTER),
	      "V.32 fresh caller conditioning did not initialize");
	for (unsigned int sample = 0;
	     sample < TEST_FRESH_CALLER_SAMPLES &&
	     quiet_samples < TEST_SILENCE_RUN;
	     ++sample) {
		int16_t caller_transmit;

		CHECK(conditioning_signal_sample(
			      &caller, &caller_transmit) &&
		      process_sample(
			      operations, datapump, caller_transmit,
			      &answerer_transmit),
		      "V.32 answerer rejected fresh conditioning at sample %u",
		      sample);
		quiet_samples = is_quiet_sample(answerer_transmit) ?
				quiet_samples + 1u : 0;
	}
	CHECK(quiet_samples == TEST_SILENCE_RUN,
	      "V.32 answerer did not require fresh S-bar evidence");
	CHECK(operations->delete(datapump) == 0,
	      "V.32 answerer conditioning cleanup failed");
	modem_delete(modem);
	CHECK(v32_datapump_unregister() == 0,
	      "V.32 answerer conditioning deregistration failed");
	return true;
}

static bool test_training_deadline(void)
{
	struct modem_driver driver = {
		.name = "V.32 training deadline test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations *operations;
	struct modem *modem;
	struct dp *datapump;
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];

	CHECK(v32_datapump_register() == 0,
	      "V.32 deadline registration failed");
	operations = modem_dp_get_registered_ops(DP_V32);
	modem = modem_create(&driver, "V.32 training deadline test");
	CHECK(operations != NULL && modem != NULL,
	      "V.32 deadline fixture did not initialize");
	modem->min_rate = modem->max_rate = V32_4800_BIT_RATE;
	modem->sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 1;
	datapump = operations->create(
		modem, DP_V32, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(datapump != NULL,
	      "V.32 deadline data pump did not initialize");
	for (unsigned int frame = 0;
	     frame < MODEM_RATE / MODEM_FRAG; ++frame)
		CHECK(operations->process(
			      datapump, receive, transmit, MODEM_FRAG) ==
			      DPSTAT_OK,
		      "V.32 training expired before its inclusive deadline");
	transmit[0] = INT16_MAX;
	CHECK(operations->process(datapump, receive, transmit, 1) ==
		      DPSTAT_ERROR &&
	      transmit[0] == 0,
	      "V.32 training did not fail closed after its deadline");
	transmit[0] = INT16_MAX;
	CHECK(operations->process(datapump, receive, transmit, 1) ==
		      DPSTAT_ERROR &&
	      transmit[0] == 0,
	      "V.32 training failure was not latched");
	CHECK(operations->delete(datapump) == 0,
	      "failed V.32 training retained ownership");
	modem_delete(modem);
	CHECK(v32_datapump_unregister() == 0,
	      "V.32 deadline deregistration failed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] conditioning QAM adapter\n");
	if (!test_conditioning_qam_adapter()) {
		fprintf(stderr, "[  FAILED  ] conditioning QAM adapter\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] conditioning QAM adapter\n");
	fprintf(stderr, "[ RUN      ] training deadline\n");
	if (!test_training_deadline()) {
		fprintf(stderr, "[  FAILED  ] training deadline\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] training deadline\n");
	fprintf(stderr, "[ RUN      ] fresh answerer conditioning\n");
	if (!test_answerer_requires_fresh_conditioning()) {
		fprintf(stderr,
			"[  FAILED  ] fresh answerer conditioning\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] fresh answerer conditioning\n");
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
