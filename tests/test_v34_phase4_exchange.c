// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_exchange.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	SAMPLE_LIMIT = 70000,
};

struct endpoint {
	struct v34_phase3_exchange phase3;
	struct v34_phase4_exchange phase4;
	struct v34_phase3_exchange_handoff handoff;
	struct v34_phase4_exchange_configuration phase4_configuration;
	unsigned int data_preparation_count;
	bool phase4_active;
};

static struct v34_phase3_profile profile(enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator =
			role == V34_CALLING_STATION ? 1800u : 2000u,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.preemphasis_filter_index =
			role == V34_CALLING_STATION ? 2u : 3u,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = role == V34_ANSWERING_STATION,
	};
}

static struct v34_phase3_exchange_configuration phase3_configuration(
	enum v34_constellation_size requested)
{
	return (struct v34_phase3_exchange_configuration){
		.receive = {
			.s_gain_adaptation = 0.1,
			.pp_equalizer_adaptation = 0.2,
			.tracking_equalizer_adaptation = 0.05,
		},
		.requested_remote_constellation = requested,
		.sample_rate = SAMPLE_RATE,
		.nominal_peak_amplitude = 7000,
	};
}

static struct v34_mp_parameters mp_parameters(
	enum v34_station_role role)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units =
			role == V34_CALLING_STATION ? 10u : 8u,
		.maximum_answer_to_call_rate_2400_bps_units =
			role == V34_CALLING_STATION ? 9u : 7u,
		.auxiliary_channel_enabled = role == V34_CALLING_STATION,
		.remote_transmitter_trellis =
			role == V34_CALLING_STATION ?
				V34_TRELLIS_32_STATE : V34_TRELLIS_16_STATE,
		.remote_transmitter_nonlinear_encoding =
			role == V34_ANSWERING_STATION,
		.remote_transmitter_expanded_shaping = true,
		.data_rate_capability_mask = 0x00ffu,
		.asymmetric_rates_enabled = true,
	};
}

static struct v34_mp_record mp_record(enum v34_station_role role)
{
	struct v34_mp_record record = {
		.type = role == V34_CALLING_STATION ?
			V34_MP_RECORD_TYPE1 : V34_MP_RECORD_TYPE0,
	};

	if (record.type == V34_MP_RECORD_TYPE0) {
		record.parameters.type0 = mp_parameters(role);
	} else {
		record.parameters.type1 = (struct v34_mp_type1_parameters){
			.common = mp_parameters(role),
			.precoder_coefficients = {
				{ .real = 0x1234, .imaginary = -1 },
				{ .real = -8192, .imaginary = 4096 },
				{ .real = 0, .imaginary = 2048 },
			},
		};
	}
	return record;
}

static bool mp_records_are_equal(
	const struct v34_mp_record *left,
	const struct v34_mp_record *right)
{
	uint8_t left_frame[V34_MP_TYPE1_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V34_MP_TYPE1_FRAME_BYTES] = { 0 };
	enum v34_mp_result left_result;
	enum v34_mp_result right_result;

	if (left->type != right->type)
		return false;
	if (left->type == V34_MP_RECORD_TYPE0) {
		left_result = v34_mp_type0_encode(
			&left->parameters.type0, left_frame,
			sizeof(left_frame));
		right_result = v34_mp_type0_encode(
			&right->parameters.type0, right_frame,
			sizeof(right_frame));
	} else if (left->type == V34_MP_RECORD_TYPE1) {
		left_result = v34_mp_type1_encode(
			&left->parameters.type1, left_frame,
			sizeof(left_frame));
		right_result = v34_mp_type1_encode(
			&right->parameters.type1, right_frame,
			sizeof(right_frame));
	} else {
		return false;
	}
	return left_result == V34_MP_OK && right_result == V34_MP_OK &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static struct v34_phase4_exchange_configuration
phase4_configuration(enum v34_station_role role)
{
	return (struct v34_phase4_exchange_configuration){
		.receive = {
			.phase3 = {
				.s_gain_adaptation = 0.1,
				.pp_equalizer_adaptation = 0.2,
				.tracking_equalizer_adaptation = 0.05,
			},
			.phase4_equalizer_adaptation = 0.03,
		},
		.transmit_mp = mp_record(role),
		.data_equalizer_adaptation = 0.02,
	};
}

static bool endpoint_init(
	struct endpoint *endpoint, enum v34_station_role role)
{
	struct v34_phase3_profile local = profile(role);
	struct v34_phase3_profile peer = profile(
		role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION : V34_CALLING_STATION);
	struct v34_phase3_exchange_configuration configuration =
		phase3_configuration(
			role == V34_CALLING_STATION ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT);
	struct endpoint initialized = {
		.phase4_configuration = phase4_configuration(role),
	};

	if (endpoint == NULL ||
	    v34_phase3_exchange_init(
		    &initialized.phase3, &local, &peer,
		    &configuration) != V34_PHASE3_EXCHANGE_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_sample(
	struct endpoint *endpoint, int16_t received, int16_t *transmit,
	uint32_t *phase4_events)
{
	if (endpoint->phase4_active) {
		enum v34_phase4_exchange_result result =
			v34_phase4_exchange_sample(
				&endpoint->phase4, received, transmit,
				phase4_events);

		if (result != V34_PHASE4_EXCHANGE_OK) {
			fprintf(stderr, "Phase 4 sample: %s\n",
				v34_phase4_exchange_result_name(result));
			return false;
		}
		if (!v34_phase4_exchange_needs_data_preparation(
			    &endpoint->phase4))
			return true;
		endpoint->data_preparation_count++;
		result = v34_phase4_exchange_prepare_data_startup(
			&endpoint->phase4);
		if (result != V34_PHASE4_EXCHANGE_OK)
			fprintf(stderr, "Phase 4 data preparation: %s\n",
				v34_phase4_exchange_result_name(result));
		return result == V34_PHASE4_EXCHANGE_OK;
	}
	{
		uint32_t phase3_events;

		*phase4_events = 0;
		if (v34_phase3_exchange_sample(
			    &endpoint->phase3, received, transmit,
			    &phase3_events) != V34_PHASE3_EXCHANGE_OK)
			return false;
		if (!v34_phase3_exchange_is_handoff_ready(&endpoint->phase3))
			return true;
		if (v34_phase3_exchange_handoff(
			    &endpoint->phase3, &endpoint->handoff) !=
				    V34_PHASE3_EXCHANGE_OK ||
		    v34_phase4_exchange_init(
			    &endpoint->phase4, &endpoint->handoff,
			    &endpoint->phase4_configuration) !=
				    V34_PHASE4_EXCHANGE_OK)
			return false;
		endpoint->phase4_active = true;
		return true;
	}
}

static bool add_events(uint32_t events, uint32_t *seen)
{
	if ((events & *seen) != 0)
		return false;
	*seen |= events;
	return true;
}

static bool run_pair(
	struct endpoint *caller, struct endpoint *answerer,
	uint32_t *caller_seen, uint32_t *answerer_seen)
{
	struct endpoint calling;
	struct endpoint answering;
	int16_t caller_receive = 0;
	int16_t answerer_receive = 0;
	uint32_t caller_events_seen = 0;
	uint32_t answerer_events_seen = 0;

	CHECK(endpoint_init(&calling, V34_CALLING_STATION) &&
	      endpoint_init(&answering, V34_ANSWERING_STATION),
	      "continuous Phase 3/4 endpoints did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t caller_transmit;
		int16_t answerer_transmit;
		uint32_t caller_events;
		uint32_t answerer_events;

		CHECK(endpoint_sample(
			      &calling, caller_receive,
			      &caller_transmit, &caller_events) &&
		      endpoint_sample(
			      &answering, answerer_receive,
			      &answerer_transmit, &answerer_events),
		      "continuous Phase 3/4 exchange stopped at sample %zu",
		      index);
		CHECK(add_events(caller_events, &caller_events_seen) &&
		      add_events(answerer_events, &answerer_events_seen),
		      "Phase 4 repeated an event at sample %zu: 0x%x/0x%x",
		      index, caller_events, answerer_events);
		caller_receive = answerer_transmit;
		answerer_receive = caller_transmit;
		if (calling.phase4_active && answering.phase4_active &&
		    v34_phase4_exchange_is_ready_for_e(&calling.phase4) &&
		    v34_phase4_exchange_is_ready_for_e(&answering.phase4)) {
			*caller = calling;
			*answerer = answering;
			*caller_seen = caller_events_seen;
			*answerer_seen = answerer_events_seen;
			return true;
		}
	}
	CHECK(false,
	      "Phase 4 did not prepare reciprocal E: active=%d/%d "
	      "events=0x%x/0x%x",
	      calling.phase4_active, answering.phase4_active,
	      caller_events_seen, answerer_events_seen);
}

static bool directions_are_equal(
	const struct v34_data_direction_parameters *left,
	const struct v34_data_direction_parameters *right)
{
	return left->trellis_selection == right->trellis_selection &&
	       left->nonlinear_parameter == right->nonlinear_parameter &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->expanded_shaping == right->expanded_shaping &&
	       memcmp(left->precoder_coefficients, right->precoder_coefficients,
		      sizeof(left->precoder_coefficients)) == 0;
}

static bool test_generated_data_preparation(void)
{
	const uint32_t common_events =
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_READY |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_READY_FOR_E;
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_data_startup caller_startup;
	struct v34_data_startup answerer_startup;
	struct v34_mp_record caller_peer;
	struct v34_mp_record answerer_peer;
	struct v34_mp_record expected_caller_peer =
		mp_record(V34_ANSWERING_STATION);
	struct v34_mp_record expected_answerer_peer =
		mp_record(V34_CALLING_STATION);
	uint32_t caller_seen;
	uint32_t answerer_seen;

	CHECK(run_pair(
		      &caller, &answerer, &caller_seen, &answerer_seen),
	      "generated Phase 4 pair failed");
	CHECK(caller_seen ==
		      (common_events |
		       V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED |
		       V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_J_FINISHED) &&
	      answerer_seen == common_events,
	      "Phase 4 event coverage differed: caller=0x%x answerer=0x%x",
	      caller_seen, answerer_seen);
	CHECK(caller.phase4.transmit.training.stage ==
		      V34_TRAINING_TRANSMITTER_PHASE4_MP &&
	      answerer.phase4.transmit.training.stage ==
		      V34_TRAINING_TRANSMITTER_PHASE4_MP &&
	      caller.phase4.receive.training.state.phase4.control.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E &&
	      answerer.phase4.receive.training.state.phase4.control.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E &&
	      caller.phase4.transmit.training.phase4_trn_symbols >=
		      V34_PHASE4_TRN_MIN_SYMBOLS &&
	      answerer.phase4.transmit.training.phase4_trn_symbols >=
		      V34_PHASE4_TRN_MIN_SYMBOLS &&
	      !v34_phase4_exchange_has_failed(&caller.phase4) &&
	      !v34_phase4_exchange_has_failed(&answerer.phase4) &&
	      caller.data_preparation_count == 1 &&
	      answerer.data_preparation_count == 1,
	      "Phase 4 owners reached the wrong prepared boundary");
	CHECK(v34_phase4_exchange_get_data_startup(
		      &caller.phase4, &caller_startup) &&
	      v34_phase4_exchange_get_data_startup(
		      &answerer.phase4, &answerer_startup) &&
	      v34_data_startup_is_valid(&caller_startup) &&
	      v34_data_startup_is_valid(&answerer_startup) &&
	      caller_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 8 &&
	      caller_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 7 &&
	      answerer_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 7 &&
	      answerer_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 8 &&
	      directions_are_equal(
		      &caller_startup.mode.transmit,
		      &answerer_startup.mode.receive) &&
	      directions_are_equal(
		      &caller_startup.mode.receive,
		      &answerer_startup.mode.transmit),
	      "Phase 4 prepared incompatible post-E data directions");
	CHECK(v34_phase4_exchange_get_peer_mp(
		      &caller.phase4, &caller_peer) &&
	      v34_phase4_exchange_get_peer_mp(
		      &answerer.phase4, &answerer_peer) &&
	      mp_records_are_equal(&caller_peer, &expected_caller_peer) &&
	      mp_records_are_equal(
		      &answerer_peer, &expected_answerer_peer),
	      "Phase 4 owners did not retain normalized peer records");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_phase4_exchange_configuration invalid =
		phase4_configuration(V34_CALLING_STATION);
	struct v34_phase4_exchange exchange;
	struct v34_phase4_exchange saved;
	struct v34_data_startup startup = { 0 };
	struct v34_data_startup saved_startup;
	struct v34_mp_record peer = mp_record(V34_CALLING_STATION);
	struct v34_mp_record saved_peer = peer;
	struct v34_mp_record invalid_record = peer;
	union {
		uint32_t events;
		int16_t samples[2];
	} overlap = { .events = UINT32_C(0xa55a5aa5) };
	uint32_t caller_seen;
	uint32_t answerer_seen;
	int16_t sample = 1234;
	uint32_t events = UINT32_C(0x5aa5a55a);

	CHECK(run_pair(
		      &caller, &answerer, &caller_seen, &answerer_seen),
	      "validation Phase 4 pair failed");
	memset(&exchange, 0xa5, sizeof(exchange));
	saved = exchange;
	invalid.receive.phase4_equalizer_adaptation = NAN;
	CHECK(v34_phase4_exchange_init(
		      &exchange, &caller.handoff, &invalid) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "invalid Phase 4 initialization changed the destination");
	invalid = phase4_configuration(V34_CALLING_STATION);
	invalid.data_equalizer_adaptation = NAN;
	memset(&exchange, 0xa5, sizeof(exchange));
	saved = exchange;
	CHECK(v34_phase4_exchange_init(
		      &exchange, &caller.handoff, &invalid) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "invalid data adaptation changed the Phase 4 destination");
	invalid = phase4_configuration(V34_CALLING_STATION);
	invalid.transmit_mp.parameters.type1.common.acknowledges_peer_mp = true;
	memset(&exchange, 0xa5, sizeof(exchange));
	saved = exchange;
	CHECK(v34_phase4_exchange_init(
		      &exchange, &caller.handoff, &invalid) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "pre-acknowledged MP configuration changed the destination");
	saved = caller.phase4;
	CHECK(v34_phase4_exchange_sample(
		      &caller.phase4, 0, NULL, &events) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0, &sample, NULL) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0,
		      &overlap.samples[0], &overlap.events) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0,
		      &overlap.samples[1], &overlap.events) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&caller.phase4, &saved, sizeof(saved)) == 0 &&
	      sample == 1234 && events == UINT32_C(0x5aa5a55a) &&
	      overlap.events == UINT32_C(0xa55a5aa5),
	      "rejected Phase 4 samples were not transactional");
	saved = caller.phase4;
	CHECK(v34_phase4_exchange_prepare_data_startup(&caller.phase4) ==
		      V34_PHASE4_EXCHANGE_INVALID_STATE &&
	      memcmp(&caller.phase4, &saved, sizeof(saved)) == 0,
	      "out-of-sequence data preparation changed the exchange");
	memset(&startup, 0xa5, sizeof(startup));
	saved_startup = startup;
	CHECK(!v34_phase4_exchange_is_valid(NULL) &&
	      !v34_phase4_exchange_has_failed(NULL) &&
	      !v34_phase4_exchange_is_trn_ready(NULL) &&
	      !v34_phase4_exchange_needs_data_preparation(NULL) &&
	      !v34_phase4_exchange_is_ready_for_e(NULL) &&
	      !v34_phase4_exchange_get_data_startup(NULL, &startup) &&
	      !v34_phase4_exchange_get_data_startup(
		      &caller.phase4, NULL) &&
	      memcmp(&startup, &saved_startup, sizeof(startup)) == 0 &&
	      !v34_phase4_exchange_get_peer_mp(NULL, &peer) &&
	      !v34_phase4_exchange_get_peer_mp(&caller.phase4, NULL) &&
	      memcmp(&peer, &saved_peer, sizeof(peer)) == 0 &&
	      !v34_mp_record_is_valid(NULL) &&
	      strcmp(v34_phase4_exchange_result_name(
		     V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v34_phase4_exchange_result_name(
		     V34_PHASE4_EXCHANGE_NO_COMMON_RATE),
		     "no common rate") == 0 &&
	      strcmp(v34_phase4_exchange_result_name(
		     (enum v34_phase4_exchange_result)99),
		     "unknown") == 0,
	      "Phase 4 validation helpers returned the wrong result");
	invalid_record.type = (enum v34_mp_record_type)99;
	CHECK(!v34_mp_record_is_valid(&invalid_record),
	      "Phase 4 accepted an unknown MP record type");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated data preparation",
		  test_generated_data_preparation },
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
