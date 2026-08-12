// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_j_equalizer_receiver.h>
#include <v34_phase3_pp_receiver.h>
#include <v34_phase3_pp_sequence.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum { CHANNEL_TAPS = 3 };

struct test_channel {
	struct qam_point history[CHANNEL_TAPS];
};

struct receive_fixture {
	struct test_channel channel;
	struct v34_phase3_trn trn;
	struct v34_equalizer_handoff pp_handoff;
};

static const struct qam_point CHANNEL[CHANNEL_TAPS] = {
	{ 0.92, 0.17 },
	{ 0.12, -0.06 },
	{ -0.04, 0.03 },
};

static struct qam_point multiply(
	const struct qam_point *left, const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static struct qam_point channel_point(
	struct test_channel *channel, const struct qam_point *transmitted)
{
	struct qam_point received = { 0 };

	for (size_t index = CHANNEL_TAPS - 1; index != 0; --index)
		channel->history[index] = channel->history[index - 1];
	channel->history[0] = *transmitted;
	for (size_t index = 0; index < CHANNEL_TAPS; ++index) {
		struct qam_point term = multiply(
			&CHANNEL[index], &channel->history[index]);

		received.in_phase += term.in_phase;
		received.quadrature += term.quadrature;
	}
	return received;
}

static bool prepare_fixture(
	struct receive_fixture *fixture, enum v34_station_role role)
{
	struct v34_phase3_pp_receiver receiver;
	struct v34_phase3_pp_sequence pp;
	struct qam_point transmitted;
	uint8_t rotation;

	memset(fixture, 0, sizeof(*fixture));
	if (v34_phase3_pp_receiver_init(&receiver, 0.2) !=
		    V34_PHASE3_PP_RECEIVER_OK ||
	    v34_phase3_pp_sequence_init(&pp) != V34_PHASE3_PP_SEQUENCE_OK ||
	    v34_phase3_trn_init(&fixture->trn, role) != V34_PHASE3_TRN_OK)
		return false;
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point received;

		if (v34_phase3_pp_sequence_next(&pp, &transmitted) !=
			    V34_PHASE3_PP_SEQUENCE_OK)
			return false;
		received = channel_point(&fixture->channel, &transmitted);
		if (v34_phase3_pp_receive_point(&receiver, &received) !=
		    V34_PHASE3_PP_RECEIVER_OK)
			return false;
	}
	for (unsigned int symbol = 0; symbol < QAM_EQUALIZER_DELAY; ++symbol) {
		struct qam_point received;

		if (v34_phase3_trn_next(
			    &fixture->trn, &transmitted, &rotation) !=
			    V34_PHASE3_TRN_OK)
			return false;
		received = channel_point(&fixture->channel, &transmitted);
		if (v34_phase3_pp_receive_point(&receiver, &received) !=
		    V34_PHASE3_PP_RECEIVER_OK)
			return false;
	}
	return v34_phase3_pp_receiver_is_complete(&receiver) &&
	       v34_phase3_pp_receiver_handoff(
		       &receiver, &fixture->pp_handoff) ==
		       V34_PHASE3_PP_RECEIVER_OK;
}

static bool receive_point(
	struct v34_phase3_j_equalizer_receiver *receiver,
	struct test_channel *channel, const struct qam_point *transmitted,
	enum v34_phase3_j_equalizer_receiver_result expected)
{
	struct qam_point received = channel_point(channel, transmitted);

	return v34_phase3_j_equalizer_receive_point(receiver, &received) ==
	       expected;
}

static bool run_stream(
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	unsigned int trn_symbols)
{
	struct receive_fixture fixture;
	struct v34_phase3_j_equalizer_receiver receiver;
	struct v34_phase3_j_sequence j;
	struct v34_phase3_j_sequence detection_boundary;
	struct v34_phase3_j_handoff handoff;
	struct qam_point retained[QAM_EQUALIZER_DELAY];
	struct qam_point transmitted;
	uint8_t rotation;

	CHECK(prepare_fixture(&fixture, role) &&
	      v34_phase3_j_equalizer_receiver_init(
		      &receiver, &fixture.pp_handoff, role, 0.05) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_OK,
	      "role %d Phase 3 equalizer fixture did not initialize", role);
	for (unsigned int symbol = QAM_EQUALIZER_DELAY;
	     symbol < trn_symbols; ++symbol) {
		CHECK(v34_phase3_trn_next(
			      &fixture.trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK &&
		      receive_point(
			      &receiver, &fixture.channel, &transmitted,
			      V34_PHASE3_J_EQUALIZER_RECEIVER_OK),
		      "role %d TRN point %u failed", role, symbol);
	}
	CHECK(v34_phase3_j_sequence_init_from_trn(
		      &j, &fixture.trn, constellation) ==
		      V34_PHASE3_J_SEQUENCE_OK,
	      "role %d J source did not initialize", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS +
			      QAM_EQUALIZER_DELAY;
	     ++symbol) {
		enum v34_phase3_j_equalizer_receiver_result expected =
			symbol + 1u ==
				V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS +
					QAM_EQUALIZER_DELAY ?
				V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED :
				V34_PHASE3_J_EQUALIZER_RECEIVER_OK;

		CHECK(v34_phase3_j_sequence_next(
			      &j, &transmitted, &rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK,
		      "role %d J point %u did not generate", role, symbol);
		if (symbol + 1u == V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
			detection_boundary = j;
		if (symbol >= V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
			retained[symbol -
				 V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS] =
				channel_point(&fixture.channel, &transmitted);
		else
			CHECK(receive_point(
				      &receiver, &fixture.channel, &transmitted,
				      expected),
			      "role %d J point %u returned early", role, symbol);
		if (symbol >= V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS) {
			CHECK(v34_phase3_j_equalizer_receive_point(
				      &receiver,
				      &retained[symbol -
					 V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS]) ==
				      expected,
			      "role %d retained J point %u failed", role, symbol);
		}
	}
	CHECK(v34_phase3_j_equalizer_receiver_is_complete(&receiver) &&
	      v34_phase3_j_equalizer_receiver_handoff(
		      &receiver, &handoff) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_OK &&
	      v34_phase3_j_handoff_is_valid(&handoff) &&
	      handoff.detection.trn_symbols == trn_symbols &&
	      handoff.detection.errors == 0 &&
	      handoff.detection.sequence.remote_constellation == constellation &&
	      memcmp(&handoff.detection.sequence, &detection_boundary,
		     sizeof(detection_boundary)) == 0,
	      "role %d J handoff lost its detected boundary", role);
	{
		struct v34_phase3_j_equalizer_receiver complete = receiver;

		CHECK(v34_phase3_j_equalizer_receive_point(
			      &receiver, &retained[0]) ==
			      V34_PHASE3_J_EQUALIZER_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
		      "role %d completed receiver consumed another point", role);
	}
	for (size_t index = 0; index < ARRAY_LENGTH(retained); ++index) {
		size_t history = ARRAY_LENGTH(retained) - index - 1u;

		CHECK(handoff.equalizer.equalizer.history[index].in_phase ==
			      retained[history].in_phase &&
		      handoff.equalizer.equalizer.history[index].quadrature ==
			      retained[history].quadrature,
		      "role %d J handoff reordered raw lookahead", role);
	}
	{
		struct qam_point received;
		struct qam_point output;
		struct qam_point expected_point;
		uint8_t quarter_point;
		uint8_t output_rotation;
		uint8_t expected_rotation;
		bool ready;

		CHECK(v34_phase3_j_sequence_next(
			      &j, &transmitted, &rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK,
		      "role %d J continuation did not generate", role);
		received = channel_point(&fixture.channel, &transmitted);
		CHECK(v34_phase3_j_sequence_next(
			      &handoff.detection.sequence, &expected_point,
			      &expected_rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK &&
		      qam_equalizer_push(
			      &handoff.equalizer.equalizer, &received, &output,
			      &ready) == QAM_EQUALIZER_OK &&
		      ready &&
		      v34_training_constellation_slice(
			      V34_CONSTELLATION_4_POINT, &output,
			      &quarter_point, &output_rotation) ==
			      V34_CONSTELLATION_OK &&
		      quarter_point == 0 && output_rotation == expected_rotation,
		      "role %d delayed handoff did not produce the next J decision",
		      role);
	}
	return true;
}

static bool test_generated_channel_timeline(void)
{
	CHECK(run_stream(
		      V34_CALLING_STATION, V34_CONSTELLATION_4_POINT, 613) &&
	      run_stream(
		      V34_ANSWERING_STATION, V34_CONSTELLATION_16_POINT, 777),
	      "generated PP/TRN/J channel timeline failed");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct receive_fixture fixture;
	struct v34_phase3_j_equalizer_receiver receiver;
	struct v34_phase3_j_equalizer_receiver saved;
	struct v34_phase3_j_handoff handoff;
	struct v34_phase3_j_handoff saved_handoff;
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point excessive = { DBL_MAX, DBL_MAX };

	CHECK(prepare_fixture(&fixture, V34_CALLING_STATION),
	      "validation PP fixture did not initialize");
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase3_j_equalizer_receiver_init(
		      NULL, &fixture.pp_handoff, V34_CALLING_STATION, 0.05) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receiver_init(
		      &receiver, NULL, V34_CALLING_STATION, 0.05) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receiver_init(
		      &receiver, &fixture.pp_handoff,
		      (enum v34_station_role)99, 0.05) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receiver_init(
		      &receiver, &fixture.pp_handoff,
		      V34_CALLING_STATION, NAN) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid composed receiver initialization changed state");
	CHECK(v34_phase3_j_equalizer_receiver_init(
		      &receiver, &fixture.pp_handoff,
		      V34_CALLING_STATION, 0.05) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_OK,
	      "valid composed receiver did not initialize");
	saved = receiver;
	CHECK(v34_phase3_j_equalizer_receive_point(NULL, &invalid) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receive_point(&receiver, NULL) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receive_point(&receiver, &invalid) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receive_point(&receiver, &excessive) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "rejected composed point changed receiver state");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v34_phase3_j_equalizer_receiver_handoff(
		      &receiver, &handoff) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0 &&
	      v34_phase3_j_equalizer_receiver_handoff(NULL, &handoff) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_equalizer_receiver_handoff(&receiver, NULL) ==
		      V34_PHASE3_J_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      !v34_phase3_j_handoff_is_valid(NULL),
	      "composed handoff was not transactional");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++symbol) {
		struct qam_point transmitted;
		uint8_t rotation;

		CHECK(v34_phase3_trn_next(
			      &fixture.trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK &&
		      receive_point(
			      &receiver, &fixture.channel, &transmitted,
			      V34_PHASE3_J_EQUALIZER_RECEIVER_OK),
		      "saturation fixture stopped at %u", symbol);
	}
	receiver.detector.received_symbols = UINT32_MAX;
	receiver.detector.next_decision = (uint8_t)(
		UINT32_MAX % V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS);
	CHECK(v34_phase3_j_equalizer_receiver_is_valid(&receiver),
	      "saturated composed receiver was not valid");
	saved = receiver;
	{
		struct qam_point transmitted;
		struct qam_point received;
		uint8_t rotation;

		CHECK(v34_phase3_trn_next(
			      &fixture.trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK,
		      "saturated fixture continuation did not generate");
		received = channel_point(&fixture.channel, &transmitted);
		CHECK(v34_phase3_j_equalizer_receive_point(
			      &receiver, &received) ==
			      V34_PHASE3_J_EQUALIZER_RECEIVER_OUT_OF_RANGE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "saturated composed receiver consumed a point");
	}
	saved = receiver;
	saved.equalizer.can_adapt = true;
	CHECK(!v34_phase3_j_equalizer_receiver_is_valid(&saved) &&
	      !v34_phase3_j_equalizer_receiver_is_complete(&saved) &&
	      !v34_phase3_j_detection_is_valid(&handoff.detection) &&
	      strcmp(v34_phase3_j_equalizer_receiver_result_name(
			     V34_PHASE3_J_EQUALIZER_RECEIVER_DETECTED),
		     "detected") == 0 &&
	      strcmp(v34_phase3_j_equalizer_receiver_result_name(
			     V34_PHASE3_J_EQUALIZER_RECEIVER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_phase3_j_equalizer_receiver_result_name(
			     (enum v34_phase3_j_equalizer_receiver_result)99),
		     "unknown") == 0,
	      "composed validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated channel timeline", test_generated_channel_timeline },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
