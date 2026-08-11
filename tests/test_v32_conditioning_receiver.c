// SPDX-License-Identifier: BSD-3-Clause

#include <qam_gain.h>
#include <conditioning.h>
#include <conditioning_receiver.h>
#include <constellation.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_ACQUISITION_START =
		SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS + 4,
	TEST_SEQUENCE_SYMBOLS = SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS +
				SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS +
				SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS,
};

static struct qam_point apply_channel(
	const struct qam_point *point,
	const struct qam_point *gain)
{
	return (struct qam_point){
		.in_phase = point->in_phase * gain->in_phase -
			    point->quadrature * gain->quadrature,
		.quadrature = point->in_phase * gain->quadrature +
			      point->quadrature * gain->in_phase,
	};
}

static bool generated_point(
	uint8_t quadrant, const struct qam_point *gain,
	struct qam_point *received)
{
	struct qam_point transmitted;

	CHECK(sippymodem_v32_4800_constellation_point(
		      quadrant, &transmitted) ==
		      SIPPYMODEM_V32_CONSTELLATION_OK,
	      "could not generate V.32 conditioning point %u", quadrant);
	*received = apply_channel(&transmitted, gain);
	return true;
}

static bool run_sequence(
	struct sippymodem_v32_conditioning_receiver *receiver,
	enum sippymodem_v32_transmitter_role role,
	const struct qam_point *channel,
	struct sippymodem_v32_conditioning_transmitter *finished_transmitter)
{
	struct sippymodem_v32_conditioning_transmitter transmitter;
	bool ready = false;

	CHECK(sippymodem_v32_conditioning_transmitter_init(
		      &transmitter, role,
		      SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      SIPPYMODEM_V32_CONDITIONING_OK,
	      "V.32 conditioning fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point received;
		uint8_t quadrant;

		CHECK(sippymodem_v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
		      "V.32 conditioning fixture stopped at symbol %u", symbol);
		if (symbol < TEST_ACQUISITION_START)
			continue;
		CHECK(generated_point(quadrant, channel, &received) &&
		      sippymodem_v32_conditioning_receiver_observe(
			      receiver, &received, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
		      "V.32 conditioning receiver stopped at symbol %u", symbol);
		CHECK(ready == (symbol + 1u == TEST_SEQUENCE_SYMBOLS),
		      "V.32 conditioning readiness changed at symbol %u", symbol);
	}
	CHECK(ready &&
		      sippymodem_v32_conditioning_receiver_is_complete(receiver),
	      "V.32 conditioning receiver did not complete");
	*finished_transmitter = transmitter;
	return true;
}

static bool check_role(
	enum sippymodem_v32_transmitter_role role,
	const struct qam_point *channel)
{
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_v32_conditioning_transmitter transmitter;
	struct sippymodem_v32_symbol_decoder decoder;
	struct qam_gain estimated;
	double gain_error;
	bool ready = true;

	CHECK(sippymodem_v32_conditioning_receiver_init(&receiver, role) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
	      "V.32 conditioning receiver did not initialize for role %d", role);
	CHECK(run_sequence(
		      &receiver, role, channel, &transmitter),
	      "V.32 conditioning sequence failed for role %d", role);
	CHECK(sippymodem_v32_conditioning_receiver_framing_state(
		      &receiver, &decoder, &estimated) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK &&
		      decoder.descrambler.history ==
			      transmitter.trn_encoder.scrambler.history &&
		      decoder.descrambler.transmitter_role == role &&
		      decoder.phase_quadrant ==
			      transmitter.trn_encoder.phase_quadrant,
	      "V.32 conditioning framing state changed for role %d", role);
	gain_error = hypot(
		estimated.channel_gain.in_phase - channel->in_phase,
		estimated.channel_gain.quadrature - channel->quadrature);
	CHECK(gain_error < 1e-9,
	      "V.32 conditioning channel error was %.12f for role %d",
	      gain_error, role);
	CHECK(sippymodem_v32_conditioning_receiver_observe(
		      &receiver, &receiver.previous_point, &ready) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_COMPLETE &&
		      ready,
	      "completed V.32 conditioning receiver changed output");
	return true;
}

static bool test_calling_source(void)
{
	const struct qam_point channel = { 1.25, -0.375 };

	return check_role(SIPPYMODEM_V32_CALLING_TRANSMITTER, &channel);
}

static bool test_answering_source(void)
{
	const struct qam_point channel = { -0.45, 1.6 };

	return check_role(SIPPYMODEM_V32_ANSWERING_TRANSMITTER, &channel);
}

static bool test_phase_slip_recovery(void)
{
	const struct qam_point channel = { 1.1, 0.25 };
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_v32_conditioning_transmitter transmitter;
	bool phase_shifted = false;
	bool ready = false;

	CHECK(sippymodem_v32_conditioning_receiver_init(
		      &receiver, SIPPYMODEM_V32_ANSWERING_TRANSMITTER) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK &&
		      sippymodem_v32_conditioning_transmitter_init(
			      &transmitter,
			      SIPPYMODEM_V32_ANSWERING_TRANSMITTER,
			      SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
	      "V.32 phase-slip fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point received;
		uint8_t quadrant;

		CHECK(sippymodem_v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
		      "V.32 phase-slip fixture stopped");
		if (symbol < TEST_ACQUISITION_START)
			continue;
		if (receiver.phase == SIPPYMODEM_V32_CONDITIONING_TRAIN_TRN)
			phase_shifted = true;
		if (phase_shifted)
			quadrant = (uint8_t)((quadrant + 1u) & 3u);
		CHECK(generated_point(quadrant, &channel, &received) &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &received, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
		      "V.32 conditioning rejected a persistent phase slip");
	}
	CHECK(phase_shifted && ready &&
		      sippymodem_v32_conditioning_receiver_is_complete(&receiver) &&
		      receiver.phase_corrections != 0,
	      "V.32 conditioning did not recover the persistent phase slip");
	return true;
}

static bool test_mapping_transition_rephase(void)
{
	const struct qam_point channel = { 0.85, -0.2 };
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_v32_conditioning_transmitter transmitter;
	const uint16_t phase_step_symbol =
		SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS +
		SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS +
		SIPPYMODEM_V32_CONDITIONING_TRN_PREFIX_SYMBOLS - 1u;
	bool ready = false;

	CHECK(sippymodem_v32_conditioning_receiver_init(
		      &receiver, SIPPYMODEM_V32_ANSWERING_TRANSMITTER) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK &&
		      sippymodem_v32_conditioning_transmitter_init(
			      &transmitter,
			      SIPPYMODEM_V32_ANSWERING_TRANSMITTER,
			      SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
	      "V.32 mapping-transition fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point received;
		uint8_t quadrant;

		CHECK(sippymodem_v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
		      "V.32 mapping-transition fixture stopped at symbol %u",
		      symbol);
		if (symbol < TEST_ACQUISITION_START)
			continue;
		if (symbol >= phase_step_symbol)
			quadrant = (uint8_t)((quadrant + 1u) & 3u);
		CHECK(generated_point(quadrant, &channel, &received) &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &received, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
		      "V.32 receiver rejected a mapping-boundary phase step at %u",
		      symbol);
	}
	CHECK(ready &&
		      sippymodem_v32_conditioning_receiver_is_complete(&receiver) &&
		      receiver.quadrant_offset != 0,
	      "V.32 receiver did not resolve the mapping-boundary phase step");
	return true;
}

static bool test_trn_prefix_acquisition(void)
{
	const struct qam_point channel = { 0.75, 0.15 };
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_v32_conditioning_transmitter transmitter;
	bool ready = false;

	CHECK(sippymodem_v32_conditioning_receiver_init(
		      &receiver, SIPPYMODEM_V32_ANSWERING_TRANSMITTER) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK &&
		      sippymodem_v32_conditioning_transmitter_init(
			      &transmitter,
			      SIPPYMODEM_V32_ANSWERING_TRANSMITTER,
			      SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
	      "V.32 TRN-prefix fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point received;
		uint8_t quadrant;

		CHECK(sippymodem_v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      SIPPYMODEM_V32_CONDITIONING_OK,
		      "V.32 TRN-prefix fixture stopped at symbol %u", symbol);
		if (symbol < SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS +
				     SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS)
			continue;
		CHECK(generated_point(quadrant, &channel, &received) &&
			      sippymodem_v32_conditioning_receiver_observe(
				      &receiver, &received, &ready) ==
				      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
		      "V.32 receiver rejected direct TRN acquisition at %u",
		      symbol);
	}
	CHECK(ready &&
		      sippymodem_v32_conditioning_receiver_is_complete(&receiver),
	      "V.32 receiver did not acquire directly from the TRN prefix");
	return true;
}

static bool test_validation(void)
{
	const struct qam_point valid_point = { 1.0, 0.0 };
	const struct qam_point zero_point = { 0.0, 0.0 };
	const struct qam_point invalid_point = { NAN, 0.0 };
	struct sippymodem_v32_conditioning_receiver receiver;
	struct sippymodem_v32_conditioning_receiver saved;
	struct sippymodem_v32_symbol_decoder decoder = { 0 };
	struct qam_gain gain = { 0 };
	bool ready = true;

	CHECK(sippymodem_v32_conditioning_receiver_init(
		      &receiver, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK,
	      "V.32 conditioning validation receiver did not initialize");
	saved = receiver;
	CHECK(sippymodem_v32_conditioning_receiver_init(
		      NULL, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_conditioning_receiver_init(
			      &receiver,
			      (enum sippymodem_v32_transmitter_role)99) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning receiver changed state on failed init");
	CHECK(sippymodem_v32_conditioning_receiver_observe(
		      &receiver, NULL, &ready) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &zero_point, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &invalid_point, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &valid_point, NULL) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      ready && memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning receiver validation was not transactional");
	CHECK(sippymodem_v32_conditioning_receiver_framing_state(
		      &receiver, &decoder, &gain) ==
		      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
	      "incomplete V.32 conditioning receiver exported state");
	receiver.prefix_fallback[0] = 1;
	CHECK(!sippymodem_v32_conditioning_receiver_is_valid(&receiver) &&
		      !sippymodem_v32_conditioning_receiver_is_complete(&receiver) &&
		      sippymodem_v32_conditioning_receiver_observe(
			      &receiver, &valid_point, &ready) ==
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      ready,
	      "V.32 conditioning receiver accepted corrupt state");
	CHECK(strcmp(
		      sippymodem_v32_conditioning_receiver_result_name(
			      SIPPYMODEM_V32_CONDITIONING_RECEIVER_OK),
		      "ok") == 0 &&
		      strcmp(
			      sippymodem_v32_conditioning_receiver_result_name(
			      (enum sippymodem_v32_conditioning_receiver_result)99),
		      "unknown") == 0,
	      "V.32 conditioning receiver result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "calling source", test_calling_source },
		{ "answering source", test_answering_source },
		{ "mapping transition rephase",
		  test_mapping_transition_rephase },
		{ "phase-slip recovery", test_phase_slip_recovery },
		{ "TRN-prefix acquisition", test_trn_prefix_acquisition },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
