// SPDX-License-Identifier: BSD-3-Clause

#include <v34_call.h>

#include <g711.h>

#include "test.h"

#include <string.h>

enum {
	SAMPLE_LIMIT = 140000,
	LINE_DELAY_SAMPLES = 13,
	LINE_DELAY_CAPACITY = 1440,
	ECHO_DELAY_CAPACITY = 256,
	TEST_ECHO_DELAY_SAMPLES = 32,
	NOMINAL_AMPLITUDE = 7000,
};

struct self_echo {
	int16_t samples[ECHO_DELAY_CAPACITY];
	size_t cursor;
	size_t delay_samples;
	int16_t gain_q15;
	bool disable_filter;
};

struct channel {
	int16_t call_to_answer[LINE_DELAY_CAPACITY];
	int16_t answer_to_call[LINE_DELAY_CAPACITY];
	struct self_echo echo[V34_ANSWERING_STATION + 1];
	size_t cursor;
	/* Zero retains the ordinary short-delay fixture. */
	size_t delay_samples;
	bool pcma;
};

struct endpoint {
	struct v34_call call;
	unsigned int mapping_frames_supplied;
	unsigned int mapping_frames_received;
	uint32_t transmit_primary_bits;
	uint32_t receive_primary_bits;
	uint32_t mapping_pattern_seed;
	uint32_t peer_mapping_pattern_seed;
	uint8_t primary_idle_bits_remaining;
	uint8_t peer_primary_delay_bits;
	uint8_t transmit_bit;
	bool generated_mapping_frames;
	bool verify_mapping_frames;
	bool entered_phase3;
	bool entered_phase4;
	bool suppress_failure_diagnostic;
};

static bool cleardown_rejects_retained_observation(
	const struct v34_cleardown *ready)
{
	struct v34_cleardown pending = *ready;
	struct v34_cleardown rejected;
	struct v34_cleardown saved;
	bool retained = false;

	for (unsigned int index = 0u; index < 16u && !retained; ++index) {
		struct v34_adaptive_equalizer_observation observation;
		enum v34_line_receiver_result result = v34_line_receiver_sample(
			&pending.receive_line, 0, &observation);

		CHECK(result == V34_LINE_RECEIVER_OK ||
		      result == V34_LINE_RECEIVER_OBSERVATION_READY,
		      "cleardown boundary fixture could not observe the line");
		if (result == V34_LINE_RECEIVER_OBSERVATION_READY) {
			CHECK(v34_adaptive_equalizer_retain_observation(
				      &pending.receive_line.equalizer, &observation) ==
				      V34_ADAPTIVE_EQUALIZER_OK,
			      "cleardown boundary fixture could not retain a decision");
			retained = true;
		}
	}
	CHECK(retained && v34_line_receiver_is_valid(&pending.receive_line) &&
	      !pending.receive_line.equalizer.decision_pending &&
	      pending.receive_line.equalizer.retained.pending &&
	      !v34_cleardown_is_valid(&pending),
	      "cleardown accepted an unresolved paired observation");
	memset(&rejected, 0xa5, sizeof(rejected));
	saved = rejected;
	CHECK(v34_cleardown_init(
		      &rejected, &pending.transmit_line, &pending.receive_line,
		      &pending.parameters) == V34_CLEARDOWN_INVALID_ARGUMENT &&
	      memcmp(&rejected, &saved, sizeof(rejected)) == 0,
	      "rejected cleardown handoff changed its destination");
	return true;
}

static bool clear_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel);

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
	return (struct v34_call_configuration){
		.local_role = role,
		.phase2_nominal_amplitude = NOMINAL_AMPLITUDE,
		.local_info0 = info0(role),
		.local_info1 = info1(role),
		.phase3 = {
			.requested_remote_constellation =
				role == V34_CALLING_STATION ?
					V34_CONSTELLATION_16_POINT :
					V34_CONSTELLATION_4_POINT,
			.sample_rate = V34_PHASE2_SAMPLE_RATE,
			.nominal_peak_amplitude = NOMINAL_AMPLITUDE,
		},
		.phase4 = {
			.transmit_mp = mp_record(role),
		},
	};
}

static struct v34_call_configuration symmetric_configuration(
	enum v34_station_role role, enum v34_symbol_rate symbol_rate)
{
	enum { PRIMARY_RATE_UNITS = 10 };
	struct v34_call_configuration selected = configuration(role);
	uint8_t preemphasis_filter_index =
		symbol_rate == V34_SYMBOL_RATE_3000 ? 2u :
		symbol_rate == V34_SYMBOL_RATE_3429 ? 7u : 0u;
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
		.low_carrier_3000_supported =
			symbol_rate == V34_SYMBOL_RATE_3000,
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
			.projected_data_rate_2400_bps_units =
				PRIMARY_RATE_UNITS;
	} else {
		selected.local_info1.answering.common
			.frequency_offset_1_50_hz_units =
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		selected.local_info1.answering.call_transmit
			.high_carrier = high_carrier;
		selected.local_info1.answering.call_transmit
			.preemphasis_filter_index = preemphasis_filter_index;
		selected.local_info1.answering.call_transmit
			.projected_data_rate_2400_bps_units =
				PRIMARY_RATE_UNITS;
		selected.local_info1.answering.answer_to_call_symbol_rate =
			symbol_rate;
		selected.local_info1.answering.call_to_answer_symbol_rate =
			symbol_rate;
	}
	selected.phase4.transmit_mp.parameters.type0
		.maximum_call_to_answer_rate_2400_bps_units = PRIMARY_RATE_UNITS;
	selected.phase4.transmit_mp.parameters.type0
		.maximum_answer_to_call_rate_2400_bps_units = PRIMARY_RATE_UNITS;
	selected.phase4.transmit_mp.parameters.type0.data_rate_capability_mask =
		(uint16_t)((1u << PRIMARY_RATE_UNITS) - 1u);
	return selected;
}

static bool supply_mapping_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct endpoint *endpoint = context;

	if (endpoint == NULL || request == NULL || primary_bits == NULL)
		return false;
	for (uint8_t index = 0; index < request->primary_bits; ++index) {
		uint32_t bit_index = endpoint->transmit_primary_bits++;
		uint32_t mixed = endpoint->mapping_pattern_seed +
			bit_index * UINT32_C(0x9e3779b9);

		mixed ^= mixed >> 16;
		mixed *= UINT32_C(0x7feb352d);
		mixed ^= mixed >> 15;
		primary_bits[index] = endpoint->generated_mapping_frames ?
			(uint8_t)(mixed >> 31) : endpoint->transmit_bit;
	}
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
	uint8_t expected_auxiliary = (uint8_t)(endpoint->transmit_bit ^ 1u);

	if (!endpoint->verify_mapping_frames)
		return true;
	for (uint8_t index = 0; index < received->primary_bit_count; ++index) {
		uint32_t bit_index = endpoint->receive_primary_bits;
		uint32_t mixed = endpoint->peer_mapping_pattern_seed +
			bit_index * UINT32_C(0x9e3779b9);
		uint8_t bit;

		mixed ^= mixed >> 16;
		mixed *= UINT32_C(0x7feb352d);
		mixed ^= mixed >> 15;
		bit = endpoint->primary_idle_bits_remaining != 0 ? 1u :
		      endpoint->generated_mapping_frames ?
			      (uint8_t)(mixed >> 31) :
			      (uint8_t)(endpoint->transmit_bit ^ 1u);

		if (received->primary_bits[index] != bit)
			return false;
		if (endpoint->primary_idle_bits_remaining != 0) {
			endpoint->primary_idle_bits_remaining--;
		} else {
			endpoint->receive_primary_bits++;
		}
	}
	if (received->auxiliary_bit_count != 0 &&
	    received->auxiliary_bit != expected_auxiliary)
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
		.mapping_pattern_seed =
			role == V34_CALLING_STATION ? UINT32_C(0x34c011ab) :
						      UINT32_C(0x34a05e17),
		.peer_mapping_pattern_seed =
			role == V34_CALLING_STATION ? UINT32_C(0x34a05e17) :
						      UINT32_C(0x34c011ab),
		.transmit_bit = role == V34_ANSWERING_STATION,
		.generated_mapping_frames = true,
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
	if (observation->processed == V34_CALL_PHASE4 &&
	    (observation->events.phase4 &
	     V34_PHASE4_EXCHANGE_EVENT_PEER_B1_RECEIVED) != 0) {
		if (endpoint->peer_primary_delay_bits == 0)
			return false;
		endpoint->primary_idle_bits_remaining =
			endpoint->peer_primary_delay_bits;
	}
	return observation->processed != V34_CALL_PHASE4 ||
	       (observation->events.phase4 &
		V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) == 0 ||
	       accept_mapping_frame(endpoint, received);
}

static void note_peer_primary_delays(
	struct endpoint *caller, struct endpoint *answerer)
{
	if (caller->call.stage == V34_CALL_PHASE4 &&
	    caller->call.state.phase4.transmit_data_active)
		answerer->peer_primary_delay_bits =
			caller->call.state.phase4.transmit.data.primary_delay_bits;
	if (answerer->call.stage == V34_CALL_PHASE4 &&
	    answerer->call.state.phase4.transmit_data_active)
		caller->peer_primary_delay_bits =
			answerer->call.state.phase4.transmit.data.primary_delay_bits;
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

	if (result != V34_CALL_OK &&
	    !endpoint->suppress_failure_diagnostic)
		fprintf(stderr,
			"V.34 endpoint role=%d stage=%d tone=%u failed: %d\n",
			endpoint->call.local_role, endpoint->call.stage,
			endpoint->call.retrain.detector.tone_samples,
			result);
	return result == V34_CALL_OK &&
	       endpoint_accept_observation(endpoint, observation, &received);
}

static int16_t mix_echo(int16_t remote, int16_t local, int16_t gain_q15)
{
	int32_t mixed = (int32_t)remote +
		(int32_t)((int64_t)local * gain_q15 / 32768);

	if (mixed > INT16_MAX)
		return INT16_MAX;
	if (mixed < INT16_MIN)
		return INT16_MIN;
	return (int16_t)mixed;
}

static int16_t self_echo_receive(
	const struct self_echo *echo, int16_t remote)
{
	return mix_echo(remote, echo->samples[echo->cursor], echo->gain_q15);
}

static void self_echo_transmit(struct self_echo *echo, int16_t local)
{
	if (echo->delay_samples == 0)
		return;
	echo->samples[echo->cursor] = local;
	echo->cursor = (echo->cursor + 1u) % echo->delay_samples;
}

static void channel_receive(
	const struct channel *channel, int16_t *caller, int16_t *answerer)
{
	*caller = self_echo_receive(
		&channel->echo[V34_CALLING_STATION],
		channel->answer_to_call[channel->cursor]);
	*answerer = self_echo_receive(
		&channel->echo[V34_ANSWERING_STATION],
		channel->call_to_answer[channel->cursor]);
}

static int16_t codec_round_trip(int16_t sample, bool pcma)
{
	return pcma ? g711_decode_pcma(g711_encode_pcma(sample)) :
		      g711_decode_pcmu(g711_encode_pcmu(sample));
}

static void channel_transmit(
	struct channel *channel, int16_t caller, int16_t answerer)
{
	self_echo_transmit(&channel->echo[V34_CALLING_STATION], caller);
	self_echo_transmit(&channel->echo[V34_ANSWERING_STATION], answerer);
	channel->call_to_answer[channel->cursor] =
		codec_round_trip(caller, channel->pcma);
	channel->answer_to_call[channel->cursor] =
		codec_round_trip(answerer, channel->pcma);
	channel->cursor = (channel->cursor + 1u) %
		(channel->delay_samples != 0u ? channel->delay_samples :
					       LINE_DELAY_SAMPLES);
}

static bool channel_sample_pair(
	struct channel *channel, struct endpoint *caller,
	struct endpoint *answerer,
	struct v34_call_observation *caller_observation,
	struct v34_call_observation *answerer_observation)
{
	int16_t caller_receive;
	int16_t answerer_receive;
	int16_t caller_transmit;
	int16_t answerer_transmit;

	if (channel->echo[V34_CALLING_STATION].disable_filter)
		caller->call.echo.filtering = false;
	if (channel->echo[V34_ANSWERING_STATION].disable_filter)
		answerer->call.echo.filtering = false;
	channel_receive(channel, &caller_receive, &answerer_receive);
	note_peer_primary_delays(caller, answerer);
	if (!endpoint_sample(
		    caller, caller_receive, supply_mapping_frame,
		    &caller_transmit, caller_observation) ||
	    !endpoint_sample(
		    answerer, answerer_receive, supply_mapping_frame,
		    &answerer_transmit, answerer_observation))
		return false;
	channel_transmit(channel, caller_transmit, answerer_transmit);
	return true;
}

static bool run_initialized_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel)
{
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;

		if (!channel_sample_pair(
			    channel, caller, answerer,
			    &caller_observation, &answerer_observation))
			return false;
		if (v34_call_is_online(&caller->call) &&
		    v34_call_is_online(&answerer->call) &&
		    caller->mapping_frames_received >= 4u &&
		    answerer->mapping_frames_received >= 4u)
			return true;
	}
	return false;
}

static bool run_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *connected_channel, bool pcma)
{
	struct channel channel = { .pcma = pcma };
	struct endpoint calling;
	struct endpoint answering;

	CHECK(endpoint_init(&calling, V34_CALLING_STATION) &&
	      endpoint_init(&answering, V34_ANSWERING_STATION),
	      "full-call endpoints did not initialize");
	CHECK(run_initialized_pair(&calling, &answering, &channel),
	      "full V.34 call did not reach mapped data");
	*caller = calling;
	*answerer = answering;
	if (connected_channel != NULL)
		*connected_channel = channel;
	return true;
}

static bool test_delayed_calls(void)
{
	/* At this delay the asymmetric 3000/3200-baud pair reaches final S near
	 * a hard-decision quadrant boundary. Both endpoints must acquire it and
	 * exchange verified mapped data, not just report training completion. */
	for (unsigned int pcma = 0; pcma < 2u; ++pcma) {
		struct endpoint caller;
		struct endpoint answerer;
		struct channel channel = {
			.pcma = pcma != 0u,
			.delay_samples = LINE_DELAY_CAPACITY,
		};

		CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
		      endpoint_init(&answerer, V34_ANSWERING_STATION),
		      "delayed call endpoints did not initialize");
		caller.generated_mapping_frames = answerer.generated_mapping_frames = true;
		caller.mapping_pattern_seed = answerer.peer_mapping_pattern_seed =
			UINT32_C(0x34c011ab);
		answerer.mapping_pattern_seed = caller.peer_mapping_pattern_seed =
			UINT32_C(0x34a05e17);
		CHECK(run_initialized_pair(&caller, &answerer, &channel),
		      "delayed law %u call did not reach two-way mapped data", pcma);
		CHECK(caller.receive_primary_bits > 0u &&
		      answerer.receive_primary_bits > 0u,
		      "delayed law %u call did not verify payload", pcma);
	}
	return true;
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
	      caller.call.stage == V34_CALL_PHASE4 &&
	      answerer.call.stage == V34_CALL_PHASE4 &&
	      !v34_call_has_failed(&caller.call) &&
	      !v34_call_has_failed(&answerer.call) &&
	      caller.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      answerer.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      caller.call.state.phase4.receive.semantic.data
		      .data_receiver.channel_receiver.frame_decoder
		      .channel_decoder.survivor.selection ==
			      V34_TRELLIS_32_STATE &&
	      answerer.call.state.phase4.receive.semantic.data
		      .data_receiver.channel_receiver.frame_decoder
		      .channel_decoder.survivor.selection ==
			      V34_TRELLIS_16_STATE &&
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
	{
		struct v34_call failed = caller.call;

		failed.state.phase4.failed = true;
		CHECK(!v34_call_is_valid(&failed) && !v34_call_has_failed(&failed),
		      "call accepted an unpropagated child failure");
		failed.failed = true;
		CHECK(v34_call_is_valid(&failed) && v34_call_has_failed(&failed),
		      "call rejected a propagated child failure");
		failed.state.phase4.receive.semantic.data.data_receiver
			.channel_receiver.frame_decoder.channel_decoder
			.survivor.path_index = V34_SURVIVOR_TRACEBACK_INTERVALS;
		CHECK(!v34_call_is_valid(&failed) && !v34_call_has_failed(&failed),
		      "failure flag hid invalid nested receive history");
	}
	return true;
}

static bool run_echo_call(
	enum v34_station_role echoed_role, bool pcma, bool disable_filter,
	struct endpoint *result)
{
	struct channel channel = {
		.pcma = pcma,
	};
	struct endpoint caller;
	struct endpoint answerer;
	bool connected;

	channel.echo[echoed_role] = (struct self_echo){
		.delay_samples = TEST_ECHO_DELAY_SAMPLES,
		.gain_q15 = 14000,
		.disable_filter = disable_filter,
	};
	CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
	      endpoint_init(&answerer, V34_ANSWERING_STATION),
	      "echoed endpoints did not initialize");
	caller.suppress_failure_diagnostic = disable_filter;
	answerer.suppress_failure_diagnostic = disable_filter;
	connected = run_initialized_pair(&caller, &answerer, &channel);
	*result = echoed_role == V34_CALLING_STATION ? caller : answerer;
	return connected;
}

static bool check_echo_call(enum v34_station_role role, bool pcma)
{
	struct endpoint filtered;
	uint64_t maximum_residual_percent =
		role == V34_CALLING_STATION ? 5u : 1u;
	const char *station = role == V34_CALLING_STATION ? "calling" :
							       "answering";

	CHECK(!run_echo_call(role, pcma, true, &filtered),
	      "uncancelled %s-station echo reached mapped data", station);
	CHECK(run_echo_call(role, pcma, false, &filtered),
	      "cancelled %s-station echo did not reach mapped data", station);
	CHECK(filtered.call.echo.filtering &&
	      filtered.call.echo.validation_input_energy != 0 &&
	      filtered.call.echo.validation_residual_energy * 100u <
		      filtered.call.echo.validation_input_energy *
			      maximum_residual_percent,
	      "%s-station echo model was not retained", station);
	return true;
}

static bool test_echo_calls(void)
{
	CHECK(check_echo_call(V34_ANSWERING_STATION, false),
	      "PCMU answerer echo call failed");
	CHECK(check_echo_call(V34_ANSWERING_STATION, true),
	      "PCMA answerer echo call failed");
	CHECK(check_echo_call(V34_CALLING_STATION, false),
	      "PCMU caller echo call failed");
	CHECK(check_echo_call(V34_CALLING_STATION, true),
	      "PCMA caller echo call failed");
	return true;
}

static bool check_symmetric_call(
	enum v34_symbol_rate symbol_rate, bool pcma)
{
	struct channel channel = { .pcma = pcma };
	struct endpoint caller;
	struct endpoint answerer;

	CHECK(endpoint_init_symmetric(
		      &caller, V34_CALLING_STATION, symbol_rate) &&
	      endpoint_init_symmetric(
		      &answerer, V34_ANSWERING_STATION, symbol_rate),
	      "symmetric rate %u endpoints did not initialize", symbol_rate);
	CHECK(run_initialized_pair(&caller, &answerer, &channel),
	      "symmetric rate %u call did not reach mapped data", symbol_rate);
	CHECK(caller.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      answerer.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      caller.call.local_info1.calling.answer_transmit[symbol_rate]
		      .preemphasis_filter_index == 6 &&
	      answerer.call.local_info1.answering.call_transmit
		      .preemphasis_filter_index == 6,
	      "rate %u call lost data or measured pre-emphasis", symbol_rate);
	CHECK(clear_online_pair(&caller, &answerer, &channel),
	      "rate %u call did not clear the trained line", symbol_rate);
	return true;
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
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3000, false),
	      "PCMU symmetric 3000 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3000, true),
	      "PCMA symmetric 3000 call failed");
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
		note_peer_primary_delays(&caller, &answerer);
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

static bool retrain_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel, enum v34_station_role initiator_role)
{
	bool caller_retrained = false;
	bool answerer_retrained = false;
	bool verifying_resumed_data = false;

	caller->mapping_frames_received = 0;
	answerer->mapping_frames_received = 0;
	caller->verify_mapping_frames = false;
	answerer->verify_mapping_frames = false;
	CHECK(v34_call_start_retrain(
		      initiator_role == V34_CALLING_STATION ?
			      &caller->call : &answerer->call) == V34_CALL_OK,
	      "local full retrain was rejected");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;

		CHECK(channel_sample_pair(
			      channel, caller, answerer,
			      &caller_observation, &answerer_observation),
		      "full retrain stopped at sample %zu", index);
		caller_retrained |=
			caller_observation.next == V34_CALL_RETRAIN_SILENCE;
		answerer_retrained |=
			answerer_observation.next == V34_CALL_RETRAIN_SILENCE;
		if (!verifying_resumed_data && caller_retrained &&
		    answerer_retrained &&
		    v34_call_is_online(&caller->call) &&
		    v34_call_is_online(&answerer->call)) {
			caller->mapping_frames_received = 0;
			answerer->mapping_frames_received = 0;
			caller->verify_mapping_frames = true;
			answerer->verify_mapping_frames = true;
			verifying_resumed_data = true;
		}
		if (verifying_resumed_data &&
		    caller->mapping_frames_received >= 4u &&
		    answerer->mapping_frames_received >= 4u)
			return true;
	}
	CHECK(false,
	      "full retrain did not reconnect both roles and resume data");
}

static bool exercise_full_retrain(
	enum v34_station_role initiator_role, bool pcma)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;

	CHECK(run_online_pair(&caller, &answerer, &channel, pcma),
	      "retrain pair failed to connect");
	return retrain_online_pair(
		&caller, &answerer, &channel, initiator_role);
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

static bool test_echo_retrain(void)
{
	for (unsigned int law = 0; law < 2; ++law) {
		struct endpoint caller;
		struct endpoint answerer;
		struct channel channel = { .pcma = law != 0 };

		channel.echo[V34_ANSWERING_STATION] = (struct self_echo){
			.delay_samples = TEST_ECHO_DELAY_SAMPLES,
			.gain_q15 = 14000,
		};
		CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
		      endpoint_init(&answerer, V34_ANSWERING_STATION) &&
		      run_initialized_pair(&caller, &answerer, &channel) &&
		      retrain_online_pair(&caller, &answerer, &channel,
				  V34_CALLING_STATION) &&
		      retrain_online_pair(&caller, &answerer, &channel,
				  V34_ANSWERING_STATION),
		      "law %u retrain lost mapped data through answerer echo", law);
	}
	return true;
}

static bool check_retrain_selects_new_rate(bool pcma)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;

	CHECK(run_online_pair(&caller, &answerer, &channel, pcma),
	      "rate-selection pair failed to connect");
	CHECK(caller.call.local_info1.calling
		      .answer_transmit[V34_SYMBOL_RATE_2400]
		      .projected_data_rate_2400_bps_units == 0,
	      "initial caller unexpectedly offered the fallback rate");
	answerer.call.local_info1.answering.answer_to_call_symbol_rate =
		V34_SYMBOL_RATE_2400;
	answerer.call.local_info1.answering.call_to_answer_symbol_rate =
		V34_SYMBOL_RATE_2400;
	CHECK(v34_call_is_valid(&caller.call) &&
	      v34_call_is_valid(&answerer.call),
	      "new-rate configuration invalidated the online pair");
	CHECK(retrain_online_pair(
		      &caller, &answerer, &channel,
		      pcma ? V34_ANSWERING_STATION : V34_CALLING_STATION) &&
	      caller.call.state.phase4.data_startup.transmit
		      .mapping.transmitting_profile.symbol_rate == V34_SYMBOL_RATE_2400 &&
	      caller.call.state.phase4.data_startup.receive
		      .transmitting_profile.symbol_rate == V34_SYMBOL_RATE_2400 &&
	      caller.call.local_info1.calling
		      .answer_transmit[V34_SYMBOL_RATE_2400]
		      .projected_data_rate_2400_bps_units == 4 &&
	      answerer.call.state.phase4.data_startup.transmit
		      .mapping.transmitting_profile.symbol_rate == V34_SYMBOL_RATE_2400 &&
	      answerer.call.state.phase4.data_startup.receive
		      .transmitting_profile.symbol_rate == V34_SYMBOL_RATE_2400,
	      "retrain did not adopt the new INFO1a symbol rates");
	return true;
}

static bool test_retrain_selects_new_rate(void)
{
	CHECK(check_retrain_selects_new_rate(false),
	      "PCMU new-rate retrain failed");
	CHECK(check_retrain_selects_new_rate(true),
	      "PCMA new-rate retrain failed");
	return true;
}

static bool test_probe_recovery(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;
	bool probe_started = false;

	CHECK(run_online_pair(&caller, &answerer, &channel, false) &&
	      v34_call_start_retrain(&answerer.call) == V34_CALL_OK,
	      "failed-probe fixture did not start retraining");
	caller.verify_mapping_frames = false;
	answerer.verify_mapping_frames = false;
	for (size_t index = 0; index < SAMPLE_LIMIT && !probe_started;
	     ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;

		CHECK(channel_sample_pair(
			      &channel, &caller, &answerer,
			      &caller_observation, &answerer_observation),
		      "failed-probe setup stopped at sample %zu", index);
		probe_started =
			answerer_observation.processed == V34_CALL_PHASE2 &&
			answerer_observation.events.phase2.kind ==
				V34_PHASE2_EVENT_INITIAL_PROBE_STARTED;
	}
	CHECK(probe_started, "answerer did not begin its initial probe");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation observation = { 0 };
		struct v34_received_mapping_frame received = { 0 };
		enum v34_call_result result;
		int16_t transmit = 0;

		result = v34_call_sample(
			&answerer.call, 0, NULL, NULL, &transmit,
			&observation, &received);
		CHECK(result == V34_CALL_OK &&
		      observation.processed == V34_CALL_PHASE2 &&
		      v34_call_is_valid(&answerer.call) &&
		      !v34_call_has_failed(&answerer.call),
		      "missing Tone B aborted the call instead of restarting ranging");
		if (answerer.call.state.phase2.stage == V34_PHASE2_RANGING) {
			CHECK(answerer.call.state.phase2.state.ranging.control.phase ==
				      PHASE2_SIGNAL_A_WAIT_INITIAL_TONE,
			      "probe recovery did not return to first ranging");
			return true;
		}
	}
	CHECK(false, "missing Tone B did not reach bounded ranging recovery");
}

static bool check_retrain_tone_guard(
	enum v34_station_role local_role, bool pcma)
{
	enum v34_station_role remote_role =
		v34_station_peer_role(local_role);
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
	      endpoint->call.retrain.detector.silence_samples == 0,
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

static bool complete_remote_retrain_silence(
	struct endpoint *endpoint,
	struct v34_phase2_tone_transmitter *remote_tone, bool pcma)
{
	struct v34_phase2_tone_detector retained =
		endpoint->call.retrain.detector.tone;

	CHECK(endpoint->call.stage == V34_CALL_RETRAIN_SILENCE &&
	      endpoint->call.retrain.peer_tone_retained,
	      "remote retrain did not retain its peer tone");
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES;
	     ++index) {
		struct v34_phase2_tone_event tone_event;
		struct v34_call_observation observation;
		int16_t remote_sample;
		int16_t received_sample;
		int16_t transmitted;
		enum v34_call_stage next =
			index + 1u ==
					V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ?
				V34_CALL_PHASE2 : V34_CALL_RETRAIN_SILENCE;

		CHECK(v34_phase2_tone_transmitter_sample(
			      remote_tone, &remote_sample) == V34_PHASE2_TONE_OK,
		      "remote retrain continuation failed at sample %zu", index);
		received_sample = codec_round_trip(remote_sample, pcma);
		CHECK(v34_phase2_tone_detector_sample(
			      &retained, received_sample, &tone_event) ==
			      V34_PHASE2_TONE_OK &&
		      endpoint_sample(
			      endpoint, received_sample, supply_mapping_frame,
			      &transmitted, &observation) &&
		      transmitted == 0 &&
		      observation.processed == V34_CALL_RETRAIN_SILENCE &&
		      observation.next == next,
		      "remote retrain silence failed at sample %zu", index);
	}
	CHECK(!endpoint->call.retrain.peer_tone_retained &&
	      memcmp(&endpoint->call.state.phase2.state.ranging.info0_exchange
			      .transport.tone_detector,
		     &retained, sizeof(retained)) == 0,
	      "remote retrain lost the peer tone at the Phase 2 handoff");
	return true;
}

static bool info1_timeout_restarts_answerer(
	const struct endpoint *initial, bool pcma)
{
	struct endpoint endpoint = *initial;
	const struct phase2_info1_exchange *exchange =
		&initial->call.state.phase2.state.info1.control;
	struct v34_info0 peer_info0;
	struct v34_retrain_detector detector;
	bool detected;
	bool qualified = false;
	uint32_t remaining = PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES +
		2u * exchange->round_trip_delay_samples -
		exchange->peer_deadline_elapsed_samples;

	CHECK(initial->call.local_role == V34_ANSWERING_STATION &&
	      exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER &&
	      v34_phase2_peer_info0(&initial->call.state.phase2, &peer_info0) ==
		      V34_PHASE2_OK && remaining > 0,
	      "answerer INFO1 timeout fixture was not ready");
	for (uint32_t sample = 0; sample < remaining; ++sample) {
		struct v34_call_observation observation;
		int16_t transmitted;
		bool expired = sample + 1u == remaining;

		CHECK(endpoint_sample(
			      &endpoint, codec_round_trip(0, pcma),
			      supply_mapping_frame, &transmitted, &observation) &&
		      observation.processed == V34_CALL_PHASE2 &&
		      observation.next == (expired ? V34_CALL_RETRAIN_SILENCE :
					     V34_CALL_PHASE2) &&
		      (observation.events.phase2.kind ==
			       V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED) == expired,
		      "answerer INFO1 timeout missed its deadline at %u/%u",
		      sample + 1u, remaining);
	}
	CHECK(endpoint.call.retrain_active &&
	      !endpoint.call.retrain.peer_tone_retained &&
	      endpoint.call.peer_info0_stored &&
	      memcmp(&endpoint.call.peer_info0, &peer_info0, sizeof(peer_info0)) == 0 &&
	      endpoint.call.round_trip_delay_samples ==
		      initial->call.round_trip_delay_samples &&
	      v34_retrain_detector_init(&detector, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "answerer INFO1 timeout lost its retrain agreement");
	for (size_t sample = 0;
	     sample < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++sample) {
		struct v34_call_observation observation;
		int16_t transmitted;

		CHECK(endpoint_sample(
			      &endpoint, codec_round_trip(0, pcma),
			      supply_mapping_frame, &transmitted, &observation) &&
		      observation.processed == V34_CALL_RETRAIN_SILENCE &&
		      transmitted == 0 &&
		      v34_retrain_detector_sample(
			      &detector, codec_round_trip(transmitted, pcma),
			      &detected) == V34_PHASE2_TONE_OK && !detected,
		      "answerer INFO1 timeout silence failed at %zu", sample);
	}
	CHECK(endpoint.call.stage == V34_CALL_PHASE2 &&
	      endpoint.call.state.phase2.stage == V34_PHASE2_RANGING,
	      "answerer INFO1 timeout did not restart ranging");
	for (size_t sample = 0;
	     sample < V34_RETRAIN_TONE_SAMPLES + 2u * V34_PHASE2_TONE_WINDOW_SAMPLES;
	     ++sample) {
		struct v34_call_observation observation;
		int16_t transmitted;

		CHECK(endpoint_sample(
			      &endpoint, codec_round_trip(0, pcma),
			      supply_mapping_frame, &transmitted, &observation) &&
		      v34_retrain_detector_sample(
			      &detector, codec_round_trip(transmitted, pcma),
			      &detected) == V34_PHASE2_TONE_OK,
		      "answerer INFO1 timeout Tone A failed at %zu", sample);
		qualified |= detected;
	}
	CHECK(qualified, "answerer INFO1 timeout sent no qualifying retrain");
	return true;
}

static bool info1_retrain_before_deadline(
	const struct endpoint *initial, bool pcma, uint32_t remaining_samples)
{
	struct endpoint endpoint = *initial;
	struct v34_phase2_tone_transmitter remote_tone;
	bool reached_boundary = false;
	bool recovered = false;
	bool responded = false;

	CHECK(v34_phase2_tone_transmitter_init(
		      &remote_tone, v34_station_peer_role(endpoint.call.local_role),
		      NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "INFO1 peer tone did not initialize");
	/* Let the real INFO1 deadline run while the peer is quiet. No detector
	 * state or deadline is injected at the wait/recovery boundary. */
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		const struct phase2_info1_exchange *exchange =
			&endpoint.call.state.phase2.state.info1.control;
		uint32_t deadline = exchange->signal_role == PHASE2_SIGNAL_B ?
			(uint32_t)PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES +
				exchange->round_trip_delay_samples :
			(uint32_t)PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES +
				2u * exchange->round_trip_delay_samples;
		struct v34_call_observation observation;
		int16_t transmitted;

		if (exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER &&
		    deadline - exchange->peer_deadline_elapsed_samples ==
			    remaining_samples) {
			reached_boundary = true;
			break;
		}
		CHECK(endpoint_sample(
			      &endpoint, codec_round_trip(0, pcma),
			      supply_mapping_frame, &transmitted, &observation) &&
		      endpoint.call.stage == V34_CALL_PHASE2 &&
		      endpoint.call.state.phase2.stage == V34_PHASE2_INFO1,
		      "INFO1 silence left its exchange at sample %zu", index);
	}
	CHECK(reached_boundary, "INFO1 deadline boundary was not reached");
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES +
			     2u * V34_PHASE2_TONE_WINDOW_SAMPLES; ++index) {
		struct v34_call_observation observation;
		int16_t remote_sample;
		int16_t transmitted;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) == V34_PHASE2_TONE_OK &&
		      endpoint_sample(
			      &endpoint, codec_round_trip(remote_sample, pcma),
			      supply_mapping_frame, &transmitted, &observation),
		      "INFO1 peer tone failed at sample %zu", index);
		recovered |= observation.events.phase2.kind ==
			     V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED;
		if (endpoint.call.stage == V34_CALL_RETRAIN_SILENCE) {
			responded = true;
			break;
		}
		CHECK(endpoint.call.stage == V34_CALL_PHASE2 &&
		      endpoint.call.state.phase2.stage == V34_PHASE2_INFO1,
		      "INFO1 peer tone left its exchange before retrain");
	}
	CHECK(responded && endpoint.call.retrain_active &&
	      recovered == (remaining_samples < V34_RETRAIN_TONE_SAMPLES),
	      "INFO1 role=%d pcma=%d remaining=%u recovery=%d response=%d",
	      initial->call.local_role, pcma, remaining_samples,
	      recovered, responded);
	if (recovered && initial->call.local_role == V34_ANSWERING_STATION) {
		CHECK(!endpoint.call.retrain.peer_tone_retained,
		      "answerer timeout claimed a qualified peer tone");
		return true;
	}
	return complete_remote_retrain_silence(&endpoint, &remote_tone, pcma);
}

static bool test_info1_retrain_preamble(void)
{
	for (unsigned int law = 0; law < 2; ++law) {
		struct channel channel = { .pcma = law != 0 };
		struct endpoint caller;
		struct endpoint answerer;
		struct endpoint retained[2];
		bool stored[2] = { false, false };

		CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
		      endpoint_init(&answerer, V34_ANSWERING_STATION),
		      "INFO1 preamble endpoints did not initialize");
		for (size_t index = 0;
		     index < SAMPLE_LIMIT && !(stored[0] && stored[1]); ++index) {
			struct v34_call_observation caller_observation;
			struct v34_call_observation answerer_observation;
			const struct endpoint *endpoints[] = { &caller, &answerer };

			CHECK(channel_sample_pair(
				      &channel, &caller, &answerer,
				      &caller_observation, &answerer_observation),
			      "INFO1 preamble setup stopped at sample %zu", index);
			for (size_t role = 0; role < ARRAY_LENGTH(endpoints); ++role) {
				const struct v34_call *call = &endpoints[role]->call;

				if (!stored[role] && call->stage == V34_CALL_PHASE2 &&
				    call->state.phase2.stage == V34_PHASE2_INFO1 &&
				    call->state.phase2.state.info1.control.phase ==
					    PHASE2_INFO1_WAITING_FOR_PEER) {
					retained[role] = *endpoints[role];
					stored[role] = true;
				}
			}
		}
		CHECK(stored[0] && stored[1], "INFO1 calls were not retained");
		CHECK(info1_timeout_restarts_answerer(&retained[1], law != 0),
		      "INFO1 answerer timeout failed law=%u", law);
		for (size_t role = 0; role < ARRAY_LENGTH(retained); ++role)
			CHECK(info1_retrain_before_deadline(
				      &retained[role], law != 0,
				      2u * V34_RETRAIN_TONE_SAMPLES) &&
			      info1_retrain_before_deadline(
				      &retained[role], law != 0,
				      V34_PHASE2_TONE_ACQUIRE_SAMPLES / 2u),
			      "INFO1 retrain preamble failed law=%u role=%zu", law, role);
	}
	return true;
}

static bool check_retrain_tone_stage_guard(
	enum v34_station_role local_role, bool pcma,
	enum v34_call_stage target_stage, bool retraining,
	bool transmit_b1_complete, bool online)
{
	enum v34_station_role remote_role =
		v34_station_peer_role(local_role);
	struct channel channel = { .pcma = pcma };
	struct endpoint caller;
	struct endpoint answerer;
	struct endpoint *endpoint;
	struct v34_phase2_tone_transmitter remote_tone;
	struct v34_call_observation observation;
	struct v34_retrain_detector before;
	int16_t trigger_sample = 0;
	int16_t transmitted;
	bool expect_retrain =
		(target_stage == V34_CALL_PHASE3 && retraining) ||
		(target_stage == V34_CALL_PHASE4 && !retraining && online);
	bool trigger_found = false;
	bool detected;

	CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
	      endpoint_init(&answerer, V34_ANSWERING_STATION),
	      "retrain stage guard endpoints did not initialize");
	endpoint = local_role == V34_CALLING_STATION ? &caller : &answerer;
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_call_observation caller_observation;
		struct v34_call_observation answerer_observation;

		CHECK(channel_sample_pair(
			      &channel, &caller, &answerer,
			      &caller_observation, &answerer_observation),
		      "retrain stage guard setup stopped at sample %zu",
		      index);
		if (endpoint->call.stage == target_stage &&
		    (target_stage != V34_CALL_PHASE4 ||
		     endpoint->call.state.phase4.transmit_b1_complete ==
			     transmit_b1_complete) &&
		    (target_stage != V34_CALL_PHASE4 ||
		     endpoint->call.state.phase4.online_announced == online))
			break;
	}
	if (target_stage == V34_CALL_PHASE3 ||
	    target_stage == V34_CALL_PHASE4)
		endpoint->call.retrain_active = retraining;
	CHECK(endpoint->call.stage == target_stage &&
	      (target_stage != V34_CALL_PHASE4 ||
	       endpoint->call.state.phase4.transmit_b1_complete ==
		       transmit_b1_complete) &&
	      (target_stage != V34_CALL_PHASE4 ||
	       endpoint->call.state.phase4.online_announced == online) &&
	      v34_call_is_valid(&endpoint->call) &&
	      (target_stage != V34_CALL_PHASE4 ||
	       v34_call_is_online(&endpoint->call) == online) &&
	      v34_retrain_detector_reset(
		      &endpoint->call.retrain.detector) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &remote_tone, remote_role, NOMINAL_AMPLITUDE) ==
			      V34_PHASE2_TONE_OK,
	      "retrain stage guard did not reach stage %d", target_stage);
	for (size_t index = 0; index < V34_RETRAIN_MIN_SILENCE_SAMPLES;
	     ++index)
		CHECK(v34_retrain_detector_sample(
			      &endpoint->call.retrain.detector, 0, &detected) ==
			      V34_PHASE2_TONE_OK && !detected,
		      "retrain stage guard silence failed at sample %zu",
		      index);
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES +
			     V34_RETRAIN_TONE_SAMPLES;
	     ++index) {
		struct v34_retrain_detector candidate =
			endpoint->call.retrain.detector;
		int16_t remote_sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) == V34_PHASE2_TONE_OK,
		      "retrain stage guard tone failed at sample %zu", index);
		trigger_sample = codec_round_trip(remote_sample, pcma);
		CHECK(v34_retrain_detector_sample(
			      &candidate, trigger_sample, &detected) ==
			      V34_PHASE2_TONE_OK,
		      "retrain stage detector failed at sample %zu", index);
		if (detected) {
			trigger_found = true;
			break;
		}
		endpoint->call.retrain.detector = candidate;
	}
	CHECK(trigger_found, "retrain stage guard did not find a trigger");
	before = endpoint->call.retrain.detector;
	CHECK(endpoint_sample(
		      endpoint, trigger_sample, supply_mapping_frame,
		      &transmitted, &observation) &&
	      observation.processed == target_stage &&
	      (expect_retrain ?
		       observation.next == V34_CALL_RETRAIN_SILENCE &&
			       endpoint->call.retrain_active :
		       observation.next == target_stage &&
			       memcmp(&endpoint->call.retrain.detector, &before,
				      sizeof(before)) == 0),
	      "retrain stage %d produced the wrong detector transition",
	      target_stage);
	if (expect_retrain)
		CHECK(complete_remote_retrain_silence(
			      endpoint, &remote_tone, pcma),
		      "retrain stage %d lost its peer tone", target_stage);
	return true;
}

static bool test_retrain_tone_stage_guard(void)
{
	for (enum v34_station_role role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role)
		for (unsigned int law = 0; law < 2; ++law) {
			CHECK(check_retrain_tone_stage_guard(
				      role, law != 0, V34_CALL_PHASE3,
				      false, false, false),
			      "Phase 3 retrain guard failed role=%d law=%u",
			      role, law);
			CHECK(check_retrain_tone_stage_guard(
				      role, law != 0, V34_CALL_PHASE3,
				      true, false, false),
			      "active Phase 3 retrain failed role=%d law=%u",
			      role, law);
			CHECK(check_retrain_tone_stage_guard(
				      role, law != 0, V34_CALL_PHASE4,
				      false, false, false),
			      "pre-B1 Phase 4 retrain guard failed role=%d law=%u",
			      role, law);
			/* This generated role exposes the valid interval after local B1
			 * transmission but before the receive direction reaches B1. */
			if (role == V34_ANSWERING_STATION)
				CHECK(check_retrain_tone_stage_guard(
					      role, law != 0,
					      V34_CALL_PHASE4, false, true,
					      false),
				      "pre-online Phase 4 retrain guard failed "
				      "role=%d law=%u",
				      role, law);
			CHECK(check_retrain_tone_stage_guard(
				      role, law != 0, V34_CALL_PHASE4,
				      false, true, true),
			      "online Phase 4 retrain failed role=%d law=%u",
			      role, law);
			CHECK(check_retrain_tone_stage_guard(
				      role, law != 0, V34_CALL_PHASE4,
				      true, false, false),
			      "active retrain guard failed role=%d law=%u",
			      role, law);
		}
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

		CHECK(channel_sample_pair(
			      &channel, &caller, &answerer,
			      &caller_observation, &answerer_observation),
		      "unusable INFO1 call stopped at sample %zu", index);
		if (answerer_observation.processed == V34_CALL_PHASE2 &&
		    answerer_observation.next == V34_CALL_RETRAIN_SILENCE) {
			restarted = true;
			break;
		}
	}
	CHECK(restarted && v34_call_is_valid(&answerer.call) &&
	      !v34_call_has_failed(&answerer.call),
	      "unusable INFO1 did not start a valid full retrain");
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES;
	     ++index) {
		struct v34_call_observation observation;
		int16_t transmit;

		CHECK(endpoint_sample(
			      &answerer, 0, supply_mapping_frame, &transmit,
			      &observation) && transmit == 0,
		      "INFO1 retrain silence failed at sample %zu", index);
	}
	CHECK(answerer.call.stage == V34_CALL_PHASE2 &&
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

static struct v34_call_configuration fallback_configuration(
	enum v34_station_role role)
{
	struct v34_call_configuration selected = symmetric_configuration(
		role, V34_SYMBOL_RATE_3429);

	selected.local_info0.low_carrier_3200_supported = true;
	if (role == V34_CALLING_STATION)
		selected.local_info1.calling
			.answer_transmit[V34_SYMBOL_RATE_3200] =
				(struct v34_info1_probe_result){
					.projected_data_rate_2400_bps_units = 10,
				};
	return selected;
}

static bool check_info1_fallback(bool pcma)
{
	struct v34_call_configuration caller_config =
		fallback_configuration(V34_CALLING_STATION);
	struct v34_call_configuration answerer_config =
		fallback_configuration(V34_ANSWERING_STATION);
	struct channel channel = { .pcma = pcma };
	struct endpoint caller = {
		.transmit_bit = 0,
		.verify_mapping_frames = true,
	};
	struct endpoint answerer = {
		.transmit_bit = 1,
		.verify_mapping_frames = true,
	};
	enum v34_station_role first_initiator =
		pcma ? V34_CALLING_STATION : V34_ANSWERING_STATION;

	CHECK(v34_call_init(&caller.call, &caller_config) == V34_CALL_OK &&
	      v34_call_init(&answerer.call, &answerer_config) == V34_CALL_OK,
	      "fallback endpoints did not initialize");
	CHECK(run_initialized_pair(&caller, &answerer, &channel) &&
	      answerer.call.local_info1.answering
		      .answer_to_call_symbol_rate == V34_SYMBOL_RATE_3429 &&
	      answerer.call.local_info1.answering
		      .call_to_answer_symbol_rate == V34_SYMBOL_RATE_3429,
	      "fallback pair did not begin at 3429 symbol/s");

	/* A fresh caller probe withdraws only the selected profile. The answerer
	 * must fall back once and use that selection for later retrains. */
	caller.call.local_info1.calling
		.answer_transmit[V34_SYMBOL_RATE_3429] =
			(struct v34_info1_probe_result){ 0 };
	CHECK(retrain_online_pair(
		      &caller, &answerer, &channel, first_initiator) &&
	      answerer.call.local_info1.answering
		      .answer_to_call_symbol_rate == V34_SYMBOL_RATE_3200 &&
	      answerer.call.local_info1.answering
		      .call_to_answer_symbol_rate == V34_SYMBOL_RATE_3200,
	      "first retrain did not retain the 3200 fallback");
	CHECK(retrain_online_pair(
		      &caller, &answerer, &channel,
		      v34_station_peer_role(first_initiator)) &&
	      answerer.call.local_info1.answering
		      .answer_to_call_symbol_rate == V34_SYMBOL_RATE_3200 &&
	      answerer.call.local_info1.answering
		      .call_to_answer_symbol_rate == V34_SYMBOL_RATE_3200,
	      "second retrain returned to the withdrawn profile");
	return true;
}

static bool test_info1_fallback(void)
{
	CHECK(check_info1_fallback(false),
	      "PCMU repeated INFO1 fallback failed");
	CHECK(check_info1_fallback(true),
	      "PCMA repeated INFO1 fallback failed");
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
	config.phase3.nominal_peak_amplitude = 0;
	CHECK(v34_call_init(&call, &config) == V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid Phase 3 amplitude changed the destination");
	config = configuration(V34_CALLING_STATION);
	CHECK(v34_call_init(&call, &config) == V34_CALL_OK,
	      "validation call did not initialize");
	{
		struct v34_call invalid = call;

		invalid.retrain.response_silence_samples_remaining = 1u;
		CHECK(v34_full_retrain_is_valid(&invalid.retrain) &&
		      !v34_call_is_valid(&invalid),
		      "call accepted a retrain response outside retrain silence");
		invalid = call;
		invalid.retrain.detector.silence_history_count =
			V34_RETRAIN_SILENCE_WINDOW_SAMPLES + 1u;
		CHECK(!v34_call_is_valid(&invalid),
		      "call accepted an invalid retrain detector");
	}
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
	      v34_call_start_cleardown(&call) == V34_CALL_INVALID_STATE &&
	      v34_call_start_cleardown(NULL) == V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "rejected call-controller operations were not transactional");
	return true;
}

static bool clear_online_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel)
{
	struct endpoint *endpoints[] = { caller, answerer };
	unsigned int supplied[2];
	bool completed[2] = { false, false };

	for (unsigned int role = 0u; role < 2u; ++role) {
		struct endpoint *endpoint = endpoints[role];
		struct v34_line_receiver receive_line =
			endpoint->call.state.phase4.receive.line;
		struct v34_training_line transmit_line =
			endpoint->call.state.phase4.transmit.data.line;
		const struct v34_cleardown *cleardown =
			&endpoint->call.state.cleardown;

		supplied[role] = endpoint->mapping_frames_supplied;
		CHECK(v34_call_start_cleardown(&endpoint->call) == V34_CALL_OK &&
		      v34_call_start_cleardown(&endpoint->call) == V34_CALL_OK &&
		      v34_call_start_retrain(&endpoint->call) ==
			      V34_CALL_INVALID_STATE &&
		      memcmp(&receive_line, &cleardown->receive_line,
			     sizeof(receive_line)) == 0 &&
		      memcmp(&transmit_line, &cleardown->transmit_line,
			     sizeof(transmit_line)) == 0,
		      "role %u cleardown reset or shared the physical line", role);
		CHECK(cleardown_rejects_retained_observation(cleardown),
		      "role %u cleardown accepted an unresolved receive boundary", role);
	}
	for (unsigned int index = 0u; index < 16000u; ++index) {
		int16_t receive[2];
		int16_t transmit[2];

		channel_receive(channel, &receive[0], &receive[1]);
		for (unsigned int role = 0u; role < 2u; ++role) {
			struct v34_call_observation observation;
			struct v34_received_mapping_frame received;
			enum v34_call_result result = v34_call_sample(
				&endpoints[role]->call, receive[role],
				supply_mapping_frame, endpoints[role], &transmit[role],
				&observation, &received);

			CHECK(result == V34_CALL_OK ||
			      result == V34_CALL_CLEARDOWN_COMPLETE,
			      "role %u cleardown failed at %u: %d", role, index, result);
			completed[role] = result == V34_CALL_CLEARDOWN_COMPLETE;
		}
		channel_transmit(channel, transmit[0], transmit[1]);
		if (completed[0] && completed[1])
			break;
	}
	for (unsigned int role = 0u; role < 2u; ++role) {
		const struct v34_cleardown *cleardown =
			&endpoints[role]->call.state.cleardown;
		const struct v34_mp_parameters *peer = &cleardown->peer_parameters;

		CHECK(completed[role] && cleardown->peer_mp_received &&
		      cleardown->mp_prime_sent && peer->acknowledges_peer_mp &&
		      peer->maximum_call_to_answer_rate_2400_bps_units == 0u &&
		      peer->maximum_answer_to_call_rate_2400_bps_units == 0u &&
		      cleardown->tail_samples == 0u,
		      "role %u did not decode and drain reciprocal zero-rate MP-prime "
		      "(stage %u, S %u, MP %u, peer ack %u)", role,
		      cleardown->stage, cleardown->receive_s.complete,
		      cleardown->peer_mp_received, peer->acknowledges_peer_mp);
		CHECK(supplied[role] == endpoints[role]->mapping_frames_supplied,
		      "role %u cleardown sourced another host data frame", role);
	}
	return true;
}

static bool test_cleardown(void)
{
	for (unsigned int pcma = 0u; pcma <= 1u; ++pcma) {
		struct endpoint caller;
		struct endpoint answerer;
		struct channel channel;

		CHECK(run_online_pair(&caller, &answerer, &channel, pcma != 0u) &&
		      clear_online_pair(&caller, &answerer, &channel),
		      "law %u asymmetric cleardown failed", pcma);
	}
	return true;
}

struct cleardown_peer {
	struct v34_training_line line;
	struct v34_phase3_s_sequence s;
	struct v34_phase4_mp mp;
	struct v34_mp_parameters parameters;
};

static bool cleardown_peer_sample(struct cleardown_peer *peer, int16_t *sample)
{
	struct qam_point point;
	bool accepted;
	bool needs_point = v34_training_line_needs_point(&peer->line);

	if (needs_point) {
		if (!v34_phase3_s_sequence_is_complete(&peer->s)) {
			enum v34_phase3_s_section section;

			CHECK(v34_phase3_s_sequence_next(&peer->s, &point, &section) ==
				      V34_PHASE3_S_SEQUENCE_OK,
			      "scripted cleardown peer did not emit S/S-bar");
		} else {
			uint8_t quarter_point;
			uint8_t rotation;
			bool complete;

			if (v34_phase4_mp_record_is_complete(&peer->mp))
				CHECK(v34_phase4_mp_start_next_type0_record(
					      &peer->mp, &peer->parameters) ==
					      V34_PHASE4_MP_OK,
				      "scripted peer did not repeat its MP");
			CHECK(v34_phase4_mp_next(
				      &peer->mp, &point, &quarter_point, &rotation,
				      &complete) == V34_PHASE4_MP_OK,
			      "scripted peer MP encoder failed");
		}
	}
	return v34_training_line_sample(
		       &peer->line, needs_point ? &point : NULL, sample,
		       &accepted) == V34_TRAINING_LINE_OK;
}

static bool test_cleardown_waits_for_prime(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;
	struct cleardown_peer peer = { 0 };
	const struct v34_cleardown *cleardown = &caller.call.state.cleardown;
	const struct v34_mp_parameters *received_mp = &cleardown->peer_parameters;
	bool completed = false;

	CHECK(run_online_pair(&caller, &answerer, &channel, false),
	      "delayed acknowledgement call did not reach data");
	peer.line = answerer.call.state.phase4.transmit.data.line;
	peer.parameters = *v34_mp_record_common_parameters(
		&answerer.call.state.phase4.transmit_mp);
	peer.mp.role = V34_ANSWERING_STATION;
	peer.mp.constellation = V34_CONSTELLATION_4_POINT;
	peer.mp.frame_bit_count = V34_MP_TYPE0_FRAME_BITS;
	peer.mp.previous_clockwise_quarter_turns = 3u;
	CHECK(v34_transmit_scrambler_init(&peer.mp.scrambler, peer.mp.role) ==
		      V34_FRAMING_OK &&
	      v34_mp_type0_encode(&peer.parameters, peer.mp.frame,
				  sizeof(peer.mp.frame)) == V34_MP_OK &&
	      v34_call_start_cleardown(&caller.call) == V34_CALL_OK,
	      "delayed acknowledgement fixture did not initialize");
	for (unsigned int index = 0u; index < 16000u; ++index) {
		struct v34_call_observation observation;
		struct v34_received_mapping_frame received;
		int16_t caller_receive;
		int16_t ignored;
		int16_t caller_transmit;
		int16_t peer_transmit;
		enum v34_call_result result;

		if (index == 10000u) {
			CHECK(cleardown->stage == V34_CLEARDOWN_MP &&
			      cleardown->peer_mp_received && cleardown->mp_prime_sent &&
			      !cleardown->peer_parameters.acknowledges_peer_mp,
			      "cleardown did not wait for the reciprocal MP-prime");
			peer.parameters.acknowledges_peer_mp = true;
		}
		channel_receive(&channel, &caller_receive, &ignored);
		result = v34_call_sample(
			&caller.call, caller_receive, NULL, NULL, &caller_transmit,
			&observation, &received);
		CHECK(cleardown_peer_sample(&peer, &peer_transmit),
		      "delayed acknowledgement peer stopped");
		channel_transmit(&channel, caller_transmit, peer_transmit);
		CHECK(result == V34_CALL_OK ||
		      (index > 10000u && result == V34_CALL_CLEARDOWN_COMPLETE),
		      "cleardown completed or failed before MP-prime at %u: %d",
		      index, result);
		if (result == V34_CALL_CLEARDOWN_COMPLETE) {
			completed = true;
			break;
		}
	}
	CHECK(completed && cleardown->peer_mp_received &&
	      received_mp->acknowledges_peer_mp &&
	      received_mp->maximum_call_to_answer_rate_2400_bps_units ==
		      peer.parameters.maximum_call_to_answer_rate_2400_bps_units,
	      "cleardown did not accept the responding modem's ordinary-rate MP-prime");
	return true;
}

static bool test_phase2_handoff(void)
{
	struct v34_call_configuration caller =
		configuration(V34_CALLING_STATION);
	struct v34_call_configuration answerer =
		configuration(V34_ANSWERING_STATION);
	struct v34_phase2_handoff handoff = {
		.local_info0 = caller.local_info0,
		.peer_info0 = answerer.local_info0,
		.caller_info1c = caller.local_info1.calling,
		.answerer_info1a = answerer.local_info1.answering,
		.round_trip_delay_samples = LINE_DELAY_SAMPLES * 2u,
		.receive_gain = INT16_MAX,
		.local_role = V34_CALLING_STATION,
	};
	struct v34_call call;
	struct v34_call saved;

	memset(&call, 0xa5, sizeof(call));
	CHECK(v34_phase2_handoff_is_valid(&handoff) &&
	      v34_call_init_from_phase2(&call, &caller, &handoff) ==
		      V34_CALL_OK &&
	      v34_call_is_valid(&call) && call.stage == V34_CALL_PHASE3 &&
	      call.local_role == V34_CALLING_STATION &&
	      call.round_trip_delay_samples ==
		      handoff.round_trip_delay_samples &&
	      memcmp(&call.local_info0, &handoff.local_info0,
		     sizeof(call.local_info0)) == 0 &&
	      memcmp(&call.peer_info0, &handoff.peer_info0,
		     sizeof(call.peer_info0)) == 0,
	      "completed Phase 2 did not initialize the V.34 Phase 3 owner");
	handoff.receive_gain = 0;
	memset(&call, 0xa5, sizeof(call));
	saved = call;
	CHECK(!v34_phase2_handoff_is_valid(&handoff) &&
	      v34_call_init_from_phase2(&call, &caller, &handoff) ==
		      V34_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid Phase 2 handoff changed the V.34 call destination");
	return true;
}

static bool test_rate_renegotiation_timeout(void)
{
	for (unsigned int variant = 0; variant < 3u; ++variant) {
		bool calling_peer = variant == 1u;
		bool cme = variant == 2u;
		struct endpoint caller = { .verify_mapping_frames = true };
		struct endpoint answerer = { .verify_mapping_frames = true, .transmit_bit = 1u };
		struct endpoint *responder = calling_peer ? &answerer : &caller;
		struct endpoint *initiator = calling_peer ? &caller : &answerer;
		struct channel channel = { 0 };
		struct v34_training_transmitter peer;
		struct v34_call_configuration call_config = configuration(V34_CALLING_STATION);
		struct v34_call_configuration answer_config = configuration(V34_ANSWERING_STATION);
		uint32_t armed_at = UINT32_MAX;
		uint32_t timeout;
		bool retrained = false;

		call_config.local_info0.cme_modem = cme;
		answer_config.local_info0.cme_modem = cme;
		CHECK(v34_call_init(&caller.call, &call_config) == V34_CALL_OK &&
		      v34_call_init(&answerer.call, &answer_config) == V34_CALL_OK &&
		      run_initialized_pair(&caller, &answerer, &channel),
		      "rate renegotiation timeout fixture did not connect");
		timeout = cme ? 240000u : 20000u + 3u * responder->call.round_trip_delay_samples;
		CHECK(v34_training_transmitter_resume_online(&peer,
			      &initiator->call.state.phase4.transmit.data.line) == V34_TRAINING_TRANSMITTER_OK,
		      "rate renegotiation timeout peer did not initialize");
		/* Deliberately leave the peer in TRN: no MP-prime/E acknowledgement
		 * may keep an online renegotiation alive beyond its protocol deadline. */
		for (uint32_t sample = 0; sample < timeout + 2000u; ++sample) {
			struct v34_call_observation observation;
			struct v34_received_mapping_frame received;
			int16_t caller_receive, answerer_receive, generated, peer_output;

			channel_receive(&channel, &caller_receive, &answerer_receive);
			CHECK(v34_call_sample(&responder->call,
				      calling_peer ? answerer_receive : caller_receive,
				      NULL, NULL, &generated, &observation, &received) == V34_CALL_OK &&
			      v34_training_transmitter_sample(&peer, &peer_output) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "rate renegotiation timeout path failed: variant=%u sample=%u", variant, sample);
			channel_transmit(&channel, calling_peer ? peer_output : generated,
				calling_peer ? generated : peer_output);
			if (armed_at == UINT32_MAX && responder->call.rate_renegotiation_remaining_samples) {
				armed_at = sample;
				CHECK(responder->call.rate_renegotiation_remaining_samples == timeout,
				      "rate renegotiation armed the wrong timeout");
			}
			if (responder->call.stage == V34_CALL_RETRAIN_SILENCE) {
				CHECK(armed_at != UINT32_MAX && sample - armed_at == timeout &&
				      responder->call.rate_renegotiation_remaining_samples == 0u,
				      "rate renegotiation retrained at the wrong deadline");
				retrained = true;
				break;
			}
		}
		CHECK(retrained, "unacknowledged rate renegotiation never requested full retrain");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bounded online rate renegotiation", test_rate_renegotiation_timeout },
		{ "reciprocal zero-rate cleardown", test_cleardown },
		{ "cleardown waits for peer MP-prime", test_cleardown_waits_for_prime },
		{ "completed Phase 2 handoff", test_phase2_handoff },
		{ "generated PCMU call", test_pcmu_call },
		{ "delayed PCMU and PCMA calls", test_delayed_calls },
		{ "generated station echo", test_echo_calls },
		{ "symmetric selected-rate calls", test_symmetric_calls },
		{ "local and remote full retrain", test_full_retrain },
		{ "full retrain through answerer echo", test_echo_retrain },
		{ "retrain selects new INFO1a rate pair",
		  test_retrain_selects_new_rate },
		{ "unanswered probe recovery", test_probe_recovery },
		{ "retrain tone guard", test_retrain_tone_guard },
		{ "INFO1 retrain preamble", test_info1_retrain_preamble },
		{ "retrain tone stage guard",
		  test_retrain_tone_stage_guard },
		{ "unusable INFO1 starts full retrain",
		  test_unusable_info1_retrains },
		{ "repeated retrain retains INFO1 fallback",
		  test_info1_fallback },
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
