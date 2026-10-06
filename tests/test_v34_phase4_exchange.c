// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_exchange.h>

#include "test.h"

#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	SAMPLE_LIMIT = 70000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
};

struct endpoint {
	struct v34_phase3_exchange phase3;
	struct v34_phase4_exchange phase4;
	struct v34_phase3_exchange_handoff handoff;
	struct v34_phase4_exchange_configuration phase4_configuration;
	struct v34_phase4_exchange preparation_snapshot;
	struct v34_data_startup_preparation preparation;
	unsigned int data_preparation_count;
	unsigned int preparation_samples;
	unsigned int mapping_frames_supplied;
	unsigned int mapping_frames_received;
	uint32_t trn_symbols_at_mp;
	uint32_t j_symbols_at_peer_s;
	uint32_t j_symbols_at_finish;
	int16_t next_received;
	uint8_t primary_idle_bits_remaining;
	uint8_t transmit_bit;
	bool constellations_routed;
	bool has_preparation_snapshot;
	bool phase4_active;
	bool incremental_preparation;
};

static bool supply_mapping_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct endpoint *endpoint = context;

	if (endpoint == NULL || request == NULL || primary_bits == NULL)
		return false;
	memset(primary_bits, endpoint->transmit_bit, request->primary_bits);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = endpoint->transmit_bit;
	endpoint->mapping_frames_supplied++;
	return true;
}

static bool reject_mapping_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	(void)context;
	(void)request;
	(void)primary_bits;
	(void)auxiliary_bit;
	return false;
}

static bool accept_mapping_frame(
	struct endpoint *endpoint,
	const struct v34_received_mapping_frame *received)
{
	uint8_t expected = (uint8_t)(endpoint->transmit_bit ^ 1u);

	for (uint8_t index = 0; index < received->primary_bit_count; ++index) {
		uint8_t bit = endpoint->primary_idle_bits_remaining != 0 ?
			1u : expected;

		if (received->primary_bits[index] != bit)
			return false;
		if (endpoint->primary_idle_bits_remaining != 0)
			endpoint->primary_idle_bits_remaining--;
	}
	if (received->auxiliary_bit_count != 0 &&
	    received->auxiliary_bit != expected)
		return false;
	endpoint->mapping_frames_received++;
	return true;
}

static enum v34_constellation_size handoff_detected_constellation(
	const struct v34_phase3_exchange_handoff *handoff)
{
	return handoff->receive.phase3_handoff.j.sequence.remote_constellation;
}

static bool receive_constellation_is(
	const struct v34_phase4_exchange *exchange,
	enum v34_constellation_size constellation)
{
	return exchange->receive.phase4_constellation == constellation &&
	       exchange->receive.semantic.phase4.mp_receiver.constellation ==
		       constellation;
}

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
		.requested_remote_constellation = requested,
		.sample_rate = SAMPLE_RATE,
		.nominal_peak_amplitude = 7000,
	};
}

static struct v34_mp_parameters mp_parameters(
	enum v34_station_role role)
{
	return (struct v34_mp_parameters){
		/* Establish the first online controller at a conservative 9.6
		 * kbit/s in each direction; faster shell-mapped line modes remain a
		 * separate robustness step. */
		.maximum_call_to_answer_rate_2400_bps_units = 4,
		.maximum_answer_to_call_rate_2400_bps_units = 4,
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

static struct v34_phase4_exchange_configuration
phase4_configuration(enum v34_station_role role)
{
	return (struct v34_phase4_exchange_configuration){
		.transmit_mp = mp_record(role),
	};
}

static bool endpoint_init_with_rates(
	struct endpoint *endpoint, enum v34_station_role role,
	uint8_t local_projected_rate, uint8_t peer_projected_rate,
	uint8_t mp_rate_limit)
{
	struct v34_phase3_profile local = profile(role);
	struct v34_phase3_profile peer = profile(v34_station_peer_role(role));
	struct v34_phase3_exchange_configuration configuration =
		phase3_configuration(
			role == V34_CALLING_STATION ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT);
	struct endpoint initialized = {
		.phase4_configuration = phase4_configuration(role),
		.transmit_bit = role == V34_ANSWERING_STATION,
	};
	struct v34_mp_parameters *parameters =
		v34_mp_record_common_parameters_mutable(
			&initialized.phase4_configuration.transmit_mp);

	local.projected_data_rate_2400_bps_units = local_projected_rate;
	peer.projected_data_rate_2400_bps_units = peer_projected_rate;
	parameters->maximum_call_to_answer_rate_2400_bps_units =
		mp_rate_limit;
	parameters->maximum_answer_to_call_rate_2400_bps_units =
		mp_rate_limit;

	if (endpoint == NULL ||
	    v34_phase3_exchange_init(
		    &initialized.phase3, &local, &peer,
		    &configuration, TEST_INITIAL_RECEIVE_GAIN) !=
		    V34_PHASE3_EXCHANGE_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_init(
	struct endpoint *endpoint, enum v34_station_role role)
{
	return endpoint_init_with_rates(endpoint, role, 8, 8, 4);
}

static bool endpoint_sample(
	struct endpoint *endpoint, int16_t received, int16_t *transmit,
	uint32_t *phase4_events)
{
	if (endpoint->phase4_active) {
		struct v34_received_mapping_frame received_frame;
		enum v34_phase4_exchange_result result =
			v34_phase4_exchange_sample(
				&endpoint->phase4, received,
				supply_mapping_frame, endpoint,
				transmit, phase4_events, &received_frame);

		if (result != V34_PHASE4_EXCHANGE_OK) {
			fprintf(stderr, "Phase 4 sample: %d\n", result);
			return false;
		}
		if ((*phase4_events &
		     V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED) != 0)
			endpoint->j_symbols_at_peer_s =
				endpoint->phase4.transmit.training.phase3.j_symbols;
		if ((*phase4_events &
		     V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_J_FINISHED) != 0)
			endpoint->j_symbols_at_finish =
				endpoint->phase4.transmit.training.phase3.j_symbols;
		if ((*phase4_events &
		     V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED) != 0)
			endpoint->trn_symbols_at_mp =
				endpoint->phase4.transmit.training
					.phase4_trn_symbols;
		if ((*phase4_events &
		     V34_PHASE4_EXCHANGE_EVENT_PEER_B1_RECEIVED) != 0)
			endpoint->primary_idle_bits_remaining = 16;
		if ((*phase4_events &
		     V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) != 0 &&
		    !accept_mapping_frame(endpoint, &received_frame)) {
			fprintf(stderr, "Phase 4 mapping frame differed\n");
			return false;
		}
		if (!v34_phase4_exchange_needs_data_preparation(
			    &endpoint->phase4))
			return true;
		if (!endpoint->has_preparation_snapshot) {
			endpoint->preparation_snapshot = endpoint->phase4;
			endpoint->has_preparation_snapshot = true;
		}
		if (endpoint->incremental_preparation) {
			struct v34_data_startup startup;
			enum v34_data_startup_result prepared;

			if (!endpoint->phase4.data_preparation_active) {
				endpoint->data_preparation_count++;
				if (v34_phase4_exchange_begin_data_preparation(&endpoint->phase4,
					&endpoint->preparation) != V34_PHASE4_EXCHANGE_OK)
					return false;
			}
			endpoint->preparation_samples++;
			prepared = v34_data_startup_step_preparation(&endpoint->preparation, 1, &startup);
			if (prepared == V34_DATA_STARTUP_IN_PROGRESS)
				return !endpoint->phase4.transmit_mp_prime_started &&
				       !endpoint->phase4.transmit_data_active;
			return prepared == V34_DATA_STARTUP_OK &&
			       v34_phase4_exchange_complete_data_preparation(&endpoint->phase4,
				       &startup) == V34_PHASE4_EXCHANGE_OK;
		}
		endpoint->data_preparation_count++;
		result = v34_phase4_exchange_prepare_data_startup(
			&endpoint->phase4);
		if (result != V34_PHASE4_EXCHANGE_OK)
			fprintf(stderr, "Phase 4 data preparation: %d\n", result);
		return result == V34_PHASE4_EXCHANGE_OK;
	}
	{
		struct v34_phase34_receiver expected_receive;
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
				    V34_PHASE3_EXCHANGE_OK)
			return false;
		expected_receive = endpoint->handoff.receive;
		if (endpoint->handoff.transmit.line.profile.local_role ==
			    V34_CALLING_STATION &&
		    v34_phase34_receiver_rearm_phase4_s(
			    &expected_receive) !=
			    V34_PHASE34_RECEIVER_OK)
			return false;
		if (v34_phase4_exchange_init(
			    &endpoint->phase4, &endpoint->handoff,
			    &endpoint->phase4_configuration, 0u) !=
				    V34_PHASE4_EXCHANGE_OK)
			return false;
		endpoint->constellations_routed =
			endpoint->phase4.transmit.training.phase4_constellation ==
				handoff_detected_constellation(&endpoint->handoff) &&
			memcmp(&endpoint->phase4.transmit.training,
			       &endpoint->handoff.transmit,
			       sizeof(endpoint->handoff.transmit)) == 0 &&
			receive_constellation_is(
				&endpoint->phase4,
				endpoint->handoff.transmit.phase3
					.remote_constellation) &&
			memcmp(&endpoint->phase4.receive,
			       &expected_receive,
			       sizeof(endpoint->phase4.receive)) == 0;
		if (!endpoint->constellations_routed) {
			fprintf(stderr,
				"Phase 4 crossed the two J constellation selections\n");
			return false;
		}
		endpoint->phase4_active = true;
		return true;
	}
}

static bool add_events(uint32_t events, uint32_t *seen)
{
	uint32_t one_shot =
		events & ~V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED;

	if ((one_shot & *seen) != 0)
		return false;
	*seen |= one_shot;
	return true;
}

static bool run_initialized_pair(
	struct endpoint *calling, struct endpoint *answering,
	uint32_t *caller_seen, uint32_t *answerer_seen)
{
	int16_t caller_receive = 0;
	int16_t answerer_receive = 0;
	uint32_t caller_events_seen = 0;
	uint32_t answerer_events_seen = 0;

	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t caller_transmit;
		int16_t answerer_transmit;
		uint32_t caller_events;
		uint32_t answerer_events;

		CHECK(endpoint_sample(
			      calling, caller_receive,
			      &caller_transmit, &caller_events) &&
		      endpoint_sample(
			      answering, answerer_receive,
			      &answerer_transmit, &answerer_events),
		      "continuous Phase 3/4 exchange stopped at sample %zu",
		      index);
		CHECK(add_events(caller_events, &caller_events_seen) &&
		      add_events(answerer_events, &answerer_events_seen),
		      "Phase 4 repeated an event at sample %zu: 0x%x/0x%x",
		      index, caller_events, answerer_events);
		caller_receive = answerer_transmit;
		answerer_receive = caller_transmit;
		if (calling->phase4_active && answering->phase4_active &&
		    v34_phase4_exchange_is_online(&calling->phase4) &&
		    v34_phase4_exchange_is_online(&answering->phase4) &&
		    calling->mapping_frames_received >= 4u &&
		    answering->mapping_frames_received >= 4u) {
			calling->next_received = caller_receive;
			answering->next_received = answerer_receive;
			*caller_seen = caller_events_seen;
			*answerer_seen = answerer_events_seen;
			return true;
		}
	}
	CHECK(false,
	      "Phase 4 did not reach reciprocal data: active=%d/%d "
	      "events=0x%x/0x%x frames=%u/%u",
	      calling->phase4_active, answering->phase4_active,
	      caller_events_seen, answerer_events_seen,
	      calling->mapping_frames_received,
	      answering->mapping_frames_received);
}

static bool run_pair(
	struct endpoint *caller, struct endpoint *answerer,
	uint32_t *caller_seen, uint32_t *answerer_seen)
{
	CHECK(endpoint_init(caller, V34_CALLING_STATION) &&
	      endpoint_init(answerer, V34_ANSWERING_STATION),
	      "continuous Phase 3/4 endpoints did not initialize");
	return run_initialized_pair(
		caller, answerer, caller_seen, answerer_seen);
}

/* A protocol peer built from the public line/sequence primitives. It sends
 * S/S-bar, optional TRN, repeated MP/MP-prime, E, B1, and fresh data; it does
 * not alter the responder's state or fabricate a Phase 3 handoff. */
struct renegotiating_peer {
	struct v34_training_transmitter training;
	struct v34_phase34_receiver receive;
	struct v34_data_stream data;
	struct v34_data_startup startup;
	struct v34_mp_record local_mp;
	unsigned int acknowledgment_delay_records;
	bool damage_acknowledgment;
	bool acknowledgment_damaged;
	bool peer_mp_received;
	bool peer_acknowledged;
	bool transmitting_mp;
	bool transmitting_data;
};

static bool renegotiating_peer_sample(
	struct renegotiating_peer *peer, struct endpoint *endpoint,
	int16_t input, int16_t *output)
{
	struct v34_phase34_receive_output received;
	enum v34_phase34_receiver_result result = v34_phase34_receiver_sample(
		&peer->receive, input, &received);
	struct v34_mp_parameters *local =
		v34_mp_record_common_parameters_mutable(&peer->local_mp);

	if (result == V34_PHASE34_RECEIVER_RECORD_READY) {
		struct v34_mp_record remote = { 0 };
		const struct v34_mp_parameters *parameters;

		if (received.mp.type == V34_MP_RECEIVE_REJECTED) {
			CHECK(v34_phase34_receiver_continue_mp(&peer->receive) ==
				      V34_PHASE34_RECEIVER_OK, "peer rejected MP could not continue");
			goto transmit;
		}
		remote.type = received.mp.type == V34_MP_RECEIVE_TYPE0 ?
			V34_MP_RECORD_TYPE0 : V34_MP_RECORD_TYPE1;
		remote.parameters = received.mp.parameters;
		parameters = v34_mp_record_common_parameters(&remote);
		if (!peer->peer_mp_received) {
			struct v34_data_mode mode;

			CHECK(v34_data_mode_select_startup(peer->training.line.profile.local_role,
				      &peer->local_mp, &remote, &mode) == V34_DATA_MODE_OK &&
			      v34_data_startup_prepare(&peer->startup, &mode,
				      &peer->training.line.profile,
				      &peer->receive.transmitting_profile, SAMPLE_RATE) == V34_DATA_STARTUP_OK,
			      "rate renegotiation peer could not select data");
			peer->peer_mp_received = true;
		}
		peer->peer_acknowledged |= parameters->acknowledges_peer_mp;
		CHECK(v34_phase34_receiver_continue_mp(&peer->receive) ==
			      V34_PHASE34_RECEIVER_OK,
		      "rate renegotiation peer could not continue MP");
	} else if (result == V34_PHASE34_RECEIVER_E_COMPLETE) {
		const struct v34_data_line_receiver_configuration *configuration =
			&peer->startup.receive;

		CHECK(peer->peer_mp_received && v34_phase34_receiver_start_data(
			      &peer->receive, &configuration->normalizer,
			      &configuration->geometry, configuration->expanded_shaping,
			      configuration->precoder_coefficients,
			      configuration->trellis_selection) == V34_PHASE34_RECEIVER_OK,
		      "rate renegotiation peer could not start B1");
	} else if (result == V34_PHASE34_RECEIVER_B1_COMPLETE) {
		endpoint->primary_idle_bits_remaining = 16;
	} else if (result == V34_PHASE34_RECEIVER_MAPPING_FRAME_READY) {
		CHECK(accept_mapping_frame(endpoint, &received.received),
		      "rate renegotiation peer received bad data");
	} else {
		CHECK(result == V34_PHASE34_RECEIVER_OK ||
		      result == V34_PHASE34_RECEIVER_RATE_RENEGOTIATION ||
		      result == V34_PHASE34_RECEIVER_INTERVAL_READY ||
		      result == V34_PHASE34_RECEIVER_B1_IN_PROGRESS,
		      "rate renegotiation peer receive failed: %d", result);
	}
transmit:
	if (!peer->transmitting_data &&
	    v34_training_transmitter_phase4_e_complete(&peer->training)) {
		CHECK(v34_data_stream_continue_prepared_b1(&peer->data,
			      &peer->training.line, &peer->startup.transmit) == V34_DATA_STREAM_OK,
		      "rate renegotiation peer could not transmit B1");
		peer->transmitting_data = true;
	}
	if (peer->transmitting_data) {
		bool accepted;

		return v34_data_stream_sample(&peer->data, supply_mapping_frame,
			endpoint, output, &accepted) == V34_DATA_STREAM_OK;
	}
	if ((!peer->transmitting_mp &&
	     v34_training_transmitter_phase4_trn_ready(&peer->training)) ||
	    v34_training_transmitter_phase4_mp_record_complete(&peer->training)) {
		if (local->acknowledges_peer_mp && peer->peer_acknowledged) {
			CHECK(v34_training_transmitter_start_e(&peer->training) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "rate renegotiation peer could not transmit E");
		} else {
			if (peer->peer_mp_received &&
			    peer->acknowledgment_delay_records != 0u)
				peer->acknowledgment_delay_records--;
			else
				local->acknowledges_peer_mp = peer->peer_mp_received;
			CHECK((peer->local_mp.type == V34_MP_RECORD_TYPE0 ?
			       v34_training_transmitter_start_mp_type0(&peer->training,
				       &peer->local_mp.parameters.type0) :
			       v34_training_transmitter_start_mp_type1(&peer->training,
				       &peer->local_mp.parameters.type1)) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "rate renegotiation peer could not transmit MP");
			peer->transmitting_mp = true;
		}
	}
	/* Corrupt one transmitted MP-prime payload symbol while preserving the
	 * peer's actual scrambler and line timeline. The following E stays intact. */
	if (peer->damage_acknowledgment && local->acknowledges_peer_mp &&
	    peer->training.stage == V34_TRAINING_TRANSMITTER_PHASE4_MP &&
	    peer->training.phase4.mp.bit_index == 40u &&
	    v34_training_line_needs_point(&peer->training.line)) {
		struct qam_point point;
		uint8_t quarter, rotation;
		bool complete, accepted;

		CHECK(v34_phase4_mp_next(&peer->training.phase4.mp, &point,
			      &quarter, &rotation, &complete) == V34_PHASE4_MP_OK &&
		      v34_training_constellation_point(V34_CONSTELLATION_4_POINT,
			      quarter, (uint8_t)((rotation + 1u) & 3u), &point) ==
			      V34_CONSTELLATION_OK &&
		      v34_training_line_sample(&peer->training.line, &point,
			      output, &accepted) == V34_TRAINING_LINE_OK && accepted,
		      "damaged MP-prime symbol could not be emitted");
		peer->acknowledgment_damaged = true;
		return true;
	}
	return v34_training_transmitter_sample(&peer->training, output) ==
		V34_TRAINING_TRANSMITTER_OK;
}

static bool test_online_rate_renegotiation(void)
{
	for (unsigned int variant = 0; variant < 6u; ++variant) {
		unsigned int calling_peer = variant & 1u;
		struct endpoint caller, answerer;
		struct renegotiating_peer peer = { 0 };
		struct endpoint *initiator = calling_peer ? &caller : &answerer;
		struct endpoint *responder = calling_peer ? &answerer : &caller;
		uint32_t caller_events, answerer_events;
		uint32_t events_seen = 0;
		bool waited_for_acknowledgment = false;

		CHECK(run_pair(&caller, &answerer, &caller_events, &answerer_events),
		      "rate renegotiation initial call failed");
		CHECK(v34_training_transmitter_resume_online(&peer.training,
			      &initiator->phase4.transmit.data.line) == V34_TRAINING_TRANSMITTER_OK,
		      "rate renegotiation peer could not retain transmit line");
		peer.receive = initiator->phase4.receive;
		peer.local_mp = initiator->phase4.transmit_mp;
		peer.acknowledgment_delay_records = variant >= 2u && variant < 4u ? 6u : 0u;
		peer.damage_acknowledgment = variant >= 4u;
		v34_mp_record_common_parameters_mutable(&peer.local_mp)
			->maximum_call_to_answer_rate_2400_bps_units = 3;
		v34_mp_record_common_parameters_mutable(&peer.local_mp)
			->maximum_answer_to_call_rate_2400_bps_units = 3;
		initiator->mapping_frames_received = 0;
		responder->mapping_frames_received = 0;
		for (size_t sample = 0; sample < 16000u; ++sample) {
			int16_t peer_output, responder_output;
			uint32_t events;

			CHECK(renegotiating_peer_sample(&peer, initiator,
				      initiator->next_received, &peer_output) &&
			      endpoint_sample(responder, responder->next_received,
				      &responder_output, &events),
			      "rate renegotiation stopped: variant=%u sample=%zu", variant, sample);
			initiator->next_received = responder_output;
			responder->next_received = peer_output;
			events_seen |= events;
			if (responder->phase4.rate_renegotiation &&
			    peer.acknowledgment_delay_records != 0u &&
			    responder->phase4.transmit_mp_prime_complete) {
				CHECK(!responder->phase4.peer_mp_prime_received &&
				      !responder->phase4.transmit_data_active &&
				      responder->phase4.transmit.training.stage ==
					      V34_TRAINING_TRANSMITTER_PHASE4_MP,
				      "local MP-prime did not wait for reciprocal acknowledgment");
				waited_for_acknowledgment = true;
			}
			if (peer.transmitting_data &&
			    v34_phase4_exchange_is_online(&responder->phase4) &&
			    initiator->mapping_frames_received >= 4u &&
			    responder->mapping_frames_received >= 4u)
				break;
		}
		CHECK((variant < 2u || variant >= 4u || waited_for_acknowledgment) &&
		      (events_seen & V34_PHASE4_EXCHANGE_EVENT_RATE_RENEGOTIATION) != 0 &&
		      (events_seen & V34_PHASE4_EXCHANGE_EVENT_ONLINE) != 0 &&
		      peer.transmitting_data && initiator->mapping_frames_received >= 4u &&
		      responder->mapping_frames_received >= 4u &&
		      responder->phase4.data_startup.mode.transmit
			      .primary_data_rate_2400_bps_units == 3u &&
		      responder->phase4.data_startup.mode.receive
			      .primary_data_rate_2400_bps_units == 3u,
		      "rate renegotiation did not restore reduced-rate data: variant=%u events=0x%x frames=%u/%u",
		      variant, events_seen, initiator->mapping_frames_received,
		      responder->mapping_frames_received);
		CHECK(!peer.damage_acknowledgment ||
		      (peer.acknowledgment_damaged &&
		       !responder->phase4.peer_mp_prime_received &&
		       (events_seen & V34_PHASE4_EXCHANGE_EVENT_PEER_E_RECEIVED) != 0),
		      "missed MP-prime did not recover through E: variant=%u", variant);
	}
	return true;
}

static bool test_control_frame_deferral(void)
{
	struct endpoint caller, answerer;
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receive_output output;
	struct v34_received_mapping_frame frames[2] = {
		{ .primary_bit_count = 1, .primary_bits = { 1 } },
		{ .primary_bit_count = 1, .primary_bits = { 0 } },
	};
	uint32_t caller_events, answerer_events;

	CHECK(run_pair(&caller, &answerer, &caller_events, &answerer_events),
	      "control-frame queue fixture did not connect");
	/* Exercise the bounded queue contract at its wrap boundary, over a real
	 * initialized online receiver. A rejected candidate releases both frames. */
	receiver = caller.phase4.receive;
	receiver.pending_head = V34_PHASE34_PENDING_FRAMES - 1u;
	receiver.pending_count = 2u;
	receiver.pending_frames[receiver.pending_head] = frames[0];
	receiver.pending_frames[0] = frames[1];
	CHECK(v34_phase34_receiver_set_data_erasure(&receiver, true) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_sample(&receiver, 0, &output) ==
		      V34_PHASE34_RECEIVER_OK && receiver.pending_count == 2u &&
	      v34_phase34_receiver_set_data_erasure(&receiver, false) ==
		      V34_PHASE34_RECEIVER_OK,
	      "line erasure released or discarded the pending frames");
	for (unsigned int index = 0; index < 2u; ++index)
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &output) ==
			      V34_PHASE34_RECEIVER_MAPPING_FRAME_READY &&
		      memcmp(&output.received, &frames[index], sizeof(frames[index])) == 0,
		      "rejected S candidate lost or reordered frame %u", index);
	CHECK(receiver.pending_count == 0u && v34_phase34_receiver_is_valid(&receiver),
	      "rejected candidate queue did not drain");

	receiver = caller.phase4.receive;
	receiver.pending_frames[0] = frames[0];
	receiver.pending_frames[1] = frames[1];
	receiver.pending_count = 2u;
	receiver.confirmed_count = 1u;
	for (unsigned int symbol = 0; symbol < 12u; ++symbol) {
		struct v34_analytic_sample point;
		struct v34_phase3_s_adaptation adaptation;
		bool control;

		CHECK(v34_training_constellation_fixed_point(V34_CONSTELLATION_4_POINT,
			      0u, symbol & 1u ? 3u : 0u, &point) == V34_CONSTELLATION_OK &&
		      v34_phase3_s_hunt_receive(&receiver.rate_renegotiation, point,
			      &adaptation, &control) == V34_PHASE3_S_RECEIVER_OK,
		      "new S candidate fixture failed");
	}
	CHECK(receiver.rate_renegotiation.candidate &&
	      v34_phase34_receiver_sample(&receiver, 0, &output) ==
		      V34_PHASE34_RECEIVER_MAPPING_FRAME_READY &&
	      memcmp(&output.received, &frames[0], sizeof(frames[0])) == 0 &&
	      v34_phase34_receiver_sample(&receiver, 0, &output) !=
		      V34_PHASE34_RECEIVER_MAPPING_FRAME_READY &&
	      receiver.pending_count != 0u,
	      "new candidate either stalled confirmed data or leaked held control data");
	return true;
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

static bool test_mp_transition_schedule(void)
{
	const struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = 1,
	};
	static const struct {
		enum v34_symbol_rate rate;
		uint32_t zero_delay;
		uint32_t delayed_answerer;
	} cases[] = {
		{ V34_SYMBOL_RATE_2400, 4200u, 4733u },
		{ V34_SYMBOL_RATE_2743, 4800u, 5409u },
		{ V34_SYMBOL_RATE_2800, 4900u, 5522u },
		{ V34_SYMBOL_RATE_3000, 5250u, 5916u },
		{ V34_SYMBOL_RATE_3200, 5600u, 6310u },
		{ V34_SYMBOL_RATE_3429, 6000u, 6761u },
	};
	static const uint16_t delays[] = { 0u, 1u, 1775u, 3920u, UINT16_MAX };
	const uint16_t measured_delay = 1775u;
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;
	struct v34_phase3_profile invalid;
	uint32_t symbols;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		CHECK(v34_phase3_profile_init(
			      &caller, V34_CALLING_STATION,
			      cases[index].rate, &probe, 0u, 0u) ==
			      V34_PHASE3_PROFILE_OK &&
		      v34_phase3_profile_init(
			      &answerer, V34_ANSWERING_STATION,
			      cases[index].rate, &probe, 0u, 0u) ==
			      V34_PHASE3_PROFILE_OK,
		      "Phase 4 schedule profile failed for rate %u",
		      (unsigned int)cases[index].rate);
		CHECK(v34_phase4_mp_transition_symbols(
			      &caller, 0u,
			      &symbols) &&
		      symbols == cases[index].zero_delay &&
		      v34_phase4_mp_transition_symbols(
			      &answerer, 0u,
			      &symbols) &&
		      symbols == cases[index].zero_delay &&
		      v34_phase4_mp_transition_symbols(
			      &answerer, measured_delay, &symbols) &&
		      symbols == cases[index].delayed_answerer &&
		      v34_phase4_mp_transition_symbols(
			      &caller, measured_delay, &symbols) &&
		      symbols == cases[index].zero_delay,
		      "Phase 4 MP transition changed for rate %u",
		      (unsigned int)cases[index].rate);
		for (size_t delay = 0; delay < ARRAY_LENGTH(delays); ++delay) {
			uint32_t caller_symbols;
			uint64_t duration;
			uint64_t limit;

			CHECK(v34_phase4_mp_transition_symbols(
				      &caller, delays[delay], &caller_symbols) &&
			      caller_symbols == cases[index].zero_delay &&
			      v34_phase4_mp_transition_symbols(
				      &answerer, delays[delay], &symbols),
			      "RTD changed caller TRN or rejected answerer TRN");
			/* Cross-multiply to check wall-clock limits independently
			 * of the implementation's sample-to-symbol conversion. */
			duration = (uint64_t)caller_symbols *
				caller.symbol_rate_denominator;
			CHECK(caller_symbols >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
			      duration <= 2u * caller.symbol_rate_numerator,
			      "caller TRN is outside the 512T..2s window");
			duration = (uint64_t)symbols * SAMPLE_RATE *
				answerer.symbol_rate_denominator;
			limit = (uint64_t)(2u * SAMPLE_RATE + delays[delay]) *
				answerer.symbol_rate_numerator;
			CHECK(symbols >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
			      duration <= limit,
			      "answerer TRN is outside the 512T..2s+RTD window");
		}
	}
	invalid = caller;
	invalid.local_role = (enum v34_station_role)99;
	symbols = UINT32_C(0xa55a5aa5);
	CHECK(!v34_phase4_mp_transition_symbols(
		      &invalid, 0u, &symbols) &&
	      !v34_phase4_mp_transition_symbols(
		      NULL, 0u, &symbols) &&
	      !v34_phase4_mp_transition_symbols(&caller, 0u, NULL) &&
	      symbols == UINT32_C(0xa55a5aa5),
	      "invalid MP transition arguments changed the output");
	return true;
}

static bool test_incremental_exchange(void)
{
	for (unsigned int variant = 1; variant <= 3; ++variant) {
		struct endpoint caller, answerer;
		uint32_t caller_seen, answerer_seen;

		CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
		      endpoint_init(&answerer, V34_ANSWERING_STATION),
		      "incremental endpoints did not initialize");
		caller.incremental_preparation = (variant & 1u) != 0;
		answerer.incremental_preparation = (variant & 2u) != 0;
		CHECK(run_initialized_pair(&caller, &answerer, &caller_seen, &answerer_seen),
		      "incremental exchange failed against a synchronous peer: %u", variant);
		struct endpoint *ends[] = { &caller, &answerer };
		for (size_t i = 0; i < ARRAY_LENGTH(ends); ++i) {
			CHECK(ends[i]->data_preparation_count == 1 &&
			      (!ends[i]->incremental_preparation ||
			       (ends[i]->preparation_samples > 160 && ends[i]->preparation_samples < 3000)),
			      "calibration did not span bounded line samples");
			CHECK(v34_phase4_exchange_is_online(&ends[i]->phase4),
			      "incremental exchange did not deliver both data directions");
		}
	}
	return true;
}

static bool test_generated_online_exchange(void)
{
	const uint32_t common_events =
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_E_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_PEER_E_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_STARTED |
		V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_COMPLETE |
		V34_PHASE4_EXCHANGE_EVENT_PEER_B1_RECEIVED |
		V34_PHASE4_EXCHANGE_EVENT_ONLINE;
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_data_startup caller_startup;
	struct v34_data_startup answerer_startup;
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
	CHECK(caller.phase4.transmit_data_active &&
	      answerer.phase4.transmit_data_active &&
	      caller.phase4.transmit_mp_prime_complete &&
	      answerer.phase4.transmit_mp_prime_complete &&
	      caller.phase4.receive_data_active &&
	      answerer.phase4.receive_data_active &&
	      caller.phase4.transmit_b1_complete &&
	      answerer.phase4.transmit_b1_complete &&
	      caller.phase4.receive_b1_complete &&
	      answerer.phase4.receive_b1_complete &&
	      v34_phase4_exchange_is_online(&caller.phase4) &&
	      v34_phase4_exchange_is_online(&answerer.phase4) &&
	      !v34_phase4_exchange_has_failed(&caller.phase4) &&
	      !v34_phase4_exchange_has_failed(&answerer.phase4) &&
	      caller.data_preparation_count == 1 &&
	      answerer.data_preparation_count == 1 &&
	      caller.j_symbols_at_peer_s != 0 &&
	      caller.j_symbols_at_finish >=
		      V34_PHASE4_J_RELEASE_MIN_SYMBOLS &&
	      caller.j_symbols_at_finish >= caller.j_symbols_at_peer_s &&
	      caller.j_symbols_at_finish -
		      (caller.j_symbols_at_peer_s <
			       V34_PHASE4_J_RELEASE_MIN_SYMBOLS ?
		       V34_PHASE4_J_RELEASE_MIN_SYMBOLS :
		       caller.j_symbols_at_peer_s) <
		      V34_PHASE3_J_PATTERN_SYMBOLS &&
	      caller.j_symbols_at_finish % V34_PHASE3_J_PATTERN_SYMBOLS == 0 &&
	      caller.trn_symbols_at_mp >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
	      caller.trn_symbols_at_mp <= 2u * 3000u &&
	      answerer.trn_symbols_at_mp == 5250u &&
	      caller.constellations_routed &&
	      answerer.constellations_routed &&
	      caller.mapping_frames_supplied >=
		      answerer.mapping_frames_received &&
	      answerer.mapping_frames_supplied >=
		      caller.mapping_frames_received,
	      "Phase 4 owners reached the wrong online boundary (J %u->%u, TRN %u/%u)",
	      caller.j_symbols_at_peer_s, caller.j_symbols_at_finish,
	      caller.trn_symbols_at_mp, answerer.trn_symbols_at_mp);
	CHECK(v34_phase4_exchange_get_data_startup(
		      &caller.phase4, &caller_startup) &&
	      v34_phase4_exchange_get_data_startup(
		      &answerer.phase4, &answerer_startup) &&
	      v34_data_startup_is_valid(&caller_startup) &&
	      v34_data_startup_is_valid(&answerer_startup) &&
	      caller_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 4 &&
	      caller_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 4 &&
	      answerer_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 4 &&
	      answerer_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 4 &&
	      directions_are_equal(
		      &caller_startup.mode.transmit,
		      &answerer_startup.mode.receive) &&
	      directions_are_equal(
		      &caller_startup.mode.receive,
		      &answerer_startup.mode.transmit),
	      "Phase 4 prepared incompatible post-E data directions");
	CHECK(caller.phase4.peer_mp_received &&
	      answerer.phase4.peer_mp_received &&
	      v34_mp_record_equal(
		      &caller.phase4.peer_mp,
		      &answerer.phase4.transmit_mp) &&
	      v34_mp_record_equal(
		      &answerer.phase4.peer_mp,
		      &caller.phase4.transmit_mp) &&
	      caller.phase4.receive.feedback_training_complete &&
	      answerer.phase4.receive.feedback_training_complete,
	      "Phase 4 owners did not retain normalized peer records");
	return true;
}

static bool test_peer_mp_preserves_trn_schedule(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_training_transmitter peer_transmit;
	int16_t caller_receive = 0;
	int16_t answerer_receive = 0;
	uint32_t trn_symbols_at_peer_mp = 0;
	bool trn_ready = false;

	CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
	      endpoint_init(&answerer, V34_ANSWERING_STATION),
	      "early-peer-MP endpoints did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t caller_transmit;
		int16_t answerer_transmit;
		uint32_t caller_events;
		uint32_t answerer_events;

		CHECK(endpoint_sample(
			      &caller, caller_receive,
			      &caller_transmit, &caller_events) &&
		      endpoint_sample(
			      &answerer, answerer_receive,
			      &answerer_transmit, &answerer_events),
		      "early-peer-MP setup stopped at sample %zu", index);
		caller_receive = answerer_transmit;
		answerer_receive = caller_transmit;
		if (caller.phase4_active && answerer.phase4_active &&
		    caller.phase4.transmit.training.stage ==
			    V34_TRAINING_TRANSMITTER_PHASE4_TRN &&
		    answerer.phase4.transmit.training.stage ==
			    V34_TRAINING_TRANSMITTER_PHASE4_TRN &&
		    caller.phase4.transmit.training.phase4_trn_symbols >= 1024u) {
			trn_ready = true;
			break;
		}
	}
	CHECK(trn_ready && !caller.phase4.peer_mp_received &&
	      !caller.phase4.transmit_mp_started,
	      "early-peer-MP setup missed the open TRN interval");
	peer_transmit = answerer.phase4.transmit.training;
	CHECK(v34_training_transmitter_start_mp_type0(
		      &peer_transmit,
		      &answerer.phase4_configuration.transmit_mp.parameters.type0) ==
		      V34_TRAINING_TRANSMITTER_OK,
	      "early peer MP did not start");
	{
		int16_t discarded;
		uint32_t events;

		CHECK(endpoint_sample(
			      &caller, caller_receive, &discarded, &events) &&
		      (events & V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED) == 0,
		      "pending peer TRN changed the local MP boundary");
	}
	for (size_t index = 0; index < 2000u; ++index) {
		int16_t peer_sample;
		int16_t discarded;
		uint32_t events;

		if (v34_training_transmitter_phase4_mp_record_complete(
			    &peer_transmit)) {
			CHECK(v34_training_transmitter_start_mp_type0(
				      &peer_transmit,
				      &answerer.phase4_configuration.transmit_mp
					       .parameters.type0) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "early peer MP repeat did not start");
		}
		CHECK(v34_training_transmitter_sample(
			      &peer_transmit, &peer_sample) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      endpoint_sample(
			      &caller, peer_sample, &discarded, &events),
		      "early peer MP exchange stopped at sample %zu", index);
		if ((events & V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED) == 0)
			continue;
		trn_symbols_at_peer_mp = caller.phase4.transmit.training
			.phase4_trn_symbols;
		CHECK((events & V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED) == 0 &&
		      trn_symbols_at_peer_mp >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
		      trn_symbols_at_peer_mp <
			      caller.phase4.mp_transition_symbols &&
		      !caller.phase4.transmit_mp_started &&
		      caller.phase4.receive.feedback_training_complete &&
		      caller.data_preparation_count == 1u,
		      "valid peer MP crossed the local TRN schedule at %u/%uT",
		      trn_symbols_at_peer_mp,
		      caller.phase4.mp_transition_symbols);
		break;
	}
	CHECK(trn_symbols_at_peer_mp != 0u,
	      "early peer MP was not accepted");
	{
		struct v34_phase4_exchange crossed =
			caller.preparation_snapshot;
		struct v34_received_mapping_frame received = { 0 };
		int16_t sample = 0;
		uint32_t events = 0;

		crossed.peer_mp_prime_received = true;
		CHECK(caller.has_preparation_snapshot &&
		      crossed.receive.feedback_training_complete &&
		      !crossed.transmit_mp_started &&
		      v34_phase4_exchange_is_valid(&crossed) &&
		      v34_phase4_exchange_prepare_data_startup(&crossed) ==
			      V34_PHASE4_EXCHANGE_OK &&
		      crossed.receive.semantic.phase4.stage ==
			      V34_PHASE4_DECISION_RECEIVER_MP,
		      "early peer MP-prime did not arm independent E reception");
		for (size_t index = 0;
		     crossed.transmit.training.phase4_trn_symbols <
			     crossed.mp_transition_symbols;
		     ++index) {
			CHECK(index < SAMPLE_LIMIT &&
			      v34_training_transmitter_sample(
				      &crossed.transmit.training, &sample) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "crossed E/TRN state missed the local MP boundary");
		}
		CHECK(v34_phase4_exchange_sample(
			      &crossed, 0, NULL, NULL, &sample, &events,
			      &received) == V34_PHASE4_EXCHANGE_OK &&
		      events == V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED &&
		      crossed.transmit_mp_started &&
		      crossed.receive.semantic.phase4.stage ==
			      V34_PHASE4_DECISION_RECEIVER_MP,
		      "independent E reception blocked the scheduled local MP");
	}
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t peer_sample;
		int16_t discarded;
		uint32_t events;

		if (v34_training_transmitter_phase4_mp_record_complete(
			    &peer_transmit)) {
			CHECK(v34_training_transmitter_start_mp_type0(
				      &peer_transmit,
				      &answerer.phase4_configuration.transmit_mp
					       .parameters.type0) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "peer MP repeat did not restart");
		}
		CHECK(v34_training_transmitter_sample(
			      &peer_transmit, &peer_sample) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      endpoint_sample(
			      &caller, peer_sample, &discarded, &events),
		      "scheduled local TRN stopped at sample %zu", index);
		if ((events & V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED) == 0)
			continue;
		CHECK(caller.trn_symbols_at_mp ==
			      caller.phase4.mp_transition_symbols &&
		      caller.trn_symbols_at_mp > trn_symbols_at_peer_mp &&
		      caller.phase4.receive.feedback_training_complete,
		      "local MP started outside its schedule at %u/%uT",
		      caller.trn_symbols_at_mp,
		      caller.phase4.mp_transition_symbols);
		return true;
	}
	CHECK(false, "local MP did not start on its retained schedule");
}

static bool test_phase4_owns_selected_rates(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_data_startup caller_startup;
	struct v34_data_startup answerer_startup;
	const struct v34_mp_parameters *caller_mp;
	const struct v34_mp_parameters *answerer_mp;
	const struct v34_mp_parameters *caller_configuration;
	const struct v34_mp_parameters *answerer_configuration;
	uint32_t caller_seen;
	uint32_t answerer_seen;

	CHECK(endpoint_init_with_rates(
		      &caller, V34_CALLING_STATION, 4, 6, 8) &&
	      endpoint_init_with_rates(
		      &answerer, V34_ANSWERING_STATION, 6, 4, 8),
	      "asymmetric projected-rate endpoints did not initialize");
	caller_configuration = v34_mp_record_common_parameters(
		&caller.phase4_configuration.transmit_mp);
	answerer_configuration = v34_mp_record_common_parameters(
		&answerer.phase4_configuration.transmit_mp);
	CHECK(caller_configuration
		      ->maximum_call_to_answer_rate_2400_bps_units == 8 &&
	      caller_configuration
		      ->maximum_answer_to_call_rate_2400_bps_units == 8 &&
	      answerer_configuration
		      ->maximum_call_to_answer_rate_2400_bps_units == 8 &&
	      answerer_configuration
		      ->maximum_answer_to_call_rate_2400_bps_units == 8,
	      "projected-rate setup changed its MP configurations");
	CHECK(run_initialized_pair(
		      &caller, &answerer, &caller_seen, &answerer_seen),
	      "asymmetric projected-rate exchange failed");
	caller_mp = v34_mp_record_common_parameters(
		&caller.phase4.transmit_mp);
	answerer_mp = v34_mp_record_common_parameters(
		&answerer.phase4.transmit_mp);
	/* Calibrated Phase 3 power supports the configured limit in both
	 * directions, independently of the lower Phase 2 projections. */
	CHECK(caller_mp->maximum_call_to_answer_rate_2400_bps_units == 8 &&
	      caller_mp->maximum_answer_to_call_rate_2400_bps_units == 8 &&
	      answerer_mp->maximum_call_to_answer_rate_2400_bps_units == 8 &&
	      answerer_mp->maximum_answer_to_call_rate_2400_bps_units == 8,
	      "Phase 2 projections or trained quality changed Phase 4 MP rates");
	CHECK(v34_phase4_exchange_get_data_startup(
		      &caller.phase4, &caller_startup) &&
	      v34_phase4_exchange_get_data_startup(
		      &answerer.phase4, &answerer_startup) &&
	      caller_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 8 &&
	      caller_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 8 &&
	      answerer_startup.mode.transmit
		      .primary_data_rate_2400_bps_units == 8 &&
	      answerer_startup.mode.receive
		      .primary_data_rate_2400_bps_units == 8 &&
	      directions_are_equal(
		      &caller_startup.mode.transmit,
		      &answerer_startup.mode.receive) &&
	      directions_are_equal(
		      &caller_startup.mode.receive,
		      &answerer_startup.mode.transmit),
	      "Phase 4 did not own the negotiated data rates");
	return true;
}

static bool test_source_backpressure(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	uint32_t caller_seen;
	uint32_t answerer_seen;

	CHECK(run_pair(
		      &caller, &answerer, &caller_seen, &answerer_seen),
	      "source-backpressure pair failed");
	for (size_t index = 0; index < 1000u; ++index) {
		struct v34_phase4_exchange before = caller.phase4;
		struct v34_received_mapping_frame received;
		struct v34_received_mapping_frame received_before;
		int16_t caller_transmit = 1234;
		int16_t answerer_transmit;
		uint32_t caller_events = UINT32_C(0x5aa5a55a);
		uint32_t answerer_events;
		enum v34_phase4_exchange_result result;

		memset(&received, 0xa5, sizeof(received));
		received_before = received;
		result = v34_phase4_exchange_sample(
			&caller.phase4, caller.next_received,
			reject_mapping_frame, NULL, &caller_transmit,
			&caller_events, &received);
		if (result == V34_PHASE4_EXCHANGE_SOURCE_FAILURE) {
			CHECK(memcmp(&caller.phase4, &before, sizeof(before)) == 0 &&
			      caller_transmit == 1234 &&
			      caller_events == UINT32_C(0x5aa5a55a) &&
			      memcmp(&received, &received_before,
				     sizeof(received)) == 0,
			      "failed mapping source changed Phase 4 state or outputs");
			return true;
		}
		CHECK(result == V34_PHASE4_EXCHANGE_OK &&
		      endpoint_sample(
			      &answerer, answerer.next_received,
			      &answerer_transmit, &answerer_events),
		      "source-backpressure timeline failed at sample %zu", index);
		caller.next_received = answerer_transmit;
		answerer.next_received = caller_transmit;
	}
	CHECK(false, "mapping source was not consulted within one data frame");
}

static bool complete_local_mp_record(struct v34_phase4_exchange *exchange)
{
	for (size_t sample_index = 0; sample_index < 1000u; ++sample_index) {
		int16_t sample;

		if (v34_training_transmitter_phase4_mp_record_complete(
			    &exchange->transmit.training))
			return true;
		if (v34_training_transmitter_sample(
			    &exchange->transmit.training, &sample) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool test_mp_acknowledgment_boundaries(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_phase4_exchange pending;
	const struct v34_phase4_exchange *snapshot;
	uint32_t caller_seen;
	uint32_t answerer_seen;
	uint32_t events = 0;

	CHECK(run_pair(
		      &caller, &answerer, &caller_seen, &answerer_seen),
	      "acknowledged-record setup pair failed");
	CHECK(caller.has_preparation_snapshot &&
	      answerer.has_preparation_snapshot,
	      "generated exchange did not expose its preparation boundary");
	{
		struct v34_phase4_exchange crossed = caller.preparation_snapshot;

		crossed.transmit.training.rate_renegotiation = true;
		CHECK(v34_training_transmitter_is_valid(&crossed.transmit.training) &&
		      !v34_phase4_exchange_is_valid(&crossed),
		      "initial exchange accepted an online transmit context");
	}
	snapshot = !caller.preparation_snapshot.peer_mp_prime_received ?
		&caller.preparation_snapshot :
		&answerer.preparation_snapshot;
	CHECK(!snapshot->peer_mp_prime_received &&
	      snapshot->peer_mp_received &&
	      snapshot->receive.feedback_training_complete &&
	      snapshot->transmit_mp_started &&
	      v34_phase4_exchange_needs_data_preparation(snapshot),
	      "preparation snapshot cannot model an acknowledged first record");
	pending = *snapshot;
	pending.peer_mp_prime_received = true;
	CHECK(v34_phase4_exchange_is_valid(&pending),
	      "early reciprocal MP-prime made pending state invalid");
	CHECK(v34_phase4_exchange_prepare_data_startup(
		      &pending) == V34_PHASE4_EXCHANGE_OK &&
	      v34_phase4_exchange_is_valid(&pending) &&
	      !v34_phase4_exchange_needs_data_preparation(&pending) &&
	      pending.receive.semantic.phase4.stage ==
		      V34_PHASE4_DECISION_RECEIVER_MP,
	      "early reciprocal MP-prime did not arm E reception");
	CHECK(!pending.transmit_mp_prime_started &&
	      !pending.transmit_mp_prime_complete,
	      "ordinary MP already retained an acknowledgment");
	{
		struct v34_received_mapping_frame received = { 0 };
		int16_t sample = 0;

		CHECK(complete_local_mp_record(&pending),
		      "ordinary MP did not reach its current record boundary");
		events = 0;
		CHECK(v34_phase4_exchange_sample(
			      &pending, 0, NULL, NULL, &sample, &events,
			      &received) == V34_PHASE4_EXCHANGE_OK,
		      "ordinary MP record did not complete");
		CHECK(pending.transmit_mp_prime_started &&
		      !pending.transmit_mp_prime_complete &&
		      events == V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED,
		      "current ordinary MP boundary did not start MP-prime");
	}
	{
		struct v34_received_mapping_frame received = { 0 };
		int16_t sample = 0;

		CHECK(complete_local_mp_record(&pending),
		      "local MP-prime did not reach its first record boundary");
		events = 0;
		CHECK(v34_phase4_exchange_sample(
			      &pending, 0, NULL, NULL, &sample, &events,
			      &received) == V34_PHASE4_EXCHANGE_OK &&
		      pending.transmit_mp_prime_complete &&
		      pending.transmit.training.stage ==
			      V34_TRAINING_TRANSMITTER_PHASE4_E &&
		      events == V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_E_STARTED,
		      "acknowledged MP-prime did not release E at its first "
		      "complete record boundary");
	}
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
	struct v34_mp_record invalid_record =
		mp_record(V34_CALLING_STATION);
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
	exchange = caller.phase4;
	exchange.receive.failed = true;
	CHECK(!v34_phase4_exchange_is_valid(&exchange) &&
	      !v34_phase4_exchange_has_failed(&exchange),
	      "Phase 4 accepted an unpropagated receiver failure");
	exchange.failed = true;
	CHECK(v34_phase4_exchange_is_valid(&exchange) &&
	      v34_phase4_exchange_has_failed(&exchange),
	      "Phase 4 rejected a propagated receiver failure");
	exchange.receive.semantic.data.data_receiver.channel_receiver
		.frame_decoder.channel_decoder.survivor.path_index =
			V34_SURVIVOR_TRACEBACK_INTERVALS;
	CHECK(!v34_phase4_exchange_is_valid(&exchange) &&
	      !v34_phase4_exchange_has_failed(&exchange),
	      "failure flag hid invalid nested Phase 4 receive history");
	{
		struct v34_phase4_exchange_configuration configuration =
			phase4_configuration(V34_CALLING_STATION);
		struct v34_phase34_receiver expected_receive;
		struct v34_receive_timing expected_timing =
			caller.handoff.receive.line.timing;

		expected_receive = caller.handoff.receive;
		expected_timing.timing.feedback_held = false;
		CHECK(v34_phase4_exchange_init(
			      &exchange, &caller.handoff, &configuration, 17u) ==
			      V34_PHASE4_EXCHANGE_OK &&
		      v34_phase34_receiver_rearm_phase4_s(
			      &expected_receive) ==
			      V34_PHASE34_RECEIVER_OK &&
		      exchange.receive.phase4_s_rearmed &&
		      !exchange.receive.line.timing.timing.feedback_held &&
		      v34_receive_timing_is_acquired(&exchange.receive.line.timing) &&
		      memcmp(&exchange.receive.line.timing,
			     &expected_timing,
			     sizeof(exchange.receive.line.timing)) == 0 &&
		      memcmp(&exchange.receive.line.equalizer,
			     &caller.handoff.receive.line.equalizer,
			     sizeof(exchange.receive.line.equalizer)) == 0 &&
		      memcmp(&exchange.receive, &expected_receive,
			     sizeof(exchange.receive)) == 0,
		      "caller Phase 4 did not retain the receive lineage");
		{
			struct v34_phase3_exchange_handoff partial = caller.handoff;
			struct v34_line_receiver *line = &partial.receive.line;

			for (unsigned int index = 0u;
			     index < 16u && line->equalizer.inputs_since_observation == 0u;
			     ++index) {
				struct v34_adaptive_equalizer_observation observation;
				enum v34_line_receiver_result result =
					v34_line_receiver_sample(line, 0, &observation);

				CHECK(result == V34_LINE_RECEIVER_OK ||
				      (result == V34_LINE_RECEIVER_OBSERVATION_READY &&
				       v34_adaptive_equalizer_discard(
					       &line->equalizer, &observation) ==
					       V34_ADAPTIVE_EQUALIZER_OK),
				      "partial handoff could not advance the physical clock");
			}
			expected_timing = line->timing;
			expected_timing.timing.feedback_held = false;
			CHECK(line->equalizer.inputs_since_observation != 0u &&
			      v34_phase4_exchange_init(
				      &exchange, &partial, &configuration, 17u) ==
				      V34_PHASE4_EXCHANGE_OK &&
			      memcmp(&exchange.receive.line.equalizer, &line->equalizer,
				     sizeof(line->equalizer)) == 0 &&
			      memcmp(&exchange.receive.line.timing, &expected_timing,
				     sizeof(expected_timing)) == 0,
			      "Phase 4 discarded or restarted a partial physical symbol");
		}

		configuration = phase4_configuration(V34_ANSWERING_STATION);
		CHECK(v34_phase4_exchange_init(
			      &exchange, &answerer.handoff, &configuration, 17u) ==
			      V34_PHASE4_EXCHANGE_OK &&
		      !exchange.receive.phase4_s_rearmed &&
		      !exchange.receive.line.timing.reanchor_at_s &&
		      memcmp(&exchange.receive, &answerer.handoff.receive,
			     sizeof(exchange.receive)) == 0,
		      "answerer incorrectly applied the caller-only S rearm");
	}
	exchange = caller.phase4;
	exchange.transmit_mp_prime_complete = false;
	CHECK(!v34_phase4_exchange_is_valid(&exchange),
	      "Phase 4 accepted data without a completed local MP-prime");
	exchange = caller.phase4;
	exchange.transmit_mp_prime_started = false;
	CHECK(!v34_phase4_exchange_is_valid(&exchange),
	      "Phase 4 accepted MP-prime completions without MP-prime");
	exchange = caller.phase4;
	exchange.mp_transition_symbols = 0u;
	CHECK(!v34_phase4_exchange_is_valid(&exchange),
	      "Phase 4 accepted a changed MP transition target");
	invalid = phase4_configuration(V34_CALLING_STATION);
	invalid.transmit_mp.parameters.type1.common.acknowledges_peer_mp = true;
	memset(&exchange, 0xa5, sizeof(exchange));
	saved = exchange;
	CHECK(v34_phase4_exchange_init(
		      &exchange, &caller.handoff, &invalid, 0u) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "pre-acknowledged MP configuration changed the destination");
	saved = caller.phase4;
	CHECK(v34_phase4_exchange_sample(
		      &caller.phase4, 0, NULL, NULL,
		      NULL, &events, (struct v34_received_mapping_frame[1]){ 0 }) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0, NULL, NULL,
		      &sample, NULL, (struct v34_received_mapping_frame[1]){ 0 }) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0, NULL, NULL,
		      &sample, &events, NULL) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0,
		      NULL, NULL, &overlap.samples[0], &overlap.events,
		      (struct v34_received_mapping_frame[1]){ 0 }) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase4_exchange_sample(
		      &caller.phase4, 0,
		      NULL, NULL, &overlap.samples[1], &overlap.events,
		      (struct v34_received_mapping_frame[1]){ 0 }) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&caller.phase4, &saved, sizeof(saved)) == 0 &&
	      sample == 1234 && events == UINT32_C(0x5aa5a55a) &&
	      overlap.events == UINT32_C(0xa55a5aa5),
	      "rejected Phase 4 samples were not transactional");
	saved = caller.phase4;
	CHECK(v34_phase4_exchange_prepare_data_startup(
		      &caller.phase4) ==
		      V34_PHASE4_EXCHANGE_INVALID_STATE &&
	      v34_phase4_exchange_prepare_data_startup(
		      NULL) ==
		      V34_PHASE4_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&caller.phase4, &saved, sizeof(saved)) == 0 &&
	      events == UINT32_C(0x5aa5a55a),
	      "out-of-sequence data preparation changed the exchange");
	memset(&startup, 0xa5, sizeof(startup));
	saved_startup = startup;
	CHECK(!v34_phase4_exchange_is_valid(NULL) &&
	      !v34_phase4_exchange_has_failed(NULL) &&
	      !v34_phase4_exchange_needs_data_preparation(NULL) &&
	      !v34_phase4_exchange_is_online(NULL) &&
	      !v34_phase4_exchange_get_data_startup(NULL, &startup) &&
	      !v34_phase4_exchange_get_data_startup(
		      &caller.phase4, NULL) &&
	      memcmp(&startup, &saved_startup, sizeof(startup)) == 0 &&
	      !v34_mp_record_is_valid(NULL),
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
		{ "role-aware MP transition schedule",
		  test_mp_transition_schedule },
		{ "generated online exchange",
		  test_generated_online_exchange },
		{ "incremental exchange with synchronous peers", test_incremental_exchange },
		{ "online rate renegotiation in either direction",
		  test_online_rate_renegotiation },
		{ "unconfirmed control frames retain data", test_control_frame_deferral },
		{ "peer MP preserves local TRN schedule",
		  test_peer_mp_preserves_trn_schedule },
		{ "Phase 4 owns selected data rates",
		  test_phase4_owns_selected_rates },
		{ "mapping source backpressure",
		  test_source_backpressure },
		{ "MP acknowledgment boundaries",
		  test_mp_acknowledgment_boundaries },
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
