// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v34_phase3_j_sequence.h>
#include <v34_phase3_pp_sequence.h>
#include <v34_phase3_receiver.h>
#include <v34_phase3_s_sequence.h>
#include <v34_phase3_trn.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum { CHANNEL_TAPS = 3 };

struct test_channel {
	struct qam_point history[CHANNEL_TAPS];
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

static struct v34_phase3_receiver_configuration configuration(
	enum v34_station_role role, uint32_t md_symbols)
{
	return (struct v34_phase3_receiver_configuration){
		.transmitting_role = role,
		.md_symbols = md_symbols,
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static enum v34_phase3_receiver_result receive_point(
	struct v34_phase3_receiver *receiver,
	struct test_channel *channel, const struct qam_point *transmitted)
{
	struct qam_point received = channel_point(channel, transmitted);

	return v34_phase3_receive_point(receiver, &received);
}

static bool run_timeline(
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	unsigned int trn_symbols, uint32_t md_symbols)
{
	struct v34_phase3_receiver_configuration config =
		configuration(role, md_symbols);
	struct v34_phase3_receiver receiver;
	struct v34_phase3_s_sequence s;
	struct v34_phase3_pp_sequence pp;
	struct v34_phase3_trn trn;
	struct v34_phase3_j_sequence j;
	struct v34_phase3_j_sequence detection_boundary;
	struct v34_phase3_handoff handoff;
	struct v34_phase3_s_detection s_detection;
	struct v34_phase3_s_detection first_s_detection;
	struct test_channel channel = { 0 };
	struct qam_point transmitted;
	uint8_t rotation;

	CHECK(v34_phase3_receiver_init(&receiver, &config) ==
		      V34_PHASE3_RECEIVER_OK &&
	      v34_phase3_s_sequence_init(&s) ==
		      V34_PHASE3_S_SEQUENCE_OK &&
	      v34_phase3_pp_sequence_init(&pp) ==
		      V34_PHASE3_PP_SEQUENCE_OK &&
	      v34_phase3_trn_init(&trn, role) == V34_PHASE3_TRN_OK,
	      "role %d Phase 3 timeline did not initialize", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		enum v34_phase3_s_section section;
		enum v34_phase3_receiver_result expected =
			symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
				V34_PHASE3_RECEIVER_S_DETECTED :
				V34_PHASE3_RECEIVER_OK;

		CHECK(v34_phase3_s_sequence_next(
			      &s, &transmitted, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      receive_point(&receiver, &channel, &transmitted) ==
			      expected,
		      "role %d S/S-bar point %u did not cross its boundary",
		      role, symbol);
	}
	CHECK(v34_phase3_receiver_get_s_detection(
		      &receiver, &first_s_detection) &&
	      v34_phase3_s_detection_is_valid(&first_s_detection),
	      "role %d S/S-bar result was not retained", role);
	if (md_symbols != 0) {
		struct v34_phase3_receiver saved = receiver;
		struct qam_point invalid = { NAN, 0.0 };

		CHECK(receiver.stage == V34_PHASE3_RECEIVER_STAGE_MD,
		      "role %d did not enter its MD interval", role);
		CHECK(v34_phase3_receive_point(&receiver, &invalid) ==
			      V34_PHASE3_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "role %d invalid MD point changed its interval", role);
		for (uint32_t symbol = 0; symbol < md_symbols; ++symbol) {
			int in_phase = (int)(symbol % 11u) - 5;
			int quadrature = (int)((symbol * 3u) % 13u) - 6;
			enum v34_phase3_receiver_result expected =
				symbol + 1u == md_symbols ?
					V34_PHASE3_RECEIVER_MD_COMPLETE :
					V34_PHASE3_RECEIVER_OK;

			transmitted = (struct qam_point){
				.in_phase = in_phase / 7.0,
				.quadrature = quadrature / 8.0,
			};
			CHECK(receive_point(
				      &receiver, &channel, &transmitted) ==
				      expected,
			      "role %d MD point %u failed", role, symbol);
		}
		CHECK(receiver.stage ==
			      V34_PHASE3_RECEIVER_STAGE_REPEATED_S &&
		      v34_phase3_s_sequence_init(&s) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "role %d did not begin repeated S/S-bar", role);
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
			enum v34_phase3_s_section section;
			enum v34_phase3_receiver_result expected =
				symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
					V34_PHASE3_RECEIVER_REPEATED_S_DETECTED :
					V34_PHASE3_RECEIVER_OK;

			CHECK(v34_phase3_s_sequence_next(
				      &s, &transmitted, &section) ==
				      V34_PHASE3_S_SEQUENCE_OK &&
			      receive_point(
				      &receiver, &channel, &transmitted) ==
				      expected,
			      "role %d repeated S/S-bar point %u failed",
			      role, symbol);
		}
	} else {
		CHECK(receiver.stage == V34_PHASE3_RECEIVER_STAGE_PP,
		      "role %d zero-MD path did not enter PP", role);
	}
	CHECK(receiver.stage == V34_PHASE3_RECEIVER_STAGE_PP &&
	      v34_phase3_receiver_get_s_detection(
		      &receiver, &s_detection) &&
	      v34_phase3_s_detection_is_valid(&s_detection),
	      "role %d training S/S-bar result was not retained", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		CHECK(v34_phase3_pp_sequence_next(&pp, &transmitted) ==
			      V34_PHASE3_PP_SEQUENCE_OK &&
		      receive_point(&receiver, &channel, &transmitted) ==
			      V34_PHASE3_RECEIVER_OK,
		      "role %d PP point %u failed", role, symbol);
	}
	for (unsigned int symbol = 0; symbol < trn_symbols; ++symbol) {
		enum v34_phase3_receiver_result expected =
			symbol + 1u == QAM_EQUALIZER_DELAY ?
				V34_PHASE3_RECEIVER_PP_COMPLETE :
				V34_PHASE3_RECEIVER_OK;

		CHECK(v34_phase3_trn_next(
			      &trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK &&
		      receive_point(&receiver, &channel, &transmitted) ==
			      expected,
		      "role %d TRN point %u failed", role, symbol);
		if (symbol + 1u == QAM_EQUALIZER_DELAY)
			CHECK(receiver.stage ==
				      V34_PHASE3_RECEIVER_STAGE_J,
			      "role %d did not enter J search", role);
	}
	CHECK(v34_phase3_j_sequence_init_from_trn(
		      &j, &trn, constellation) ==
		      V34_PHASE3_J_SEQUENCE_OK,
	      "role %d J source did not initialize", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS +
			      QAM_EQUALIZER_DELAY;
	     ++symbol) {
		enum v34_phase3_receiver_result expected =
			symbol + 1u ==
				V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS +
					QAM_EQUALIZER_DELAY ?
				V34_PHASE3_RECEIVER_J_DETECTED :
				V34_PHASE3_RECEIVER_OK;

		CHECK(v34_phase3_j_sequence_next(
			      &j, &transmitted, &rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK,
		      "role %d J source stopped at %u", role, symbol);
		if (symbol + 1u == V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS)
			detection_boundary = j;
		CHECK(receive_point(&receiver, &channel, &transmitted) ==
			      expected,
		      "role %d J point %u did not cross its boundary",
		      role, symbol);
	}
	CHECK(v34_phase3_receiver_is_complete(&receiver) &&
	      v34_phase3_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_RECEIVER_OK &&
	      v34_phase3_handoff_is_valid(&handoff) &&
	      memcmp(&handoff.s, &s_detection, sizeof(s_detection)) == 0 &&
	      handoff.j.detection.trn_symbols == trn_symbols &&
	      handoff.j.detection.errors == 0 &&
	      handoff.j.detection.sequence.remote_constellation ==
		      constellation &&
	      memcmp(&handoff.j.detection.sequence, &detection_boundary,
		     sizeof(detection_boundary)) == 0,
	      "role %d Phase 3 handoff lost its exact boundary", role);
	{
		struct v34_phase3_receiver corrupt = receiver;

		corrupt.configuration.transmitting_role =
			role == V34_CALLING_STATION ? V34_ANSWERING_STATION :
						       V34_CALLING_STATION;
		CHECK(!v34_phase3_receiver_is_valid(&corrupt),
		      "role %d handoff accepted conflicting ownership", role);
	}
	{
		struct v34_phase3_receiver complete = receiver;

		CHECK(v34_phase3_receive_point(&receiver, &transmitted) ==
			      V34_PHASE3_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
		      "role %d completed receiver consumed another point", role);
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
			      V34_PHASE3_J_SEQUENCE_OK &&
		      v34_phase3_j_sequence_next(
			      &handoff.j.detection.sequence, &expected_point,
			      &expected_rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK,
		      "role %d delayed J continuation did not generate", role);
		received = channel_point(&channel, &transmitted);
		CHECK(qam_equalizer_push(
			      &handoff.j.equalizer.equalizer, &received,
			      &output, &ready) == QAM_EQUALIZER_OK &&
		      ready &&
		      v34_training_constellation_slice(
			      V34_CONSTELLATION_4_POINT, &output,
			      &quarter_point, &output_rotation) ==
			      V34_CONSTELLATION_OK &&
		      quarter_point == 0 &&
		      output_rotation == expected_rotation,
		      "role %d handoff did not produce the next J decision",
		      role);
	}
	return true;
}

static bool test_generated_continuous_timeline(void)
{
	CHECK(run_timeline(
		      V34_CALLING_STATION, V34_CONSTELLATION_4_POINT, 613, 0) &&
	      run_timeline(
		      V34_ANSWERING_STATION, V34_CONSTELLATION_16_POINT, 777,
		      137),
	      "generated Phase 3 timelines failed");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_receiver_configuration config =
		configuration(V34_CALLING_STATION, 0);
	struct v34_phase3_receiver receiver;
	struct v34_phase3_receiver saved;
	struct v34_phase3_receiver corrupt;
	struct v34_phase3_handoff handoff;
	struct v34_phase3_handoff saved_handoff;
	struct v34_phase3_s_detection detection = { 3, 4, 1 };
	struct v34_phase3_s_detection saved_detection = detection;
	struct qam_point zero = { 0 };
	struct qam_point invalid = { NAN, 0.0 };

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(!v34_phase3_receiver_configuration_is_valid(NULL),
	      "NULL Phase 3 configuration was valid");
	config.transmitting_role = (enum v34_station_role)99;
	CHECK(!v34_phase3_receiver_configuration_is_valid(&config) &&
	      v34_phase3_receiver_init(&receiver, &config) ==
		      V34_PHASE3_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid Phase 3 role changed destination state");
	config = configuration(V34_CALLING_STATION, 0);
	config.s_gain_adaptation = NAN;
	CHECK(!v34_phase3_receiver_configuration_is_valid(&config),
	      "non-finite Phase 3 adaptation was valid");
	config = configuration(V34_CALLING_STATION, 0);
	config.md_symbols = V34_PHASE3_RECEIVER_MAX_MD_SYMBOLS;
	CHECK(v34_phase3_receiver_configuration_is_valid(&config),
	      "maximum Phase 3 MD interval was invalid");
	config.md_symbols = V34_PHASE3_RECEIVER_MAX_MD_SYMBOLS + 1u;
	CHECK(!v34_phase3_receiver_configuration_is_valid(&config),
	      "oversized Phase 3 MD interval was valid");
	config = configuration(V34_CALLING_STATION, 0);
	CHECK(v34_phase3_receiver_init(&receiver, &config) ==
		      V34_PHASE3_RECEIVER_OK &&
	      v34_phase3_receiver_is_valid(&receiver) &&
	      !v34_phase3_receiver_is_complete(&receiver),
	      "valid Phase 3 receiver did not initialize");
	saved = receiver;
	CHECK(v34_phase3_receive_point(NULL, &zero) ==
		      V34_PHASE3_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_receive_point(&receiver, NULL) ==
		      V34_PHASE3_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_receive_point(&receiver, &invalid) ==
		      V34_PHASE3_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_receive_point(&receiver, &zero) ==
		      V34_PHASE3_RECEIVER_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "rejected Phase 3 points changed receiver state");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(!v34_phase3_receiver_get_s_detection(
		      &receiver, &detection) &&
	      memcmp(&detection, &saved_detection, sizeof(detection)) == 0 &&
	      v34_phase3_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_RECEIVER_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "Phase 3 receiver exported an incomplete boundary");
	corrupt = receiver;
	corrupt.stage = (enum v34_phase3_receiver_stage)99;
	CHECK(!v34_phase3_receiver_is_valid(NULL) &&
	      !v34_phase3_receiver_is_valid(&corrupt) &&
	      !v34_phase3_handoff_is_valid(NULL) &&
	      !v34_phase3_s_detection_is_valid(NULL) &&
	      strcmp(v34_phase3_receiver_result_name(
		     V34_PHASE3_RECEIVER_PP_COMPLETE),
		     "PP complete") == 0 &&
	      strcmp(v34_phase3_receiver_result_name(
		     (enum v34_phase3_receiver_result)99),
		     "unknown") == 0,
	      "Phase 3 validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated continuous timeline",
		  test_generated_continuous_timeline },
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
