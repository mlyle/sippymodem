// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_call.h>

#include <g711.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	CALL_SAMPLE_LIMIT = 120000,
	MAX_LINE_DELAY_CHARACTERS = 13,
	PHASE2_NOMINAL_AMPLITUDE = 12000,
	PHASE3_NOMINAL_AMPLITUDE = 6000,
	V92_SU_MINIMUM_MAGNITUDE = 256,
	DATA_FRAMES_TO_RECEIVE = 2,
	DATA_DOWNSTREAM_RATE = 28000,
};

struct channel {
	uint8_t analog_to_digital[MAX_LINE_DELAY_CHARACTERS];
	uint8_t digital_to_analog[MAX_LINE_DELAY_CHARACTERS];
	size_t delay;
	size_t cursor;
};

struct downstream_source {
	size_t frames;
	bool reject_next;
};

struct upstream_source {
	size_t frames;
	bool reject_next;
};

struct endpoint {
	struct pcm_call call;
	struct downstream_source downstream_source;
	struct upstream_source upstream_source;
	size_t mixed_octets;
	size_t phase4_octets;
	size_t downstream_frames;
	size_t upstream_frames;
	uint16_t phase4_entry_ri_symbols;
	bool entered_phase3;
	bool entered_mixed;
	bool entered_phase4;
	bool phase4_boundary_checked;
	bool data_path_started;
	bool idle_source_checked;
	bool source_failure_checked;
};

static uint8_t silence_octet(enum g711_law law)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(0) : g711_encode_pcmu(0);
}

static struct v90_info0 analog_info0(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
	};
}

static struct v90_info0d digital_info0(enum g711_law law)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.low_carrier_3000_supported = true,
			.high_carrier_3000_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = law == G711_LAW_PCMA ?
				   V90_PCM_A_LAW : V90_PCM_MU_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1 = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = -23,
		},
	};

	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate)
		info1.answer_transmit[rate] =
			(struct v34_info1_probe_result){
				.high_carrier = (rate & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(rate + 2u),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(rate + 6u),
			};
	info1.answer_transmit[V34_SYMBOL_RATE_3000].high_carrier = true;
	return info1;
}

static struct v90_info1a analog_info1(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 5,
		.training_ucode = 91,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3000,
		.frequency_offset_1_50_hz_units = 17,
	};
}

static struct v90_phase3_analog_rx_configuration receive_configuration(void)
{
	return (struct v90_phase3_analog_rx_configuration){
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters training_parameters(enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = -16,
		.shaping_a2_q1_6 = 8,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = 13,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_cp_parameters data_parameters(enum g711_law law)
{
	struct v90_cp_parameters parameters = training_parameters(law);

	parameters.kind = V90_CP_DATA;
	parameters.selected_rate_index = 1;
	return parameters;
}

static struct v90_mp_record initial_mp(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.upstream_rate_capability_mask = 0x1555u,
		},
	};
}

static struct v92_pcm_info1a v92_analog_info1(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 192,
		.maximum_section_coefficients = 128,
		.training_ucode = 91,
	};
}

static struct v92_info1d v92_digital_info1(void)
{
	struct v92_info1d info1 = {
		.v34 = digital_info1(),
		.pcm_upstream_supported = true,
	};

	info1.v34.answer_transmit[
		V34_SYMBOL_RATE_3429].high_carrier = false;
	return info1;
}

static bool v92_digital_info1_equal(
	const struct v92_info1d *left, const struct v92_info1d *right)
{
	uint8_t left_payload[V92_INFO1D_PAYLOAD_BYTES];
	uint8_t right_payload[V92_INFO1D_PAYLOAD_BYTES];

	return v92_info1d_encode(
		       left, left_payload, sizeof(left_payload)) == V92_INFO1D_OK &&
	       v92_info1d_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       V92_INFO1D_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static struct v92_cpt_parameters v92_cpt_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 24u);
	add_ucode(&parameters.transmit_constellations[0], 80u);
	return parameters;
}

static struct pcm_call_configuration v92_configuration(
	enum v90_modem_role role, enum g711_law law)
{
	const uint32_t rate_mask = UINT32_C(0x52491);
	struct pcm_call_configuration configuration = {
		.protocol = PCM_PROTOCOL_V92,
		.local_role = role,
		.law = law,
		.enabled_downstream_rate_mask = role == V90_ANALOG_MODEM ?
			UINT32_C(1) : V92_JD_RATE_CAPABILITY_MASK,
		.phase2_nominal_amplitude = PHASE2_NOMINAL_AMPLITUDE,
	};

	if (role == V90_ANALOG_MODEM) {
		configuration.local_info0.v92_analog = (struct v92_info0a){
			.v90 = analog_info0(),
			.v92_capable = true,
		};
		configuration.local_info1.v92_analog = v92_analog_info1();
		configuration.phase3.v92_analog =
			(struct v92_phase3_analog_station_configuration){
				.local_info1a = v92_analog_info1(),
				.cpt = v92_cpt_parameters(law),
				.upstream_rate_mask = rate_mask,
				.initial_trn1u_symbols =
					V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS,
				.law = law,
				.upstream_lu_ucode = 91u,
			};
	} else {
		configuration.local_info0.v92_digital = (struct v92_info0d){
			.v90 = digital_info0(law),
			.v92_capable = true,
		};
		configuration.local_info1.v92_digital = v92_digital_info1();
		configuration.phase3.v92_digital =
			(struct pcm_call_v92_digital_phase3_configuration){
				.jd = {
					.data_rate_capability_mask =
						V92_JD_RATE_CAPABILITY_MASK,
					.maximum_spectral_shaping_lookahead = 3,
				},
				.jp = {
					.training_constellation =
						V92_CONSTELLATION_8_POINT,
					.rate_renegotiation_constellation =
						V92_CONSTELLATION_4_POINT,
				},
				.equalizer_adaptation = 0.0,
				.su_minimum_magnitude =
					V92_SU_MINIMUM_MAGNITUDE,
			};
	}
	return configuration;
}

static struct pcm_call_configuration configuration(
	enum v90_modem_role role, enum g711_law law)
{
	struct pcm_call_configuration configuration = {
		.protocol = PCM_PROTOCOL_V90,
		.local_role = role,
		.law = law,
		.enabled_downstream_rate_mask = role == V90_ANALOG_MODEM ?
			UINT32_C(1) : V90_JD_RATE_CAPABILITY_MASK,
		.phase2_nominal_amplitude = PHASE2_NOMINAL_AMPLITUDE,
	};

	if (role == V90_ANALOG_MODEM) {
		configuration.local_info0.analog = analog_info0();
		configuration.local_info1.analog = analog_info1();
		configuration.phase3.analog.nominal_peak_amplitude =
			PHASE3_NOMINAL_AMPLITUDE;
		configuration.analog_phase4.training_parameters =
			training_parameters(law);
		configuration.analog_phase4.data_parameters =
			data_parameters(law);
	} else {
		configuration.local_info0.digital = digital_info0(law);
		configuration.local_info1.digital = digital_info1();
		configuration.phase3.digital.receive = receive_configuration();
		configuration.phase3.digital.jd = jd_parameters();
		configuration.digital_phase4.initial_mp = initial_mp();
	}
	return configuration;
}

static void generate_data_bits(
	uint8_t *bits, size_t bit_count, size_t frame)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u + (frame >> (bit % 3u))) & 1u);
}

static bool source_downstream_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	struct downstream_source *source = context;

	if (source == NULL || bits == NULL || bit_count == 0 ||
	    bit_count > V90_PCM_FRAME_MAX_BITS || source->reject_next)
		return false;
	generate_data_bits(bits, bit_count, source->frames + 1u);
	source->frames++;
	return true;
}

static bool source_upstream_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct upstream_source *source = context;

	if (source == NULL || request == NULL || primary_bits == NULL ||
	    request->primary_bits == 0 ||
	    request->primary_bits > V34_MAX_MAPPING_FRAME_BITS ||
	    source->reject_next)
		return false;
	generate_data_bits(
		primary_bits, request->primary_bits, source->frames + 97u);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = (uint8_t)(
			(source->frames + request->index) & 1u);
	source->frames++;
	return true;
}

static bool downstream_frame_matches(
	const struct v90_pcm_stream_frame *received, size_t frame_index)
{
	uint8_t expected[V90_PCM_FRAME_MAX_BITS];

	if (received->bit_count == 0 ||
	    received->bit_count > V90_PCM_FRAME_MAX_BITS)
		return false;
	generate_data_bits(expected, received->bit_count, frame_index + 1u);
	return memcmp(
		       received->bits, expected, received->bit_count) == 0;
}

static bool upstream_frame_matches(
	const struct v34_received_mapping_frame *received, size_t frame_index)
{
	uint8_t expected[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t expected_auxiliary = (uint8_t)(
		(frame_index + received->mapping_frame.index) & 1u);

	if (received->primary_bit_count == 0 ||
	    received->primary_bit_count !=
		    received->mapping_frame.primary_bits ||
	    received->auxiliary_bit_count !=
		    (uint8_t)received->mapping_frame.carries_auxiliary_bit)
		return false;
	generate_data_bits(
		expected, received->primary_bit_count, frame_index + 97u);
	return memcmp(
		       received->primary_bits, expected,
		       received->primary_bit_count) == 0 &&
	       (received->auxiliary_bit_count == 0 ||
		received->auxiliary_bit == expected_auxiliary);
}

static bool channel_init(
	struct channel *channel, enum g711_law law, size_t delay)
{
	uint8_t silence = silence_octet(law);

	if (channel == NULL || delay == 0 ||
	    delay > MAX_LINE_DELAY_CHARACTERS)
		return false;
	for (size_t index = 0; index < MAX_LINE_DELAY_CHARACTERS; ++index) {
		channel->analog_to_digital[index] = silence;
		channel->digital_to_analog[index] = silence;
	}
	channel->delay = delay;
	channel->cursor = 0;
	return true;
}

static void channel_receive(
	const struct channel *channel, uint8_t *analog, uint8_t *digital)
{
	*analog = channel->digital_to_analog[channel->cursor];
	*digital = channel->analog_to_digital[channel->cursor];
}

static void channel_transmit(
	struct channel *channel, uint8_t analog, uint8_t digital)
{
	channel->analog_to_digital[channel->cursor] = analog;
	channel->digital_to_analog[channel->cursor] = digital;
	channel->cursor = (channel->cursor + 1u) % channel->delay;
}

static bool endpoint_init(
	struct endpoint *endpoint, enum v90_modem_role role,
	enum g711_law law, uint32_t enabled_downstream_rate_mask,
	uint8_t selected_downstream_rate_index)
{
	struct pcm_call_configuration selected = configuration(role, law);
	struct endpoint initialized = { 0 };

	selected.enabled_downstream_rate_mask = enabled_downstream_rate_mask;
	if (role == V90_ANALOG_MODEM)
		selected.analog_phase4.data_parameters.selected_rate_index =
			selected_downstream_rate_index;
	else
		selected.phase3.digital.jd.data_rate_capability_mask =
			enabled_downstream_rate_mask;
	if (endpoint == NULL ||
	    pcm_call_init(&initialized.call, &selected) != PCM_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool phase4_boundary_is_transactional(
	struct endpoint *endpoint, enum g711_law law);

static bool source_direction_is_online(const struct endpoint *endpoint)
{
	return endpoint->call.local_role == V90_ANALOG_MODEM ?
		       v90_analog_path_upstream_is_online(
			       &endpoint->call.state.data_path.analog) :
		       v90_digital_path_downstream_is_online(
			       &endpoint->call.state.data_path.digital);
}

static bool source_is_rejecting(const struct endpoint *endpoint)
{
	return endpoint->call.local_role == V90_ANALOG_MODEM ?
		       endpoint->upstream_source.reject_next :
		       endpoint->downstream_source.reject_next;
}

static void set_source_rejection(struct endpoint *endpoint, bool reject)
{
	if (endpoint->call.local_role == V90_ANALOG_MODEM)
		endpoint->upstream_source.reject_next = reject;
	else
		endpoint->downstream_source.reject_next = reject;
}

static bool endpoint_data_octet(
	struct endpoint *endpoint, uint8_t received, uint8_t *transmitted)
{
	struct pcm_call_data_sources sources = {
		.upstream = source_upstream_frame,
		.upstream_context = &endpoint->upstream_source,
		.downstream = source_downstream_frame,
		.downstream_context = &endpoint->downstream_source,
	};
	struct pcm_call_data_event event;
	struct pcm_call_data_event event_before;
	struct pcm_call call_before;
	uint8_t generated = 0xa5;
	bool expecting_source_failure;
	const struct pcm_call_data_sources *selected_sources;
	enum pcm_call_result result;

	if (!endpoint->source_failure_checked &&
	    !source_is_rejecting(endpoint) &&
	    source_direction_is_online(endpoint))
		set_source_rejection(endpoint, true);
	expecting_source_failure = source_is_rejecting(endpoint);
	call_before = endpoint->call;
	memset(&event, 0x5a, sizeof(event));
	event_before = event;
	selected_sources = endpoint->idle_source_checked ? &sources : NULL;
	result = pcm_call_data_octet(
		&endpoint->call, received, selected_sources, &generated, &event);
	if (result == PCM_CALL_SOURCE_FAILURE) {
		if (!expecting_source_failure || generated != 0xa5 ||
		    memcmp(&event, &event_before, sizeof(event)) != 0 ||
		    memcmp(&endpoint->call, &call_before,
			   sizeof(call_before)) != 0)
			return false;
		set_source_rejection(endpoint, false);
		endpoint->source_failure_checked = true;
		generated = 0xa5;
		memset(&event, 0x5a, sizeof(event));
		result = pcm_call_data_octet(
			&endpoint->call, received, &sources,
			&generated, &event);
	}
	if (result != PCM_CALL_OK)
		return false;
	endpoint->idle_source_checked = true;
	if (event.kind == PCM_CALL_DOWNSTREAM_FRAME) {
		if (endpoint->call.local_role != V90_ANALOG_MODEM ||
		    !downstream_frame_matches(
			    &event.frame.downstream,
			    endpoint->downstream_frames))
			return false;
		endpoint->downstream_frames++;
	} else if (event.kind == PCM_CALL_UPSTREAM_FRAME) {
		if (endpoint->call.local_role != V90_DIGITAL_MODEM ||
		    !upstream_frame_matches(
			    &event.frame.upstream,
			    endpoint->upstream_frames))
			return false;
		endpoint->upstream_frames++;
	} else if (event.kind != PCM_CALL_DATA_NONE) {
		return false;
	}
	*transmitted = generated;
	return true;
}

static bool endpoint_octet(
	struct endpoint *endpoint, uint8_t received,
	enum g711_law law, uint8_t *transmitted)
{
	struct pcm_call_observation observation;

	if (pcm_call_phase4_is_ready(&endpoint->call)) {
		if (!phase4_boundary_is_transactional(endpoint, law) ||
		    pcm_call_start_data_path(&endpoint->call) != PCM_CALL_OK)
			return false;
		endpoint->phase4_boundary_checked = true;
		endpoint->data_path_started = true;
	}
	if (pcm_call_current_stage(&endpoint->call) == PCM_CALL_DATA_PATH)
		return endpoint_data_octet(endpoint, received, transmitted);
	if (pcm_call_octet(
		    &endpoint->call, received, transmitted,
		    &observation) != PCM_CALL_OK)
		return false;
	endpoint->mixed_octets +=
		observation.processed == PCM_CALL_PHASE3_MIXED;
	endpoint->phase4_octets +=
		observation.processed == PCM_CALL_PHASE4;
	endpoint->entered_phase3 |= observation.next == PCM_CALL_PHASE3;
	endpoint->entered_mixed |=
		observation.next == PCM_CALL_PHASE3_MIXED;
	endpoint->entered_phase4 |= observation.next == PCM_CALL_PHASE4;
	if (observation.processed != PCM_CALL_PHASE4 &&
	    observation.next == PCM_CALL_PHASE4) {
		endpoint->phase4_entry_ri_symbols =
			endpoint->call.local_role == V90_ANALOG_MODEM ?
				endpoint->call.state.phase4.analog.state.initial
					.receive
					.ri_symbols :
				endpoint->call.state.phase4.digital.state.initial
					.transmit
					.ri_symbols;
	}
	return true;
}

static bool ri_progress_is_exact(const struct endpoint *endpoint)
{
	size_t expected = endpoint->mixed_octets;

	if (expected > V90_PHASE4_RI_MIN_SYMBOLS)
		expected = V90_PHASE4_RI_MIN_SYMBOLS;
	return endpoint->phase4_entry_ri_symbols == expected;
}

static bool phase4_boundary_is_transactional(
	struct endpoint *endpoint, enum g711_law law)
{
	struct pcm_call before = endpoint->call;
	struct pcm_call_observation observation;
	struct pcm_call_observation observation_before;
	uint8_t output = 0xa5;

	memset(&observation, 0x5a, sizeof(observation));
	observation_before = observation;
	return pcm_call_octet(
		       &endpoint->call, silence_octet(law),
		       &output, &observation) == PCM_CALL_PHASE4_READY &&
	       output == 0xa5 &&
	       memcmp(&observation, &observation_before,
		      sizeof(observation)) == 0 &&
	       memcmp(&endpoint->call, &before, sizeof(before)) == 0;
}

enum call_goal {
	CALL_GOAL_PHASE4_ENTRY,
	CALL_GOAL_DATA_PATH,
	CALL_GOAL_ONLINE_DATA,
};

static bool goal_is_reached(
	enum call_goal goal, const struct endpoint *analog,
	const struct endpoint *digital)
{
	if (goal == CALL_GOAL_PHASE4_ENTRY)
		return analog->entered_phase4 && digital->entered_phase4;
	if (!analog->data_path_started || !digital->data_path_started)
		return false;
	return goal == CALL_GOAL_DATA_PATH ||
	       (pcm_call_is_online(&analog->call) &&
		pcm_call_is_online(&digital->call) &&
		analog->downstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		digital->upstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		analog->source_failure_checked &&
		digital->source_failure_checked);
}

static bool run_configured_call(
	enum g711_law law, size_t delay, enum call_goal goal,
	uint32_t analog_rate_mask, uint8_t analog_selected_rate_index,
	uint32_t digital_rate_mask, uint32_t expected_downstream_rate)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool milestone_reached = false;

	CHECK(channel_init(&channel, law, delay) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, analog_rate_mask,
		      analog_selected_rate_index) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law, digital_rate_mask, 0),
	      "law %u delay %zu V.90 endpoints did not initialize",
	      (unsigned int)law, delay);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t analog_transmitted;
		uint8_t digital_transmitted;

		channel_receive(
			&channel, &analog_received, &digital_received);
		CHECK(endpoint_octet(
			      &analog, analog_received, law,
			      &analog_transmitted) &&
		      endpoint_octet(
			      &digital, digital_received, law,
			      &digital_transmitted),
		      "law %u delay %zu V.90 call stopped at character %zu",
		      (unsigned int)law, delay, clock);
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		if (goal_is_reached(goal, &analog, &digital)) {
			milestone_reached = true;
			break;
		}
	}
	CHECK(milestone_reached &&
	      analog.entered_phase3 && digital.entered_phase3 &&
	      analog.entered_mixed && digital.entered_mixed &&
	      analog.entered_phase4 && digital.entered_phase4 &&
	      ri_progress_is_exact(&analog) &&
	      ri_progress_is_exact(&digital) &&
	      !pcm_call_has_failed(&analog.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u delay %zu V.90 call missed Phase 4: "
	      "reached=%d entered=%d/%d/%d/%d/%d/%d stages=%d/%d "
	      "Ri=%u/%zu,%u/%zu",
	      (unsigned int)law, delay, milestone_reached,
	      analog.entered_phase3, digital.entered_phase3,
	      analog.entered_mixed, digital.entered_mixed,
	      analog.entered_phase4, digital.entered_phase4,
	      analog.call.stage, digital.call.stage,
	      analog.phase4_entry_ri_symbols, analog.mixed_octets,
	      digital.phase4_entry_ri_symbols, digital.mixed_octets);
	if (goal == CALL_GOAL_PHASE4_ENTRY)
		CHECK(pcm_call_current_stage(&analog.call) == PCM_CALL_PHASE4 &&
		      pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE4 &&
		      v90_phase4_analog_startup_is_valid(
			      &analog.call.state.phase4.analog) &&
		      v90_phase4_digital_startup_is_valid(
			      &digital.call.state.phase4.digital),
		      "law %u delay %zu did not retain Phase 4 entry",
		      (unsigned int)law, delay);
	if (goal != CALL_GOAL_PHASE4_ENTRY)
		CHECK(analog.phase4_octets > 0 && digital.phase4_octets > 0 &&
		      analog.phase4_boundary_checked &&
		      digital.phase4_boundary_checked &&
		      analog.data_path_started && digital.data_path_started &&
		      analog.idle_source_checked && digital.idle_source_checked &&
		      pcm_call_current_stage(&analog.call) ==
			      PCM_CALL_DATA_PATH &&
		      pcm_call_current_stage(&digital.call) ==
			      PCM_CALL_DATA_PATH,
		      "law %u delay %zu did not preserve Phase 4 handoffs",
		      (unsigned int)law, delay);
	if (goal == CALL_GOAL_ONLINE_DATA) {
		uint32_t analog_transmit_rate = 0;
		uint32_t analog_receive_rate = 0;
		uint32_t digital_transmit_rate = 0;
		uint32_t digital_receive_rate = 0;
		uint32_t aliased_rate = UINT32_C(0xa5a55a5a);

		CHECK(pcm_call_is_online(&analog.call) &&
		      pcm_call_is_online(&digital.call) &&
		      analog.downstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		      digital.upstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		      analog.source_failure_checked &&
		      digital.source_failure_checked &&
		      digital.downstream_source.frames >=
			      analog.downstream_frames &&
		      analog.upstream_source.frames >= digital.upstream_frames,
		      "law %u V.90 data path stopped at frames %zu/%zu "
		      "sources %zu/%zu failures %d/%d",
		      (unsigned int)law, analog.downstream_frames,
		      digital.upstream_frames,
		      digital.downstream_source.frames,
		      analog.upstream_source.frames,
		      analog.source_failure_checked,
		      digital.source_failure_checked);
		CHECK(pcm_call_get_data_rates(
			      &analog.call, &analog_transmit_rate,
			      &analog_receive_rate) &&
		      pcm_call_get_data_rates(
			      &digital.call, &digital_transmit_rate,
			      &digital_receive_rate) &&
		      analog_transmit_rate == digital_receive_rate &&
		      analog_receive_rate == digital_transmit_rate &&
		      analog_receive_rate == expected_downstream_rate,
		      "law %u V.90 role-local rates did not agree: "
		      "%u/%u and %u/%u",
		      (unsigned int)law, analog_transmit_rate,
		      analog_receive_rate, digital_transmit_rate,
		      digital_receive_rate);
		CHECK(!pcm_call_get_data_rates(
			       &analog.call, &aliased_rate, &aliased_rate) &&
		      aliased_rate == UINT32_C(0xa5a55a5a),
		      "overlapping V.90 rate outputs were changed");
	}
	return true;
}

static bool run_call(
	enum g711_law law, size_t delay, enum call_goal goal)
{
	return run_configured_call(
		law, delay, goal, UINT32_C(1), 1,
		V90_JD_RATE_CAPABILITY_MASK, DATA_DOWNSTREAM_RATE);
}

static bool phase4_entry_boundary_is_transactional(
	struct pcm_call *call, enum g711_law law)
{
	struct pcm_call before = *call;
	struct pcm_call_observation observation;
	struct pcm_call_observation observation_before;
	uint8_t output = 0xa5;

	memset(&observation, 0x5a, sizeof(observation));
	observation_before = observation;
	return pcm_call_octet(
		       call, silence_octet(law), &output, &observation) ==
		       PCM_CALL_PHASE4_ENTRY_READY &&
	       output == 0xa5 &&
	       memcmp(&observation, &observation_before,
		      sizeof(observation)) == 0 &&
	       memcmp(call, &before, sizeof(before)) == 0;
}

static bool run_v92_to_phase4_entry(
	enum g711_law law, size_t delay)
{
	struct pcm_call_configuration analog_configuration =
		v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		v92_configuration(V90_DIGITAL_MODEM, law);
	struct channel channel;
	struct pcm_call analog;
	struct pcm_call digital;
	bool analog_entered_phase3 = false;
	bool digital_entered_phase3 = false;
	bool reached = false;

	CHECK(channel_init(&channel, law, delay) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "law %u delay %zu V.92 calls did not initialize",
	      (unsigned int)law, delay);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_call_observation analog_observation;
		struct pcm_call_observation digital_observation;
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t analog_transmitted;
		uint8_t digital_transmitted;
		enum pcm_call_result analog_result;
		enum pcm_call_result digital_result;

		channel_receive(
			&channel, &analog_received, &digital_received);
		analog_result = pcm_call_octet(
			&analog, analog_received, &analog_transmitted,
			&analog_observation);
		digital_result = pcm_call_octet(
			&digital, digital_received, &digital_transmitted,
			&digital_observation);
		CHECK(analog_result == PCM_CALL_OK &&
		      digital_result == PCM_CALL_OK,
		      "law %u delay %zu V.92 call stopped at character %zu: "
		      "%s/%s stages %u/%u",
		      (unsigned int)law, delay, clock,
		      pcm_call_result_name(analog_result),
		      pcm_call_result_name(digital_result),
		      (unsigned int)analog.stage,
		      (unsigned int)digital.stage);
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		analog_entered_phase3 |=
			analog_observation.next == PCM_CALL_PHASE3;
		digital_entered_phase3 |=
			digital_observation.next == PCM_CALL_PHASE3;
		if (pcm_call_phase4_entry_is_ready(&analog) ||
		    pcm_call_phase4_entry_is_ready(&digital)) {
			reached = true;
			break;
		}
	}
	CHECK(reached && analog_entered_phase3 && digital_entered_phase3 &&
	      pcm_call_is_valid(&analog) && pcm_call_is_valid(&digital) &&
	      !pcm_call_has_failed(&analog) && !pcm_call_has_failed(&digital) &&
	      analog.phase3_entry_stored && digital.phase3_entry_stored &&
	      analog.phase3_entry.selection.startup.protocol ==
		      PCM_PROTOCOL_V92 &&
	      digital.phase3_entry.selection.startup.protocol ==
		      PCM_PROTOCOL_V92 &&
	      analog.phase3_entry.selection.upstream == PCM_UPSTREAM_PCM &&
	      digital.phase3_entry.selection.upstream == PCM_UPSTREAM_PCM &&
	      v92_digital_info1_equal(
		      &analog.phase3_entry.selection.digital.record.v92,
		      &digital_configuration.local_info1.v92_digital) &&
	      v92_digital_info1_equal(
		      &digital.phase3_entry.selection.digital.record.v92,
		      &digital_configuration.local_info1.v92_digital) &&
	      v92_pcm_info1a_equal(
		      &analog.phase3_entry.selection.analog.pcm,
		      &analog_configuration.local_info1.v92_analog) &&
	      v92_pcm_info1a_equal(
		      &digital.phase3_entry.selection.analog.pcm,
		      &analog_configuration.local_info1.v92_analog),
	      "law %u delay %zu V.92 call lost its Phase 3 selection",
	      (unsigned int)law, delay);
	if (pcm_call_phase4_entry_is_ready(&analog))
		CHECK(phase4_entry_boundary_is_transactional(&analog, law),
		      "law %u analogue Phase 4 boundary consumed input",
		      (unsigned int)law);
	if (pcm_call_phase4_entry_is_ready(&digital))
		CHECK(phase4_entry_boundary_is_transactional(&digital, law),
		      "law %u digital Phase 4 boundary consumed input",
		      (unsigned int)law);
	CHECK(!pcm_call_phase4_is_ready(&analog) &&
	      !pcm_call_phase4_is_ready(&digital) &&
	      !pcm_call_is_online(&analog) && !pcm_call_is_online(&digital) &&
	      pcm_call_start_data_path(&analog) == PCM_CALL_INVALID_STATE &&
	      pcm_call_start_data_path(&digital) == PCM_CALL_INVALID_STATE,
	      "law %u V.92 Phase 3 was misreported as online",
	      (unsigned int)law);
	return true;
}

static bool test_v92_phase4_entry(void)
{
	for (size_t delay = 1; delay <= 3; ++delay)
		CHECK(run_v92_to_phase4_entry(G711_LAW_PCMU, delay) &&
		      run_v92_to_phase4_entry(G711_LAW_PCMA, delay),
		      "delay %zu V.92 Phase 4 entry failed", delay);
	return true;
}

static bool test_downstream_rate_negotiation(void)
{
	CHECK(run_configured_call(
		      G711_LAW_PCMU, 1, CALL_GOAL_ONLINE_DATA,
		      UINT32_C(3), 2, UINT32_C(1), DATA_DOWNSTREAM_RATE),
	      "call did not select the highest mutually enabled rate");
	return true;
}

static bool test_generated_calls(void)
{
	static const size_t complete_delays[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(complete_delays); ++index)
		CHECK(run_call(
			      G711_LAW_PCMU, complete_delays[index],
			      CALL_GOAL_DATA_PATH) &&
		      run_call(
			      G711_LAW_PCMA, complete_delays[index],
			      CALL_GOAL_DATA_PATH),
		      "delay %zu generated V.90 handoffs failed",
		      complete_delays[index]);
	CHECK(run_call(G711_LAW_PCMU, 1, CALL_GOAL_ONLINE_DATA) &&
	      run_call(G711_LAW_PCMA, 1, CALL_GOAL_ONLINE_DATA),
	      "generated V.90 online data calls failed");
	/* Retain the longer-delay pre-Phase-4 handoff regression separately
	 * from the carrier-phase combinations covered through CPt/Ri above. */
	CHECK(run_call(G711_LAW_PCMU, 13, CALL_GOAL_PHASE4_ENTRY) &&
	      run_call(G711_LAW_PCMA, 13, CALL_GOAL_PHASE4_ENTRY),
	      "generated V.90 calls failed");
	return true;
}

static bool test_validation(void)
{
	struct pcm_call_configuration selected =
		configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	struct pcm_call call;
	struct pcm_call saved;
	struct pcm_call_observation observation;
	struct pcm_call_observation saved_observation;
	struct pcm_call_data_event data_event;
	struct pcm_call_data_event saved_data_event;
	uint32_t transmit_rate = UINT32_C(0xa5a55a5a);
	uint32_t receive_rate = UINT32_C(0x5a5aa5a5);
	uint8_t output = 0xa5;

	memset(&call, 0xa5, sizeof(call));
	saved = call;
	CHECK(pcm_call_init(NULL, &selected) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_init(&call, NULL) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a null initializer argument");
	selected.local_role = (enum v90_modem_role)99;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an invalid role");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.enabled_downstream_rate_mask = 0;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an empty downstream rate set");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.enabled_downstream_rate_mask = UINT32_C(2);
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "analogue call accepted a disabled selected rate");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.enabled_downstream_rate_mask |= UINT32_C(1) << 21u;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "analogue call enabled a rate its CP cannot represent");
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.enabled_downstream_rate_mask = UINT32_C(1);
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "digital call advertised rates outside its enabled set");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.phase3.analog.nominal_peak_amplitude = 0;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a zero Phase 3 amplitude");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.analog_phase4.training_parameters.request_silence = true;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a non-initial Phase 4 training record");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.analog_phase4.data_parameters.kind = V90_CP_TRAINING;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted non-data online CP parameters");
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.digital_phase4.initial_mp.parameters.type0.acknowledges_cp =
		true;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an acknowledging initial MP");
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.law = G711_LAW_PCMA;
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid configurations changed the call");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_OK &&
	      pcm_call_is_valid(&call) &&
	      !pcm_call_has_failed(&call) &&
	      !pcm_call_phase4_entry_is_ready(&call) &&
	      !pcm_call_phase4_is_ready(&call) &&
	      !pcm_call_is_online(&call),
	      "valid call did not initialize");
	saved = call;
	memset(&observation, 0x5a, sizeof(observation));
	saved_observation = observation;
	memset(&data_event, 0x3c, sizeof(data_event));
	saved_data_event = data_event;
	CHECK(pcm_call_octet(
		      NULL, silence_octet(G711_LAW_PCMU),
		      &output, &observation) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      NULL, &observation) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      &output, NULL) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      (uint8_t *)&call, &observation) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      &output, (struct pcm_call_observation *)&call) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_start_data_path(NULL) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_start_data_path(&call) == PCM_CALL_INVALID_STATE &&
	      !pcm_call_get_data_rates(
		      NULL, &transmit_rate, &receive_rate) &&
	      !pcm_call_get_data_rates(
		      &call, &transmit_rate, &receive_rate) &&
	      transmit_rate == UINT32_C(0xa5a55a5a) &&
	      receive_rate == UINT32_C(0x5a5aa5a5) &&
	      pcm_call_data_octet(
		      &call, silence_octet(G711_LAW_PCMU), NULL,
		      &output, &data_event) == PCM_CALL_INVALID_STATE &&
	      output == 0xa5 &&
	      memcmp(&observation, &saved_observation,
		     sizeof(observation)) == 0 &&
	      memcmp(&data_event, &saved_data_event,
		     sizeof(data_event)) == 0 &&
	      memcmp(&call, &saved, sizeof(call)) == 0 &&
	      !pcm_call_is_valid(NULL) && !pcm_call_has_failed(NULL) &&
	      !pcm_call_phase4_entry_is_ready(NULL) &&
	      !pcm_call_phase4_is_ready(NULL) &&
	      !pcm_call_is_online(NULL) &&
	      pcm_call_current_stage(NULL) == (enum pcm_call_stage)-1,
	      "invalid call operation changed state or output");
	return true;
}

static bool test_v92_validation(void)
{
	struct pcm_call_configuration selected =
		v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	struct pcm_call analog;
	struct pcm_call digital;
	struct pcm_call untouched;
	struct pcm_call before;

	memset(&untouched, 0xa5, sizeof(untouched));
	before = untouched;
	CHECK(pcm_call_init(&analog, &selected) == PCM_CALL_OK,
	      "valid analogue V.92 configuration was rejected");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	CHECK(pcm_call_init(&digital, &selected) == PCM_CALL_OK,
	      "valid digital V.92 configuration was rejected");
	selected.protocol = (enum pcm_protocol)99;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an invalid PCM protocol");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.local_info1.v92_analog.maximum_total_coefficients--;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted invalid V.92 INFO1a state");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.local_info1.v92_analog.training_ucode++;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted mismatched V.92 INFO1a state");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.phase3.v92_analog.local_info1a
		.local_md_length_276_symbol_units = 1u;
	selected.local_info1.v92_analog =
		selected.phase3.v92_analog.local_info1a;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an unowned manufacturer interval");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.local_info0.v92_analog.short_phase2_requested = true;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted unsupported short Phase 2");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.local_info1.v92_digital.pcm_upstream_supported = false;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a V.92 configuration without PCM upstream");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.phase3.v92_digital.jd.data_rate_capability_mask >>= 1u;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted mismatched V.92 downstream rates");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.phase3.v92_digital.equalizer_adaptation = NAN;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&untouched, &before, sizeof(untouched)) == 0,
	      "invalid V.92 configuration changed the call");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"phase 4 entry ready",
		"phase 4 ready",
		"frame source failure",
		"failed",
		"invalid argument",
		"invalid state",
		"invalid selection",
		"unusable selection",
		"media mismatch",
		"out of range",
		"clipping risk",
		"sequence mismatch",
		"numeric failure",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index)
		CHECK(strcmp(
			      pcm_call_result_name(
				      (enum pcm_call_result)index),
			      expected[index]) == 0,
		      "call result name %zu changed", index);
	CHECK(strcmp(
		      pcm_call_result_name((enum pcm_call_result)99),
		      "unknown") == 0,
	      "unknown call result name changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated PCMU and PCMA calls through online data",
		  test_generated_calls },
		{ "downstream rate negotiation",
		  test_downstream_rate_negotiation },
		{ "V.92 Phase 4 entry", test_v92_phase4_entry },
		{ "validation and transactionality", test_validation },
		{ "V.92 validation", test_v92_validation },
		{ "result names", test_result_names },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
