// SPDX-License-Identifier: BSD-3-Clause

#include <v34_call.h>
#include <v34_phase2_selection.h>

#include <g711.h>

#include "test.h"

#include <string.h>

enum {
	SAMPLE_LIMIT = 140000,
	LINE_DELAY_SAMPLES = 13,
	LINE_DELAY_CAPACITY = 1666,
	ECHO_DELAY_CAPACITY = 3333,
	TEST_ECHO_DELAY_SAMPLES = 32,
	NOMINAL_AMPLITUDE = 7000,
	S_CONFIRMATION_SAMPLES = 8u * 8u,
	SHORT_TRN_SAMPLES = 5u * 8u,
};

struct self_echo {
	int16_t samples[ECHO_DELAY_CAPACITY];
	size_t cursor;
	size_t delay_samples;
	int16_t gain_q15;
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
	uint32_t receive_errors;
	uint32_t phase3_trn_symbols;
	uint32_t phase3_transmit_samples;
	uint32_t mapping_pattern_seed;
	uint32_t peer_mapping_pattern_seed;
	uint8_t primary_idle_bits_remaining;
	uint8_t peer_primary_delay_bits;
	uint8_t transmit_bit;
	bool generated_mapping_frames;
	bool verify_mapping_frames;
	bool allow_receive_errors;
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
	enum v34_station_role role, enum v34_symbol_rate symbol_rate,
	uint8_t primary_rate_units)
{
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
				primary_rate_units;
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
				primary_rate_units;
		selected.local_info1.answering.answer_to_call_symbol_rate =
			symbol_rate;
		selected.local_info1.answering.call_to_answer_symbol_rate =
			symbol_rate;
	}
	selected.phase4.transmit_mp.parameters.type0
		.maximum_call_to_answer_rate_2400_bps_units = primary_rate_units;
	selected.phase4.transmit_mp.parameters.type0
		.maximum_answer_to_call_rate_2400_bps_units = primary_rate_units;
	selected.phase4.transmit_mp.parameters.type0.data_rate_capability_mask =
		(uint16_t)((1u << primary_rate_units) - 1u);
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

		if (received->primary_bits[index] != bit) {
			if (!endpoint->allow_receive_errors)
				return false;
			endpoint->receive_errors++;
		}
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
	enum v34_symbol_rate symbol_rate, uint8_t primary_rate_units)
{
	struct v34_call_configuration config =
		symmetric_configuration(role, symbol_rate, primary_rate_units);
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
	if (observation->processed == V34_CALL_PHASE3 &&
	    (observation->events.phase3 &
	     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED) != 0) {
		const struct v34_training_transmitter *transmit =
			observation->next == V34_CALL_PHASE4 ?
				&endpoint->call.state.phase4.transmit.training :
				&endpoint->call.state.phase3.transmit;

		endpoint->phase3_trn_symbols = transmit->phase3.trn_symbols;
		endpoint->phase3_transmit_samples = transmit->line.generated_samples;
	}
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
	if (echo->delay_samples == 0)
		return remote;
	size_t index = (echo->cursor + ECHO_DELAY_CAPACITY -
		echo->delay_samples) % ECHO_DELAY_CAPACITY;
	return mix_echo(remote, echo->samples[index], echo->gain_q15);
}

static void self_echo_transmit(struct self_echo *echo, int16_t local)
{
	echo->samples[echo->cursor] = local;
	echo->cursor = (echo->cursor + 1u) % ECHO_DELAY_CAPACITY;
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
	caller = codec_round_trip(caller, channel->pcma);
	answerer = codec_round_trip(answerer, channel->pcma);
	self_echo_transmit(&channel->echo[V34_CALLING_STATION], caller);
	self_echo_transmit(&channel->echo[V34_ANSWERING_STATION], answerer);
	channel->call_to_answer[channel->cursor] = caller;
	channel->answer_to_call[channel->cursor] = answerer;
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

static bool run_pair_until_frames(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel, unsigned int minimum_frames)
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
		    caller->mapping_frames_received >= minimum_frames &&
		    answerer->mapping_frames_received >= minimum_frames) {
			CHECK(caller->call.peer_j_remaining_samples == 0u &&
			      answerer->call.peer_j_remaining_samples == 0u &&
			      caller->call.peer_s_remaining_samples == 0u &&
			      answerer->call.peer_s_remaining_samples == 0u &&
			      caller->call.phase4_e_remaining_samples == 0u &&
			      answerer->call.phase4_e_remaining_samples == 0u,
			      "online data retained a startup deadline");
			return true;
		}
	}
	return false;
}

static bool run_initialized_pair(
	struct endpoint *caller, struct endpoint *answerer,
	struct channel *channel)
{
	return run_pair_until_frames(caller, answerer, channel, 4u);
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
	enum v34_station_role echoed_role, bool pcma,
	size_t echo_delay, size_t line_delay, struct endpoint *result)
{
	struct channel channel = {
		.pcma = pcma,
		.delay_samples = line_delay,
	};
	struct endpoint caller;
	struct endpoint answerer;
	bool connected;

	channel.echo[echoed_role] = (struct self_echo){
		.delay_samples = echo_delay,
		.gain_q15 = 14000,
	};
	CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
	      endpoint_init(&answerer, V34_ANSWERING_STATION),
	      "echoed endpoints did not initialize");
	caller.generated_mapping_frames = answerer.generated_mapping_frames = true;
	caller.mapping_pattern_seed = answerer.peer_mapping_pattern_seed =
		UINT32_C(0x34c011ab);
	answerer.mapping_pattern_seed = caller.peer_mapping_pattern_seed =
		UINT32_C(0x34a05e17);
	connected = run_pair_until_frames(&caller, &answerer, &channel, 256u);
	*result = echoed_role == V34_CALLING_STATION ? caller : answerer;
	return connected;
}

static bool check_echo_call(
	enum v34_station_role role, bool pcma,
	size_t echo_delay, size_t line_delay)
{
	struct endpoint filtered;
	const char *station = role == V34_CALLING_STATION ? "calling" :
							       "answering";

	CHECK(run_echo_call(role, pcma, echo_delay, line_delay, &filtered),
	      "cancelled %s-station echo did not reach mapped data", station);
	CHECK(pcm_host_echo_is_ready(&filtered.call.echo) &&
	      filtered.call.echo.window_delay_samples <= echo_delay &&
	      filtered.call.echo.window_delay_samples +
		      PCM_HOST_ECHO_FILTER_TAPS > echo_delay,
	      "%s-station echo model was not retained", station);
	return true;
}

static bool test_echo_calls(void)
{
	CHECK(check_echo_call(V34_ANSWERING_STATION, false, 32, LINE_DELAY_SAMPLES),
	      "PCMU answerer echo call failed");
	CHECK(check_echo_call(V34_ANSWERING_STATION, true, 32, LINE_DELAY_SAMPLES),
	      "PCMA answerer echo call failed");
	CHECK(check_echo_call(V34_CALLING_STATION, false, 32, LINE_DELAY_SAMPLES),
	      "PCMU caller echo call failed");
	CHECK(check_echo_call(V34_CALLING_STATION, true, 32, LINE_DELAY_SAMPLES),
	      "PCMA caller echo call failed");
	return true;
}

static bool test_delayed_echo_calls(void)
{
	/* Long propagation can still have a near return. Ranging determines
	 * the deadline, while the transmit reference determines the echo path. */
	CHECK(check_echo_call(V34_CALLING_STATION, true, 32, 1666) &&
	      check_echo_call(V34_ANSWERING_STATION, false, 32, 1666),
	      "near echo failed with long propagation");
	for (unsigned law = 0; law < 2; ++law)
		CHECK(check_echo_call(V34_CALLING_STATION, law != 0, 3333, 1666) &&
		      check_echo_call(V34_ANSWERING_STATION, law != 0, 3333, 1666),
		      "law %u delayed return did not verify mapped data", law);
	return true;
}

static bool test_weak_echo_delay_step(void)
{
	for (unsigned law = 0; law < 2; ++law) {
		struct endpoint caller, answerer;
		struct channel channel = { .pcma = law != 0 };
		struct self_echo *echo = &channel.echo[V34_CALLING_STATION];

		*echo = (struct self_echo){ .delay_samples = 256, .gain_q15 = 700 };
		CHECK(endpoint_init_symmetric(&caller, V34_CALLING_STATION,
			V34_SYMBOL_RATE_3200, 12) &&
		      endpoint_init_symmetric(&answerer, V34_ANSWERING_STATION,
			V34_SYMBOL_RATE_3200, 12) &&
		      run_pair_until_frames(&caller, &answerer, &channel, 256),
		      "weak echo setup failed for codec %u", law);
		CHECK(pcm_host_echo_is_ready(&caller.call.echo),
		      "weak echo did not qualify during training");
		uint32_t delay = caller.call.echo.window_delay_samples;
		for (unsigned i = 0; i < 8000; ++i) {
			struct v34_call_observation c, a;
			CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a) &&
			      caller.call.echo.window_delay_samples == delay,
			      "stationary weak echo retimed or corrupted mapped data");
		}
		/* Keep the reference history intact: only the physical return's
		 * delay changes. Continue checking the random stream's position
		 * while allowing errors during the recovery interval. */
		echo->delay_samples += 80;
		caller.allow_receive_errors = true;
		for (unsigned i = 0; i < 10000; ++i) {
			struct v34_call_observation c, a;
			CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a),
			      "delay step lost the data path for codec %u", law);
		}
		CHECK(caller.call.echo.window_delay_samples == delay + 80,
		      "weak return did not follow delay step: codec %u, %u -> %u",
		      law, delay, caller.call.echo.window_delay_samples);
		caller.allow_receive_errors = false;
		for (unsigned i = 0; i < 4000; ++i) {
			struct v34_call_observation c, a;
			CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a),
			      "recovered return corrupted mapped data for codec %u", law);
		}
		CHECK(v34_call_is_online(&caller.call) && v34_call_is_online(&answerer.call),
		      "delay recovery required a retrain");
	}
	return true;
}

static bool test_fast_echo_delay_step(void)
{
	const enum v34_symbol_rate rates[] = { V34_SYMBOL_RATE_3200, V34_SYMBOL_RATE_3429 };
	const unsigned units[] = { 12, 14 };
	for (unsigned law = 0; law < 2; ++law) {
		for (unsigned rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			struct endpoint caller, answerer;
			struct channel channel = { .pcma = law != 0 };
			struct self_echo *echo = &channel.echo[V34_CALLING_STATION];

			*echo = (struct self_echo){ .delay_samples = 256, .gain_q15 = 700 };
			CHECK(endpoint_init_symmetric(&caller, V34_CALLING_STATION,
				rates[rate], units[rate]) &&
			      endpoint_init_symmetric(&answerer, V34_ANSWERING_STATION,
				rates[rate], units[rate]), "fast echo setup failed");
			caller.mapping_pattern_seed ^= 17;
			answerer.peer_mapping_pattern_seed ^= 17;
			answerer.mapping_pattern_seed ^= 51;
			caller.peer_mapping_pattern_seed ^= 51;
			CHECK(run_pair_until_frames(&caller, &answerer, &channel, 256),
			      "fast echo training failed: law %u, rate %u", law, rate);
			uint32_t delay = caller.call.echo.window_delay_samples;
			for (unsigned i = 0; i < 8000; ++i) {
				struct v34_call_observation c, a;
				CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a) &&
				      caller.call.echo.window_delay_samples == delay,
				      "stationary return retimed or corrupted data");
			}
			echo->delay_samples += 80;
			caller.allow_receive_errors = true;
			unsigned recovered = 0;
			for (unsigned i = 0; i < 8000; ++i) {
				struct v34_call_observation c, a;
				CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a),
				      "fast echo recovery lost the data path");
				uint32_t current = caller.call.echo.window_delay_samples;
				CHECK(current == delay || current == delay + 80,
				      "echo tracker selected an unrelated delay");
				if (current == delay + 80 && recovered == 0)
					recovered = i + 1;
			}
			CHECK(recovered != 0 && recovered <= 3200,
			      "strong echo improvement took %u samples: law %u, rate %u",
			      recovered, law, rate);
			/* The longer tracker takes over 4500 samples on these streams.
			 * Quicker correction avoids all errors in the mu-law 28.8k case. */
			CHECK(law != 0 || rate != 0 || caller.receive_errors == 0,
			      "fast recovery did not preserve the known 28.8k bit stream");
			caller.allow_receive_errors = false;
			for (unsigned i = 0; i < 4000; ++i) {
				struct v34_call_observation c, a;
				CHECK(channel_sample_pair(&channel, &caller, &answerer, &c, &a) &&
				      caller.call.echo.window_delay_samples == delay + 80,
				      "recovered return corrupted data or moved again");
			}
			CHECK(v34_call_is_online(&caller.call) && v34_call_is_online(&answerer.call),
			      "fast delay recovery required a retrain");
		}
	}
	return true;
}

static bool test_quiet_training_completion(void)
{
	struct endpoint caller;
	struct endpoint answerer;
	struct channel channel;

	CHECK(run_online_pair(&caller, &answerer, &channel, false),
	      "quiet training did not reach verified data");
	const struct endpoint *endpoints[] = { &caller, &answerer };
	for (size_t role = 0; role < ARRAY_LENGTH(endpoints); ++role) {
		CHECK(!pcm_host_echo_is_ready(&endpoints[role]->call.echo) &&
		      endpoints[role]->phase3_trn_symbols >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
		      endpoints[role]->phase3_transmit_samples >=
			PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES &&
		      endpoints[role]->phase3_transmit_samples < V34_PHASE2_SAMPLE_RATE,
		      "role %zu accepted a quiet model or missed its bounded early finish",
		      role);
	}
	return true;
}

static bool check_symmetric_call(
	enum v34_symbol_rate symbol_rate, uint8_t primary_rate_units, bool pcma)
{
	struct channel channel = { .pcma = pcma };
	struct endpoint caller;
	struct endpoint answerer;

	CHECK(endpoint_init_symmetric(
		      &caller, V34_CALLING_STATION, symbol_rate, primary_rate_units) &&
	      endpoint_init_symmetric(
		      &answerer, V34_ANSWERING_STATION, symbol_rate, primary_rate_units),
	      "symmetric rate %u endpoints did not initialize", symbol_rate);
	CHECK(run_initialized_pair(&caller, &answerer, &channel),
	      "symmetric rate %u call did not reach mapped data", symbol_rate);
	CHECK(caller.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      answerer.call.state.phase4.receive.stage ==
		      V34_PHASE34_RECEIVER_DATA &&
	      caller.call.state.phase4.data_startup.receive.geometry
		      .primary_data_rate_2400_bps_units == primary_rate_units &&
	      answerer.call.state.phase4.data_startup.receive.geometry
		      .primary_data_rate_2400_bps_units == primary_rate_units &&
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
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2743, 10u, false),
	      "PCMU symmetric 2743 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2743, 10u, true),
	      "PCMA symmetric 2743 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2800, 10u, false),
	      "PCMU symmetric 2800 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_2800, 10u, true),
	      "PCMA symmetric 2800 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3000, 10u, false),
	      "PCMU symmetric 3000 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3000, 10u, true),
	      "PCMA symmetric 3000 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3200, 10u, false),
	      "PCMU symmetric 3200 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3200, 10u, true),
	      "PCMA symmetric 3200 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3429, 10u, false),
	      "PCMU symmetric 3429 call failed");
	CHECK(check_symmetric_call(V34_SYMBOL_RATE_3429, 10u, true),
	      "PCMA symmetric 3429 call failed");
	for (unsigned int law = 0; law < 2; ++law)
		for (uint8_t units = 1; units <= 2; ++units)
			CHECK(check_symmetric_call(V34_SYMBOL_RATE_2400, units, law != 0),
			      "minimum-rate B1 call failed law=%u units=%u", law, units);
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

static bool check_startup_retrain(
	enum v34_station_role role, bool pcma, bool corrupt_b1)
{
	struct endpoint caller, answerer;
	struct channel channel = { .pcma = pcma };
	struct endpoint *local = role == V34_CALLING_STATION ? &caller : &answerer;
	struct endpoint *peer = role == V34_CALLING_STATION ? &answerer : &caller;
	bool injected = false, requested = false;
	bool caller_retrained = false, answerer_retrained = false;
	bool verifying = false;

	CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
	      endpoint_init(&answerer, V34_ANSWERING_STATION),
	      "startup-retrain pair did not initialize");
	caller.verify_mapping_frames = answerer.verify_mapping_frames = false;
	for (size_t sample = 0; sample < 2u * SAMPLE_LIMIT; ++sample) {
		struct v34_call_observation co, ao;
		const struct v34_call_observation *observation =
			role == V34_CALLING_STATION ? &co : &ao;

		if (corrupt_b1 && !requested && peer->call.stage == V34_CALL_PHASE4 &&
		    peer->call.state.phase4.transmit_data_active) {
			struct v34_data_stream *stream = &peer->call.state.phase4.transmit.data;
			struct v34_data_points *points = &stream->points;
			unsigned frames = points->schedule.geometry.mapping_frames_per_data_frame;

			if (stream->b1_active && v34_data_points_needs_mapping_frame(points) &&
			    points->schedule.next_mapping_frame >= frames / 2u &&
			    points->schedule.next_mapping_frame < frames) {
				struct v34_mapping_frame request;
				uint8_t primary[V34_MAX_MAPPING_FRAME_BITS] = { 0 };
				uint8_t auxiliary = 0;

				/* Send valid coded symbols with a known wrong B1 payload. */
				CHECK(v34_data_points_preview_mapping_frame(points, &request) == V34_DATA_POINTS_OK &&
				      v34_data_points_queue_mapping_frame(points, primary, request.primary_bits,
					      request.carries_auxiliary_bit ? &auxiliary : NULL) == V34_DATA_POINTS_OK,
				      "could not generate a corrupt B1 frame");
				injected = true;
			}
		}
		CHECK(channel_sample_pair(&channel, &caller, &answerer, &co, &ao),
		      "startup retrain stopped role=%u law=%u sample=%zu", role, pcma, sample);
		if (corrupt_b1 && injected && !requested) {
			CHECK(!v34_call_is_online(&local->call) &&
			      (observation->processed != V34_CALL_PHASE4 ||
			       (observation->events.phase4 & V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) == 0),
			      "corrupt B1 published online data role=%u law=%u", role, pcma);
			requested = local->call.stage == V34_CALL_RETRAIN_SILENCE;
		} else if (!corrupt_b1 && !requested && v34_call_is_online(&local->call)) {
			CHECK(role != V34_CALLING_STATION || !v34_call_is_online(&answerer.call),
			      "early-retrain fixture missed the B1 overlap");
			CHECK(v34_call_start_retrain(&local->call) == V34_CALL_OK,
			      "immediate retrain was rejected");
			requested = true;
		}
		if (requested) {
			caller_retrained |= caller.call.stage == V34_CALL_RETRAIN_SILENCE;
			answerer_retrained |= answerer.call.stage == V34_CALL_RETRAIN_SILENCE;
		}
		if (!verifying && caller_retrained && answerer_retrained &&
		    v34_call_is_online(&caller.call) && v34_call_is_online(&answerer.call)) {
			caller.mapping_frames_received = answerer.mapping_frames_received = 0;
			caller.verify_mapping_frames = answerer.verify_mapping_frames = true;
			verifying = true;
		}
		if (verifying && caller.mapping_frames_received >= 4u && answerer.mapping_frames_received >= 4u)
			break;
	}
	CHECK(verifying && caller.mapping_frames_received >= 4u && answerer.mapping_frames_received >= 4u,
	      "startup retrain did not recover role=%u law=%u corrupt=%u requested=%u caller=%u answerer=%u",
	      role, pcma, corrupt_b1, requested, caller_retrained, answerer_retrained);
	return true;
}

static bool test_startup_retrain(void)
{
	for (unsigned int corrupt = 0; corrupt < 2; ++corrupt)
		for (unsigned int law = 0; law < 2; ++law)
			for (enum v34_station_role role = V34_CALLING_STATION;
			     role <= V34_ANSWERING_STATION; ++role)
				CHECK(check_startup_retrain(role, law != 0, corrupt != 0),
				      "startup retrain failed role=%u law=%u corrupt=%u", role, law, corrupt);
	return true;
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

static bool info1_continuous_tone_after_deadline(
	const struct endpoint *initial, bool pcma)
{
	struct endpoint endpoint = *initial;
	const struct phase2_info1_exchange *exchange =
		&initial->call.state.phase2.state.info1.control;
	struct v34_phase2_tone_transmitter remote_tone;
	uint32_t remaining = PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES +
		exchange->round_trip_delay_samples -
		exchange->peer_deadline_elapsed_samples;
	bool recovered = false;

	CHECK(initial->call.local_role == V34_CALLING_STATION &&
	      exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER &&
	      v34_phase2_tone_transmitter_init(
		      &remote_tone, V34_ANSWERING_STATION,
		      NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "continuous INFO1 Tone A fixture did not initialize");
	/* Keep Tone A present from the INFO1 wait through its real deadline.
	 * V.34 11.2.2.1.6 then conditions reception for Tone A; it does not
	 * require a new full-retrain silence preamble before recognizing it. */
	for (uint32_t sample = 0;
	     sample < remaining + V34_RETRAIN_TONE_SAMPLES +
			      2u * V34_PHASE2_TONE_WINDOW_SAMPLES;
	     ++sample) {
		struct v34_call_observation observation;
		int16_t remote_sample;
		int16_t transmitted;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) == V34_PHASE2_TONE_OK &&
		      endpoint_sample(
			      &endpoint, codec_round_trip(remote_sample, pcma),
			      supply_mapping_frame, &transmitted, &observation),
		      "continuous INFO1 Tone A failed at sample %u", sample);
		recovered |= observation.events.phase2.kind ==
			     V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED;
		if (endpoint.call.stage == V34_CALL_RETRAIN_SILENCE) {
			CHECK(recovered && sample + 1u >
				      remaining + V34_PHASE2_SAMPLE_RATE / 20u &&
			      endpoint.call.retrain.peer_tone_retained,
			      "continuous Tone A retrain missed its INFO1 deadline");
			return complete_remote_retrain_silence(
				&endpoint, &remote_tone, pcma);
		}
	}
	CHECK(false, "continuous Tone A left caller waiting after INFO1 deadline");
}

static bool info1_late_reply(
	const struct endpoint *initial, const struct endpoint *answerer, bool pcma)
{
	struct endpoint endpoint = *initial;
	const struct phase2_info1_exchange *exchange =
		&initial->call.state.phase2.state.info1.control;
	struct v34_phase2_info_audio_transmitter reply;
	struct v34_info1a selection;
	uint8_t payload[V34_INFO1A_PAYLOAD_BYTES];
	uint32_t remaining = PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES +
		exchange->round_trip_delay_samples -
		exchange->peer_deadline_elapsed_samples;
	bool complete = false;

	CHECK(v34_phase2_select_answerer_info1a(
		      &initial->call.local_info0, &answerer->call.local_info0,
		      &initial->call.local_info1.calling,
		      &answerer->call.local_info1.answering, &selection) ==
		      V34_PHASE2_SELECTION_OK &&
	      v34_info1a_encode(&selection, payload, sizeof(payload)) ==
		      V34_INFO1_OK &&
	      v34_phase2_info_audio_transmitter_init(
		      &reply, V34_ANSWERING_STATION, NOMINAL_AMPLITUDE,
		      payload, V34_INFO1A_PAYLOAD_BITS) == V34_PHASE2_INFO_AUDIO_OK,
	      "late INFO1a fixture did not initialize");
	for (uint32_t sample = 0; sample < remaining + 80u; ++sample) {
		struct v34_call_observation observation;
		int16_t transmitted;

		CHECK(endpoint_sample(&endpoint, codec_round_trip(0, pcma),
			      supply_mapping_frame, &transmitted, &observation) &&
		      endpoint.call.stage == V34_CALL_PHASE2,
		      "late INFO1a wait failed at sample %u", sample);
	}
	CHECK(endpoint.call.state.phase2.state.info1.control.phase ==
		      PHASE2_INFO1_RECOVERING,
	      "late INFO1a did not cross the real deadline");
	for (uint32_t sample = 0; sample < 4000u; ++sample) {
		struct v34_call_observation observation;
		int16_t received = 0;
		int16_t transmitted;

		CHECK((complete || v34_phase2_info_audio_transmitter_sample(
			       &reply, &received, &complete) ==
			       V34_PHASE2_INFO_AUDIO_OK) &&
		      endpoint_sample(&endpoint, codec_round_trip(received, pcma),
			      supply_mapping_frame, &transmitted, &observation),
		      "late INFO1a failed at sample %u", sample);
		if (endpoint.call.stage == V34_CALL_PHASE3)
			return true;
		CHECK(endpoint.call.stage == V34_CALL_PHASE2,
		      "late valid INFO1a triggered retrain at sample %u", sample);
	}
	CHECK(false, "late valid INFO1a did not reach Phase3");
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
		CHECK(info1_continuous_tone_after_deadline(&retained[0], law != 0),
		      "continuous INFO1 Tone A recovery failed law=%u", law);
		CHECK(info1_late_reply(&retained[0], &retained[1], law != 0),
		      "late INFO1a recovery failed law=%u", law);
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
	if (target_stage == V34_CALL_PHASE4 &&
	    !endpoint->call.state.phase4.transmit_data_active &&
	    !endpoint->call.state.phase4.receive_data_active) {
		bool responded = false;

		/* Exercise the real initial receive path, including its S/TRN/MP
		 * owner, rather than installing an already qualified detector.
		 * Both initial training and a restarted exchange must respond. */
		for (unsigned sample = 0; sample < 70u * 8u; ++sample)
			CHECK(endpoint_sample(endpoint, 0, supply_mapping_frame,
				      &transmitted, &observation) &&
			      observation.next == V34_CALL_PHASE4,
			      "startup retrain consumed quiet as a request");
		for (unsigned sample = 0; sample < 120u * 8u; ++sample) {
			int16_t remote_sample;

			CHECK(v34_phase2_tone_transmitter_sample(&remote_tone,
				      &remote_sample) == V34_PHASE2_TONE_OK &&
			      endpoint_sample(endpoint,
				      codec_round_trip(remote_sample, pcma),
				      supply_mapping_frame, &transmitted, &observation),
			      "startup retrain tone reception failed");
			if (observation.next == V34_CALL_RETRAIN_SILENCE) {
				CHECK(sample + 1u >= V34_RETRAIN_TONE_SAMPLES &&
				      endpoint->call.retrain_active,
				      "startup retrain bypassed the continuous tone guard");
				responded = true;
				break;
			}
			CHECK(observation.next == V34_CALL_PHASE4,
			      "startup tone left Phase 4 without a retrain response");
		}
		CHECK(responded && complete_remote_retrain_silence(
			      endpoint, &remote_tone, pcma),
		      "startup retrain request was ignored or lost its peer tone");
		return true;
	}
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
			       (target_stage == V34_CALL_PHASE4 && transmit_b1_complete ?
					memcmp(&endpoint->call.retrain.detector.tone,
					       &before.tone, sizeof(before.tone)) == 0 &&
					memcmp(&endpoint->call.retrain.detector.answer_guard,
					       &before.answer_guard, sizeof(before.answer_guard)) == 0 :
					memcmp(&endpoint->call.retrain.detector, &before,
					       sizeof(before)) == 0)),
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
			      "initial Phase 4 retrain response failed role=%d law=%u",
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
			      "restarted Phase 4 retrain response failed role=%d law=%u",
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
		role, V34_SYMBOL_RATE_3429, 10u);

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
		.first_record_elapsed_samples = 1234,
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
	      call.peer_j_remaining_samples == 2800u * 8u + 2u * handoff.round_trip_delay_samples - 1234u &&
	      call.round_trip_delay_samples ==
		      handoff.round_trip_delay_samples &&
	      memcmp(&call.local_info0, &handoff.local_info0,
		     sizeof(call.local_info0)) == 0 &&
	      memcmp(&call.peer_info0, &handoff.peer_info0,
		     sizeof(call.peer_info0)) == 0,
	      "completed Phase 2 did not initialize the V.34 Phase 3 owner");
	/* Foreign Phase 2 owners may hand over an almost or already expired
	 * epoch. Neither path may grant another whole Phase 3 wait. */
	const uint32_t budget = 2800u * 8u + 2u * handoff.round_trip_delay_samples;
	const uint32_t ages[] = { budget - 1u, budget, UINT32_MAX };
	for (size_t i = 0; i < ARRAY_LENGTH(ages); ++i) {
		struct v34_call_observation observed;
		struct v34_received_mapping_frame received;
		int16_t transmitted;

		handoff.first_record_elapsed_samples = ages[i];
		CHECK(v34_call_init_from_phase2(&call, &caller, &handoff) == V34_CALL_OK &&
		      call.peer_j_remaining_samples == 1u &&
		      v34_call_sample(&call, 0, NULL, NULL, &transmitted, &observed, &received) == V34_CALL_OK &&
		      call.stage == V34_CALL_RETRAIN_SILENCE && call.retrain_active && !call.failed,
		      "expired Phase 2 epoch renewed the initial J deadline");
	}

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

static bool test_missing_probe_recovery(void)
{
	for (unsigned int pcma = 0; pcma < 2u; ++pcma) {
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role) {
			struct endpoint caller = { .verify_mapping_frames = true };
			struct endpoint answerer = {
				.transmit_bit = 1, .verify_mapping_frames = true,
			};
			struct endpoint *waiting = role == V34_CALLING_STATION ? &caller : &answerer;
			struct channel channel = { .pcma = pcma != 0u };
			bool erased = false, restarted = false;
			struct v34_info0 peer = { 0 };
			uint16_t rtd = 0;
			struct v34_call_configuration caller_config =
				configuration(V34_CALLING_STATION);
			struct v34_call_configuration answerer_config =
				configuration(V34_ANSWERING_STATION);
			const struct v34_info1c *bounded = &caller.call.local_info1.calling;

			/* Caller proposals precede peer INFO0. Include excluded baud
			 * rates, a carrier requiring correction, and unsupported power
			 * and constellation requests; missing L1 must still retrain. */
			caller_config.local_info0.symbol_rate_2743_supported = true;
			caller_config.local_info0.symbol_rate_3429_supported = true;
			caller_config.local_info1.calling.common
				.minimum_peer_power_reduction_db = 1;
			caller_config.local_info1.calling.answer_transmit[V34_SYMBOL_RATE_2743] =
				(struct v34_info1_probe_result){
					.high_carrier = true,
					.projected_data_rate_2400_bps_units = 4,
				};
			caller_config.local_info1.calling.answer_transmit[V34_SYMBOL_RATE_3429] =
				(struct v34_info1_probe_result){
					.projected_data_rate_2400_bps_units = 4,
				};
			caller_config.local_info1.calling.answer_transmit[V34_SYMBOL_RATE_3200] =
				(struct v34_info1_probe_result){
					.projected_data_rate_2400_bps_units = 13,
				};
			answerer_config.local_info0.constellation_1664_supported = false;
			CHECK(v34_call_init(&caller.call, &caller_config) == V34_CALL_OK &&
			      v34_call_init(&answerer.call, &answerer_config) == V34_CALL_OK,
			      "provisional caller INFO1 fixture did not initialize");
			caller.generated_mapping_frames = answerer.generated_mapping_frames = true;
			caller.mapping_pattern_seed = answerer.peer_mapping_pattern_seed = UINT32_C(0x34c011ab);
			answerer.mapping_pattern_seed = caller.peer_mapping_pattern_seed = UINT32_C(0x34a05e17);
			for (size_t clock = 0; clock < SAMPLE_LIMIT && !restarted; ++clock) {
				int16_t inputs[2], outputs[2];
				struct v34_call_observation observed[2];

				channel_receive(&channel, &inputs[0], &inputs[1]);
				if (waiting->call.stage == V34_CALL_PHASE2 &&
				    waiting->call.state.phase2.stage == (role == V34_CALLING_STATION ?
					    V34_PHASE2_INITIAL_PROBE : V34_PHASE2_RECIPROCAL_PROBE)) {
					if (!erased) {
						CHECK(v34_phase2_peer_info0(&waiting->call.state.phase2, &peer) == V34_PHASE2_OK &&
						      waiting->call.round_trip_delay_stored,
						      "missing-probe fixture lost its call agreement");
						rtd = waiting->call.round_trip_delay_samples;
					}
					inputs[role] = 0;
					erased = true;
				}
				note_peer_primary_delays(&caller, &answerer);
				CHECK(endpoint_sample(&caller, inputs[0], supply_mapping_frame, &outputs[0], &observed[0]) &&
				      endpoint_sample(&answerer, inputs[1], supply_mapping_frame, &outputs[1], &observed[1]),
				      "law %u role %u missing probe stopped the pair at %zu", pcma, (unsigned)role, clock);
				channel_transmit(&channel, outputs[0], outputs[1]);
				if (observed[role].processed == V34_CALL_PHASE2 &&
				    observed[role].events.phase2.kind == V34_PHASE2_EVENT_FAILED) {
					CHECK(erased && observed[role].events.phase2.failure == V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND &&
					      waiting->call.stage == V34_CALL_RETRAIN_SILENCE && waiting->call.retrain_active &&
					      waiting->call.peer_info0_stored &&
					      waiting->call.round_trip_delay_stored == (role == V34_ANSWERING_STATION) &&
					      waiting->call.round_trip_delay_samples == (role == V34_ANSWERING_STATION ? rtd : 0u) &&
					      v34_info0_equal(&waiting->call.peer_info0, &peer),
					      "missing-probe recovery retained the wrong delay or lost call agreement");
					CHECK(bounded->answer_transmit[V34_SYMBOL_RATE_2743]
						      .projected_data_rate_2400_bps_units == 0 &&
					      bounded->answer_transmit[V34_SYMBOL_RATE_3429]
						      .projected_data_rate_2400_bps_units == 0 &&
					      bounded->answer_transmit[V34_SYMBOL_RATE_3200].high_carrier &&
					      bounded->answer_transmit[V34_SYMBOL_RATE_3200]
						      .projected_data_rate_2400_bps_units == 12 &&
					      bounded->common.minimum_peer_power_reduction_db == 0,
					      "caller INFO1 exceeded peer capabilities");
					restarted = true;
				}
			}
			CHECK(restarted, "missing probe did not start recovery");
			for (unsigned silence = 0;
			     silence < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES &&
				     waiting->call.stage == V34_CALL_RETRAIN_SILENCE;
			     ++silence) {
				struct v34_call_observation caller_observed, answerer_observed;

				CHECK(channel_sample_pair(&channel, &caller, &answerer,
					      &caller_observed, &answerer_observed),
				      "missing-probe silence did not advance");
			}
			CHECK(waiting->call.stage == V34_CALL_PHASE2 &&
			      waiting->call.state.phase2.stage == V34_PHASE2_RANGING &&
			      waiting->call.state.phase2.prior_round_trip_delay_samples ==
				      (role == V34_ANSWERING_STATION ? rtd : 0u),
			      "missing-probe recovery passed the wrong prior delay");
			CHECK(run_initialized_pair(&caller, &answerer, &channel) &&
			      caller.mapping_frames_received >= 4u && answerer.mapping_frames_received >= 4u,
			      "law %u role %u missing probe did not recover verified data", pcma, (unsigned)role);
		}
	}
	return true;
}

static bool test_final_tone_timeout(void)
{
	const size_t delays[] = { LINE_DELAY_SAMPLES, 800u };

	for (unsigned pcma = 0; pcma < 2u; ++pcma) {
		for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index) {
			struct endpoint caller = { 0 }, answerer = { 0 };
			struct channel channel = { .delay_samples = delays[index], .pcma = pcma };
			struct v34_call_configuration configurations[] = {
				configuration(V34_CALLING_STATION), configuration(V34_ANSWERING_STATION),
			};
			bool started = false;

			CHECK(v34_call_init(&caller.call, &configurations[0]) == V34_CALL_OK &&
			      v34_call_init(&answerer.call, &configurations[1]) == V34_CALL_OK,
			      "final Tone-A deadline endpoints did not initialize");
			for (size_t sample = 0; sample < SAMPLE_LIMIT; ++sample) {
				struct v34_call_observation observations[2];

				CHECK(channel_sample_pair(&channel, &caller, &answerer,
					      &observations[0], &observations[1]),
				      "final Tone-A deadline setup failed at %zu", sample);
				if (caller.call.stage == V34_CALL_PHASE2 &&
				    caller.call.state.phase2.stage == V34_PHASE2_RECIPROCAL_PROBE) {
					started = true;
					break;
				}
			}
			CHECK(started && caller.call.round_trip_delay_stored &&
			      caller.call.state.phase2.state.reciprocal_probe.control.path.transmit.samples_emitted == 0,
			      "final Tone-A deadline missed the first reciprocal L1 sample");
			struct v34_info0 peer = caller.call.state.phase2.peer_info0;
			uint16_t rtd = caller.call.round_trip_delay_samples;
			uint32_t deadline = (160u + 650u) * 8u + rtd;
			struct v34_call_observation observation;
			struct v34_received_mapping_frame received;
			int16_t transmitted;

			/* Remove the peer naturally at the reciprocal probe boundary. */
			for (uint32_t elapsed = 1u; elapsed <= deadline; ++elapsed) {
				enum v34_call_result result = v34_call_sample(&caller.call, 0, NULL, NULL,
					&transmitted, &observation, &received);

				CHECK(result == V34_CALL_OK && !caller.call.failed && v34_call_is_valid(&caller.call) &&
				      observation.processed == V34_CALL_PHASE2 &&
				      observation.next == (elapsed == deadline ? V34_CALL_RETRAIN_SILENCE : V34_CALL_PHASE2),
				      "law %u delay %zu final Tone-A recovery at %u/%u returned %d",
				      pcma, delays[index], elapsed, deadline, result);
				if (elapsed > (160u + 550u) * 8u + rtd)
					CHECK(transmitted == 0, "reciprocal probe exceeded its wire limit");
			}
			CHECK(observation.events.phase2.kind == V34_PHASE2_EVENT_FAILED &&
			      observation.events.phase2.failure == V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT &&
			      caller.call.retrain_active && caller.call.peer_info0_stored &&
			      caller.call.round_trip_delay_samples == rtd &&
			      v34_info0_equal(&caller.call.peer_info0, &peer),
			      "final Tone-A recovery lost the failure observation or call agreement");
			for (unsigned sample = 0; sample < 70u * 8u; ++sample)
				CHECK(v34_call_sample(&caller.call, 0, NULL, NULL, &transmitted,
					      &observation, &received) == V34_CALL_OK && transmitted == 0 &&
				      observation.processed == V34_CALL_RETRAIN_SILENCE,
				      "final Tone-A recovery did not retain its complete silence interval");
			CHECK(caller.call.stage == V34_CALL_PHASE2 && caller.call.retrain_active &&
			      !caller.call.failed && caller.call.round_trip_delay_samples == rtd &&
			      v34_info0_equal(&caller.call.peer_info0, &peer),
			      "final Tone-A recovery did not restart Phase 2 with the call agreement");
		}
	}
	return true;
}


static bool peer_s_observed(
	const struct v34_call_observation *observation, enum v34_station_role role)
{
	if (role == V34_CALLING_STATION)
		return observation->processed == V34_CALL_PHASE4 &&
		       (observation->events.phase4 &
			V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED) != 0;
	return observation->processed == V34_CALL_PHASE3 &&
	       (observation->events.phase3 &
		V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED) != 0;
}

/* Inserting silence changes the receiver's fractional sampling phase. Find a
 * shared waveform position where S/S-bar is complete at the deadline but TRN
 * remains unconfirmed, both with and without the following training. This
 * constructs the boundary being tested instead of assuming the undelayed
 * qualification time survives the inserted silence. */
static bool confirmation_prefix(
	const struct endpoint *waiting, const int16_t *samples, size_t count,
	uint32_t deadline, size_t qualified_count, size_t peer_trn_count,
	uint32_t *prefix)
{
	CHECK(qualified_count < count && qualified_count <= deadline &&
	      peer_trn_count < count,
	      "invalid confirmation waveform bounds");
	for (unsigned offset = 0; offset < S_CONFIRMATION_SAMPLES; ++offset) {
		uint32_t candidate_prefix = deadline - (uint32_t)qualified_count + offset;
		bool supported = true;

		for (unsigned truncate = 0; truncate < 2u && supported; ++truncate) {
			struct endpoint candidate = *waiting;

			for (uint32_t sample = 0; sample < deadline; ++sample) {
				struct v34_call_observation observation;
				struct v34_received_mapping_frame received;
				int16_t transmitted;
				int16_t input = sample < candidate_prefix ? 0 :
					samples[sample - candidate_prefix];

				if (truncate && sample >= candidate_prefix + peer_trn_count +
				    SHORT_TRN_SAMPLES)
					input = 0;
				CHECK(v34_call_sample(&candidate.call, input, NULL, NULL,
					      &transmitted, &observation, &received) == V34_CALL_OK,
				      "confirmation boundary search failed");
				if (candidate.call.stage == V34_CALL_RETRAIN_SILENCE)
					break;
			}
			supported = candidate.call.stage == V34_CALL_PHASE4 &&
				candidate.call.peer_s_remaining_samples == S_CONFIRMATION_SAMPLES &&
				v34_phase3_s_hunt_is_complete(
					&candidate.call.state.phase4.receive.semantic.phase4.answer_s);
		}
		if (supported) {
			*prefix = candidate_prefix;
			return true;
		}
	}
	CHECK(false, "no complete S marker awaiting bounded TRN confirmation");
}

static bool check_late_s(
	enum v34_station_role role, bool at_deadline, bool truncate_trn)
{
	for (unsigned pcma = 0u; pcma < 2u; ++pcma) {
		struct endpoint caller = { 0 }, answerer = { 0 }, waiting = { 0 };
		struct endpoint *local = role == V34_CALLING_STATION ? &caller : &answerer;
		struct channel channel = { .pcma = pcma };
		int16_t samples[4000];
		size_t count = 0u;
		bool started = false, recorded = false, detected = false;
		bool pending_at_deadline = false, recovered = false;
		size_t qualified_count = 0u, peer_trn_count = 0u;

		CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
		      endpoint_init(&answerer, V34_ANSWERING_STATION),
		      "late S endpoints did not initialize");
		/* Retain the local station at its first J and generate the peer's
		 * real S/S-bar/TRN waveform over the ordinary companded channel.
		 * No prerecorded vector or synthetic slicer decisions are needed. */
		for (size_t sample = 0u; sample < SAMPLE_LIMIT; ++sample) {
			struct v34_call_observation observations[2];
			int16_t incoming, call_sample, answer_sample;

			channel_receive(&channel, &call_sample, &answer_sample);
			incoming = role == V34_CALLING_STATION ? call_sample : answer_sample;
			if (started) {
				CHECK(count < ARRAY_LENGTH(samples), "late S waveform overflow");
				samples[count++] = incoming;
			}
			CHECK(channel_sample_pair(&channel, &caller, &answerer,
				      &observations[0], &observations[1]),
			      "late S waveform generation failed");
			if (started && role == V34_CALLING_STATION &&
			    observations[V34_ANSWERING_STATION].processed == V34_CALL_PHASE4 &&
			    (observations[V34_ANSWERING_STATION].events.phase4 &
			     V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED) != 0)
				peer_trn_count = count;
			if (!started && observations[role].processed == V34_CALL_PHASE3 &&
			    (observations[role].events.phase3 &
			     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED) != 0) {
				waiting = *local;
				started = true;
			}
			if (started && peer_s_observed(&observations[role], role)) {
				qualified_count = count;
				recorded = true;
			}
			/* Retain the waveform beyond qualification: a delayed replay
			 * can acquire a slightly different sampling phase. */
			if (recorded && count >= qualified_count + 160u)
				break;
		}
		CHECK(recorded, "late S fixture did not qualify peer S");
		uint32_t nominal_deadline = 600u * 8u + waiting.call.round_trip_delay_samples;
		uint32_t deadline = 700u * 8u + waiting.call.round_trip_delay_samples;
		uint32_t prefix = 600u * 8u;

		if (at_deadline)
			CHECK(confirmation_prefix(&waiting, samples, count, deadline,
				      qualified_count, peer_trn_count, &prefix),
			      "could not construct the confirmation boundary for law %u", pcma);

		/* Delay the peer waveform into the compatibility grace period.
		 * Qualification must cancel the timer immediately, before the
		 * caller finishes transmitting J-prime and starts its own TRN. */
		for (size_t sample = 0u; sample < prefix + count; ++sample) {
			struct v34_call_observation observation;
			struct v34_received_mapping_frame received;
			int16_t transmitted;
			int16_t input = sample < prefix ? 0 : samples[sample - prefix];

			/* Cut input 5 ms after the peer starts TRN, before the
			 * sixteen-symbol confirmation can fill at 2400 baud.
			 * The boundary search verifies that the erasure retains S.
			 * Complete training must qualify; unsupported training must expire. */
			if (truncate_trn && sample >= prefix + peer_trn_count + SHORT_TRN_SAMPLES)
				input = 0;
			CHECK(v34_call_sample(&waiting.call,
				      input,
				      NULL, NULL, &transmitted, &observation, &received) == V34_CALL_OK,
			      "late S reception failed for law %u", pcma);
			if (at_deadline && sample + 1u == deadline) {
				CHECK(waiting.call.stage == V34_CALL_PHASE4 &&
				      waiting.call.peer_s_remaining_samples == 64u &&
				      v34_phase3_s_hunt_is_complete(
					      &waiting.call.state.phase4.receive.semantic.phase4.answer_s),
				      "complete S did not retain bounded confirmation for law %u", pcma);
				pending_at_deadline = true;
			}
			if (truncate_trn && waiting.call.stage == V34_CALL_RETRAIN_SILENCE) {
				CHECK(pending_at_deadline && sample + 1u == deadline + 64u &&
				      waiting.call.peer_s_remaining_samples == 0u,
				      "unconfirmed S did not expire at the bounded deadline");
				recovered = true;
				break;
			}
			CHECK(waiting.call.stage != V34_CALL_RETRAIN_SILENCE,
			      "late valid S was abandoned for law %u at %zu", pcma, sample);
			if (peer_s_observed(&observation, role)) {
				CHECK(!truncate_trn, "truncated TRN falsely qualified peer S");
				CHECK(sample + 1u > nominal_deadline &&
				      waiting.call.peer_s_remaining_samples == 0u &&
				      (role == V34_ANSWERING_STATION ||
				       !waiting.call.state.phase4.transmit_trn_started),
				      "late S did not cancel its deadline before local TRN");
				detected = true;
			}
		}
		CHECK(detected == !truncate_trn && recovered == truncate_trn &&
		      (!at_deadline || pending_at_deadline),
		      "late S completion/recovery was not verified for law %u", pcma);
	}
	return true;
}

static bool test_late_s(void)
{
	return check_late_s(V34_CALLING_STATION, false, false) &&
	       check_late_s(V34_ANSWERING_STATION, false, false);
}

static bool test_s_confirmation_deadline(void)
{
	return check_late_s(V34_CALLING_STATION, true, false) &&
	       check_late_s(V34_CALLING_STATION, true, true);
}

static bool test_initial_j_recovery(void)
{
	const size_t delays[] = { LINE_DELAY_SAMPLES, 800u };

	for (unsigned pcma = 0; pcma < 2; ++pcma) {
		for (size_t d = 0; d < ARRAY_LENGTH(delays); ++d) {
			for (unsigned erase = 0; erase < 2; ++erase) {
				struct endpoint caller = { 0 }, answerer = { 0 };
				struct channel channel = { .pcma = pcma, .delay_samples = delays[d] };
				size_t info1c_end = 0, peer_j = 0, received_j = 0, recovered = 0;
				size_t horizon = 0;
				struct v34_call before_j;

				CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
				      endpoint_init(&answerer, V34_ANSWERING_STATION),
				      "initial J endpoints did not initialize");
				for (size_t sample = 1; sample < SAMPLE_LIMIT; ++sample) {
					struct v34_call_observation observed[2];
					int16_t input[2], output[2];

					channel_receive(&channel, &input[0], &input[1]);
					/* Start erasing only after healthy S/PP/TRN have reached
					 * the caller. Keep J and all following input absent until
					 * recovery, including the answerer's later retrain tone. */
					if (erase && peer_j) input[0] = 0;
					if (!erase && peer_j) before_j = caller.call;
					note_peer_primary_delays(&caller, &answerer);
					CHECK(endpoint_sample(&caller, input[0], supply_mapping_frame,
						      &output[0], &observed[0]) &&
					      endpoint_sample(&answerer, input[1], supply_mapping_frame,
						      &output[1], &observed[1]),
					      "initial J sample failed at %zu law %u delay %zu", sample, pcma, delays[d]);
					channel_transmit(&channel, output[0], output[1]);
					if (observed[0].processed == V34_CALL_PHASE2 &&
					    observed[0].events.phase2.kind == V34_PHASE2_EVENT_LOCAL_INFO1C_SENT) {
						info1c_end = sample;
						horizon = sample + 2800u * 8u + 2u * caller.call.round_trip_delay_samples;
					}
					if (observed[0].processed == V34_CALL_PHASE2 &&
					    observed[0].next == V34_CALL_PHASE3)
						CHECK(info1c_end && caller.call.peer_j_remaining_samples == horizon - sample,
						      "J recovery started a fresh Phase 3 deadline");
					if (observed[1].processed == V34_CALL_PHASE3 &&
					    (observed[1].events.phase3 & V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED))
						peer_j = sample;
					if (observed[0].processed == V34_CALL_PHASE3 &&
					    (observed[0].events.phase3 & V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED)) {
						received_j = sample;
						/* A valid J on the final allowed sample wins over
						 * recovery. Replay exactly that qualifying input. */
						struct v34_call_observation last;
						struct v34_received_mapping_frame received;
						int16_t transmitted;

						CHECK(peer_j && !erase, "J erasure unexpectedly qualified");
						before_j.peer_j_remaining_samples = 1u;
						CHECK(v34_call_sample(&before_j, input[0], NULL, NULL,
							      &transmitted, &last, &received) == V34_CALL_OK &&
						      (last.events.phase3 & V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED) &&
						      before_j.stage != V34_CALL_RETRAIN_SILENCE &&
						      before_j.peer_j_remaining_samples == 0u,
						      "valid final-sample J lost to its recovery deadline");
						CHECK(caller.call.peer_j_remaining_samples == 0u,
						      "received J did not cancel its recovery deadline");
					}
					if (caller.call.stage == V34_CALL_RETRAIN_SILENCE) recovered = sample;
					if (recovered || (!erase && received_j) || (horizon && sample > horizon)) break;
				}
				CHECK(info1c_end && peer_j && horizon &&
				      (erase ? recovered == horizon && !received_j : received_j && received_j < horizon && !recovered),
				      "initial J recovery timing failed law %u delay %zu erased %u", pcma, delays[d], erase);
				if (erase) {
					CHECK(caller.call.retrain_active && !caller.call.failed &&
					      caller.call.peer_j_remaining_samples == 0u && caller.call.peer_info0_stored &&
					      caller.call.round_trip_delay_stored,
					      "initial J recovery lost the call agreement");
				}
				CHECK(run_initialized_pair(&caller, &answerer, &channel),
				      "initial J control/recovery did not reach verified duplex data");
			}
		}
	}
	return true;
}

static bool check_missing_s_recovery(enum v34_station_role role)
{
	const size_t delays[] = { LINE_DELAY_SAMPLES, 800u };

	for (unsigned pcma = 0; pcma < 2u; ++pcma) {
		for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index) {
			struct endpoint caller = { 0 }, answerer = { 0 };
			struct endpoint *local = role == V34_CALLING_STATION ? &caller : &answerer;
			struct channel channel = { .delay_samples = delays[index], .pcma = pcma };
			bool started = false;

			CHECK(endpoint_init(&caller, V34_CALLING_STATION) &&
			      endpoint_init(&answerer, V34_ANSWERING_STATION),
			      "S deadline endpoints did not initialize");
			for (size_t sample = 0; sample < SAMPLE_LIMIT; ++sample) {
				struct v34_call_observation observations[2];

				CHECK(channel_sample_pair(&channel, &caller, &answerer,
					      &observations[0], &observations[1]),
				      "S deadline setup failed at %zu", sample);
				if (observations[role].processed == V34_CALL_PHASE3 &&
				    (observations[role].events.phase3 &
				     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED) != 0) {
					started = true;
					break;
				}
			}
			CHECK(started && local->call.peer_info0_stored &&
			      local->call.round_trip_delay_stored,
			      "S deadline missed the local station's first J sample");
			struct v34_info0 peer = local->call.peer_info0;
			uint16_t rtd = local->call.round_trip_delay_samples;
			uint32_t deadline = 700u * 8u + rtd;
			struct v34_call_observation observation;
			struct v34_received_mapping_frame received;
			int16_t transmitted;

			/* The local station has sent J, but the peer's S/S-bar never
			 * arrives. The later E deadline cannot start in this state. */
			for (uint32_t elapsed = 1u; elapsed <= deadline; ++elapsed) {
				CHECK(v34_call_sample(&local->call, 0, NULL, NULL,
					      &transmitted, &observation, &received) == V34_CALL_OK,
				      "law %u delay %zu missing S stopped at %u/%u",
				      pcma, delays[index], elapsed, deadline);
				CHECK((local->call.stage == V34_CALL_RETRAIN_SILENCE) ==
					      (elapsed == deadline),
				      "law %u delay %zu missing S recovered at the wrong sample %u/%u",
				      pcma, delays[index], elapsed, deadline);
			}
			CHECK(local->call.retrain_active && !local->call.failed &&
			      local->call.peer_s_remaining_samples == 0u &&
			      local->call.phase4_e_remaining_samples == 0u &&
			      local->call.round_trip_delay_samples == rtd &&
			      v34_info0_equal(&local->call.peer_info0, &peer),
			      "missing S recovery lost the negotiated call agreement");
			for (unsigned sample = 0; sample < 70u * 8u; ++sample)
				CHECK(v34_call_sample(&local->call, 0, NULL, NULL, &transmitted,
					      &observation, &received) == V34_CALL_OK && transmitted == 0 &&
				      observation.processed == V34_CALL_RETRAIN_SILENCE,
				      "missing S recovery did not retain its full silence interval");
			CHECK(local->call.stage == V34_CALL_PHASE2 &&
			      local->call.retrain_active && !local->call.failed,
			      "missing S recovery did not resume Phase 2");
		}
	}
	return true;
}

static bool test_missing_s_recovery(void)
{
	return check_missing_s_recovery(V34_CALLING_STATION) &&
	       check_missing_s_recovery(V34_ANSWERING_STATION);
}

static bool test_initial_e_timeout(void)
{
	for (unsigned int role = 0; role < 2u; ++role) {
		for (unsigned int variant = 0; variant < 3u; ++variant) {
			struct endpoint caller = { 0 }, answerer = { 0 };
			struct endpoint *waiting = role == V34_CALLING_STATION ?
				&caller : &answerer;
			struct channel channel = { 0 };
			struct v34_call_configuration configurations[] = {
				configuration(V34_CALLING_STATION),
				configuration(V34_ANSWERING_STATION),
			};
			bool armed = false;
			bool peer_cme = variant == 1u;

			configurations[role].local_info0.cme_modem = variant == 2u;
			configurations[1u - role].local_info0.cme_modem = peer_cme;
			CHECK(v34_call_init(&caller.call, &configurations[0]) == V34_CALL_OK &&
			      v34_call_init(&answerer.call, &configurations[1]) == V34_CALL_OK,
			      "initial E deadline endpoints did not initialize");
			for (size_t sample = 0; sample < SAMPLE_LIMIT; ++sample) {
				struct v34_call_observation observations[2];

				CHECK(channel_sample_pair(&channel, &caller, &answerer,
					      &observations[0], &observations[1]),
				      "initial E deadline fixture failed at %zu", sample);
				if (observations[role].processed == V34_CALL_PHASE4 &&
				    (observations[role].events.phase4 &
				     V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED) != 0) {
					armed = true;
					break;
				}
			}
			uint32_t timeout = peer_cme ? 240000u : 20000u +
				(role == V34_CALLING_STATION ? 2u : 3u) *
					waiting->call.round_trip_delay_samples;

			CHECK(armed && waiting->call.phase4_e_remaining_samples == timeout,
			      "role %u variant %u armed the wrong initial E deadline", role, variant);
			/* Stop the peer before MP/E. The same silence must not reset
			 * the deadline, and our own CME flag must not extend it. */
			for (uint32_t sample = 1; sample <= timeout; ++sample) {
				struct v34_call_observation observation;
				struct v34_received_mapping_frame received;
				int16_t transmitted;

				CHECK(v34_call_sample(&waiting->call, 0, NULL, NULL,
					      &transmitted, &observation, &received) == V34_CALL_OK,
				      "initial E silence failed: role=%u variant=%u sample=%u",
				      role, variant, sample);
				CHECK((waiting->call.stage == V34_CALL_RETRAIN_SILENCE) ==
					      (sample == timeout),
				      "initial E retrain occurred at the wrong sample: %u/%u",
				      sample, timeout);
			}
			CHECK(waiting->call.phase4_e_remaining_samples == 0u &&
			      waiting->call.retrain_active,
			      "initial E expiry did not clear the timer and start retrain");
		}
	}
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
			if (armed_at == UINT32_MAX && responder->call.phase4_e_remaining_samples) {
				armed_at = sample;
				CHECK(responder->call.phase4_e_remaining_samples == timeout,
				      "rate renegotiation armed the wrong timeout");
			}
			if (responder->call.stage == V34_CALL_RETRAIN_SILENCE) {
				CHECK(armed_at != UINT32_MAX && sample - armed_at == timeout &&
				      responder->call.phase4_e_remaining_samples == 0u,
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
		{ "final Tone-A timeout retrain", test_final_tone_timeout },
		{ "late S qualification", test_late_s },
		{ "bounded S confirmation", test_s_confirmation_deadline },
		{ "initial J recovery", test_initial_j_recovery },
		{ "missing S recovery", test_missing_s_recovery },
		{ "bounded initial E exchange", test_initial_e_timeout },
		{ "bounded online rate renegotiation", test_rate_renegotiation_timeout },
		{ "reciprocal zero-rate cleardown", test_cleardown },
		{ "cleardown waits for peer MP-prime", test_cleardown_waits_for_prime },
		{ "completed Phase 2 handoff", test_phase2_handoff },
		{ "generated PCMU call", test_pcmu_call },
		{ "delayed PCMU and PCMA calls", test_delayed_calls },
		{ "generated station echo", test_echo_calls },
		{ "delayed station echo", test_delayed_echo_calls },
		{ "weak echo delay step", test_weak_echo_delay_step },
		{ "fast echo delay step", test_fast_echo_delay_step },
		{ "quiet training completion", test_quiet_training_completion },
		{ "symmetric selected-rate calls", test_symmetric_calls },
		{ "local and remote full retrain", test_full_retrain },
		{ "startup retrain and B1 validation", test_startup_retrain },
		{ "full retrain through answerer echo", test_echo_retrain },
		{ "retrain selects new INFO1a rate pair",
		  test_retrain_selects_new_rate },
		{ "unanswered probe recovery", test_probe_recovery },
		{ "missing probe recovery", test_missing_probe_recovery },
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
