// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <qam.h>
#include <single_tone.h>
#include <carrier.h>
#include <conditioning.h>
#include <constellation.h>
#include <datapump.h>
#include <echo_training.h>
#include <line_startup.h>
#include <rate_path.h>
#include <v8_datapump.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>

/* This standalone fixture has no automatic-negotiation owner. Real handoffs
 * are exercised by the native-core and paired-media integration tests. */
bool v8_datapump_v32_handoff(const struct dp *dp, struct single_tone_detector *carrier)
{
	(void)dp;
	(void)carrier;
	return false;
}

enum {
	TEST_TONE_AMPLITUDE = 12000,
	TEST_SILENCE_RUN = 24,
	TEST_ANSWERER_LINE_LIMIT = MODEM_RATE * 8,
	TEST_ANSWERER_TRAIN_ECHO_SAMPLES = 4500,
	TEST_ANSWERER_OFFER_SAMPLES =
		((V32_CONDITIONING_S_SYMBOLS +
		  V32_CONDITIONING_S_BAR_SYMBOLS +
		  V32_ANSWERING_SECOND_TRN_SYMBOLS + 1) *
			 MODEM_RATE +
		 V32_CARRIER_SYMBOL_RATE - 1) /
			V32_CARRIER_SYMBOL_RATE +
		256,
	TEST_ANSWERER_OFFER_POWER_SAMPLES = 400,
	TEST_FRESH_CALLER_SAMPLES = 4000,
	/* A half-second physical round trip must survive the line handoff. */
	TEST_LONG_ONE_WAY_DELAY_SAMPLES = MODEM_RATE / 4,
	TEST_LONG_LINE_LIMIT = MODEM_RATE * 12,
	TEST_LONG_DRAIN_SAMPLES = TEST_LONG_ONE_WAY_DELAY_SAMPLES +
		V32_ANSWERER_DROP_SILENCE_SAMPLES + 512,
};

struct test_sample_delay {
	int16_t samples[TEST_LONG_ONE_WAY_DELAY_SAMPLES];
	size_t cursor;
};

struct test_conditioning_signal {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter transmitter;
	struct qam_point point;
	struct v32_symbol_encoder encoder;
	unsigned int rate_dibits;
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

static bool is_quiet_sample(int16_t sample)
{
	return sample == 0 || sample == -1;
}

static int16_t mix_samples(int16_t left, int16_t right)
{
	int32_t mixed = (int32_t)left + right;

	if (mixed > INT16_MAX)
		return INT16_MAX;
	if (mixed < INT16_MIN)
		return INT16_MIN;
	return (int16_t)mixed;
}

static int16_t delay_sample(
	struct test_sample_delay *delay, int16_t sample)
{
	int16_t delayed = delay->samples[delay->cursor];

	delay->samples[delay->cursor] = sample;
	delay->cursor = (delay->cursor + 1u) %
		TEST_LONG_ONE_WAY_DELAY_SAMPLES;
	return delayed;
}

static bool drive_long_line_startup(
	struct dp_operations *operations, struct dp *datapump,
	bool candidate_is_caller, uint32_t *measured_round_trip,
	uint64_t *post_startup_power)
{
	struct v32_line_startup caller = { 0 };
	struct v32_answerer_line_startup answerer = { 0 };
	struct test_sample_delay candidate_to_peer = { 0 };
	struct test_sample_delay peer_to_candidate = { 0 };
	int16_t candidate_transmit = 0;
	int16_t peer_transmit = 0;
	bool peer_complete = false;

	CHECK(measured_round_trip != NULL && post_startup_power != NULL &&
	      (candidate_is_caller ?
		       v32_answerer_line_startup_init(
			       &answerer, TEST_TONE_AMPLITUDE) :
		       v32_line_startup_init(
			       &caller, TEST_TONE_AMPLITUDE)) ==
		      V32_LINE_STARTUP_OK,
	      "long V.32 line peer did not initialize");
	for (unsigned int sample = 0;
	     sample < TEST_LONG_LINE_LIMIT && !peer_complete; ++sample) {
		int16_t candidate_receive = delay_sample(
			&peer_to_candidate, peer_transmit);
		int16_t peer_receive = delay_sample(
			&candidate_to_peer, candidate_transmit);
		int16_t next_candidate;
		int16_t next_peer;
		enum v32_line_startup_result peer_result;

		peer_result = candidate_is_caller ?
			v32_answerer_line_startup_process(
				&answerer, peer_receive, &next_peer,
				&peer_complete) :
			v32_line_startup_process(
				&caller, peer_receive, &next_peer,
				&peer_complete);
		CHECK(peer_result == V32_LINE_STARTUP_OK &&
		      process_sample(
			      operations, datapump, candidate_receive,
			      &next_candidate),
		      "long V.32 line exchange stopped at sample %u",
		      sample);
		candidate_transmit = next_candidate;
		peer_transmit = next_peer;
	}
	CHECK(peer_complete, "long V.32 line peer did not complete");
	*measured_round_trip = candidate_is_caller ?
		v32_answerer_line_startup_round_trip_samples(&answerer) :
		v32_line_startup_round_trip_samples(&caller);
	*post_startup_power = 0;
	for (unsigned int sample = 0;
	     sample < TEST_LONG_DRAIN_SAMPLES; ++sample) {
		int16_t candidate_receive = delay_sample(
			&peer_to_candidate, 0);
		int16_t next_candidate;

		CHECK(process_sample(
			      operations, datapump, candidate_receive,
			      &next_candidate),
		      "V.32 endpoint rejected the long-delay handoff at %u",
		      sample);
		if (sample >= TEST_LONG_DRAIN_SAMPLES - 512u)
			*post_startup_power += (uint64_t)(
				(int32_t)next_candidate * next_candidate);
	}
	return true;
}

static bool test_long_round_trip_line_startup(void)
{
	struct modem_driver driver = {
		.name = "V.32 long round-trip test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations *operations;
	uint32_t minimum_turnaround_samples =
		2u * TEST_LONG_ONE_WAY_DELAY_SAMPLES;

	CHECK(v32_datapump_register() == 0,
	      "V.32 long round-trip registration failed");
	operations = modem_dp_get_registered_ops(DP_V32);
	CHECK(operations != NULL,
	      "V.32 long round-trip operations are unavailable");
	for (unsigned int candidate_is_caller = 0;
	     candidate_is_caller <= 1; ++candidate_is_caller) {
		struct modem *modem = modem_create(
			&driver, candidate_is_caller ?
					"V.32 long caller" :
					"V.32 long answerer");
		struct dp *datapump;
		uint32_t measured_round_trip = 0;
		uint64_t post_startup_power = 0;

		CHECK(modem != NULL,
		      "V.32 long round-trip modem did not initialize");
		modem->min_rate = modem->max_rate = V32_4800_BIT_RATE;
		datapump = operations->create(
			modem, DP_V32, (int)candidate_is_caller,
			MODEM_RATE, MODEM_FRAG, operations);
		CHECK(datapump != NULL && drive_long_line_startup(
			      operations, datapump,
			      candidate_is_caller != 0,
			      &measured_round_trip, &post_startup_power) &&
		      measured_round_trip > minimum_turnaround_samples &&
		      measured_round_trip < minimum_turnaround_samples +
			      3u * V32_REVERSAL_RESPONSE_SAMPLES &&
		      (candidate_is_caller != 0 ||
		       post_startup_power > UINT64_C(1000000000)),
		      "V.32 role %u rejected/clipped its %u-sample line timing",
		      candidate_is_caller, measured_round_trip);
		CHECK(operations->delete(datapump) == 0,
		      "V.32 long round-trip cleanup failed");
		modem_delete(modem);
	}
	CHECK(v32_datapump_unregister() == 0,
	      "V.32 long round-trip deregistration failed");
	return true;
}

static bool conditioning_signal_init(
	struct test_conditioning_signal *signal,
	enum v32_transmitter_role role)
{
	struct qam_channel channel;

	*signal = (struct test_conditioning_signal){ 0 };
	return v32_carrier_channel(&channel) == V32_CARRIER_OK &&
	       qam_modulator_init(
		       &signal->modulator, &channel,
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

		if (!v32_conditioning_transmitter_is_complete(&signal->transmitter)) {
			if (v32_conditioning_transmitter_next(
				    &signal->transmitter, &quadrant) != V32_CONDITIONING_OK)
				return false;
		} else {
			uint8_t dibit;

			if (signal->rate_dibits == 0 &&
			    v32_conditioning_transmitter_framing_state(
				    &signal->transmitter, &signal->encoder) != V32_CONDITIONING_OK)
				return false;
			if (v32_rate_signal_dibit(0x0111,
				    signal->rate_dibits % V32_RATE_SIGNAL_DIBITS, &dibit) !=
				    V32_RATE_SIGNAL_OK ||
			    v32_4800_symbol_encode(&signal->encoder, dibit, &quadrant) !=
				    V32_FRAMING_OK)
				return false;
			signal->rate_dibits++;
		}
		if (v32_4800_constellation_point(
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

	/* Loop back enough initial answerer conditioning to qualify S-bar.  Keep
	 * that learned echo path present while changing only the remote signal;
	 * stale receive evidence must not suppress the following rate offer. */
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
		int16_t received;

		CHECK(single_tone_transmitter_sample(
			      &residual_carrier, &residual) == SINGLE_TONE_OK,
		      "V.32 answerer residual carrier stopped");
		received = mix_samples(answerer_transmit, residual);
		CHECK(process_sample(
			      operations, datapump, received,
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
		int16_t received;

		CHECK(conditioning_signal_sample(
			      &caller, &caller_transmit),
		      "V.32 fresh caller conditioning stopped at sample %u",
		      sample);
		received = mix_samples(answerer_transmit, caller_transmit);
		CHECK(process_sample(
			      operations, datapump, received,
			      &answerer_transmit),
		      "V.32 answerer rejected fresh conditioning at sample %u",
		      sample);
		quiet_samples = is_quiet_sample(answerer_transmit) ?
				quiet_samples + 1u : 0;
	}
	CHECK(quiet_samples == TEST_SILENCE_RUN,
	      "V.32 answerer did not require fresh S-bar evidence");
	{
		struct v32_conditioning_path reply;
		struct v32_rate_path rate;
		struct v32_echo_reference silence;
		struct qam_point zero = { 0 };
		int16_t received = 0;
		bool rate_started = false;
		bool cleared = false;
		bool disconnected = false;

		CHECK(v32_echo_reference_init(&silence, &zero, 0) == V32_ECHO_OK,
		      "silent decoder reference did not initialize");
		CHECK(v32_conditioning_path_init(&reply, V32_ANSWERING_TRANSMITTER,
			      0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
			      V32_CONDITIONING_PATH_OK,
		      "peer reply receiver did not initialize");
		for (unsigned int sample = 0;
		     sample < MODEM_RATE * 3u && !disconnected; ++sample) {
			int16_t caller_transmit;
			/* This independent receiver has no local echo, but must still
			 * supply a silent reference on every transmit-symbol epoch. */
			const struct v32_echo_frontend *echo = rate_started ?
				&rate.receive.echo : &reply.receive.echo;
			const struct v32_echo_reference *reference =
				echo->filter.inputs_needed == 1 && echo->echo.phase == 0 ?
				&silence : NULL;
			int status;

			CHECK(conditioning_signal_sample(&caller, &caller_transmit),
			      "peer cleardown source stopped");
			received = mix_samples(answerer_transmit, caller_transmit);
			status = operations->process(
				datapump, &received, &answerer_transmit, 1);
			CHECK(status == DPSTAT_OK || status == DPSTAT_ERROR,
			      "peer cleardown incorrectly connected: %d", status);
			disconnected = status == DPSTAT_ERROR;
			if (disconnected)
				break;
			if (!rate_started) {
				enum v32_conditioning_path_result result =
					v32_conditioning_path_sample(
						&reply, answerer_transmit, reference);

				CHECK(result == V32_CONDITIONING_PATH_OK ||
				      result == V32_CONDITIONING_PATH_COMPLETE,
				      "answerer cleardown conditioning stopped: %d", result);
				if (result == V32_CONDITIONING_PATH_COMPLETE) {
					struct v32_conditioning_path_handoff handoff;

					CHECK(v32_conditioning_path_handoff(&reply, &handoff) ==
						      V32_CONDITIONING_PATH_OK &&
					      v32_rate_path_init(&rate, &handoff) == V32_RATE_PATH_OK,
					      "answerer did not emit valid second conditioning");
					rate_started = true;
				}
			} else if (!cleared) {
				enum v32_rate_path_result result =
					v32_rate_path_sample(
						&rate, answerer_transmit, reference);

				CHECK(result == V32_RATE_PATH_OK ||
				      result == V32_RATE_PATH_CLEARDOWN,
				      "answerer selected data after refusal: %d", result);
				cleared = result == V32_RATE_PATH_CLEARDOWN;
			}
		}
		CHECK(disconnected && cleared && answerer_transmit == 0,
		      "peer refusal did not finish a bounded on-wire cleardown");
		received = 0;
		CHECK(operations->process(datapump, &received, &answerer_transmit, 1) ==
			      DPSTAT_ERROR && answerer_transmit == 0,
		      "cleardown failure was not latched");
	}
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
	fprintf(stderr, "[ RUN      ] long round-trip line startup\n");
	if (!test_long_round_trip_line_startup()) {
		fprintf(stderr,
			"[  FAILED  ] long round-trip line startup\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] long round-trip line startup\n");
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
	return 0;
}
