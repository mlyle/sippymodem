// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_equalizer_receiver.h>
#include <v34_phase3_s_sequence.h>
#include <v34_phase4_e_sequence.h>
#include <v34_phase4_mp.h>
#include <v34_phase4_trn.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	PRETRAINING_SYMBOLS = 1024,
	TRAINING_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 256,
	ANSWER_S_DRAIN_SYMBOLS = 40,
	CHANNEL_TAPS = 3,
};

struct fixture_channel {
	struct qam_point history[CHANNEL_TAPS];
	struct qam_point rotation;
};

struct receive_fixture {
	struct v34_phase4_equalizer_receiver receiver;
	struct fixture_channel channel;
	struct v34_mp_parameters first_parameters;
	struct v34_mp_parameters second_parameters;
	unsigned int answer_s_events;
	unsigned int accepted_records;
	unsigned int rejected_records;
};

static const struct qam_point CHANNEL[CHANNEL_TAPS] = {
	{ 0.95, 0.18 },
	{ 0.09, -0.04 },
	{ -0.03, 0.02 },
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
	struct fixture_channel *channel, const struct qam_point *transmitted)
{
	struct qam_point received = { 0 };
	struct qam_point rotated;

	for (size_t index = CHANNEL_TAPS - 1; index != 0; --index)
		channel->history[index] = channel->history[index - 1];
	channel->history[0] = *transmitted;
	for (size_t index = 0; index < CHANNEL_TAPS; ++index) {
		struct qam_point term = multiply(
			&CHANNEL[index], &channel->history[index]);

		received.in_phase += term.in_phase;
		received.quadrature += term.quadrature;
	}
	rotated = multiply(&received, &channel->rotation);
	return rotated;
}

static struct v34_mp_parameters parameters(bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis = V34_TRELLIS_32_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = acknowledges_peer,
		.data_rate_capability_mask = 0x1a55,
		.asymmetric_rates_enabled = true,
	};
}

static bool parameter_records_are_equal(
	const struct v34_mp_parameters *left,
	const struct v34_mp_parameters *right)
{
	uint8_t left_frame[V34_MP_TYPE0_FRAME_BYTES];
	uint8_t right_frame[V34_MP_TYPE0_FRAME_BYTES];

	return v34_mp_type0_encode(
		       left, left_frame, sizeof(left_frame)) == V34_MP_OK &&
	       v34_mp_type0_encode(
		       right, right_frame, sizeof(right_frame)) == V34_MP_OK &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static bool receive_point(
	struct receive_fixture *fixture,
	const struct qam_point *received,
	enum v34_phase4_equalizer_receiver_result *result)
{
	struct v34_mp_receive_event event;
	struct v34_mp_receive_event saved_event;

	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	*result = v34_phase4_equalizer_receive_point(
		&fixture->receiver, received, &event);
	if (*result == V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED)
		fixture->answer_s_events++;
	if (*result != V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY) {
		CHECK(memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "non-record result changed the MP event");
		return true;
	}
	{
		struct v34_phase4_equalizer_receiver paused =
			fixture->receiver;
		struct v34_mp_receive_event ignored = { 0 };

		CHECK(v34_phase4_equalizer_receive_point(
			      &fixture->receiver, received, &ignored) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE &&
		      memcmp(&fixture->receiver, &paused,
			     sizeof(paused)) == 0,
		      "pending MP record did not pause input transactionally");
	}
	if (event.type == V34_MP_RECEIVE_REJECTED) {
		fixture->rejected_records++;
		CHECK(v34_phase4_equalizer_receiver_continue_mp(
			      &fixture->receiver) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_OK,
		      "rejected MP record did not resume search");
		return true;
	}
	CHECK(event.type == V34_MP_RECEIVE_TYPE0 &&
	      event.frame_result == V34_MP_OK &&
	      fixture->accepted_records < 2,
	      "unexpected accepted MP record %u type=%d result=%d",
	      fixture->accepted_records, event.type, event.frame_result);
	if (fixture->accepted_records == 0) {
		CHECK(parameter_records_are_equal(
			      &event.parameters.type0,
			      &fixture->first_parameters) &&
		      v34_phase4_equalizer_receiver_continue_mp(
			      &fixture->receiver) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_OK,
		      "first MP record changed: rates=%u/%u ack=%u mask=0x%04x",
		      event.parameters.type0
			      .maximum_call_to_answer_rate_2400_bps_units,
		      event.parameters.type0
			      .maximum_answer_to_call_rate_2400_bps_units,
		      event.parameters.type0.acknowledges_peer_mp,
		      event.parameters.type0.data_rate_capability_mask);
	} else {
		CHECK(parameter_records_are_equal(
			      &event.parameters.type0,
			      &fixture->second_parameters) &&
		      v34_phase4_equalizer_receiver_start_e(
			      &fixture->receiver) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_OK,
		      "MP-prime record did not enter E");
	}
	fixture->accepted_records++;
	return true;
}

static bool transmit_point(
	struct receive_fixture *fixture,
	const struct qam_point *transmitted,
	enum v34_phase4_equalizer_receiver_result *result,
	struct qam_point *received)
{
	*received = channel_point(&fixture->channel, transmitted);
	return receive_point(fixture, received, result);
}

static bool run_stream(
	enum v34_station_role role,
	enum v34_constellation_size constellation, double phase4_rotation,
	bool corrupt_s_bar_transition)
{
	struct receive_fixture fixture = {
		.channel.rotation = { 1.0, 0.0 },
		.first_parameters = parameters(false),
		.second_parameters = parameters(true),
	};
	struct v34_phase4_trn trn;
	struct v34_phase3_s_sequence answer_s;
	struct v34_phase4_mp mp;
	struct v34_phase4_e_sequence e;
	struct qam_equalizer equalizer;
	struct qam_point target_history[QAM_EQUALIZER_MAX_TAP_COUNT] = { 0 };
	struct qam_point transmitted;
	struct qam_point received;
	struct qam_point lookahead[V34_EQUALIZER_DECISION_DELAY];
	enum v34_phase4_equalizer_receiver_result result;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	enum v34_phase3_s_section answer_s_section;
	bool record_complete;
	bool equalized_ready;
	bool trn_handoff_checked = role != V34_ANSWERING_STATION;

	CHECK(v34_phase4_trn_init(&trn, role, constellation) ==
		      V34_PHASE4_TRN_OK &&
	      v34_phase4_trn_next(
		      &trn, &transmitted, &quarter_point,
		      &clockwise_quarter_turns) == V34_PHASE4_TRN_OK,
	      "Phase 4 equalizer fixture did not start TRN");
	received = channel_point(&fixture.channel, &transmitted);
	CHECK(qam_equalizer_init_geometry(
		      &equalizer, &received, &transmitted,
		      V34_EQUALIZER_TAP_COUNT,
		      V34_EQUALIZER_DECISION_DELAY) ==
		      QAM_EQUALIZER_OK,
	      "Phase 4 equalizer fixture did not seed its equalizer");
	for (size_t symbol = 0; symbol < PRETRAINING_SYMBOLS; ++symbol) {
		struct qam_point output;

		if (symbol != 0) {
			CHECK(v34_phase4_trn_next(
				      &trn, &transmitted, &quarter_point,
				      &clockwise_quarter_turns) ==
				      V34_PHASE4_TRN_OK,
			      "known-symbol training stopped at %zu", symbol);
			received = channel_point(
				&fixture.channel, &transmitted);
		}
		for (size_t index = ARRAY_LENGTH(target_history) - 1;
		     index != 0; --index)
			target_history[index] = target_history[index - 1];
		target_history[0] = transmitted;
		CHECK(qam_equalizer_push(
			      &equalizer, &received, &output,
			      &equalized_ready) == QAM_EQUALIZER_OK &&
		      (!equalized_ready ||
		       qam_equalizer_adapt(
			       &equalizer,
			       &target_history[V34_EQUALIZER_DECISION_DELAY],
			       0.2) == QAM_EQUALIZER_OK),
		      "known-symbol equalizer training failed at %zu", symbol);
	}
	CHECK(v34_phase4_equalizer_receiver_init(
		      &fixture.receiver, &equalizer, role, constellation,
		      0.05) == V34_PHASE4_EQUALIZER_RECEIVER_OK,
	      "Phase 4 equalizer fixture did not transfer trained state");
	fixture.channel.rotation = (struct qam_point){
		.in_phase = cos(phase4_rotation),
		.quadrature = sin(phase4_rotation),
	};
	if (role == V34_ANSWERING_STATION) {
		static const struct qam_point zero;

		for (size_t symbol = 0; symbol < ANSWER_S_DRAIN_SYMBOLS;
		     ++symbol) {
			CHECK(transmit_point(
				      &fixture, &zero, &result, &received) &&
			      (result ==
				       V34_PHASE4_EQUALIZER_RECEIVER_WARMING ||
			       result ==
				       V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL),
			      "Phase 3 drain failed at symbol %zu", symbol);
		}
		CHECK(v34_phase3_s_sequence_init(&answer_s) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "Phase 4 answer marker did not initialize");
		while (!v34_phase3_s_sequence_is_complete(&answer_s)) {
			CHECK(v34_phase3_s_sequence_next(
				      &answer_s, &transmitted,
				      &answer_s_section) ==
				      V34_PHASE3_S_SEQUENCE_OK,
			      "Phase 4 answer marker generation failed");
			if (corrupt_s_bar_transition &&
			    answer_s_section == V34_PHASE3_S_BAR_SECTION &&
			    answer_s.emitted_symbols <=
				    V34_PHASE3_S_SYMBOLS + 2u)
				CHECK(v34_training_constellation_point(
					      V34_CONSTELLATION_4_POINT, 0,
					      (uint8_t)((answer_s.emitted_symbols - 1u) &
							1u),
					      &transmitted) ==
					      V34_CONSTELLATION_OK,
				      "Phase 4 answer marker corruption failed");
			CHECK(
			      transmit_point(
				      &fixture, &transmitted, &result,
				      &received) &&
			      result == V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL,
			      "Phase 4 answer marker failed before its boundary");
		}
	}
	CHECK(v34_phase4_trn_init(&trn, role, constellation) ==
		      V34_PHASE4_TRN_OK,
	      "fresh Phase 4 TRN did not initialize");
	for (size_t symbol = 0; symbol < TRAINING_SYMBOLS; ++symbol) {
		enum v34_phase4_equalizer_receiver_stage stage_before =
			fixture.receiver.stage;

		CHECK(v34_phase4_trn_next(
			      &trn, &transmitted, &quarter_point,
			      &clockwise_quarter_turns) ==
			      V34_PHASE4_TRN_OK &&
		      transmit_point(
			      &fixture, &transmitted, &result, &received) &&
		      (result == V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL ||
		       result ==
			       V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED ||
		       result ==
			       V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY),
		      "TRN receive failed at symbol %zu with %s", symbol,
		      v34_phase4_equalizer_receiver_result_name(result));
		if (stage_before ==
			    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN &&
		    fixture.receiver.stage ==
			    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP) {
			CHECK(memcmp(
				      &fixture.receiver.mp_receiver.descrambler,
				      &fixture.receiver.expected_trn.scrambler,
				      sizeof(fixture.receiver.expected_trn.scrambler)) ==
				      0 &&
			      fixture.receiver.mp_receiver.has_previous_symbol &&
			      fixture.receiver.mp_receiver
					      .previous_clockwise_quarter_turns ==
				      clockwise_quarter_turns &&
			      fixture.receiver.trn_error_symbols ==
				      fixture.receiver.trn_symbols -
					      fixture.receiver.answer_s
						      .post_marker_symbols &&
			      isfinite(fixture.receiver.trn_squared_error) &&
			      fixture.receiver.trn_squared_error >= 0.0,
			      "TRN state did not initialize MP decoding");
			trn_handoff_checked = true;
		}
	}
	CHECK(trn_handoff_checked && fixture.answer_s_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      fixture.receiver.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP,
	      "Phase 4 answer marker count=%u role=%d",
	      fixture.answer_s_events, role);
	CHECK(v34_phase4_mp_init_type0_from_trn(
		      &mp, &trn, &fixture.first_parameters) ==
		      V34_PHASE4_MP_OK,
	      "first MP record did not initialize");
	do {
		CHECK(v34_phase4_mp_next(
			      &mp, &transmitted, &quarter_point,
			      &clockwise_quarter_turns, &record_complete) ==
			      V34_PHASE4_MP_OK &&
		      transmit_point(
			      &fixture, &transmitted, &result, &received),
		      "first MP record receive failed");
	} while (!record_complete);
	/* The first MP record establishes record acquisition. Repeat the local
	 * parameters before MP-prime, as the handshake does until acknowledged. */
	CHECK(v34_phase4_mp_start_next_type0_record(
		      &mp, &fixture.first_parameters) == V34_PHASE4_MP_OK,
	      "repeated MP record did not initialize");
	do {
		CHECK(v34_phase4_mp_next(
			      &mp, &transmitted, &quarter_point,
			      &clockwise_quarter_turns, &record_complete) ==
			      V34_PHASE4_MP_OK &&
		      transmit_point(
			      &fixture, &transmitted, &result, &received),
		      "repeated MP record receive failed");
	} while (!record_complete);
	CHECK(v34_phase4_mp_start_next_type0_record(
		      &mp, &fixture.second_parameters) == V34_PHASE4_MP_OK,
	      "MP-prime record did not initialize");
	do {
		CHECK(v34_phase4_mp_next(
			      &mp, &transmitted, &quarter_point,
			      &clockwise_quarter_turns, &record_complete) ==
			      V34_PHASE4_MP_OK &&
		      transmit_point(
			      &fixture, &transmitted, &result, &received),
		      "MP-prime record receive failed");
	} while (!record_complete);
	CHECK(v34_phase4_e_sequence_init_from_mp(&e, &mp) ==
		      V34_PHASE4_E_SEQUENCE_OK,
	      "E sequence did not initialize");
	while (!v34_phase4_e_sequence_is_complete(&e)) {
		CHECK(v34_phase4_e_sequence_next(
			      &e, &transmitted, &quarter_point,
			      &clockwise_quarter_turns) ==
			      V34_PHASE4_E_SEQUENCE_OK &&
		      transmit_point(
			      &fixture, &transmitted, &result, &received),
		      "E sequence receive failed with %s",
		      v34_phase4_equalizer_receiver_result_name(result));
	}
	CHECK(fixture.accepted_records >= 1 &&
	      fixture.accepted_records <= 2 &&
	      (fixture.receiver.stage ==
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP ||
	       fixture.receiver.stage ==
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E),
	      "Phase 4 left MP/E before retiring its delayed input");
	{
		struct v34_equalizer_handoff rejected;
		struct v34_equalizer_handoff saved;

		memset(&rejected, 0xa5, sizeof(rejected));
		saved = rejected;
		CHECK(v34_phase4_equalizer_receiver_handoff(
			      &fixture.receiver, &rejected) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE &&
		      memcmp(&rejected, &saved, sizeof(rejected)) == 0,
		      "Phase 4 exported a handoff before delayed E completion");
	}
	for (size_t index = 0; index < ARRAY_LENGTH(lookahead); ++index) {
		transmitted = (struct qam_point){
			.in_phase = 0.17 + 0.11 * (double)index,
			.quadrature = -0.29 + 0.07 * (double)index,
		};
		CHECK(transmit_point(
			      &fixture, &transmitted, &result,
			      &lookahead[index]),
		      "E lookahead point %zu failed", index);
		CHECK(index + 1 == ARRAY_LENGTH(lookahead) ?
			      result ==
				      V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE :
			      (result ==
				       V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL ||
			       result ==
				       V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY),
		      "E lookahead point %zu returned %s", index,
		      v34_phase4_equalizer_receiver_result_name(result));
	}
	CHECK(fixture.accepted_records == 2 &&
	      v34_phase4_equalizer_receiver_is_valid(&fixture.receiver) &&
	      fixture.receiver.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE,
	      "E completion accepted %u MP records",
	      fixture.accepted_records);
	for (size_t index = 0; index < ARRAY_LENGTH(lookahead); ++index)
		CHECK(fixture.receiver.equalizer.history[index].in_phase ==
			      lookahead[ARRAY_LENGTH(lookahead) - 1u - index]
				      .in_phase &&
		      fixture.receiver.equalizer.history[index].quadrature ==
			      lookahead[ARRAY_LENGTH(lookahead) - 1u - index]
				      .quadrature,
		      "E completion reordered B1 lookahead point %zu", index);
	{
		struct v34_equalizer_handoff handoff;

		CHECK(v34_phase4_equalizer_receiver_handoff(
			      &fixture.receiver, &handoff) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_OK &&
		      v34_equalizer_handoff_is_valid(&handoff) &&
		      memcmp(&handoff.equalizer, &fixture.receiver.equalizer,
			     sizeof(handoff.equalizer)) == 0,
		      "Phase 4 did not export its exact B1 equalizer history");
	}
	{
		struct v34_phase4_equalizer_receiver saved = fixture.receiver;
		struct v34_mp_receive_event event;
		struct v34_mp_receive_event saved_event;

		memset(&event, 0x5a, sizeof(event));
		saved_event = event;
		CHECK(v34_phase4_equalizer_receive_point(
			      &fixture.receiver, &received, &event) ==
			      V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE &&
		      memcmp(&fixture.receiver, &saved, sizeof(saved)) == 0 &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "completed Phase 4 receiver consumed another point");
	}
	return true;
}

static bool test_generated_channels(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role)
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(constellations);
		     ++constellation)
			if (!run_stream(
				    roles[role],
				    constellations[constellation], 0.0, false))
				return false;
	return true;
}

static bool test_known_trn_impairment(void)
{
	/* The Phase 3 equalizer may retain a bounded channel-phase error at the
	 * S/S-bar boundary. The known Phase 4 TRN sequence must remove it before
	 * the receiver trusts the denser 16-point MP constellation. */
	return run_stream(
		V34_ANSWERING_STATION, V34_CONSTELLATION_16_POINT, 0.52,
		true);
}

static bool test_validation(void)
{
	const struct qam_point reference = { 1.0, -1.0 };
	const struct qam_point invalid = { NAN, 0.0 };
	struct qam_equalizer equalizer;
	struct v34_phase4_equalizer_receiver receiver;
	struct v34_phase4_equalizer_receiver saved;
	struct v34_mp_receive_event event = { 0 };

	CHECK(qam_equalizer_init(
		      &equalizer, &reference, &reference) ==
		      QAM_EQUALIZER_OK,
	      "validation equalizer did not initialize");
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase4_equalizer_receiver_init(
		      NULL, &equalizer, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, 0.1) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_equalizer_receiver_init(
		      &receiver, NULL, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, 0.1) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_equalizer_receiver_init(
		      &receiver, &equalizer, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, NAN) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid Phase 4 initialization changed state");
	CHECK(v34_phase4_equalizer_receiver_init(
		      &receiver, &equalizer, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, 0.1) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_OK &&
	      v34_phase4_equalizer_receiver_is_valid(&receiver) &&
	      !v34_phase4_equalizer_receiver_is_valid(NULL),
	      "validation receiver did not initialize");
	receiver.stage = V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S;
	CHECK(!v34_phase4_equalizer_receiver_is_valid(&receiver),
	      "calling receiver accepted an answer-marker stage");
	CHECK(v34_phase4_equalizer_receiver_init(
		      &receiver, &equalizer, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_4_POINT, 0.1) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_OK &&
	      receiver.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S,
	      "answering receiver did not await its Phase 4 marker");
	CHECK(v34_phase4_equalizer_receiver_init(
		      &receiver, &equalizer, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, 0.1) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_OK,
	      "validation caller receiver did not reset");
	saved = receiver;
	CHECK(v34_phase4_equalizer_receive_point(
		      NULL, &reference, &event) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_equalizer_receive_point(
		      &receiver, &invalid, &event) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_equalizer_receive_point(
		      &receiver, &reference, NULL) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_equalizer_receiver_continue_mp(&receiver) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE &&
	      v34_phase4_equalizer_receiver_start_e(&receiver) ==
		      V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid Phase 4 operation changed state");
	receiver.equalizer.coefficients[0].in_phase = INFINITY;
	CHECK(!v34_phase4_equalizer_receiver_is_valid(&receiver) &&
	      strcmp(v34_phase4_equalizer_receiver_result_name(
		     V34_PHASE4_EQUALIZER_RECEIVER_OK),
		     "ok") == 0 &&
	      strcmp(v34_phase4_equalizer_receiver_result_name(
		     V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED),
		     "S detected") == 0 &&
	      strcmp(v34_phase4_equalizer_receiver_result_name(
		     V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY),
		     "record ready") == 0 &&
	      strcmp(v34_phase4_equalizer_receiver_result_name(
		     V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v34_phase4_equalizer_receiver_result_name(
		     (enum v34_phase4_equalizer_receiver_result)99),
		     "unknown") == 0,
	      "Phase 4 validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated multipath streams", test_generated_channels },
		{ "known TRN phase impairment", test_known_trn_impairment },
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
