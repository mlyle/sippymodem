// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v34_phase3_trn.h>
#include <v34_phase3_s_sequence.h>
#include <v34_phase4_decision_receiver.h>
#include <v34_phase4_e_sequence.h>
#include <v34_phase4_mp.h>

#include "test.h"

#include <string.h>

enum {
	J_DETECTION_SYMBOLS = 3 * V34_PHASE3_J_PATTERN_SYMBOLS,
	MP_ACQUISITION_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 128,
	TEST_DECODER_POWER_Q15 = 0x7000,
};

struct observation_clock {
	uint64_t epoch;
	uint64_t lineage;
};

struct receive_fixture {
	struct v34_phase4_decision_receiver receiver;
	struct observation_clock clock;
	struct v34_mp_parameters unacknowledged;
	struct v34_mp_parameters acknowledged;
	unsigned int rejected_records;
	unsigned int unacknowledged_records;
	unsigned int acknowledged_records;
	bool search_e;
};

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

static struct v34_adaptive_equalizer_observation observation(
	struct observation_clock *clock, struct v34_analytic_sample point)
{
	clock->epoch++;
	clock->lineage = (clock->lineage ^ (uint16_t)point.in_phase ^
		((uint64_t)(uint16_t)point.quadrature << 16u)) *
		UINT64_C(1099511628211);
	if (clock->lineage == 0u)
		clock->lineage = UINT64_C(14695981039346656037);
	return (struct v34_adaptive_equalizer_observation){
		.decision = point,
		.epoch = clock->epoch,
		.lineage = clock->lineage,
	};
}

static bool observations_are_equal(
	const struct v34_adaptive_equalizer_observation *left,
	const struct v34_adaptive_equalizer_observation *right)
{
	return left->decision.in_phase == right->decision.in_phase &&
	       left->decision.quadrature == right->decision.quadrature &&
	       left->epoch == right->epoch && left->lineage == right->lineage;
}

static bool decision_is_zero(
	const struct v34_adaptive_equalizer_decision *decision)
{
	return decision->observation.decision.in_phase == 0 &&
	       decision->observation.decision.quadrature == 0 &&
	       decision->observation.epoch == 0u &&
	       decision->observation.lineage == 0u &&
	       decision->target.in_phase == 0 &&
	       decision->target.quadrature == 0 &&
	       decision->decoder_power_q15 == 0u &&
	       decision->adaptation == V34_ADAPTIVE_EQUALIZER_SKIP;
}

static bool output_matches(
	const struct v34_phase4_decision_output *output,
	const struct v34_adaptive_equalizer_observation *input,
	struct v34_analytic_sample target, bool discard)
{
	if (discard)
		return output->action == V34_PHASE4_DECISION_DISCARD &&
		       decision_is_zero(&output->equalizer);
	return output->action == V34_PHASE4_DECISION_COMMIT &&
	       observations_are_equal(&output->equalizer.observation, input) &&
	       output->equalizer.target.in_phase == target.in_phase &&
	       output->equalizer.target.quadrature == target.quadrature &&
	       output->equalizer.decoder_power_q15 ==
		       TEST_DECODER_POWER_Q15 &&
	       output->equalizer.adaptation == V34_ADAPTIVE_EQUALIZER_FULL;
}

static bool handle_record(
	struct receive_fixture *fixture,
	const struct v34_mp_receive_event *event)
{
	struct v34_phase4_decision_receiver paused = fixture->receiver;
	struct v34_adaptive_equalizer_observation input = {
		.decision = { 2289, 2289 },
		.epoch = fixture->clock.epoch + 1u,
		.lineage = fixture->clock.lineage,
	};
	struct v34_phase4_decision_output output = { 0 };
	struct v34_mp_receive_event ignored = { 0 };

	CHECK(v34_phase4_decision_receiver_observe(
		      &fixture->receiver, &input, TEST_DECODER_POWER_Q15,
		      &output, &ignored) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_STATE &&
	      memcmp(&fixture->receiver, &paused, sizeof(paused)) == 0,
	      "pending MP record did not pause input");
	if (event->type == V34_MP_RECEIVE_REJECTED) {
		fixture->rejected_records++;
		return v34_phase4_decision_receiver_continue_mp(
			       &fixture->receiver) ==
		       V34_PHASE4_DECISION_RECEIVER_OK;
	}
	CHECK(event->type == V34_MP_RECEIVE_TYPE0 &&
	      event->frame_result == V34_MP_OK,
	      "unexpected MP event type=%d result=%d", event->type,
	      event->frame_result);
	if (!event->parameters.type0.acknowledges_peer_mp) {
		CHECK(parameter_records_are_equal(
			      &event->parameters.type0,
			      &fixture->unacknowledged),
		      "unacknowledged MP parameters changed");
		fixture->unacknowledged_records++;
		return v34_phase4_decision_receiver_continue_mp(
			       &fixture->receiver) ==
		       V34_PHASE4_DECISION_RECEIVER_OK;
	}
	CHECK(parameter_records_are_equal(
		      &event->parameters.type0, &fixture->acknowledged),
	      "acknowledged MP parameters changed");
	fixture->acknowledged_records++;
	return (fixture->search_e ?
		v34_phase4_decision_receiver_expect_e(&fixture->receiver) :
		v34_phase4_decision_receiver_start_e(&fixture->receiver)) ==
	       V34_PHASE4_DECISION_RECEIVER_OK;
}

static bool receive_observation(
	struct receive_fixture *fixture, struct v34_analytic_sample point,
	struct v34_analytic_sample target,
	enum v34_phase4_decision_receiver_result expected)
{
	struct v34_adaptive_equalizer_observation input =
		observation(&fixture->clock, point);
	struct v34_phase4_decision_output output = { 0 };
	struct v34_mp_receive_event event;
	struct v34_mp_receive_event saved_event;
	bool discard = fixture->receiver.stage ==
		V34_PHASE4_DECISION_RECEIVER_ANSWER_S;
	enum v34_phase4_decision_receiver_result result;

	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	result = v34_phase4_decision_receiver_observe(
		&fixture->receiver, &input, TEST_DECODER_POWER_Q15,
		&output, &event);
	CHECK(result == expected ||
	      (expected == V34_PHASE4_DECISION_RECEIVER_RECORD_READY &&
	       result == V34_PHASE4_DECISION_RECEIVER_OK),
	      "fixed observation returned %d instead of %d", result,
	      expected);
	CHECK(output_matches(&output, &input, target, discard),
	      "fixed observation target/action changed at epoch %llu",
	      (unsigned long long)input.epoch);
	if (result != V34_PHASE4_DECISION_RECEIVER_RECORD_READY)
		return memcmp(&event, &saved_event, sizeof(event)) == 0;
	return handle_record(fixture, &event);
}

static bool receive_fixed(
	struct receive_fixture *fixture, struct v34_analytic_sample point,
	enum v34_phase4_decision_receiver_result expected)
{
	return receive_observation(fixture, point, point, expected);
}

static bool receive_call_boundary(
	struct receive_fixture *fixture, struct v34_analytic_sample point,
	bool *candidate_mismatch)
{
	struct v34_analytic_sample target = point;

	if (fixture->receiver.stage ==
	    V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE) {
		struct v34_fixed_training_decision hard;
		struct v34_phase4_trn expected =
			fixture->receiver.expected_trn;
		struct v34_analytic_sample ignored;
		uint8_t expected_quarter;
		uint8_t expected_rotation;

		CHECK(v34_training_constellation_fixed_slice(
			      fixture->receiver.mp_receiver.constellation, point,
			      &hard) == V34_CONSTELLATION_OK &&
		      v34_phase4_trn_next_fixed(
			      &expected, &ignored, &expected_quarter,
			      &expected_rotation) == V34_PHASE4_TRN_OK,
		      "call-boundary decision could not be generated");
		target = hard.target;
		*candidate_mismatch |=
			hard.quarter_point != expected_quarter ||
			hard.clockwise_quarter_turns != expected_rotation;
	}
	return receive_observation(
		fixture, point, target, V34_PHASE4_DECISION_RECEIVER_OK);
}

static bool normalized_to_fixed(
	enum v34_constellation_size constellation,
	const struct qam_point *point, struct v34_analytic_sample *fixed)
{
	uint8_t quarter;
	uint8_t rotation;

	return v34_training_constellation_slice(
		       constellation, point, &quarter, &rotation) ==
		       V34_CONSTELLATION_OK &&
	       v34_training_constellation_fixed_point(
		       constellation, quarter, rotation, fixed) ==
		       V34_CONSTELLATION_OK;
}

static bool make_j_sequence(
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	struct v34_phase3_j_sequence *sequence)
{
	struct v34_phase3_trn trn;
	struct v34_phase3_j_sequence j;
	struct v34_analytic_sample ignored;
	uint8_t ignored_rotation;

	if (sequence == NULL ||
	    v34_phase3_trn_init(&trn, role) != V34_PHASE3_TRN_OK)
		return false;
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_TRN_MIN_SYMBOLS; ++symbol)
		if (v34_phase3_trn_next_fixed(
			    &trn, &ignored, &ignored_rotation) !=
		    V34_PHASE3_TRN_OK)
			return false;
	if (v34_phase3_j_sequence_init_from_trn(
		    &j, &trn, constellation) != V34_PHASE3_J_SEQUENCE_OK)
		return false;
	for (unsigned int symbol = 0;
	     symbol < J_DETECTION_SYMBOLS; ++symbol)
		if (v34_phase3_j_sequence_next_fixed(
			    &j, &ignored, &ignored_rotation) !=
		    V34_PHASE3_J_SEQUENCE_OK)
			return false;
	*sequence = j;
	return v34_phase3_j_sequence_at_pattern_boundary(sequence);
}

static bool feed_mp_record(
	struct receive_fixture *fixture, struct v34_phase4_mp *mp)
{
	bool complete;

	do {
		struct qam_point ignored;
		struct v34_analytic_sample point;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(v34_phase4_mp_next(
			      mp, &ignored, &quarter, &rotation, &complete) ==
			      V34_PHASE4_MP_OK &&
		      v34_training_constellation_fixed_point(
			      mp->constellation, quarter, rotation, &point) ==
			      V34_CONSTELLATION_OK,
		      "MP source stopped");
		{
			/* Receiver framing may acquire within either copy of the
			 * repeated record; allow the event at its exact bit edge. */
			CHECK(receive_fixed(
				      fixture, point,
				      V34_PHASE4_DECISION_RECEIVER_RECORD_READY),
			      "MP observation stopped");
		}
	} while (!complete);
	return true;
}

static bool run_timeline(
	enum v34_station_role role,
	enum v34_constellation_size constellation, bool search_e)
{
	struct receive_fixture fixture = {
		.clock.lineage = UINT64_C(14695981039346656037),
		.unacknowledged = parameters(false),
		.acknowledged = parameters(true),
		.search_e = search_e,
	};
	struct v34_phase3_s_sequence answer_s;
	struct v34_phase3_j_sequence j_sequence;
	struct v34_phase3_j_sequence continued_j;
	struct v34_phase4_j_prime_sequence j_prime;
	struct v34_phase4_trn trn;
	struct v34_phase4_mp mp;
	struct v34_phase4_e_sequence e;
	struct v34_analytic_sample point;
	struct qam_point normalized;
	enum v34_phase3_s_section section;
	uint8_t quarter;
	uint8_t rotation;

	CHECK(make_j_sequence(role, constellation, &j_sequence) &&
	      v34_phase4_decision_receiver_init(
		      &fixture.receiver, &j_sequence, constellation) ==
		      V34_PHASE4_DECISION_RECEIVER_OK,
	      "role %d constellation %d did not initialize", role,
	      constellation);
	if (role == V34_ANSWERING_STATION) {
		CHECK(v34_phase3_s_sequence_init(&answer_s) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "answer S did not initialize");
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
			enum v34_phase4_decision_receiver_result expected =
				symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
					V34_PHASE4_DECISION_RECEIVER_S_DETECTED :
					V34_PHASE4_DECISION_RECEIVER_OK;

			CHECK(v34_phase3_s_sequence_next(
				      &answer_s, &normalized, &section) ==
				      V34_PHASE3_S_SEQUENCE_OK &&
			      normalized_to_fixed(
				      V34_CONSTELLATION_4_POINT, &normalized,
				      &point) &&
			      receive_fixed(&fixture, point, expected),
			      "answer S observation %u stopped", symbol);
		}
	} else {
		continued_j = j_sequence;
		for (unsigned int symbol = 0;
		     symbol < 2u * V34_PHASE3_J_PATTERN_SYMBOLS; ++symbol) {
			CHECK(v34_phase3_j_sequence_next_fixed(
				      &continued_j, &point, &rotation) ==
				      V34_PHASE3_J_SEQUENCE_OK &&
			      receive_fixed(
				      &fixture, point,
				      V34_PHASE4_DECISION_RECEIVER_OK),
			      "continued J observation %u stopped", symbol);
		}
		CHECK(v34_phase4_j_prime_sequence_init_from_j(
			      &j_prime, &continued_j) ==
			      V34_PHASE4_J_PRIME_SEQUENCE_OK,
		      "J-prime source did not initialize");
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
			CHECK(v34_phase4_j_prime_sequence_next(
				      &j_prime, &normalized, &rotation) ==
				      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
			      normalized_to_fixed(
				      V34_CONSTELLATION_4_POINT, &normalized,
				      &point) &&
			      receive_fixed(
				      &fixture, point,
				      V34_PHASE4_DECISION_RECEIVER_OK),
			      "J-prime observation %u stopped", symbol);
		}
		CHECK(fixture.receiver.stage ==
			      V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE,
		      "J-prime did not open the TRN candidate");
	}
	CHECK(v34_phase4_trn_init(&trn, role, constellation) ==
		      V34_PHASE4_TRN_OK,
	      "Phase 4 TRN source did not initialize");
	for (unsigned int symbol = 0;
	     symbol < MP_ACQUISITION_TRN_SYMBOLS; ++symbol) {
		enum v34_phase4_decision_receiver_result expected =
			symbol + 1u == V34_PHASE4_TRN_MIN_SYMBOLS ?
				V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE :
				V34_PHASE4_DECISION_RECEIVER_OK;
		struct v34_analytic_sample received;
		struct v34_analytic_sample target;
		uint8_t received_rotation;

		CHECK(v34_phase4_trn_next_fixed(
			      &trn, &point, &quarter, &rotation) ==
			      V34_PHASE4_TRN_OK,
		      "Phase 4 TRN source %u stopped", symbol);
		received = point;
		target = point;
		received_rotation = rotation;
		if (role == V34_CALLING_STATION &&
		    symbol + 1u == V34_PHASE4_TRN_CONFIRM_SYMBOLS / 2u) {
			received_rotation = (uint8_t)((rotation + 1u) & 3u);
			CHECK(v34_training_constellation_fixed_point(
				      constellation, quarter, received_rotation,
				      &received) == V34_CONSTELLATION_OK,
			      "TRN confirmation error could not be generated");
			target = received;
		}
		if (symbol + 1u == V34_PHASE4_TRN_MIN_SYMBOLS) {
			received_rotation = (uint8_t)((rotation + 1u) & 3u);
			CHECK(v34_training_constellation_fixed_point(
				      constellation, quarter, received_rotation,
				      &received) == V34_CONSTELLATION_OK,
			      "TRN boundary slip could not be generated");
		}
		CHECK(receive_observation(
			      &fixture, received, target, expected),
		      "Phase 4 TRN observation %u stopped", symbol);
		if (symbol + 1u == V34_PHASE4_TRN_MIN_SYMBOLS)
			CHECK(fixture.receiver.mp_receiver.has_previous_symbol &&
				      fixture.receiver.mp_receiver
					      .previous_clockwise_quarter_turns ==
					      received_rotation,
			      "MP did not retain the received TRN phase");
	}
	CHECK(fixture.receiver.stage == V34_PHASE4_DECISION_RECEIVER_MP &&
	      fixture.receiver.mp_receiver.has_previous_symbol &&
	      fixture.receiver.mp_receiver.previous_clockwise_quarter_turns ==
		      rotation,
	      "TRN did not retain the MP phase boundary");
	CHECK(v34_phase4_mp_init_type0_from_trn(
		      &mp, &trn, &fixture.unacknowledged) ==
		      V34_PHASE4_MP_OK &&
	      feed_mp_record(&fixture, &mp),
	      "first MP record stopped");
	CHECK(v34_phase4_mp_start_next_type0_record(
		      &mp, &fixture.unacknowledged) == V34_PHASE4_MP_OK &&
	      feed_mp_record(&fixture, &mp),
	      "repeated MP record stopped");
	CHECK(v34_phase4_mp_start_next_type0_record(
		      &mp, &fixture.acknowledged) == V34_PHASE4_MP_OK &&
	      feed_mp_record(&fixture, &mp),
	      "acknowledged MP record stopped");
	CHECK(fixture.unacknowledged_records >= 1u &&
	      fixture.acknowledged_records == 1u &&
	      fixture.receiver.stage == V34_PHASE4_DECISION_RECEIVER_E,
	      "MP acquisition accepted %u/%u records (%u rejected)",
	      fixture.unacknowledged_records, fixture.acknowledged_records,
	      fixture.rejected_records);
	if (search_e) {
		CHECK(v34_phase4_mp_start_next_type0_record(
			      &mp, &fixture.acknowledged) == V34_PHASE4_MP_OK,
		      "search-E MP repeat did not initialize");
		/* E search consumes the repeated MP without publishing records. */
		do {
			bool complete;

			CHECK(v34_phase4_mp_next(
				      &mp, &normalized, &quarter, &rotation,
				      &complete) == V34_PHASE4_MP_OK &&
			      v34_training_constellation_fixed_point(
				      constellation, quarter, rotation, &point) ==
				      V34_CONSTELLATION_OK &&
			      receive_fixed(
				      &fixture, point,
				      V34_PHASE4_DECISION_RECEIVER_OK),
			      "search-E MP repeat stopped");
			if (complete)
				break;
		} while (true);
	}
	CHECK(v34_phase4_e_sequence_init_from_mp(&e, &mp) ==
		      V34_PHASE4_E_SEQUENCE_OK,
	      "E source did not initialize");
	while (!v34_phase4_e_sequence_is_complete(&e)) {
		enum v34_phase4_decision_receiver_result expected;

		CHECK(v34_phase4_e_sequence_next(
			      &e, &normalized, &quarter, &rotation) ==
			      V34_PHASE4_E_SEQUENCE_OK &&
		      v34_training_constellation_fixed_point(
			      constellation, quarter, rotation, &point) ==
			      V34_CONSTELLATION_OK,
		      "E source stopped");
		expected = v34_phase4_e_sequence_is_complete(&e) ?
			V34_PHASE4_DECISION_RECEIVER_E_COMPLETE :
			V34_PHASE4_DECISION_RECEIVER_OK;
		CHECK(receive_fixed(&fixture, point, expected),
		      "E observation stopped");
	}
	CHECK(v34_phase4_decision_receiver_is_complete(&fixture.receiver),
	      "Phase 4 semantic owner did not complete");
	{
		struct v34_phase4_decision_receiver saved = fixture.receiver;
		struct v34_adaptive_equalizer_observation input =
			observation(&fixture.clock, point);
		struct v34_phase4_decision_output output = { 0 };
		struct v34_phase4_decision_output saved_output = output;
		struct v34_mp_receive_event event = { 0 };
		struct v34_mp_receive_event saved_event = event;

		CHECK(v34_phase4_decision_receiver_observe(
			      &fixture.receiver, &input,
			      TEST_DECODER_POWER_Q15, &output, &event) ==
			      V34_PHASE4_DECISION_RECEIVER_ALREADY_COMPLETE &&
		      memcmp(&fixture.receiver, &saved, sizeof(saved)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "completed owner consumed another observation");
	}
	return true;
}

static bool test_role_constellation_timelines(void)
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
			if (!run_timeline(
				    roles[role], constellations[constellation],
				    role == 0 && constellation == 1))
				return false;
	return true;
}

static bool test_delayed_answer_marker_alignment(void)
{
	struct receive_fixture fixture = {
		.clock.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_phase3_j_sequence j_sequence;
	struct v34_phase3_s_sequence s;
	struct v34_phase4_trn trn;
	struct qam_point normalized;
	struct v34_analytic_sample point;
	enum v34_phase3_s_section section;
	uint8_t quarter;
	uint8_t rotation;

	CHECK(make_j_sequence(
		      V34_ANSWERING_STATION, V34_CONSTELLATION_4_POINT,
		      &j_sequence) &&
	      v34_phase4_decision_receiver_init(
		      &fixture.receiver, &j_sequence,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_OK &&
	      v34_phase3_s_sequence_init(&s) == V34_PHASE3_S_SEQUENCE_OK,
	      "delayed answer marker did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		CHECK(v34_phase3_s_sequence_next(
			      &s, &normalized, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      v34_training_constellation_slice(
			      V34_CONSTELLATION_4_POINT, &normalized,
			      &quarter, &rotation) == V34_CONSTELLATION_OK,
		      "delayed answer S source stopped at %u", symbol);
		if (symbol >= V34_PHASE3_S_SYMBOLS &&
		    symbol < V34_PHASE3_S_SYMBOLS +
			     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0, rotation, &point) ==
			      V34_CONSTELLATION_OK &&
		      receive_fixed(
			      &fixture, point,
			      V34_PHASE4_DECISION_RECEIVER_OK),
		      "delayed answer S observation %u stopped", symbol);
	}
	CHECK(fixture.receiver.answer_s.detection_pending &&
	      v34_phase4_trn_init(
		      &trn, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_4_POINT) == V34_PHASE4_TRN_OK,
	      "delayed answer marker did not retain its candidate");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++symbol) {
		enum v34_phase4_decision_receiver_result expected =
			symbol + 1u ==
					V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE4_DECISION_RECEIVER_S_DETECTED :
				V34_PHASE4_DECISION_RECEIVER_OK;

		CHECK(v34_phase4_trn_next_fixed(
			      &trn, &point, &quarter, &rotation) ==
			      V34_PHASE4_TRN_OK &&
		      receive_fixed(&fixture, point, expected),
		      "delayed marker confirmation %u stopped", symbol);
	}
	CHECK(fixture.receiver.stage == V34_PHASE4_DECISION_RECEIVER_TRN &&
	      fixture.receiver.trn_symbols ==
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS &&
	      memcmp(&fixture.receiver.expected_trn, &trn, sizeof(trn)) == 0,
	      "delayed marker did not advance the known TRN cursor");
	CHECK(v34_phase4_trn_next_fixed(
		      &trn, &point, &quarter, &rotation) == V34_PHASE4_TRN_OK &&
	      receive_fixed(
		      &fixture, point, V34_PHASE4_DECISION_RECEIVER_OK),
	      "first post-marker TRN target was misaligned");
	return true;
}

static bool test_j_prime_requires_training_evidence(void)
{
	struct receive_fixture fixture = {
		.clock.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_phase3_j_sequence j_sequence;
	struct v34_phase4_j_prime_sequence j_prime;
	struct qam_point normalized;
	struct v34_analytic_sample target;
	uint8_t rotation;

	CHECK(make_j_sequence(
		      V34_CALLING_STATION, V34_CONSTELLATION_4_POINT,
		      &j_sequence) &&
	      v34_phase4_decision_receiver_init(
		      &fixture.receiver, &j_sequence,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_OK &&
	      v34_phase4_j_prime_sequence_init_from_j(
		      &j_prime, &j_sequence) ==
		      V34_PHASE4_J_PRIME_SEQUENCE_OK,
	      "low-energy J-prime case did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
		struct v34_analytic_sample low_energy;

		CHECK(v34_phase4_j_prime_sequence_next(
			      &j_prime, &normalized, &rotation) ==
			      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      normalized_to_fixed(
			      V34_CONSTELLATION_4_POINT, &normalized,
			      &target),
		      "low-energy J-prime source stopped at %u", symbol);
		low_energy = (struct v34_analytic_sample){
			.in_phase = (int16_t)(target.in_phase / 16),
			.quadrature = (int16_t)(target.quadrature / 16),
		};
		CHECK(receive_observation(
			      &fixture, low_energy, target,
			      V34_PHASE4_DECISION_RECEIVER_OK),
		      "low-energy J-prime observation %u stopped", symbol);
	}
	CHECK(fixture.receiver.stage ==
		      V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME &&
	      fixture.receiver.call_j_prime.received_symbols ==
		      V34_PHASE4_J_PRIME_SYMBOLS &&
	      fixture.receiver.call_j_prime.decoded_history ==
		      V34_PHASE4_J_PRIME_PATTERN &&
	      fixture.receiver.call_j_prime.qualified_history == 0u &&
	      v34_phase4_decision_receiver_is_valid(&fixture.receiver),
	      "carrier-free J-prime pattern opened training");
	return true;
}

static bool test_false_j_prime_preserves_training(void)
{
	struct receive_fixture fixture = {
		.clock.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_phase3_j_sequence j_sequence;
	struct v34_phase3_j_sequence continued_j;
	struct v34_phase4_j_prime_sequence j_prime;
	struct v34_phase4_trn trn;
	struct qam_point normalized;
	struct v34_analytic_sample point;
	bool candidate_mismatch = false;
	bool candidate_rejected = false;
	uint8_t quarter;
	uint8_t rotation;

	CHECK(make_j_sequence(
		      V34_CALLING_STATION, V34_CONSTELLATION_16_POINT,
		      &j_sequence) &&
	      v34_phase4_decision_receiver_init(
		      &fixture.receiver, &j_sequence,
		      V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_OK,
	      "false J-prime case did not initialize");
	continued_j = j_sequence;
	CHECK(v34_phase4_j_prime_sequence_init_from_j(
		      &j_prime, &j_sequence) ==
		      V34_PHASE4_J_PRIME_SEQUENCE_OK,
	      "false J-prime source did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
		CHECK(v34_phase4_j_prime_sequence_next(
			      &j_prime, &normalized, &rotation) ==
			      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      normalized_to_fixed(
			      V34_CONSTELLATION_4_POINT, &normalized,
			      &point) &&
		      receive_call_boundary(
			      &fixture, point, &candidate_mismatch),
		      "false J-prime symbol %u stopped", symbol);
	}
	CHECK(fixture.receiver.stage ==
		      V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE,
	      "false J-prime did not open a candidate");
	for (unsigned int pattern = 0; pattern < 16u; ++pattern) {
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE3_J_PATTERN_SYMBOLS; ++symbol) {
			enum v34_phase4_decision_receiver_stage before =
				fixture.receiver.stage;

			CHECK(v34_phase3_j_sequence_next_fixed(
				      &continued_j, &point, &rotation) ==
				      V34_PHASE3_J_SEQUENCE_OK &&
			      receive_call_boundary(
				      &fixture, point,
				      &candidate_mismatch),
			      "continued J pattern %u symbol %u stopped",
			      pattern, symbol);
			candidate_rejected |=
				before ==
					V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE &&
				fixture.receiver.stage ==
					V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME;
		}
		if (pattern >= 3u && fixture.receiver.stage ==
					    V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME)
			break;
	}
	CHECK(candidate_mismatch && candidate_rejected &&
	      fixture.receiver.stage ==
		      V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME &&
	      v34_phase3_j_sequence_at_pattern_boundary(&continued_j),
	      "false J-prime candidate was not rejected cleanly");
	CHECK(v34_phase4_j_prime_sequence_init_from_j(
		      &j_prime, &continued_j) ==
		      V34_PHASE4_J_PRIME_SEQUENCE_OK,
	      "real J-prime source did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
		CHECK(v34_phase4_j_prime_sequence_next(
			      &j_prime, &normalized, &rotation) ==
			      V34_PHASE4_J_PRIME_SEQUENCE_OK &&
		      normalized_to_fixed(
			      V34_CONSTELLATION_4_POINT, &normalized,
			      &point) &&
		      receive_call_boundary(
			      &fixture, point, &candidate_mismatch),
		      "real J-prime symbol %u stopped", symbol);
	}
	CHECK(fixture.receiver.stage ==
		      V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE &&
	      v34_phase4_trn_init(
		      &trn, V34_CALLING_STATION,
		      V34_CONSTELLATION_16_POINT) == V34_PHASE4_TRN_OK,
	      "real J-prime did not open its candidate");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE4_TRN_CONFIRM_SYMBOLS; ++symbol) {
		CHECK(v34_phase4_trn_next_fixed(
			      &trn, &point, &quarter, &rotation) ==
			      V34_PHASE4_TRN_OK &&
		      receive_call_boundary(
			      &fixture, point, &candidate_mismatch),
		      "TRN confirmation symbol %u stopped", symbol);
	}
	CHECK(fixture.receiver.stage == V34_PHASE4_DECISION_RECEIVER_TRN &&
	      fixture.receiver.trn_symbols ==
		      V34_PHASE4_TRN_CONFIRM_SYMBOLS &&
	      fixture.receiver.expected_trn.scrambler.history ==
		      trn.scrambler.history &&
	      fixture.receiver.expected_trn.scrambler.polynomial ==
		      trn.scrambler.polynomial &&
	      fixture.receiver.expected_trn.last_clockwise_quarter_turns ==
		      trn.last_clockwise_quarter_turns &&
	      fixture.receiver.expected_trn.has_last_symbol ==
		      trn.has_last_symbol &&
	      v34_phase4_decision_receiver_is_valid(&fixture.receiver),
	      "real zero-seeded TRN did not confirm the boundary");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_j_sequence j_sequence;
	struct v34_phase4_decision_receiver receiver;
	struct v34_phase4_decision_receiver saved;
	struct v34_phase4_decision_output output = {
		.action = V34_PHASE4_DECISION_DISCARD,
	};
	struct v34_phase4_decision_output saved_output = output;
	struct v34_adaptive_equalizer_observation input = {
		.decision = { 2289, 2289 },
		.epoch = 1,
		.lineage = 2,
	};
	struct v34_mp_receive_event event;
	struct v34_mp_receive_event saved_event;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(make_j_sequence(
		      V34_CALLING_STATION, V34_CONSTELLATION_4_POINT,
		      &j_sequence) &&
	      v34_phase4_decision_receiver_init(
		      NULL, &j_sequence,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_init(
		      &receiver, NULL,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_init(
		      &receiver, &j_sequence,
		      (enum v34_constellation_size)8) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_init(
		      &receiver,
		      (const struct v34_phase3_j_sequence *)&receiver,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid Phase 4 initialization changed state");
	CHECK(v34_phase4_decision_receiver_init(
		      &receiver, &j_sequence,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_DECISION_RECEIVER_OK,
	      "validation owner did not initialize");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	CHECK(v34_phase4_decision_receiver_observe(
		      NULL, &input, TEST_DECODER_POWER_Q15, &output, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_STATE &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, NULL, TEST_DECODER_POWER_Q15, &output,
		      &event) == V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15, NULL, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15, &output, NULL) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver,
		      (const struct v34_adaptive_equalizer_observation *)&receiver,
		      TEST_DECODER_POWER_Q15, &output, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15,
		      (struct v34_phase4_decision_output *)&receiver, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, &input, 0u, &output, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_observe(
		      &receiver, &input, 0x8000u, &output, &event) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_decision_receiver_continue_mp(&receiver) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_STATE &&
	      v34_phase4_decision_receiver_start_e(&receiver) ==
		      V34_PHASE4_DECISION_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "invalid Phase 4 operation changed state or outputs");
	input.epoch = 0u;
	CHECK(v34_phase4_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15, &output,
		      &event) == V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT,
	      "zero observation epoch was accepted");
	receiver.stage = (enum v34_phase4_decision_receiver_stage)99;
	CHECK(!v34_phase4_decision_receiver_is_valid(NULL) &&
	      !v34_phase4_decision_receiver_is_valid(&receiver) &&
	      !v34_phase4_decision_receiver_is_complete(&receiver),
	      "corrupt Phase 4 owner was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "role and constellation timelines",
		  test_role_constellation_timelines },
		{ "delayed answer marker alignment",
		  test_delayed_answer_marker_alignment },
		{ "J-prime training evidence",
		  test_j_prime_requires_training_evidence },
		{ "false J-prime preserves training",
		  test_false_j_prime_preserves_training },
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
