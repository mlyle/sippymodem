// SPDX-License-Identifier: BSD-3-Clause

#include <v34_call.h>

#include <g711.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum {
	SAMPLE_LIMIT = 140000,
	LINE_DELAY_SAMPLES = 13,
	NOMINAL_AMPLITUDE = 7000,
};

struct channel {
	int16_t call_to_answer[LINE_DELAY_SAMPLES];
	int16_t answer_to_call[LINE_DELAY_SAMPLES];
	size_t cursor;
	bool pcma;
};

struct endpoint {
	struct v34_call call;
	unsigned int mapping_frames_supplied;
	unsigned int mapping_frames_received;
	uint8_t transmit_bit;
	bool verify_mapping_frames;
	bool entered_phase3;
	bool entered_phase4;
};

static struct v34_info0 info0(enum v34_station_role role)
{
	if (role == V34_CALLING_STATION)
		return (struct v34_info0){
			.symbol_rate_2800_supported = true,
			.low_carrier_3000_supported = true,
			.maximum_symbol_rate_difference_steps = 2,
			.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
		};
	return (struct v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source = V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static union v34_local_info1 info1(enum v34_station_role role)
{
	union v34_local_info1 selected = { 0 };

	if (role == V34_CALLING_STATION) {
		selected.calling.common.frequency_offset_1_50_hz_units = -10;
		selected.calling.answer_transmit[V34_SYMBOL_RATE_3200] =
			(struct v34_info1_probe_result){
				.high_carrier = true,
				.preemphasis_filter_index = 3,
				.projected_data_rate_2400_bps_units = 4,
			};
	} else {
		selected.answering.common.frequency_offset_1_50_hz_units = 11;
		selected.answering.call_transmit =
			(struct v34_info1_probe_result){
				.preemphasis_filter_index = 4,
				.projected_data_rate_2400_bps_units = 4,
			};
		selected.answering.answer_to_call_symbol_rate =
			V34_SYMBOL_RATE_3200;
		selected.answering.call_to_answer_symbol_rate =
			V34_SYMBOL_RATE_3000;
	}
	return selected;
}

static struct v34_mp_parameters mp_parameters(
	enum v34_station_role role)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 4,
		.maximum_answer_to_call_rate_2400_bps_units = 4,
		.auxiliary_channel_enabled = role == V34_CALLING_STATION,
		.remote_transmitter_trellis =
			role == V34_CALLING_STATION ?
				V34_TRELLIS_32_STATE : V34_TRELLIS_16_STATE,
		.remote_transmitter_nonlinear_encoding =
			role == V34_ANSWERING_STATION,
		.remote_transmitter_expanded_shaping = true,
		.data_rate_capability_mask = 0x000fu,
		.asymmetric_rates_enabled = true,
	};
}

static struct v34_mp_record mp_record(enum v34_station_role role)
{
	return (struct v34_mp_record){
		.type = V34_MP_RECORD_TYPE0,
		.parameters.type0 = mp_parameters(role),
	};
}

static struct v34_call_configuration configuration(
	enum v34_station_role role)
{
	struct v34_phase3_line_receiver_configuration receive = {
		.s_gain_adaptation = 0.1,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};

	return (struct v34_call_configuration){
		.local_role = role,
		.phase2_nominal_amplitude = NOMINAL_AMPLITUDE,
		.local_info0 = info0(role),
		.local_info1 = info1(role),
		.phase3 = {
			.receive = receive,
			.requested_remote_constellation =
				role == V34_CALLING_STATION ?
					V34_CONSTELLATION_16_POINT :
					V34_CONSTELLATION_4_POINT,
			.sample_rate = V34_PHASE2_SAMPLE_RATE,
			.nominal_peak_amplitude = NOMINAL_AMPLITUDE,
		},
		.phase4 = {
			.receive = {
				.phase3 = receive,
				.phase4_equalizer_adaptation = 0.03,
			},
			.transmit_mp = mp_record(role),
			.data_equalizer_adaptation = 0.02,
		},
	};
}

static struct v34_call_configuration symmetric_configuration(
	enum v34_station_role role, enum v34_symbol_rate symbol_rate)
{
	struct v34_call_configuration selected = configuration(role);
	uint8_t preemphasis_filter_index =
		symbol_rate == V34_SYMBOL_RATE_2800 ? 8u : 3u;
	bool high_carrier = symbol_rate == V34_SYMBOL_RATE_2743;

	selected.local_info0 = (struct v34_info0){
		.symbol_rate_2743_supported =
			symbol_rate == V34_SYMBOL_RATE_2743,
		.symbol_rate_2800_supported =
			symbol_rate == V34_SYMBOL_RATE_2800,
		.symbol_rate_3429_supported =
			symbol_rate == V34_SYMBOL_RATE_3429,
		.symbol_rate_3429_transmit_permitted =
			symbol_rate == V34_SYMBOL_RATE_3429,
		.low_carrier_3200_supported =
			symbol_rate == V34_SYMBOL_RATE_3200,
		.maximum_symbol_rate_difference_steps = 0,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
	selected.local_info1 = (union v34_local_info1){ 0 };
	if (role == V34_CALLING_STATION) {
		selected.local_info1.calling.common
			.frequency_offset_1_50_hz_units =
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		selected.local_info1.calling.answer_transmit[symbol_rate]
			.high_carrier = high_carrier;
		selected.local_info1.calling.answer_transmit[symbol_rate]
			.preemphasis_filter_index = preemphasis_filter_index;
		selected.local_info1.calling.answer_transmit[symbol_rate]
			.projected_data_rate_2400_bps_units = 4;
	} else {
		selected.local_info1.answering.common
			.frequency_offset_1_50_hz_units =
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		selected.local_info1.answering.call_transmit
			.high_carrier = high_carrier;
		selected.local_info1.answering.call_transmit
			.preemphasis_filter_index = preemphasis_filter_index;
		selected.local_info1.answering.call_transmit
			.projected_data_rate_2400_bps_units = 4;
		selected.local_info1.answering.answer_to_call_symbol_rate =
			symbol_rate;
		selected.local_info1.answering.call_to_answer_symbol_rate =
			symbol_rate;
	}
	return selected;
}

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

	if (!endpoint->verify_mapping_frames)
		return true;
	for (uint8_t index = 0; index < received->primary_bit_count; ++index)
		if (received->primary_bits[index] != expected)
			return false;
	if (received->auxiliary_bit_count != 0 &&
	    received->auxiliary_bit != expected)
		return false;
	endpoint->mapping_frames_received++;
	return true;
}

static bool endpoint_init(
	struct endpoint *endpoint, enum v34_station_role role)
{
	struct v34_call_configuration config = configuration(role);
	struct endpoint initialized = {
		.transmit_bit = role == V34_ANSWERING_STATION,
		.verify_mapping_frames = true,
	};

	if (endpoint == NULL ||
	    v34_call_init(&initialized.call, &config) != V34_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_init_symmetric(
	struct endpoint *endpoint, enum v34_station_role role,
	enum v34_symbol_rate symbol_rate)
{
	struct v34_call_configuration config =
		symmetric_configuration(role, symbol_rate);
	struct endpoint initialized = {
		.transmit_bit = role == V34_ANSWERING_STATION,
		.verify_mapping_frames = true,
	};

	if (endpoint == NULL ||
	    v34_call_init(&initialized.call, &config) != V34_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_accept_observation(
	struct endpoint *endpoint,
	const struct v34_call_observation *observation,
	const struct v34_received_mapping_frame *received)
{
	if (observation->next == V34_CALL_PHASE3)
		endpoint->entered_phase3 = true;
	if (observation->next == V34_CALL_PHASE4)
		endpoint->entered_phase4 = true;
	return observation->processed != V34_CALL_PHASE4 ||
	       (observation->events.phase4 &
		V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) == 0 ||
	       accept_mapping_frame(endpoint, received);
}

static bool endpoint_sample(
	struct endpoint *endpoint, int16_t received_sample,
	v34_mapping_frame_source source,
	int16_t *transmit_sample, struct v34_call_observation *observation)
{
	struct v34_received_mapping_frame received;
	enum v34_call_result result = v34_call_sample(
		&endpoint->call, received_sample, source, endpoint,
		transmit_sample, observation, &received);

	if (result != V34_CALL_OK)
		fprintf(stderr,
			"V.34 endpoint role=%d stage=%d tone=%u failed: %s\n",
			endpoint->call.local_role, endpoint->call.stage,
			endpoint->call.retrain_detector.tone_samples,
			v34_call_result_name(result));
	return result == V34_CALL_OK &&
	       endpoint_accept_observation(endpoint, observation, &received);
}

static void channel_receive(
	const struct channel *channel, int16_t *caller, int16_t *answerer)
{
	*caller = channel->answer_to_call[channel->cursor];
	*answerer = channel->call_to_answer[channel->cursor];
}

static int16_t codec_round_trip(int16_t sample, bool pcma)
{
	return pcma ? g711_decode_pcma(g711_encode_pcma(sample)) :
		      g711_decode_pcmu(g711_encode_pcmu(sample));
}

static void channel_transmit(
	struct channel *channel, int16_t caller, int16_t answerer)
{
	channel->call_to_answer[channel->cursor] =
		codec_round_trip(caller, channel->pcma);
	channel->answer_to_call[channel->cursor] =
		codec_round_trip(answerer, channel->pcma);
	channel->cursor = (channel->cursor + 1u) % LINE_DELAY_SAMPLES;
}

static bool run_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *connected_channel, bool pcma)
{
	struct channel channel = { .pcma = pcma };
	struct endpoint calling;
	struct endpoint answering;
	int16_t next_caller_receive = 0;
	int16_t next_answerer_receive = 0;

	CHECK(endpoint_init(&calling, V34_CALLING_STATION) &&
	      endpoint_init(&answering, V34_ANSWERING_STATION),
	      "full-call endpoints did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		channel_receive(
			&channel, &next_caller_receive, &next_answerer_receive);
		CHECK(endpoint_sample(
			      &calling, next_caller_receive,
			      supply_mapping_frame, &caller_transmit,
			      &caller_observation) &&
		      endpoint_sample(
			      &answering, next_answerer_receive,
			      supply_mapping_frame, &answerer_transmit,
			      &answerer_observation),
		      "full V.34 call stopped at sample %zu", index);
		channel_transmit(&channel, caller_transmit, answerer_transmit);
		if (v34_call_is_online(&calling.call) &&
		    v34_call_is_online(&answering.call) &&
		    calling.mapping_frames_received >= 4u &&
		    answering.mapping_frames_received >= 4u) {
			*caller = calling;
			*answerer = answering;
			if (connected_channel != NULL)
				*connected_channel = channel;
			return true;
		}
	}
	CHECK(false, "full V.34 call did not reach mapped data");
}

static bool test_pcmu_call(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct v34_data_startup caller_startup;
	struct v34_data_startup answerer_startup;

	CHECK(run_online_pair(&caller, &answerer, NULL, false),
	      "PCMU call failed");
	CHECK(caller.entered_phase3 && answerer.entered_phase3 &&
	      caller.entered_phase4 && answerer.entered_phase4 &&
	      v34_call_current_stage(&caller.call) == V34_CALL_PHASE4 &&
	      v34_call_current_stage(&answerer.call) == V34_CALL_PHASE4 &&
	      !v34_call_has_failed(&caller.call) &&
	      !v34_call_has_failed(&answerer.call) &&
	      caller.call.state.phase4.receive_mode ==
		      V34_PHASE4_EXCHANGE_RECEIVE_PERSISTENT &&
	      answerer.call.state.phase4.receive_mode ==
		      V34_PHASE4_EXCHANGE_RECEIVE_PERSISTENT &&
	      caller.call.state.phase4.receive.persistent.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      answerer.call.state.phase4.receive.persistent.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      v34_call_get_data_startup(&caller.call, &caller_startup) &&
	      v34_call_get_data_startup(&answerer.call, &answerer_startup) &&
	      caller_startup.mode.transmit
			      .primary_data_rate_2400_bps_units == 4 &&
	      caller_startup.mode.receive
			      .primary_data_rate_2400_bps_units == 4 &&
	      answerer_startup.mode.transmit
			      .primary_data_rate_2400_bps_units == 4 &&
	      answerer_startup.mode.receive
			      .primary_data_rate_2400_bps_units == 4 &&
	      caller.mapping_frames_supplied >=
		      answerer.mapping_frames_received &&
	      answerer.mapping_frames_supplied >=
		      caller.mapping_frames_received,
	      "full call lost a phase or mapping frame");
	return true;
}

static bool check_symmetric_call(
	enum v34_symbol_rate symbol_rate, bool pcma)
{
	struct channel channel = { .pcma = pcma };
	struct endpoint caller;
	struct endpoint answerer;
	int16_t next_caller_receive = 0;
	int16_t next_answerer_receive = 0;

	CHECK(endpoint_init_symmetric(
		      &caller, V34_CALLING_STATION, symbol_rate) &&
	      endpoint_init_symmetric(
		      &answerer, V34_ANSWERING_STATION, symbol_rate),
	      "symmetric rate %u endpoints did not initialize", symbol_rate);
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		channel_receive(
			&channel, &next_caller_receive, &next_answerer_receive);
		CHECK(endpoint_sample(
			      &caller, next_caller_receive,
			      supply_mapping_frame, &caller_transmit,
			      &caller_observation) &&
		      endpoint_sample(
			      &answerer, next_answerer_receive,
			      supply_mapping_frame, &answerer_transmit,
			      &answerer_observation),
		      "symmetric rate %u call stopped at sample %zu",
		      symbol_rate, index);
		channel_transmit(
			&channel, caller_transmit, answerer_transmit);
		if (v34_call_is_online(&caller.call) &&
		    v34_call_is_online(&answerer.call) &&
		    caller.mapping_frames_received >= 4u &&
		    answerer.mapping_frames_received >= 4u) {
			CHECK(caller.call.state.phase4.receive_mode ==
				      V34_PHASE4_EXCHANGE_RECEIVE_PERSISTENT &&
			      answerer.call.state.phase4.receive_mode ==
				      V34_PHASE4_EXCHANGE_RECEIVE_PERSISTENT &&
			      caller.call.state.phase4.receive.persistent.stage ==
				      V34_PHASE34_RECEIVER_DATA &&
			      answerer.call.state.phase4.receive.persistent.stage ==
				      V34_PHASE34_RECEIVER_DATA,
			      "rate %u call did not retain its receive owners",
			      symbol_rate);
			return true;
		}
	}
	CHECK(false, "symmetric rate %u call did not reach mapped data",
	      symbol_rate);
}

static bool test_symmetric_calls(void)
{
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2743, false),
	      "PCMU symmetric 2743 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2743, true),
	      "PCMA symmetric 2743 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2800, false),
	      "PCMU symmetric 2800 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2800, true),
	      "PCMA symmetric 2800 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3200, false),
	      "PCMU symmetric 3200 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3200, true),
	      "PCMA symmetric 3200 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3429, false),
	      "PCMU symmetric 3429 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3429, true),
	      "PCMA symmetric 3429 call failed");
	return true;
}

static bool test_source_backpressure(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;

	CHECK(run_online_pair(&caller, &answerer, &channel, false),
	      "backpressure pair failed");
	for (size_t index = 0; index < 1000u; ++index) {
		struct v34_call before = caller.call;
		struct v34_call_observation observation;
		struct v34_call_observation observation_before;
		struct v34_received_mapping_frame received;
		struct v34_received_mapping_frame received_before;
		int16_t caller_receive;
		int16_t answerer_receive;
		int16_t caller_transmit = 1234;
		int16_t answerer_transmit;
		enum v34_call_result result;

		channel_receive(&channel, &caller_receive, &answerer_receive);
		memset(&observation, 0xa5, sizeof(observation));
		memset(&received, 0x5a, sizeof(received));
		observation_before = observation;
		received_before = received;
		result = v34_call_sample(
			&caller.call, caller_receive, reject_mapping_frame, NULL,
			&caller_transmit, &observation, &received);
		if (result == V34_CALL_SOURCE_FAILURE) {
			CHECK(memcmp(&caller.call, &before, sizeof(before)) == 0 &&
			      caller_transmit == 1234 &&
			      memcmp(&observation, &observation_before,
				     sizeof(observation)) == 0 &&
			      memcmp(&received, &received_before,
				     sizeof(received)) == 0,
			      "source failure changed call state or outputs");
			return true;
		}
		CHECK(result == V34_CALL_OK,
		      "backpressure setup failed at sample %zu", index);
		CHECK(endpoint_accept_observation(
			      &caller, &observation, &received) &&
		      endpoint_sample(
			      &answerer, answerer_receive,
			      supply_mapping_frame, &answerer_transmit,
			      &observation),
		      "backpressure timeline stopped at sample %zu", index);
		channel_transmit(
			&channel, caller_transmit, answerer_transmit);
	}
	CHECK(false, "mapping source was not consulted");
}

static bool exercise_full_retrain(
	enum v34_station_role initiator_role, bool pcma)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;
	bool caller_retrained = false;
	bool answerer_retrained = false;
	bool verifying_resumed_data = false;

	CHECK(run_online_pair(&caller, &answerer, &channel, pcma),
	      "retrain pair failed to connect");
	caller.mapping_frames_received = 0;
	answerer.mapping_frames_received = 0;
	caller.verify_mapping_frames = false;
	answerer.verify_mapping_frames = false;
	CHECK(v34_call_start_retrain(
		      initiator_role == V34_CALLING_STATION ?
			      &caller.call : &answerer.call) == V34_CALL_OK,
	      "local full retrain was rejected");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;
		int16_t caller_receive;
		int16_t answerer_receive;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		channel_receive(&channel, &caller_receive, &answerer_receive);
		CHECK(endpoint_sample(
			      &caller, caller_receive, supply_mapping_frame,
			      &caller_transmit, &caller_observation) &&
		      endpoint_sample(
			      &answerer, answerer_receive,
			      supply_mapping_frame, &answerer_transmit,
			      &answerer_observation),
		      "full retrain stopped at sample %zu", index);
		channel_transmit(&channel, caller_transmit, answerer_transmit);
		caller_retrained |=
			caller_observation.next == V34_CALL_RETRAIN_SILENCE;
		answerer_retrained |=
			answerer_observation.next == V34_CALL_RETRAIN_SILENCE;
		if (!verifying_resumed_data && caller_retrained &&
		    answerer_retrained &&
		    v34_call_is_online(&caller.call) &&
		    v34_call_is_online(&answerer.call)) {
			caller.mapping_frames_received = 0;
			answerer.mapping_frames_received = 0;
			caller.verify_mapping_frames = true;
			answerer.verify_mapping_frames = true;
			verifying_resumed_data = true;
		}
		if (verifying_resumed_data &&
		    caller.mapping_frames_received >= 4u &&
		    answerer.mapping_frames_received >= 4u)
			return true;
	}
	CHECK(false,
	      "full retrain did not reconnect both roles and resume data");
}

static bool test_full_retrain(void)
{
	CHECK(exercise_full_retrain(V34_CALLING_STATION, false),
	      "PCMU calling-station retrain failed");
	CHECK(exercise_full_retrain(V34_ANSWERING_STATION, false),
	      "PCMU answering-station retrain failed");
	CHECK(exercise_full_retrain(V34_CALLING_STATION, true),
	      "PCMA calling-station retrain failed");
	CHECK(exercise_full_retrain(V34_ANSWERING_STATION, true),
	      "PCMA answering-station retrain failed");
	return true;
}

static bool check_retrain_tone_guard(
	enum v34_station_role local_role, bool pcma)
{
	enum v34_station_role remote_role =
		local_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION : V34_CALLING_STATION;
	struct endpoint caller;
	struct endpoint answerer;
	struct endpoint *endpoint;
	struct v34_phase2_tone_transmitter remote_tone;
	struct v34_call_observation observation;
	int16_t transmitted;
	size_t samples;

	CHECK(run_online_pair(&caller, &answerer, NULL, pcma) &&
	      v34_phase2_tone_transmitter_init(
		      &remote_tone, remote_role, NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "retrain tone deadline fixture did not initialize");
	endpoint = local_role == V34_CALLING_STATION ? &caller : &answerer;
	endpoint->verify_mapping_frames = false;
	/* An unmodulated carrier without the mandatory preceding silence is
	 * ordinary line content, not a full-retrain request. */
	for (samples = 0;
	     samples < V34_PHASE2_TONE_WINDOW_SAMPLES +
			       V34_RETRAIN_TONE_SAMPLES;
	     ++samples) {
		int16_t remote_sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) ==
			      V34_PHASE2_TONE_OK &&
		      endpoint_sample(
			      endpoint,
			      codec_round_trip(remote_sample, pcma),
			      supply_mapping_frame, &transmitted,
			      &observation),
		      "retrain tone guard failed at sample %zu", samples);
	}
	CHECK(endpoint->call.stage == V34_CALL_PHASE4 &&
	      endpoint->call.retrain_detector.silence_samples == 0,
	      "carrier without retrain silence restarted Phase 2");
	return true;
}

static bool test_retrain_tone_guard(void)
{
	for (enum v34_station_role role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role)
		CHECK(check_retrain_tone_guard(role, false) &&
		      check_retrain_tone_guard(role, true),
		      "retrain tone guard failed role=%d", role);
	return true;
}

static bool check_unusable_info1_retrains(bool pcma)
{
	struct v34_call_configuration caller_config =
		configuration(V34_CALLING_STATION);
	struct v34_call_configuration answerer_config =
		configuration(V34_ANSWERING_STATION);
	struct channel channel = { .pcma = pcma };
	struct endpoint caller = {
		.transmit_bit = 0,
		.verify_mapping_frames = true,
	};
	struct endpoint answerer = {
		.transmit_bit = 1,
		.verify_mapping_frames = true,
	};
	int16_t next_caller_receive = 0;
	int16_t next_answerer_receive = 0;
	bool restarted = false;

	/* Zero is a valid projected-rate report meaning that none of the
	 * measured answerer symbol rates can be used. */
	caller_config.local_info1.calling = (struct v34_info1c){ 0 };
	CHECK(v34_call_init(&caller.call, &caller_config) == V34_CALL_OK &&
	      v34_call_init(&answerer.call, &answerer_config) == V34_CALL_OK,
	      "unusable INFO1 endpoints did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		channel_receive(
			&channel, &next_caller_receive, &next_answerer_receive);
		CHECK(endpoint_sample(
			      &caller, next_caller_receive, supply_mapping_frame,
			      &caller_transmit, &caller_observation) &&
		      endpoint_sample(
			      &answerer, next_answerer_receive,
			      supply_mapping_frame, &answerer_transmit,
			      &answerer_observation),
		      "unusable INFO1 call stopped at sample %zu", index);
		channel_transmit(&channel, caller_transmit, answerer_transmit);
		if (answerer_observation.processed == V34_CALL_PHASE2 &&
		    answerer_observation.next == V34_CALL_RETRAIN_SILENCE) {
			restarted = true;
			break;
		}
	}
	CHECK(restarted && v34_call_is_valid(&answerer.call) &&
	      !v34_call_has_failed(&answerer.call),
	      "unusable INFO1 did not start a valid full retrain");
	for (size_t index = 0; index < V34_CALL_RETRAIN_SILENCE_SAMPLES;
	     ++index) {
		struct v34_call_observation observation;
		int16_t transmit;

		CHECK(endpoint_sample(
			      &answerer, 0, supply_mapping_frame, &transmit,
			      &observation) && transmit == 0,
		      "INFO1 retrain silence failed at sample %zu", index);
	}
	CHECK(v34_call_current_stage(&answerer.call) == V34_CALL_PHASE2 &&
	      !v34_call_has_failed(&answerer.call),
	      "INFO1 retrain did not restart Phase 2 after 70 ms");
	return true;
}

static bool test_unusable_info1_retrains(void)
{
	CHECK(check_unusable_info1_retrains(false),
	      "PCMU unusable INFO1 recovery failed");
	CHECK(check_unusable_info1_retrains(true),
	      "PCMA unusable INFO1 recovery failed");
	return true;
}

static bool test_validation(void)
{
	struct v34_call_configuration config =
		configuration(V34_CALLING_STATION);
	struct v34_call call;
	struct v34_call saved;
	struct v34_call_observation observation = { 0 };
	struct v34_received_mapping_frame received = { 0 };
	int16_t transmit = 1234;

	memset(&call, 0xa5, sizeof(call));
	saved = call;
	config.phase4.receive.phase3.s_gain_adaptation = 0.3;
	CHECK(v34_call_init(&call, &config) == V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "mismatched phase configuration changed the destination");
	config = configuration(V34_CALLING_STATION);
	config.phase3.nominal_peak_amplitude = 0;
	CHECK(v34_call_init(&call, &config) == V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid Phase 3 amplitude changed the destination");
	config = configuration(V34_CALLING_STATION);
	CHECK(v34_call_init(&call, &config) == V34_CALL_OK,
	      "validation call did not initialize");
	saved = call;
	CHECK(v34_call_sample(
		      &call, 0, NULL, NULL, NULL, &observation,
		      &received) == V34_CALL_INVALID_ARGUMENT &&
	      v34_call_sample(
		      &call, 0, NULL, NULL, &transmit, NULL,
		      &received) == V34_CALL_INVALID_ARGUMENT &&
	      v34_call_sample(
		      &call, 0, NULL, NULL, &transmit, &observation,
		      NULL) == V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0 && transmit == 1234 &&
	      !v34_call_is_valid(NULL) && !v34_call_has_failed(NULL) &&
	      !v34_call_is_online(NULL) &&
	      !v34_call_get_data_startup(NULL, NULL) &&
	      !v34_call_get_data_startup(
		      &call, (struct v34_data_startup *)&call) &&
	      v34_call_start_retrain(&call) == V34_CALL_INVALID_STATE &&
	      v34_call_current_stage(NULL) == (enum v34_call_stage)-1 &&
	      strcmp(v34_call_result_name(V34_CALL_SOURCE_FAILURE),
		     "mapping-frame source failure") == 0 &&
	      strcmp(v34_call_result_name((enum v34_call_result)99),
		     "unknown") == 0,
	      "rejected call-controller operations were not transactional");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated PCMU call", test_pcmu_call },
		{ "symmetric selected-rate calls", test_symmetric_calls },
		{ "local and remote full retrain", test_full_retrain },
		{ "retrain tone guard", test_retrain_tone_guard },
		{ "unusable INFO1 starts full retrain",
		  test_unusable_info1_retrains },
		{ "mapping source backpressure", test_source_backpressure },
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
