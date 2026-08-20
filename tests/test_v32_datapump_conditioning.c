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

#include "test.h"

#include <stdint.h>

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
};

struct test_conditioning_signal {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter transmitter;
	struct qam_point point;
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
