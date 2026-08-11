// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_e_receiver.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 7,
};

struct e_fixture {
	struct v34_phase4_e_sequence transmitter;
	struct v34_phase4_e_receiver decision_receiver;
	struct v34_phase4_e_receiver point_receiver;
};

static struct v34_mp_parameters test_parameters(bool acknowledged)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.remote_transmitter_trellis = V34_TRELLIS_32_STATE,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = acknowledged,
		.data_rate_capability_mask = 0x1555,
		.asymmetric_rates_enabled = true,
	};
}

static bool receivers_are_equal(
	const struct v34_phase4_e_receiver *left,
	const struct v34_phase4_e_receiver *right)
{
	return left->descrambler.history == right->descrambler.history &&
	       left->descrambler.polynomial == right->descrambler.polynomial &&
	       left->transmitting_role == right->transmitting_role &&
	       left->constellation == right->constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->received_bits == right->received_bits;
}

static bool initialize_fixture(
	struct e_fixture *fixture, enum v34_station_role role,
	enum v34_constellation_size constellation, bool acknowledged)
{
	struct v34_mp_parameters parameters =
		test_parameters(acknowledged);
	struct v34_phase4_trn trn;
	struct v34_phase4_mp mp;
	struct v34_phase4_mp_receiver mp_receiver;
	struct v34_mp_receive_event event = { 0 };

	CHECK(fixture != NULL &&
	      v34_phase4_trn_init(&trn, role, constellation) ==
		      V34_PHASE4_TRN_OK &&
	      v34_phase4_mp_receiver_init(
		      &mp_receiver, role, constellation) ==
		      V34_PHASE4_MP_RECEIVER_OK,
	      "E receiver fixture did not initialize");
	for (size_t symbol = 0; symbol < TEST_TRN_SYMBOLS; ++symbol) {
		struct qam_point point;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		CHECK(v34_phase4_trn_next(
			      &trn, &point, &quarter_point,
			      &clockwise_quarter_turns) ==
			      V34_PHASE4_TRN_OK &&
		      v34_phase4_mp_receive_decision(
			      &mp_receiver, quarter_point,
			      clockwise_quarter_turns, &event) ==
			      V34_PHASE4_MP_RECEIVER_OK,
		      "E receiver fixture TRN stopped at symbol %zu", symbol);
	}
	CHECK(v34_phase4_mp_init_type0_from_trn(
		      &mp, &trn, &parameters) == V34_PHASE4_MP_OK,
	      "E receiver fixture MP did not initialize");
	for (unsigned int record = 0; record < 2; ++record) {
		if (record != 0)
			CHECK(v34_phase4_mp_start_next_type0_record(
				      &mp, &parameters) ==
				      V34_PHASE4_MP_OK,
			      "E receiver fixture MP-prime did not start");
		event = (struct v34_mp_receive_event){ 0 };
		while (!v34_phase4_mp_record_is_complete(&mp)) {
			struct v34_mp_receive_event emitted;
			struct qam_point point;
			uint8_t quarter_point;
			uint8_t clockwise_quarter_turns;
			bool record_complete;

			CHECK(v34_phase4_mp_next(
				      &mp, &point, &quarter_point,
				      &clockwise_quarter_turns,
				      &record_complete) ==
				      V34_PHASE4_MP_OK &&
			      v34_phase4_mp_receive_decision(
				      &mp_receiver, quarter_point,
				      clockwise_quarter_turns, &emitted) ==
				      V34_PHASE4_MP_RECEIVER_OK,
			      "E receiver fixture MP stopped at bit %zu",
			      mp.bit_index);
			if (emitted.type != V34_MP_RECEIVE_NO_EVENT)
				event = emitted;
		}
	}
	CHECK(event.type == V34_MP_RECEIVE_TYPE0 &&
	      event.frame_result == V34_MP_OK &&
	      v34_phase4_e_sequence_init_from_mp(
		      &fixture->transmitter, &mp) ==
		      V34_PHASE4_E_SEQUENCE_OK &&
	      v34_phase4_e_receiver_init_from_mp(
		      &fixture->decision_receiver, &mp_receiver) ==
		      V34_PHASE4_E_RECEIVER_OK,
	      "E receiver fixture did not accept MP");
	fixture->point_receiver = fixture->decision_receiver;
	return true;
}

static bool check_sequence(
	enum v34_station_role role,
	enum v34_constellation_size constellation, bool acknowledged)
{
	struct e_fixture fixture;
	size_t symbol = 0;

	CHECK(initialize_fixture(&fixture, role, constellation, acknowledged),
	      "E receiver fixture failed");
	while (!v34_phase4_e_sequence_is_complete(
		       &fixture.transmitter)) {
		struct qam_point point;
		enum v34_phase4_e_receiver_result expected;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		CHECK(v34_phase4_e_sequence_next(
			      &fixture.transmitter, &point, &quarter_point,
			      &clockwise_quarter_turns) ==
			      V34_PHASE4_E_SEQUENCE_OK,
		      "E transmitter stopped at symbol %zu", symbol);
		expected = v34_phase4_e_sequence_is_complete(
				   &fixture.transmitter) ?
				   V34_PHASE4_E_RECEIVER_COMPLETE :
				   V34_PHASE4_E_RECEIVER_OK;
		CHECK(v34_phase4_e_receive_decision(
			      &fixture.decision_receiver, quarter_point,
			      clockwise_quarter_turns) == expected &&
		      v34_phase4_e_receive_point(
			      &fixture.point_receiver, &point) == expected &&
		      receivers_are_equal(
			      &fixture.decision_receiver,
			      &fixture.point_receiver),
		      "E receivers diverged at symbol %zu", symbol);
		++symbol;
	}
	CHECK(v34_phase4_e_receiver_is_complete(
		      &fixture.decision_receiver) &&
	      fixture.decision_receiver.received_bits ==
		      V34_PHASE4_E_BITS &&
	      fixture.decision_receiver.descrambler.history ==
		      fixture.transmitter.scrambler.history &&
	      fixture.decision_receiver.previous_clockwise_quarter_turns ==
		      fixture.transmitter.previous_clockwise_quarter_turns,
	      "E receiver did not finish on the transmitter boundary");
	{
		struct v34_phase4_e_receiver saved =
			fixture.decision_receiver;

		CHECK(v34_phase4_e_receive_decision(
			      &fixture.decision_receiver, 0, 0) ==
			      V34_PHASE4_E_RECEIVER_COMPLETE &&
		      receivers_are_equal(
			      &fixture.decision_receiver, &saved),
		      "completed E receiver consumed another decision");
	}
	return true;
}

static bool test_generated_sequence(void)
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
			for (unsigned int acknowledged = 0; acknowledged < 2;
			     ++acknowledged)
				if (!check_sequence(
					    roles[role],
					    constellations[constellation],
					    acknowledged != 0))
					return false;
	return true;
}

static bool test_mismatch_is_transactional(void)
{
	struct e_fixture fixture;
	struct v34_phase4_e_receiver saved;
	struct qam_point point;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	CHECK(initialize_fixture(
		      &fixture, V34_CALLING_STATION,
		      V34_CONSTELLATION_16_POINT, true) &&
	      v34_phase4_e_sequence_next(
		      &fixture.transmitter, &point, &quarter_point,
		      &clockwise_quarter_turns) ==
		      V34_PHASE4_E_SEQUENCE_OK,
	      "E mismatch fixture did not initialize");
	saved = fixture.decision_receiver;
	CHECK(v34_phase4_e_receive_decision(
		      &fixture.decision_receiver, quarter_point,
		      (uint8_t)((clockwise_quarter_turns + 1u) & 3u)) ==
		      V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH &&
	      receivers_are_equal(&fixture.decision_receiver, &saved),
	      "differential E mismatch changed state");
	saved = fixture.point_receiver;
	CHECK(v34_training_constellation_point(
		      fixture.point_receiver.constellation, quarter_point,
		      (uint8_t)((clockwise_quarter_turns + 1u) & 3u), &point) ==
		      V34_CONSTELLATION_OK &&
	      v34_phase4_e_receive_point(
		      &fixture.point_receiver, &point) ==
		      V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH &&
	      receivers_are_equal(&fixture.point_receiver, &saved),
	      "sliced E mismatch changed state");
	saved = fixture.decision_receiver;
	CHECK(v34_phase4_e_receive_decision(
		      &fixture.decision_receiver,
		      (uint8_t)(quarter_point ^ 1u),
		      clockwise_quarter_turns) ==
		      V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH &&
	      receivers_are_equal(&fixture.decision_receiver, &saved) &&
	      v34_phase4_e_receive_decision(
		      &fixture.decision_receiver, quarter_point,
		      clockwise_quarter_turns) ==
		      V34_PHASE4_E_RECEIVER_OK,
	      "mapped E mismatch changed state or blocked retry");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase4_mp_receiver mp_receiver;
	struct v34_phase4_e_receiver receiver;
	struct v34_phase4_e_receiver saved;
	struct qam_point invalid_point = { NAN, 0.0 };

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase4_mp_receiver_init(
		      &mp_receiver, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE4_MP_RECEIVER_OK &&
	      v34_phase4_e_receiver_init_from_mp(
		      NULL, &mp_receiver) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receiver_init_from_mp(
		      &receiver, NULL) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receiver_init_from_mp(
		      &receiver, &mp_receiver) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "E receiver accepted a transition without a prior symbol");
	{
		struct e_fixture fixture;

		CHECK(initialize_fixture(
			      &fixture, V34_ANSWERING_STATION,
			      V34_CONSTELLATION_4_POINT, true),
		      "E validation fixture failed");
		receiver = fixture.decision_receiver;
	}
	saved = receiver;
	CHECK(v34_phase4_e_receive_decision(NULL, 0, 0) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receive_decision(&receiver, 1, 0) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receive_decision(&receiver, 0, 4) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receive_point(&receiver, NULL) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase4_e_receive_point(
		      &receiver, &invalid_point) ==
		      V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT &&
	      receivers_are_equal(&receiver, &saved),
	      "invalid E input changed receiver state");
	receiver = saved;
	receiver.received_bits = 1;
	CHECK(!v34_phase4_e_receiver_is_valid(&receiver),
	      "E receiver accepted a partial-symbol cursor");
	receiver = saved;
	receiver.received_bits = V34_PHASE4_E_BITS + 1;
	CHECK(!v34_phase4_e_receiver_is_valid(&receiver),
	      "E receiver accepted a cursor beyond E");
	receiver = saved;
	receiver.previous_clockwise_quarter_turns = 4;
	CHECK(!v34_phase4_e_receiver_is_valid(&receiver),
	      "E receiver accepted invalid differential phase");
	receiver = saved;
	receiver.transmitting_role = V34_CALLING_STATION;
	CHECK(!v34_phase4_e_receiver_is_valid(&receiver) &&
	      !v34_phase4_e_receiver_is_valid(NULL) &&
	      !v34_phase4_e_receiver_is_complete(NULL),
	      "E receiver accepted corrupt state");
	CHECK(strcmp(v34_phase4_e_receiver_result_name(
		     V34_PHASE4_E_RECEIVER_OK),
		     "ok") == 0 &&
	      strcmp(v34_phase4_e_receiver_result_name(
		     V34_PHASE4_E_RECEIVER_COMPLETE),
		     "complete") == 0 &&
	      strcmp(v34_phase4_e_receiver_result_name(
		     V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v34_phase4_e_receiver_result_name(
		     V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_phase4_e_receiver_result_name(
		     (enum v34_phase4_e_receiver_result)99),
		     "unknown") == 0,
	      "E receiver result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated continuous sequence", test_generated_sequence },
		{ "transactional mismatch", test_mismatch_is_transactional },
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
