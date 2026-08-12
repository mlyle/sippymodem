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
};

struct endpoint {
	struct v34_call call;
	unsigned int mapping_frames_supplied;
	unsigned int mapping_frames_received;
	uint8_t transmit_bit;
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

	return result == V34_CALL_OK &&
	       endpoint_accept_observation(endpoint, observation, &received);
}

static void channel_receive(
	const struct channel *channel, int16_t *caller, int16_t *answerer)
{
	*caller = channel->answer_to_call[channel->cursor];
	*answerer = channel->call_to_answer[channel->cursor];
}

static void channel_transmit(
	struct channel *channel, int16_t caller, int16_t answerer)
{
	channel->call_to_answer[channel->cursor] =
		g711_decode_pcmu(g711_encode_pcmu(caller));
	channel->answer_to_call[channel->cursor] =
		g711_decode_pcmu(g711_encode_pcmu(answerer));
	channel->cursor = (channel->cursor + 1u) % LINE_DELAY_SAMPLES;
}

static bool run_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *connected_channel)
{
	struct channel channel = { 0 };
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

	CHECK(run_online_pair(&caller, &answerer, NULL),
	      "PCMU call failed");
	CHECK(caller.entered_phase3 && answerer.entered_phase3 &&
	      caller.entered_phase4 && answerer.entered_phase4 &&
	      v34_call_current_stage(&caller.call) == V34_CALL_PHASE4 &&
	      v34_call_current_stage(&answerer.call) == V34_CALL_PHASE4 &&
	      !v34_call_has_failed(&caller.call) &&
	      !v34_call_has_failed(&answerer.call) &&
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

static bool test_source_backpressure(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;

	CHECK(run_online_pair(&caller, &answerer, &channel),
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
