// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase4_mp.h>
#include <sippymodem_v34_phase4_mp_channel_receiver.h>
#include <sippymodem_v34_phase4_e_channel_receiver.h>
#include <sippymodem_v34_phase4_e_sequence.h>
#include <sippymodem_v34_training_channel.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_TRN_SYMBOLS = SIPPYMODEM_V34_PHASE4_TRN_MIN_SYMBOLS + 5,
};

static struct sippymodem_v34_mp_parameters test_parameters(void)
{
	return (struct sippymodem_v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis = SIPPYMODEM_V34_TRELLIS_32_STATE,
		.remote_transmitter_expanded_shaping = true,
		.data_rate_capability_mask = 0x1a55,
		.asymmetric_rates_enabled = true,
	};
}

static struct sippymodem_qam_point complex_product(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.001;
}

static struct sippymodem_qam_point pass_channel(
	struct sippymodem_qam_point *channel,
	const struct sippymodem_qam_point *drift,
	const struct sippymodem_qam_point *transmitted, uint32_t *random)
{
	struct sippymodem_qam_point received;

	*channel = complex_product(channel, drift);
	received = complex_product(channel, transmitted);
	received.in_phase += fixture_noise(random);
	received.quadrature += fixture_noise(random);
	return received;
}

static bool events_are_equal(
	const struct sippymodem_v34_mp_receive_event *left,
	const struct sippymodem_v34_mp_receive_event *right)
{
	uint8_t left_frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];
	uint8_t right_frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];

	if (left->type != right->type ||
	    left->frame_result != right->frame_result)
		return false;
	if (left->type != SIPPYMODEM_V34_MP_RECEIVE_TYPE0)
		return true;
	return sippymodem_v34_mp_type0_encode(
		       &left->parameters.type0, left_frame, sizeof(left_frame)) ==
		       SIPPYMODEM_V34_MP_OK &&
	       sippymodem_v34_mp_type0_encode(
		       &right->parameters.type0, right_frame,
		       sizeof(right_frame)) == SIPPYMODEM_V34_MP_OK &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static bool receive_pair(
	struct sippymodem_v34_phase4_mp_receiver *control,
	struct sippymodem_v34_phase4_mp_channel_receiver *channel_receiver,
	const struct sippymodem_qam_point *transmitted,
	const struct sippymodem_qam_point *received,
	struct sippymodem_v34_mp_receive_event *event)
{
	struct sippymodem_v34_mp_receive_event control_event;
	struct sippymodem_v34_mp_receive_event channel_event;

	CHECK(sippymodem_v34_phase4_mp_receive_point(
		      control, transmitted, &control_event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      sippymodem_v34_phase4_mp_channel_receive_point(
		      channel_receiver, received, &channel_event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      events_are_equal(&control_event, &channel_event),
	      "flat-channel Phase 4 receiver changed a symbol decision");
	*event = channel_event;
	return true;
}

static bool receive_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	struct sippymodem_v34_phase4_mp_receiver *control,
	struct sippymodem_v34_phase4_mp_channel_receiver *channel_receiver,
	struct sippymodem_qam_point *channel,
	const struct sippymodem_qam_point *drift, uint32_t *random,
	struct sippymodem_v34_mp_receive_event *captured)
{
	bool complete = false;

	*captured = (struct sippymodem_v34_mp_receive_event){ 0 };
	while (!complete) {
		struct sippymodem_v34_mp_receive_event event;
		struct sippymodem_qam_point transmitted;
		struct sippymodem_qam_point received;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		CHECK(sippymodem_v34_phase4_mp_next(
			      transmitter, &transmitted, &quarter_point,
			      &clockwise_quarter_turns, &complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_OK,
		      "flat-channel Phase 4 transmitter stopped early");
		received = pass_channel(
			channel, drift, &transmitted, random);
		if (!receive_pair(
			    control, channel_receiver, &transmitted, &received,
			    &event))
			return false;
		if (event.type != SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT) {
			CHECK(captured->type ==
				      SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT,
			      "flat-channel MP record emitted multiple events");
			*captured = event;
		}
	}
	return true;
}

static bool e_receivers_are_equal(
	const struct sippymodem_v34_phase4_e_receiver *left,
	const struct sippymodem_v34_phase4_e_receiver *right)
{
	return left->descrambler.history == right->descrambler.history &&
	       left->descrambler.polynomial == right->descrambler.polynomial &&
	       left->transmitting_role == right->transmitting_role &&
	       left->constellation == right->constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->received_bits == right->received_bits;
}

static bool e_channel_receivers_are_equal(
	const struct sippymodem_v34_phase4_e_channel_receiver *left,
	const struct sippymodem_v34_phase4_e_channel_receiver *right)
{
	return e_receivers_are_equal(
		       &left->symbol_receiver, &right->symbol_receiver) &&
	       left->gain.channel_gain.in_phase ==
		       right->gain.channel_gain.in_phase &&
	       left->gain.channel_gain.quadrature ==
		       right->gain.channel_gain.quadrature &&
	       left->gain.adaptation == right->gain.adaptation;
}

static bool receive_e_sequence(
	const struct sippymodem_v34_phase4_mp *mp,
	const struct sippymodem_v34_phase4_mp_receiver *control,
	const struct sippymodem_v34_phase4_mp_channel_receiver *mp_receiver,
	struct sippymodem_qam_point *channel,
	const struct sippymodem_qam_point *drift, uint32_t *random)
{
	struct sippymodem_v34_phase4_e_sequence transmitter;
	struct sippymodem_v34_phase4_e_receiver normalized_receiver;
	struct sippymodem_v34_phase4_e_channel_receiver channel_receiver;

	CHECK(sippymodem_v34_phase4_e_sequence_init_from_mp(
		      &transmitter, mp) == SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK &&
	      sippymodem_v34_phase4_e_receiver_init_from_mp(
		      &normalized_receiver, control) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK &&
	      sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
		      &channel_receiver, mp_receiver) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK,
	      "flat-channel E transition did not initialize");
	while (!sippymodem_v34_phase4_e_sequence_is_complete(&transmitter)) {
		struct sippymodem_qam_point transmitted;
		struct sippymodem_qam_point received;
		enum sippymodem_v34_phase4_e_receiver_result expected;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		CHECK(sippymodem_v34_phase4_e_sequence_next(
			      &transmitter, &transmitted, &quarter_point,
			      &clockwise_quarter_turns) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
		      "flat-channel E transmitter stopped early");
		received = pass_channel(channel, drift, &transmitted, random);
		expected = sippymodem_v34_phase4_e_sequence_is_complete(
				   &transmitter) ?
				   SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE :
				   SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK;
		CHECK(sippymodem_v34_phase4_e_receive_decision(
			      &normalized_receiver, quarter_point,
			      clockwise_quarter_turns) == expected &&
		      sippymodem_v34_phase4_e_channel_receive_point(
			      &channel_receiver, &received) == expected &&
		      e_receivers_are_equal(
			      &normalized_receiver,
			      &channel_receiver.symbol_receiver),
		      "flat-channel E receiver changed a symbol decision");
	}
	CHECK(sippymodem_v34_phase4_e_channel_receiver_is_valid(
		      &channel_receiver) &&
	      sippymodem_v34_phase4_e_receiver_is_complete(
		      &channel_receiver.symbol_receiver) &&
	      fabs(channel_receiver.gain.channel_gain.in_phase -
		   channel->in_phase) < 0.01 &&
	      fabs(channel_receiver.gain.channel_gain.quadrature -
		   channel->quadrature) < 0.01,
	      "flat-channel E receiver lost channel continuity");
	{
		struct sippymodem_v34_phase4_e_channel_receiver saved =
			channel_receiver;
		struct sippymodem_qam_point ignored = { 0 };

		CHECK(sippymodem_v34_phase4_e_channel_receive_point(
			      &channel_receiver, &ignored) ==
			      SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE &&
		      e_channel_receivers_are_equal(&channel_receiver, &saved),
		      "completed flat-channel E receiver consumed a point");
	}
	return true;
}

static bool check_channel_fixture(
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation)
{
	const struct sippymodem_qam_point drift = {
		.in_phase = 1.000001,
		.quadrature = role == SIPPYMODEM_V34_CALLING_STATION ?
				      0.000018 : -0.000016,
	};
	struct sippymodem_v34_mp_parameters parameters = test_parameters();
	struct sippymodem_v34_phase4_trn trn;
	struct sippymodem_v34_phase4_mp transmitter;
	struct sippymodem_v34_phase4_mp_receiver control;
	struct sippymodem_v34_phase4_mp_channel_receiver receiver;
	struct sippymodem_v34_mp_receive_event event;
	struct sippymodem_qam_point channel = { 0.78, -0.26 };
	struct sippymodem_qam_point transmitted;
	struct sippymodem_qam_point received;
	uint32_t random = UINT32_C(0x34040000) + (uint32_t)role +
			  (uint32_t)constellation;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	CHECK(sippymodem_v34_phase4_trn_init(&trn, role, constellation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK &&
	      sippymodem_v34_phase4_trn_next(
		      &trn, &transmitted, &quarter_point,
		      &clockwise_quarter_turns) == SIPPYMODEM_V34_PHASE4_TRN_OK,
	      "flat-channel fixture did not generate its reference");
	received = complex_product(&channel, &transmitted);
	CHECK(sippymodem_v34_phase4_mp_receiver_init(
		      &control, role, constellation) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      sippymodem_v34_phase4_mp_channel_receiver_init(
		      &receiver, role, constellation, &received,
		      &transmitted) == SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      receive_pair(
		      &control, &receiver, &transmitted, &received, &event),
	      "flat-channel fixture did not initialize");
	for (size_t symbol = 1; symbol < TEST_TRN_SYMBOLS; ++symbol) {
		CHECK(sippymodem_v34_phase4_trn_next(
			      &trn, &transmitted, &quarter_point,
			      &clockwise_quarter_turns) ==
			      SIPPYMODEM_V34_PHASE4_TRN_OK,
		      "flat-channel TRN stopped at symbol %zu", symbol);
		received = pass_channel(
			&channel, &drift, &transmitted, &random);
		if (!receive_pair(
			    &control, &receiver, &transmitted, &received, &event))
			return false;
	}
	CHECK(sippymodem_v34_phase4_mp_init_type0_from_trn(
		      &transmitter, &trn, &parameters) ==
		      SIPPYMODEM_V34_PHASE4_MP_OK &&
	      receive_record(
		      &transmitter, &control, &receiver, &channel, &drift,
		      &random, &event) &&
	      sippymodem_v34_phase4_mp_start_next_type0_record(
		      &transmitter, &parameters) == SIPPYMODEM_V34_PHASE4_MP_OK &&
	      receive_record(
		      &transmitter, &control, &receiver, &channel, &drift,
		      &random, &event) &&
	      receive_e_sequence(
		      &transmitter, &control, &receiver, &channel, &drift,
		      &random),
	      "flat-channel Phase 4 stream did not complete");
	CHECK(event.type == SIPPYMODEM_V34_MP_RECEIVE_TYPE0 &&
	      event.frame_result == SIPPYMODEM_V34_MP_OK &&
	      events_are_equal(
		      &event,
		      &(struct sippymodem_v34_mp_receive_event){
			      .type = SIPPYMODEM_V34_MP_RECEIVE_TYPE0,
			      .frame_result = SIPPYMODEM_V34_MP_OK,
			      .parameters.type0 = parameters,
		      }) &&
	      sippymodem_v34_phase4_mp_channel_receiver_is_valid(&receiver),
	      "flat-channel fixture changed its MP record");
	return true;
}

static bool test_drifting_channel(void)
{
	static const enum sippymodem_v34_station_role roles[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role)
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(constellations);
		     ++constellation)
			if (!check_channel_fixture(
				    roles[role], constellations[constellation]))
				return false;
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase4_mp_channel_receiver receiver;
	struct sippymodem_v34_phase4_mp_channel_receiver saved;
	struct sippymodem_v34_mp_receive_event event;
	struct sippymodem_v34_mp_receive_event saved_event;
	struct sippymodem_qam_point transmitted;
	struct sippymodem_qam_point gain = { 0.8, -0.2 };
	struct sippymodem_qam_point received;
	struct sippymodem_qam_point zero = { 0 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };

	CHECK(sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 0, 0,
		      &transmitted) == SIPPYMODEM_V34_CONSTELLATION_OK,
	      "flat-channel validation reference was not generated");
	received = complex_product(&gain, &transmitted);
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(sippymodem_v34_phase4_mp_channel_receiver_init(
		      NULL, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &received,
		      &transmitted) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receiver_init(
		      &receiver, (enum sippymodem_v34_station_role)2,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &received,
		      &transmitted) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receiver_init(
		      &receiver, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &zero,
		      &transmitted) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receiver_init(
		      &receiver, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &received, &zero) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid flat-channel initialization changed state");
	CHECK(sippymodem_v34_phase4_mp_channel_receiver_init(
		      &receiver, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &received,
		      &transmitted) == SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      sippymodem_v34_phase4_mp_channel_receiver_is_valid(&receiver) &&
	      !sippymodem_v34_phase4_mp_channel_receiver_is_valid(NULL),
	      "flat-channel validation fixture did not initialize");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	CHECK(sippymodem_v34_phase4_mp_channel_receive_point(
		      NULL, &received, &event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receive_point(
		      &receiver, NULL, &event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receive_point(
		      &receiver, &zero, &event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receive_point(
		      &receiver, &invalid, &event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_channel_receive_point(
		      &receiver, &received, NULL) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "invalid flat-channel point changed state or event");
	receiver = saved;
	receiver.gain.channel_gain = zero;
	CHECK(!sippymodem_v34_phase4_mp_channel_receiver_is_valid(&receiver),
	      "flat-channel receiver accepted a collapsed gain");
	receiver = saved;
	receiver.symbol_receiver.previous_clockwise_quarter_turns = 4;
	CHECK(!sippymodem_v34_phase4_mp_channel_receiver_is_valid(&receiver),
	      "flat-channel receiver accepted corrupt symbol state");
	return true;
}

static bool test_e_channel_validation(void)
{
	struct sippymodem_v34_phase4_mp_channel_receiver mp_receiver;
	struct sippymodem_v34_phase4_e_channel_receiver receiver;
	struct sippymodem_v34_phase4_e_channel_receiver saved;
	struct sippymodem_v34_mp_receive_event event;
	struct sippymodem_qam_point transmitted;
	struct sippymodem_qam_point gain = { 0.8, -0.2 };
	struct sippymodem_qam_point received;
	struct sippymodem_qam_point zero = { 0 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };

	CHECK(sippymodem_v34_training_constellation_point(
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, 0, 0,
		      &transmitted) == SIPPYMODEM_V34_CONSTELLATION_OK,
	      "E channel validation reference was not generated");
	received = complex_product(&gain, &transmitted);
	CHECK(sippymodem_v34_phase4_mp_channel_receiver_init(
		      &mp_receiver, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, &received,
		      &transmitted) == SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK,
	      "E channel validation MP receiver did not initialize");
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
		      NULL, &mp_receiver) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
		      &receiver, NULL) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
		      &receiver, &mp_receiver) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "E channel accepted a transition without a prior point");
	CHECK(sippymodem_v34_phase4_mp_channel_receive_point(
		      &mp_receiver, &received, &event) ==
		      SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK &&
	      sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
		      &receiver, &mp_receiver) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK &&
	      sippymodem_v34_phase4_e_channel_receiver_is_valid(&receiver) &&
	      !sippymodem_v34_phase4_e_channel_receiver_is_valid(NULL),
	      "E channel validation receiver did not initialize");
	saved = receiver;
	CHECK(sippymodem_v34_phase4_e_channel_receive_point(NULL, &received) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_channel_receive_point(
		      &receiver, NULL) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_channel_receive_point(
		      &receiver, &zero) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_channel_receive_point(
		      &receiver, &invalid) ==
		      SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      e_channel_receivers_are_equal(&receiver, &saved),
	      "invalid E channel point changed receiver state");
	receiver = saved;
	receiver.gain.channel_gain = zero;
	CHECK(!sippymodem_v34_phase4_e_channel_receiver_is_valid(&receiver),
	      "E channel receiver accepted a collapsed gain");
	CHECK(strcmp(sippymodem_v34_training_channel_result_name(
		     SIPPYMODEM_V34_TRAINING_CHANNEL_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_training_channel_result_name(
		     SIPPYMODEM_V34_TRAINING_CHANNEL_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_training_channel_result_name(
		     (enum sippymodem_v34_training_channel_result)99),
		     "unknown") == 0,
	      "training-channel result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "drifting flat channel", test_drifting_channel },
		{ "validation", test_validation },
		{ "E channel validation", test_e_channel_validation },
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
