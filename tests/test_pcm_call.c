// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_call.h>

#include <g711.h>
#include <v34_constellation.h>
#include <v34_phase2_selection.h>
#include <v34_phase3_s_receiver.h>
#include <v90_downstream_data_handoff.h>
#include <v90_upstream_data.h>
#include <v92_cpd_direct.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	CALL_SAMPLE_LIMIT = 120000,
	V90_B1_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 15,
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
	uint16_t upstream_received_delay;
	uint16_t phase4_entry_ri_symbols;
	uint32_t phase4_entry_ri_wait_samples;
	uint32_t info1_b1_deadline_samples;
	bool entered_phase3;
	bool entered_mixed;
	bool entered_phase4;
	bool measured_info1d_checked;
	bool info1_b1_deadline_seen;
	bool phase4_boundary_checked;
	bool data_path_started;
	bool idle_source_checked;
	bool source_failure_checked;
	bool resync_data_frame;
	size_t resume_frame_index;
};

static uint8_t silence_octet(enum g711_law law)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(0) : g711_encode_pcmu(0);
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static uint8_t encode_octet(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
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

static bool v90_info1d_equal(
	const struct v34_info1c *left, const struct v34_info1c *right)
{
	uint8_t left_payload[V34_INFO1C_PAYLOAD_BYTES];
	uint8_t right_payload[V34_INFO1C_PAYLOAD_BYTES];

	return v34_info1c_encode(
		       left, left_payload, sizeof(left_payload)) == V34_INFO1_OK &&
	       v34_info1c_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       V34_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static struct v90_info1a analog_info1(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 0,
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
		.maximum_spectral_shaping_lookahead = 1,
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

	for (uint8_t index = 0; index < 8u; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v92_cpu_parameters v92_cpu_parameters(enum g711_law law)
{
	struct v92_cpu_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x7654),
		.shaping_a1_q1_6 = 7,
		.shaping_a2_q1_6 = -9,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -5,
		.data_rate_number = 11,
		.sign_redundancy = 1,
		.spectral_lookahead_frames = 2,
		.constellation_count = V92_CPU_MAX_CONSTELLATIONS,
		.codec_constellations_differ = true,
	};

	for (uint8_t index = 0u;
	     index < V92_CPU_MAX_CONSTELLATIONS; ++index) {
		/* Four 16-point and two 32-point intervals provide exactly
		 * 26 modulus bits for rate 11 with one redundant sign. */
		uint8_t point_count = index < 4u ? 16u : 32u;

		for (uint8_t point = 0u; point < point_count; ++point) {
			add_ucode(
				&parameters.transmit_constellations[index],
				(uint8_t)(4u * point + index % 4u));
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(4u * point + (index + 1u) % 4u));
		}
		parameters.frame_constellation[index] = index;
	}
	return parameters;
}

static struct v92_cpd_base_parameters v92_cpd_parameters(enum g711_law law)
{
	struct v92_cpd_parameters full;

	if (v92_cpd_direct_init(&full, law, 14u, true) != V92_CPD_OK)
		return (struct v92_cpd_base_parameters){ 0 };
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = full.four_times_gain_q0_16,
		.data_rate_number = full.data_rate_number,
		.trellis = full.trellis,
		.extend_e2u = full.extend_e2u,
	};
}

static struct pcm_call_configuration v92_configuration(
	enum v90_modem_role role, enum g711_law law)
{
	const uint32_t rate_mask = UINT32_C(0x52491);
	struct pcm_call_configuration configuration = {
		.protocol = PCM_PROTOCOL_V92,
		.local_role = role,
		.law = law,
		.enabled_downstream_rate_mask = V92_JD_RATE_CAPABILITY_MASK,
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
		configuration.v92_analog_phase4.suvu =
			(struct v92_suvu_parameters){
				.prefilter_level_db_q2_2 = -7,
				.wait_for_cpu = true,
			};
		configuration.v92_analog_phase4.cpu =
			v92_cpu_parameters(law);
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
						V92_CONSTELLATION_4_POINT,
					.rate_renegotiation_constellation =
						V92_CONSTELLATION_4_POINT,
				},
				.equalizer_adaptation = 0.0,
				.su_minimum_magnitude =
					V92_SU_MINIMUM_MAGNITUDE,
			};
		configuration.v92_digital_phase4.suvd =
			(struct v92_suvd_parameters){
				.request_silence = law == G711_LAW_PCMA,
			};
		configuration.v92_digital_phase4.cpd =
			v92_cpd_parameters(law);
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
	const struct v34_received_mapping_frame *received,
	uint16_t *received_delay, size_t frame_index)
{
	uint8_t expected[V34_MAX_MAPPING_FRAME_BITS];
	uint16_t delayed;
	uint8_t expected_auxiliary = (uint8_t)(
		(frame_index + received->mapping_frame.index) & 1u);

	if (received_delay == NULL || received->primary_bit_count == 0 ||
	    received->primary_bit_count !=
		    received->mapping_frame.primary_bits ||
	    received->auxiliary_bit_count !=
		    (uint8_t)received->mapping_frame.carries_auxiliary_bit)
		return false;
	generate_data_bits(
		expected, received->primary_bit_count, frame_index + 97u);
	delayed = *received_delay;
	for (size_t bit = 0; bit < received->primary_bit_count; ++bit) {
		uint8_t wire_bit = (uint8_t)(delayed & 1u);

		delayed = (uint16_t)((delayed >> 1) |
				     ((uint16_t)expected[bit] << 15));
		if (received->primary_bits[bit] != wire_bit)
			return false;
	}
	if (received->auxiliary_bit_count != 0 &&
	    received->auxiliary_bit != expected_auxiliary)
		return false;
	*received_delay = delayed;
	return true;
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

static bool endpoint_init_with_dil(
	struct endpoint *endpoint, enum v90_modem_role role,
	enum g711_law law, uint32_t enabled_downstream_rate_mask,
	uint8_t selected_downstream_rate_index,
	enum v90_dil_profile dil_profile)
{
	struct pcm_call_configuration selected = configuration(role, law);
	struct endpoint initialized = {
		.upstream_received_delay = UINT16_MAX,
	};

	selected.enabled_downstream_rate_mask = enabled_downstream_rate_mask;
	if (role == V90_ANALOG_MODEM) {
		selected.analog_phase4.data_parameters.selected_rate_index =
			selected_downstream_rate_index;
		selected.phase3.analog.dil_profile = dil_profile;
	} else
		selected.phase3.digital.jd.data_rate_capability_mask =
			enabled_downstream_rate_mask;
	if (endpoint == NULL ||
	    pcm_call_init(&initialized.call, &selected) != PCM_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_init(
	struct endpoint *endpoint, enum v90_modem_role role,
	enum g711_law law, uint32_t enabled_downstream_rate_mask,
	uint8_t selected_downstream_rate_index)
{
	return endpoint_init_with_dil(
		endpoint, role, law, enabled_downstream_rate_mask,
		selected_downstream_rate_index, V90_DIL_PROFILE_NONE);
}

static bool endpoint_init_with_rate_renegotiation_constellation(
	struct endpoint *endpoint, enum g711_law law,
	uint32_t enabled_downstream_rate_mask,
	enum v34_constellation_size constellation)
{
	struct pcm_call_configuration selected =
		configuration(V90_DIGITAL_MODEM, law);
	struct endpoint initialized = {
		.upstream_received_delay = UINT16_MAX,
	};

	selected.enabled_downstream_rate_mask =
		enabled_downstream_rate_mask;
	selected.phase3.digital.jd.data_rate_capability_mask =
		enabled_downstream_rate_mask;
	selected.phase3.digital.jd.rate_renegotiation_constellation =
		constellation;
	if (endpoint == NULL ||
	    pcm_call_init(&initialized.call, &selected) != PCM_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool phase4_boundary_is_transactional(
	struct pcm_call *call, enum g711_law law);

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
	struct endpoint *endpoint, uint8_t received, int16_t received_sample,
	uint8_t *transmitted)
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
		&endpoint->call, received, received_sample, selected_sources,
		&generated, &event);
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
			&endpoint->call, received, received_sample, &sources,
			&generated, &event);
	}
	if (result != PCM_CALL_OK)
		return false;
	endpoint->idle_source_checked = true;
	if (event.kind == PCM_CALL_DOWNSTREAM_FRAME) {
		size_t expected = endpoint->resync_data_frame ?
			endpoint->resume_frame_index : endpoint->downstream_frames;

		if (endpoint->call.local_role != V90_ANALOG_MODEM)
			return false;
		if (!downstream_frame_matches(&event.frame.downstream, expected)) {
			if (!endpoint->resync_data_frame)
				return false;
		} else {
			endpoint->resync_data_frame = false;
			endpoint->downstream_frames = expected + 1u;
		}
	} else if (event.kind == PCM_CALL_UPSTREAM_FRAME) {
		size_t expected = endpoint->resync_data_frame ?
			endpoint->resume_frame_index : endpoint->upstream_frames;

		if (endpoint->call.local_role != V90_DIGITAL_MODEM)
			return false;
		if (!upstream_frame_matches(
			    &event.frame.upstream,
			    &endpoint->upstream_received_delay, expected)) {
			if (!endpoint->resync_data_frame)
				return false;
		} else {
			endpoint->resync_data_frame = false;
			endpoint->upstream_frames = expected + 1u;
		}
	} else if (event.kind != PCM_CALL_DATA_NONE) {
		return false;
	}
	*transmitted = generated;
	return true;
}

static bool endpoint_octet(
	struct endpoint *endpoint, uint8_t received, int16_t received_sample,
	enum g711_law law, uint8_t *transmitted)
{
	struct pcm_call_observation observation;

	if (pcm_call_phase4_is_ready(&endpoint->call)) {
		if (!phase4_boundary_is_transactional(&endpoint->call, law) ||
		    pcm_call_start_data_path(&endpoint->call) != PCM_CALL_OK)
			return false;
		if (endpoint->call.local_role == V90_DIGITAL_MODEM)
			endpoint->upstream_received_delay = UINT16_MAX;
		endpoint->phase4_boundary_checked = true;
		endpoint->data_path_started = true;
	}
	if (pcm_call_current_stage(&endpoint->call) == PCM_CALL_DATA_PATH)
		return endpoint_data_octet(
			endpoint, received, received_sample, transmitted);
	if (pcm_call_octet(
		    &endpoint->call, received, received_sample, transmitted,
		    &observation) != PCM_CALL_OK)
		return false;
	if (endpoint->call.protocol == PCM_PROTOCOL_V90 &&
	    endpoint->call.local_role == V90_DIGITAL_MODEM &&
	    observation.phase2.kind == PCM_PHASE2_EVENT_READY_FOR_INFO1) {
		struct v34_info1c expected;
		const struct pcm_phase2 *phase2 = &endpoint->call.state.phase2;

		if (phase2->stage != PCM_PHASE2_INFO1 ||
		    v34_phase2_select_v90_info1d(
			    &phase2->remote_probe,
			    &endpoint->call.local_info1.digital,
			    endpoint->call.peer_info0.analog
				    .transmit_power_reduction_supported,
			    &expected) != V34_PHASE2_SELECTION_OK ||
		    !v90_info1d_equal(
			    &expected,
			    &phase2->state.info1.digital.record.v90))
			return false;
		endpoint->measured_info1d_checked = true;
	}
	if (endpoint->call.protocol == PCM_PROTOCOL_V90 &&
	    ((endpoint->call.local_role == V90_ANALOG_MODEM &&
	      observation.phase2.kind ==
		      PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT) ||
	     (endpoint->call.local_role == V90_DIGITAL_MODEM &&
	      observation.phase2.kind ==
		      PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED))) {
		uint32_t expected = V90_B1_BASE_DEADLINE_SAMPLES +
			5u * endpoint->call.round_trip_delay_samples;

		if (!endpoint->call.round_trip_delay_stored ||
		    !endpoint->call.b1_deadline_armed ||
		    endpoint->call.b1_deadline_remaining_samples != expected ||
		    observation.next != PCM_CALL_PHASE3 ||
		    endpoint->call.stage != PCM_CALL_PHASE3)
			return false;
		endpoint->info1_b1_deadline_samples = expected;
		endpoint->info1_b1_deadline_seen = true;
	}
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
		if (endpoint->call.local_role == V90_ANALOG_MODEM) {
			const struct v90_phase4_analog_station *station =
				&endpoint->call.state.phase4.analog.state.initial;
			const struct v90_phase4_ri_search *search =
				&station->receive;

			endpoint->phase4_entry_ri_symbols =
				search->stage == V90_PHASE4_RI_SEARCH_HUNT ?
					search->history_count :
					V90_PHASE4_RI_MIN_SYMBOLS;
			endpoint->phase4_entry_ri_wait_samples =
				station->ri_wait_remaining_samples;
		} else {
			endpoint->phase4_entry_ri_symbols =
				endpoint->call.state.phase4.digital.state.initial
					.transmit
					.ri_symbols;
		}
	}
	return true;
}

static bool ri_progress_is_consistent(const struct endpoint *endpoint)
{
	size_t expected = endpoint->mixed_octets;

	if (endpoint->call.local_role == V90_ANALOG_MODEM) {
		return endpoint->phase4_entry_ri_symbols == 0u &&
		       endpoint->phase4_entry_ri_wait_samples ==
			       endpoint->call.round_trip_delay_samples;
	}
	if (expected > V90_PHASE4_RI_MIN_SYMBOLS)
		expected = V90_PHASE4_RI_MIN_SYMBOLS;
	return endpoint->phase4_entry_ri_symbols == expected;
}

static bool phase4_boundary_is_transactional(
	struct pcm_call *call, enum g711_law law)
{
	struct pcm_call before = *call;
	struct pcm_call_observation observation;
	struct pcm_call_observation observation_before;
	uint8_t output = 0xa5;

	memset(&observation, 0x5a, sizeof(observation));
	observation_before = observation;
	return pcm_call_octet(
		       call, silence_octet(law), 0,
		       &output, &observation) == PCM_CALL_PHASE4_READY &&
	       output == 0xa5 &&
	       memcmp(&observation, &observation_before,
		      sizeof(observation)) == 0 &&
	       memcmp(call, &before, sizeof(before)) == 0;
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

static bool check_zero_rate_cleardown(
	struct pcm_call *call, const struct pcm_call *peer,
	struct channel *channel)
{
	struct pcm_downstream_rx downstream;
	struct v34_data_line_receiver data_receive;
	struct v90_phase4_cp_line_rx cp_receive;
	bool receiver_started = false;
	bool cp_started = false;
	unsigned int cp_records = 0u;
	unsigned int mp_records = 0u;
	bool complete = false;

	if (call->local_role == V90_ANALOG_MODEM)
		data_receive = peer->state.data_path.digital.upstream.data;
	CHECK(pcm_call_start_cleardown(call) == PCM_CALL_OK &&
	      pcm_call_start_cleardown(call) == PCM_CALL_OK &&
	      pcm_call_start_retrain(call) == PCM_CALL_INVALID_STATE,
	      "cleardown did not retain exclusive line ownership");
	for (unsigned int clock = 0u; clock < 16000u; ++clock) {
		struct pcm_call_data_event event;
		uint8_t analog_input;
		uint8_t digital_input;
		uint8_t output = 0u;
		enum pcm_call_result result = pcm_call_data_octet(
			call, silence_octet(call->law), 0, NULL, &output, &event);

		CHECK((result == PCM_CALL_OK ||
		       result == PCM_CALL_CLEARDOWN_COMPLETE) &&
		      event.kind == PCM_CALL_DATA_NONE && pcm_call_is_valid(call),
		      "cleardown failed at sample %u: %s",
		      clock, pcm_call_result_name(result));
		if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
			complete = true;
			if (call->local_role == V90_DIGITAL_MODEM ||
			    cp_records != 0u)
				break;
		}
		if (call->local_role == V90_ANALOG_MODEM) {
			int16_t sample;

			channel_receive(channel, &analog_input, &digital_input);
			channel_transmit(
				channel, output, silence_octet(call->law));
			sample = decode_octet(call->law, digital_input);
			if (!cp_started) {
				struct v34_survivor_decision decision;
				struct v34_received_mapping_frame frame;
				enum v34_data_receive_result received =
					v34_data_line_receiver_sample(
						&data_receive, sample, &decision, &frame);

				if (received == V34_DATA_RECEIVE_RATE_RENEGOTIATION) {
					const struct v90_digital_path *path =
						&peer->state.data_path.digital;

					CHECK(v90_phase4_cp_line_rx_resume_online(
						      &cp_receive, &data_receive,
						      &path->control_symbols,
						      path->rate_renegotiation_constellation) ==
						      V90_PHASE4_CP_LINE_RX_OK,
					      "cleardown S/S-bar did not hand off the peer receiver");
					cp_started = true;
				} else {
					CHECK(received == V34_DATA_RECEIVE_OK ||
					      received == V34_DATA_RECEIVE_INTERVAL_READY ||
					      received == V34_DATA_RECEIVE_MAPPING_FRAME_READY,
					      "cleardown preamble damaged peer receiver");
				}
			} else {
				struct v90_cp_receive_event received;
				enum v90_phase4_cp_line_rx_result decoded =
					v90_phase4_cp_line_rx_sample(
						&cp_receive, sample, &received);

				if (decoded == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
					if (v90_phase4_cp_line_rx_pending_record_is_valid(
						    &cp_receive)) {
						CHECK(received.parameters.kind == V90_CP_DATA &&
						      received.parameters.selected_rate_index == 0u,
						      "analogue cleardown rate changed on the QAM wire");
						cp_records++;
					}
					CHECK(v90_phase4_cp_line_rx_continue(
						      &cp_receive) ==
						      V90_PHASE4_CP_LINE_RX_OK,
					      "cleardown peer CP receiver did not resume");
				} else {
					CHECK(decoded == V90_PHASE4_CP_LINE_RX_OK ||
					      decoded == V90_PHASE4_CP_LINE_RX_SYMBOL,
					      "cleardown CP receiver failed");
				}
			}
		} else {
			struct v90_digital_path *path =
				&call->state.data_path.digital;
			struct pcm_downstream_event received;
			enum pcm_downstream_rx_result decoded;

			if (path->downstream_stage !=
			    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING)
				continue;
			if (!receiver_started) {
				CHECK(path->downstream.training.transmitted_symbols == 1u &&
				      pcm_downstream_rx_init(
					      &downstream, PCM_PROTOCOL_V90,
					      &path->control_mapping,
					      V90_PCM_MAPPING_TRANSMIT) ==
					      PCM_DOWNSTREAM_RX_OK,
				      "cleardown control boundary skipped a PCM character");
				receiver_started = true;
			}
			decoded = pcm_downstream_rx_receive(
				&downstream, output, &received);
			if (decoded == PCM_DOWNSTREAM_RX_MP) {
				CHECK(v90_mp_record_equal(
					      &received.mp.record, &path->local_mp),
				      "zero-rate MP changed on the PCM wire");
				mp_records++;
			} else {
				CHECK(decoded == PCM_DOWNSTREAM_RX_OK ||
				      decoded == PCM_DOWNSTREAM_RX_BUFFERING ||
				      decoded == PCM_DOWNSTREAM_RX_TRN2D_FRAME,
				      "cleardown emitted a malformed MP or unexpected Ed");
			}
		}
	}
	CHECK(complete && (call->local_role == V90_ANALOG_MODEM ?
		(cp_records != 0u &&
		 call->state.data_path.analog.cleardown_records == 2u &&
		 call->state.data_path.analog.cleardown_tail_samples == 0u) :
		mp_records == 2u),
	      "cleardown did not drain complete zero-rate records");
	return true;
}

static bool run_configured_call(
	enum g711_law law, size_t delay, enum call_goal goal,
	uint32_t analog_rate_mask, uint8_t analog_selected_rate_index,
	uint32_t digital_rate_mask, uint32_t expected_downstream_rate,
	bool replace_digital_codeword)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool milestone_reached = false;
	bool replacement_exercised = false;

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
		int16_t analog_received_sample;
		int16_t digital_received_sample;

		channel_receive(
			&channel, &analog_received, &digital_received);
		analog_received_sample = decode_octet(law, analog_received);
		digital_received_sample = decode_octet(law, digital_received);
		if (replace_digital_codeword &&
		    (pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE4 ||
		     pcm_call_current_stage(&digital.call) == PCM_CALL_DATA_PATH)) {
			uint8_t replacement = silence_octet(law);

			replacement_exercised |= replacement != digital_received;
			digital_received = replacement;
		}
		CHECK(endpoint_octet(
			      &analog, analog_received, analog_received_sample, law,
			      &analog_transmitted) &&
		      endpoint_octet(
			      &digital, digital_received, digital_received_sample, law,
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
	      (!replace_digital_codeword || replacement_exercised) &&
	      analog.entered_phase3 && digital.entered_phase3 &&
	      analog.entered_mixed && digital.entered_mixed &&
	      analog.entered_phase4 && digital.entered_phase4 &&
	      digital.measured_info1d_checked &&
	      analog.info1_b1_deadline_seen &&
	      digital.info1_b1_deadline_seen &&
	      ri_progress_is_consistent(&analog) &&
	      ri_progress_is_consistent(&digital) &&
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
	CHECK(analog.call.analog_phase4_configuration.training_parameters
		      .spectral_lookahead == 1 &&
	      analog.call.analog_phase4_configuration.data_parameters
		      .spectral_lookahead == 1,
	      "law %u V.90 lookahead did not honor peer maximum: %u/%u",
	      (unsigned int)law,
	      analog.call.analog_phase4_configuration.training_parameters
		      .spectral_lookahead,
	      analog.call.analog_phase4_configuration.data_parameters
		      .spectral_lookahead);
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
		      !analog.call.retrain_active &&
		      !digital.call.retrain_active &&
		      !analog.call.b1_deadline_armed &&
		      analog.call.b1_deadline_remaining_samples == 0u &&
		      !digital.call.b1_deadline_armed &&
		      digital.call.b1_deadline_remaining_samples == 0u &&
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
		CHECK(check_zero_rate_cleardown(&analog.call, &digital.call, &channel) &&
		      check_zero_rate_cleardown(&digital.call, &analog.call, &channel),
		      "online V.90 endpoints did not complete zero-rate cleardown");
	}
	return true;
}

static bool run_call(
	enum g711_law law, size_t delay, enum call_goal goal)
{
	return run_configured_call(
		law, delay, goal, UINT32_C(1), 1,
		V90_JD_RATE_CAPABILITY_MASK, DATA_DOWNSTREAM_RATE, false);
}

static bool step_endpoints(
	struct channel *channel, struct endpoint *analog,
	struct endpoint *digital, enum g711_law law)
{
	uint8_t analog_received;
	uint8_t digital_received;
	uint8_t analog_transmitted;
	uint8_t digital_transmitted;

	channel_receive(channel, &analog_received, &digital_received);
	if (!endpoint_octet(
		    analog, analog_received, decode_octet(law, analog_received), law,
		    &analog_transmitted) ||
	    !endpoint_octet(
		    digital, digital_received, decode_octet(law, digital_received), law,
		    &digital_transmitted))
		return false;
	channel_transmit(channel, analog_transmitted, digital_transmitted);
	return true;
}

static bool call_is_in_info1_exchange(const struct pcm_call *call)
{
	return call != NULL && call->stage == PCM_CALL_PHASE2 &&
	       call->state.phase2.stage == PCM_PHASE2_INFO1 &&
	       pcm_phase2_info1_is_valid(
		       &call->state.phase2.state.info1);
}

static bool force_initial_info1_recovery(
	struct pcm_call *call, enum g711_law law)
{
	struct pcm_phase2_info1_exchange *exchange;
	int16_t transmitted;

	if (!call_is_in_info1_exchange(call) || call->retrain_active)
		return false;
	exchange = &call->state.phase2.state.info1;
	for (size_t sample = 0;
	     sample < CALL_SAMPLE_LIMIT &&
	     exchange->control.phase != PHASE2_INFO1_FAILED; ++sample) {
		struct pcm_phase2_info1_event event;

		if (pcm_phase2_info1_sample(
			    exchange,
			    decode_octet(law, silence_octet(law)),
			    &transmitted, &event) != PHASE2_INFO1_OK)
			return false;
	}
	if (exchange->control.phase != PHASE2_INFO1_FAILED ||
	    phase2_info1_recover(&exchange->control) != PHASE2_INFO1_OK)
		return false;
	return exchange->control.phase == PHASE2_INFO1_RECOVERING &&
	       pcm_call_is_valid(call);
}

static bool call_is_in_initial_info1_recovery(const struct pcm_call *call)
{
	return call_is_in_info1_exchange(call) && !call->retrain_active &&
	       call->state.phase2.state.info1.control.phase ==
		       PHASE2_INFO1_RECOVERING;
}

static bool initial_info1_accepts_retrain_tone(
	struct pcm_call *call, enum g711_law law)
{
	enum v34_station_role local_role =
		call->local_role == V90_ANALOG_MODEM ?
			V34_ANSWERING_STATION : V34_CALLING_STATION;
	union pcm_info0_record peer_info0 = call->peer_info0;
	struct v34_phase2_tone_transmitter remote_tone;
	struct pcm_call_observation observation;
	uint16_t round_trip_delay = call->round_trip_delay_samples;
	int16_t trigger_sample = 0;
	uint8_t trigger_octet = 0;
	uint8_t transmitted;
	bool trigger_found = false;
	bool detected;

	CHECK(call_is_in_initial_info1_recovery(call) &&
	      v34_retrain_detector_reset(&call->retrain.detector) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &remote_tone, v34_station_peer_role(local_role),
		      PHASE2_NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "law %u role %u initial INFO1 guard did not initialize",
	      (unsigned int)law, (unsigned int)call->local_role);
	for (size_t index = 0; index < V34_RETRAIN_MIN_SILENCE_SAMPLES;
	     ++index)
		CHECK(v34_retrain_detector_sample(
			      &call->retrain.detector,
			      decode_octet(law, silence_octet(law)),
			      &detected) == V34_PHASE2_TONE_OK &&
		      !detected,
		      "law %u role %u retrain silence stopped at %zu",
		      (unsigned int)law, (unsigned int)call->local_role,
		      index);
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES +
			     V34_RETRAIN_TONE_SAMPLES;
	     ++index) {
		struct v34_retrain_detector candidate = call->retrain.detector;
		int16_t remote_sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) ==
			      V34_PHASE2_TONE_OK,
		      "law %u role %u retrain tone stopped at %zu",
		      (unsigned int)law, (unsigned int)call->local_role,
		      index);
		trigger_octet = encode_octet(law, remote_sample);
		trigger_sample = decode_octet(law, trigger_octet);
		CHECK(v34_retrain_detector_sample(
			      &candidate, trigger_sample, &detected) ==
			      V34_PHASE2_TONE_OK,
		      "law %u role %u retrain detector stopped at %zu",
		      (unsigned int)law, (unsigned int)call->local_role,
		      index);
		if (detected) {
			trigger_found = true;
			break;
		}
		call->retrain.detector = candidate;
	}
	CHECK(trigger_found && pcm_call_is_valid(call),
	      "law %u role %u did not form an initial INFO1 retrain trigger",
	      (unsigned int)law, (unsigned int)call->local_role);
	CHECK(pcm_call_octet(
		      call, trigger_octet, trigger_sample, &transmitted,
		      &observation) == PCM_CALL_OK &&
	      observation.processed == PCM_CALL_PHASE2 &&
	      observation.next == PCM_CALL_RETRAIN_SILENCE &&
	      call->stage == PCM_CALL_RETRAIN_SILENCE &&
	      call->retrain_active && call->retrain.peer_tone_retained &&
	      call->peer_info0_stored && call->round_trip_delay_stored &&
	      call->round_trip_delay_samples == round_trip_delay &&
	      memcmp(&call->peer_info0, &peer_info0, sizeof(peer_info0)) == 0 &&
	      pcm_call_is_valid(call),
	      "law %u role %u ignored INFO1 recovery retrain tone",
	      (unsigned int)law, (unsigned int)call->local_role);
	return true;
}

static bool initial_info1_marks_request_retrain(
	struct pcm_call *call, enum g711_law law)
{
	union pcm_info0_record peer_info0 = call->peer_info0;
	struct v34_phase2_dpsk_transmitter marks;
	uint16_t round_trip_delay = call->round_trip_delay_samples;
	bool retried = false;
	bool recovered = false;
	bool requested = false;

	CHECK(call->local_role == V90_DIGITAL_MODEM &&
	      force_initial_info1_recovery(call, law) &&
	      pcm_phase2_info1_retry_pending(&call->state.phase2.state.info1) &&
	      v34_phase2_dpsk_transmitter_init(
		      &marks, V34_ANSWERING_STATION,
		      PHASE2_NOMINAL_AMPLITUDE) == V34_PHASE2_DPSK_OK,
	      "law %u INFO1 marks fixture did not initialize",
	      (unsigned int)law);
	for (size_t index = 0; index < CALL_SAMPLE_LIMIT; ++index) {
		struct pcm_call_observation observation;
		int16_t remote_sample;
		int16_t received_sample;
		uint8_t transmitted;
		uint8_t received;
		bool bit_accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &marks, 1, &remote_sample, &bit_accepted) ==
			      V34_PHASE2_DPSK_OK,
		      "law %u INFO1 marks stopped at %zu",
		      (unsigned int)law, index);
		received = encode_octet(law, remote_sample);
		received_sample = decode_octet(law, received);
		CHECK(pcm_call_octet(
			      call, received, received_sample, &transmitted,
			      &observation) == PCM_CALL_OK &&
		      call->stage == PCM_CALL_PHASE2,
		      "law %u first INFOMARKSa stopped at %zu",
		      (unsigned int)law, index);
		if (call->state.phase2.state.info1.info1d_resends == 1u) {
			retried = true;
			break;
		}
	}
	CHECK(retried &&
	      call->state.phase2.state.info1.control.phase ==
		      PHASE2_INFO1_TRANSMITTING,
	      "law %u INFOMARKSa did not resend INFO1d",
	      (unsigned int)law);
	for (size_t index = 0; index < CALL_SAMPLE_LIMIT; ++index) {
		struct pcm_call_observation observation;
		uint8_t received = silence_octet(law);
		uint8_t transmitted;

		CHECK(pcm_call_octet(
			      call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      call->stage == PCM_CALL_PHASE2,
		      "law %u repeated INFO1d stopped at %zu",
		      (unsigned int)law, index);
		if (call->state.phase2.state.info1.control.phase ==
		    PHASE2_INFO1_RECOVERING) {
			recovered = true;
			break;
		}
	}
	CHECK(recovered &&
	      call->state.phase2.state.info1.info1d_resends == 1u &&
	      v34_phase2_dpsk_transmitter_init(
		      &marks, V34_ANSWERING_STATION,
		      PHASE2_NOMINAL_AMPLITUDE) == V34_PHASE2_DPSK_OK,
	      "law %u repeated INFO1d did not reach exhausted recovery",
	      (unsigned int)law);
	for (size_t index = 0; index < CALL_SAMPLE_LIMIT; ++index) {
		struct pcm_call_observation observation;
		int16_t remote_sample;
		int16_t received_sample;
		uint8_t transmitted;
		uint8_t received;
		bool bit_accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &marks, 1, &remote_sample, &bit_accepted) ==
			      V34_PHASE2_DPSK_OK,
		      "law %u exhausted INFO1 marks stopped at %zu",
		      (unsigned int)law, index);
		received = encode_octet(law, remote_sample);
		received_sample = decode_octet(law, received);
		CHECK(pcm_call_octet(
			      call, received, received_sample, &transmitted,
			      &observation) == PCM_CALL_OK,
		      "law %u exhausted INFOMARKSa failed at %zu",
		      (unsigned int)law, index);
		if (call->stage == PCM_CALL_RETRAIN_SILENCE) {
			CHECK(observation.processed == PCM_CALL_PHASE2 &&
			      observation.next == PCM_CALL_RETRAIN_SILENCE,
			      "law %u INFOMARKSa lost its Phase 2 boundary",
			      (unsigned int)law);
			requested = true;
			break;
		}
	}
	CHECK(requested && call->retrain_active &&
	      !call->retrain.peer_tone_retained && call->peer_info0_stored &&
	      call->round_trip_delay_stored &&
	      call->round_trip_delay_samples == round_trip_delay &&
	      memcmp(&call->peer_info0, &peer_info0, sizeof(peer_info0)) == 0 &&
	      pcm_call_is_valid(call),
	      "law %u persistent INFOMARKSa did not start full retrain",
	      (unsigned int)law);
	return true;
}

static bool check_initial_info1_retrain_tone(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct pcm_call retained[2];
	struct pcm_call marks;
	bool stored[2] = { false, false };

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u initial INFO1 guard endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0;
	     clock < CALL_SAMPLE_LIMIT && (!stored[0] || !stored[1]); ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u initial INFO1 guard stopped at %zu, stored=%d/%d "
		      "stages=%u/%u phase2=%u/%u",
		      (unsigned int)law, clock, stored[0], stored[1],
		      (unsigned int)analog.call.stage,
		      (unsigned int)digital.call.stage,
		      (unsigned int)analog.call.state.phase2.stage,
		      (unsigned int)digital.call.state.phase2.stage);
		if (!stored[0] && call_is_in_info1_exchange(&analog.call)) {
			retained[0] = analog.call;
			stored[0] = true;
		}
		if (!stored[1] && call_is_in_info1_exchange(&digital.call)) {
			retained[1] = digital.call;
			stored[1] = true;
		}
	}
	CHECK(stored[0] && stored[1],
	      "law %u initial INFO1 calls were not retained",
	      (unsigned int)law);
	marks = retained[1];
	CHECK(initial_info1_marks_request_retrain(&marks, law) &&
	      force_initial_info1_recovery(&retained[0], law) &&
	      force_initial_info1_recovery(&retained[1], law) &&
	      initial_info1_accepts_retrain_tone(&retained[0], law) &&
	      initial_info1_accepts_retrain_tone(&retained[1], law),
	      "law %u initial INFO1 retrain tone failed",
	      (unsigned int)law);
	return true;
}

static bool test_initial_info1_retrain_tone(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(check_initial_info1_retrain_tone(law),
		      "law %u initial INFO1 retrain matrix failed",
		      (unsigned int)law);
	return true;
}

static bool form_remote_retrain_trigger(
	struct pcm_call *call, enum g711_law law,
	struct v34_phase2_tone_transmitter *remote_tone,
	uint8_t *trigger_octet, int16_t *trigger_sample)
{
	enum v34_station_role local_role =
		call->local_role == V90_ANALOG_MODEM ?
			V34_ANSWERING_STATION : V34_CALLING_STATION;
	bool detected;

	if (call == NULL || remote_tone == NULL || trigger_octet == NULL ||
	    trigger_sample == NULL ||
	    v34_retrain_detector_reset(&call->retrain.detector) !=
		    V34_PHASE2_TONE_OK ||
	    v34_phase2_tone_transmitter_init(
		    remote_tone, v34_station_peer_role(local_role),
		    PHASE2_NOMINAL_AMPLITUDE) != V34_PHASE2_TONE_OK)
		return false;
	for (size_t index = 0; index < V34_RETRAIN_MIN_SILENCE_SAMPLES;
	     ++index) {
		if (v34_retrain_detector_sample(
			    &call->retrain.detector,
			    decode_octet(law, silence_octet(law)), &detected) !=
			    V34_PHASE2_TONE_OK ||
		    detected)
			return false;
	}
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES +
			     V34_RETRAIN_TONE_SAMPLES;
	     ++index) {
		struct v34_retrain_detector candidate = call->retrain.detector;
		int16_t remote_sample;

		if (v34_phase2_tone_transmitter_sample(
			    remote_tone, &remote_sample) != V34_PHASE2_TONE_OK)
			return false;
		*trigger_octet = encode_octet(law, remote_sample);
		*trigger_sample = decode_octet(law, *trigger_octet);
		if (v34_retrain_detector_sample(
			    &candidate, *trigger_sample, &detected) !=
		    V34_PHASE2_TONE_OK)
			return false;
		if (detected)
			return true;
		call->retrain.detector = candidate;
	}
	return false;
}

static bool final_training_accepts_remote_retrain(
	struct pcm_call *call, enum g711_law law,
	enum pcm_call_stage expected_stage)
{
	union pcm_info0_record peer_info0 = call->peer_info0;
	struct v34_phase2_tone_transmitter remote_tone;
	struct v34_phase2_tone_detector retained;
	struct pcm_call_observation observation;
	uint16_t prior_round_trip_delay = call->round_trip_delay_samples;
	uint8_t trigger_octet;
	int16_t trigger_sample;
	uint8_t transmitted;

	CHECK((expected_stage == PCM_CALL_PHASE3 ||
	       expected_stage == PCM_CALL_PHASE3_MIXED ||
	       expected_stage == PCM_CALL_PHASE4) &&
	      call->stage == expected_stage && !call->retrain_active &&
	      call->peer_info0_stored && call->round_trip_delay_stored &&
	      pcm_call_is_valid(call) &&
	      form_remote_retrain_trigger(
		      call, law, &remote_tone, &trigger_octet,
		      &trigger_sample),
	      "law %u role %u stage %u could not form remote retrain",
	      (unsigned int)law, (unsigned int)call->local_role,
	      (unsigned int)expected_stage);
	CHECK(pcm_call_octet(
		      call, trigger_octet, trigger_sample, &transmitted,
		      &observation) == PCM_CALL_OK &&
	      observation.processed == expected_stage &&
	      observation.next == PCM_CALL_RETRAIN_SILENCE &&
	      call->stage == PCM_CALL_RETRAIN_SILENCE &&
	      call->retrain_active && call->retrain.peer_tone_retained &&
	      call->peer_info0_stored && call->round_trip_delay_stored &&
	      call->round_trip_delay_samples == prior_round_trip_delay &&
	      memcmp(&call->peer_info0, &peer_info0, sizeof(peer_info0)) == 0 &&
	      pcm_call_is_valid(call),
	      "law %u role %u stage %u ignored remote retrain",
	      (unsigned int)law, (unsigned int)call->local_role,
	      (unsigned int)expected_stage);
	retained = call->retrain.detector.tone;
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES;
	     ++index) {
		struct v34_phase2_tone_event tone_event;
		int16_t remote_sample;
		int16_t received_sample;
		enum pcm_call_stage next =
			index + 1u ==
					V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ?
				PCM_CALL_PHASE2 : PCM_CALL_RETRAIN_SILENCE;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &remote_tone, &remote_sample) ==
			      V34_PHASE2_TONE_OK,
		      "law %u role %u retrain response tone stopped at %zu",
		      (unsigned int)law, (unsigned int)call->local_role, index);
		received_sample = decode_octet(law, encode_octet(law, remote_sample));
		CHECK(v34_phase2_tone_detector_sample(
			      &retained, received_sample, &tone_event) ==
			      V34_PHASE2_TONE_OK &&
		      pcm_call_octet(
			      call, encode_octet(law, remote_sample),
			      received_sample, &transmitted, &observation) ==
			      PCM_CALL_OK &&
		      transmitted == silence_octet(law) &&
		      observation.processed == PCM_CALL_RETRAIN_SILENCE &&
		      observation.next == next,
		      "law %u role %u retrain response stopped at %zu",
		      (unsigned int)law, (unsigned int)call->local_role, index);
	}
	CHECK(call->stage == PCM_CALL_PHASE2 && call->retrain_active &&
	      !call->retrain.peer_tone_retained && call->peer_info0_stored &&
	      call->round_trip_delay_stored &&
	      call->round_trip_delay_samples == prior_round_trip_delay &&
	      call->state.phase2.state.ranging.control
			      .prior_round_trip_delay_samples ==
		      prior_round_trip_delay &&
	      memcmp(&call->peer_info0, &peer_info0, sizeof(peer_info0)) == 0 &&
	      memcmp(&call->state.phase2.state.ranging.info0_exchange.transport
			      .tone_detector,
		     &retained, sizeof(retained)) == 0 &&
	      pcm_call_is_valid(call),
	      "law %u role %u stage %u lost remote retrain context",
	      (unsigned int)law, (unsigned int)call->local_role,
	      (unsigned int)expected_stage);
	return true;
}

static int tracked_final_training_stage(enum pcm_call_stage stage)
{
	if (stage == PCM_CALL_PHASE3)
		return 0;
	if (stage == PCM_CALL_PHASE3_MIXED)
		return 1;
	if (stage == PCM_CALL_PHASE4)
		return 2;
	return -1;
}

static bool check_final_training_remote_retrain(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool checked[2][3] = { { false } };
	size_t checked_count = 0;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u final-training retrain endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0;
	     clock < CALL_SAMPLE_LIMIT && checked_count != 6u; ++clock) {
		struct endpoint *endpoints[] = { &analog, &digital };

		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u final-training retrain setup stopped at %zu",
		      (unsigned int)law, clock);
		for (size_t role = 0; role < ARRAY_LENGTH(endpoints); ++role) {
			struct pcm_call candidate;
			int stage = tracked_final_training_stage(
				endpoints[role]->call.stage);

			if (stage < 0 || checked[role][stage])
				continue;
			candidate = endpoints[role]->call;
			CHECK(final_training_accepts_remote_retrain(
				      &candidate, law, candidate.stage),
			      "law %u role %zu stage %u remote retrain failed",
			      (unsigned int)law, role,
			      (unsigned int)candidate.stage);
			checked[role][stage] = true;
			checked_count++;
		}
	}
	CHECK(checked_count == 6u,
	      "law %u did not exercise every final-training stage (%zu/6)",
	      (unsigned int)law, checked_count);
	return true;
}

static bool test_final_training_remote_retrain(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(check_final_training_remote_retrain(law),
		      "law %u final-training retrain matrix failed",
		      (unsigned int)law);
	return true;
}

static bool rewind_downstream_b1(
	struct pcm_call *call, uint8_t *stall_octet)
{
	struct v90_analog_path *path;
	const struct v90_pcm_mapping *mapping;
	enum v90_pcm_mapping_plane plane;
	uint8_t ucode;

	if (call == NULL || stall_octet == NULL ||
	    call->local_role != V90_ANALOG_MODEM)
		return false;
	path = &call->state.data_path.analog;
	if (path->downstream_stage ==
	    V90_ANALOG_PATH_DOWNSTREAM_TRAINING) {
		struct pcm_downstream_rx fresh;

		mapping = &path->downstream.training.stream.decoder.mapping;
		plane = path->downstream.training.stream.decoder.decision_plane;
		if (pcm_downstream_rx_init(
			    &fresh, PCM_PROTOCOL_V90, mapping, plane) !=
		    PCM_DOWNSTREAM_RX_OK)
			return false;
		path->downstream.training = fresh;
		mapping = &path->downstream.training.stream.decoder.mapping;
	} else {
		struct v90_downstream_data_rx fresh;

		plane = path->downstream.data.stream.decoder.decision_plane;
		if (v90_downstream_data_rx_init(
			    &fresh, &path->data_mapping, plane) !=
		    V90_DOWNSTREAM_DATA_RX_OK)
			return false;
		path->downstream.data = fresh;
		mapping = &path->downstream.data.stream.decoder.mapping;
	}
	return v90_pcm_mapping_ucode(
		       mapping, plane, 0u, 0u, &ucode) == V90_PCM_MAPPING_OK &&
	       g711_ucode_octet(call->law, ucode, false, stall_octet) &&
	       v90_analog_path_is_valid(path) &&
	       !v90_analog_path_downstream_is_online(path);
}

static bool run_b1_timeout(
	enum g711_law law, enum v90_modem_role stalled_role)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct endpoint *stalled;
	union pcm_call_state stalled_state;
	uint32_t remaining;
	uint8_t stall_octet = silence_octet(law);
	bool data_path_reached = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u B1 timeout endpoints did not initialize",
	      (unsigned int)law);
	stalled = stalled_role == V90_ANALOG_MODEM ? &analog : &digital;
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u role %u B1 timeout setup stopped at %zu",
		      (unsigned int)law, (unsigned int)stalled_role, clock);
		if (stalled->data_path_started) {
			data_path_reached = true;
			break;
		}
	}
	remaining = stalled->call.b1_deadline_remaining_samples;
	CHECK(data_path_reached && stalled->info1_b1_deadline_seen &&
	      stalled->call.stage == PCM_CALL_DATA_PATH &&
	      stalled->call.b1_deadline_armed && remaining != 0u &&
	      remaining < stalled->info1_b1_deadline_samples &&
	      !pcm_call_is_online(&stalled->call),
	      "law %u role %u did not retain its B1 deadline",
	      (unsigned int)law, (unsigned int)stalled_role);
	/* Arming is role-specific; expiration is one common call-clock owner.
	 * Exercise the countdown on the exact-codeword downstream branch, where
	 * a fresh partial frame can be held without classifying line silence as
	 * retrain evidence. */
	if (stalled_role == V90_DIGITAL_MODEM)
		return true;
	CHECK(rewind_downstream_b1(&stalled->call, &stall_octet) &&
	      pcm_call_is_valid(&stalled->call),
	      "law %u role %u could not rewind peer B1",
	      (unsigned int)law, (unsigned int)stalled_role);
	stalled_state = stalled->call.state;
	stalled->call.b1_deadline_remaining_samples = 2u;
	{
		struct pcm_call_data_event event;
		enum pcm_call_result tick_result;
		uint8_t transmitted;

		tick_result = pcm_call_data_octet(
			&stalled->call, stall_octet, 0, NULL,
			&transmitted, &event);
		CHECK(tick_result == PCM_CALL_OK &&
		      event.kind == PCM_CALL_DATA_NONE &&
		      stalled->call.stage == PCM_CALL_DATA_PATH &&
		      stalled->call.b1_deadline_armed &&
		      stalled->call.b1_deadline_remaining_samples == 1u,
		      "law %u role %u B1 deadline tick returned %s at "
		      "stage %u, armed %d, remaining %u, online %d",
		      (unsigned int)law, (unsigned int)stalled_role,
		      pcm_call_result_name(tick_result),
		      (unsigned int)stalled->call.stage,
		      stalled->call.b1_deadline_armed,
		      stalled->call.b1_deadline_remaining_samples,
		      pcm_call_is_online(&stalled->call));
		stalled->call.state = stalled_state;
		CHECK(pcm_call_is_valid(&stalled->call),
		      "law %u role %u B1 rewind invalidated the call",
		      (unsigned int)law, (unsigned int)stalled_role);
	}
	{
		struct pcm_call_data_event event;
		struct pcm_call_observation observation;
		uint8_t transmitted;

		CHECK(pcm_call_data_octet(
			      &stalled->call, stall_octet, 0, NULL,
			      &transmitted, &event) == PCM_CALL_OK &&
		      event.kind == PCM_CALL_DATA_NONE &&
		      stalled->call.stage == PCM_CALL_RETRAIN_SILENCE &&
		      !stalled->call.b1_deadline_armed &&
		      stalled->call.b1_deadline_remaining_samples == 0u &&
		      !pcm_call_has_failed(&stalled->call) &&
		      pcm_call_octet(
			      &stalled->call, silence_octet(law), 0,
			      &transmitted,
			      &observation) == PCM_CALL_OK &&
		      transmitted == silence_octet(law) &&
		      observation.processed == PCM_CALL_RETRAIN_SILENCE &&
		      observation.next == PCM_CALL_RETRAIN_SILENCE,
		      "law %u role %u did not retrain at its B1 deadline",
		      (unsigned int)law, (unsigned int)stalled_role);
	}
	return true;
}

static bool test_b1_timeout(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(run_b1_timeout(law, V90_ANALOG_MODEM) &&
		      run_b1_timeout(law, V90_DIGITAL_MODEM),
		      "law %u V.90 B1 timeout roles failed",
		      (unsigned int)law);
	}
	return true;
}

static bool run_full_retrain(
	enum g711_law law, enum v90_modem_role initiating_role)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct endpoint *initiator;
	struct endpoint *responder;
	size_t downstream_before;
	size_t upstream_before;
	uint16_t initiator_prior_rtd;
	uint16_t responder_prior_rtd;
	bool initially_online = false;
	bool remote_retrain_started = false;
	bool initiator_prior_handed_off = false;
	bool responder_prior_handed_off = false;
	bool reconnected = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u retrain endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u retrain setup stopped at %zu",
		      (unsigned int)law, clock);
		if (pcm_call_is_online(&analog.call) &&
		    pcm_call_is_online(&digital.call) &&
		    analog.downstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		    digital.upstream_frames >= DATA_FRAMES_TO_RECEIVE) {
			initially_online = true;
			break;
		}
	}
	CHECK(initially_online, "law %u retrain pair did not reach data",
	      (unsigned int)law);
	initiator = initiating_role == V90_ANALOG_MODEM ? &analog : &digital;
	responder = initiating_role == V90_ANALOG_MODEM ? &digital : &analog;
	initiator_prior_rtd = initiator->call.round_trip_delay_samples;
	responder_prior_rtd = responder->call.round_trip_delay_samples;
	CHECK(pcm_call_start_retrain(&initiator->call) == PCM_CALL_OK &&
	      initiator->call.stage == PCM_CALL_RETRAIN_SILENCE &&
	      initiator->call.retrain_active &&
	      initiator->call.round_trip_delay_stored &&
	      initiator->call.round_trip_delay_samples == initiator_prior_rtd &&
	      initiator->call.protocol == PCM_PROTOCOL_V90,
	      "law %u role %u did not begin local full retrain",
	      (unsigned int)law, (unsigned int)initiating_role);
	/* The responder continues clocking its source until the complete remote
	 * preamble is qualified. LAPM owns replay of those physically lost frames;
	 * anchor the next fresh frame at the actual remote-retrain boundary. */
	downstream_before = digital.downstream_source.frames;
	upstream_before = analog.upstream_source.frames;
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		if (!remote_retrain_started) {
			analog.resume_frame_index =
				digital.downstream_source.frames;
			digital.resume_frame_index =
				analog.upstream_source.frames;
			analog.resync_data_frame = true;
			digital.resync_data_frame = true;
		}
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u role %u retrain stopped at %zu",
		      (unsigned int)law, (unsigned int)initiating_role, clock);
		if (initiator->call.stage == PCM_CALL_PHASE2 &&
		    initiator->call.retrain_active) {
			CHECK(initiator->call.round_trip_delay_stored &&
			      initiator->call.state.phase2.state.ranging.control
				      .prior_round_trip_delay_samples ==
				      initiator_prior_rtd,
			      "law %u role %u lost local prior RTD",
			      (unsigned int)law,
			      (unsigned int)initiating_role);
			initiator_prior_handed_off = true;
		}
		if (!remote_retrain_started &&
		    responder->call.stage == PCM_CALL_RETRAIN_SILENCE) {
			CHECK(responder->call.retrain_active &&
			      responder->call.round_trip_delay_stored &&
			      responder->call.round_trip_delay_samples ==
				      responder_prior_rtd,
			      "law %u role %u remote retrain lacked ownership",
			      (unsigned int)law,
			      (unsigned int)initiating_role);
			remote_retrain_started = true;
			analog.resume_frame_index =
				digital.downstream_source.frames;
			digital.resume_frame_index =
				analog.upstream_source.frames;
			analog.resync_data_frame = true;
			digital.resync_data_frame = true;
			downstream_before = analog.resume_frame_index;
			upstream_before = digital.resume_frame_index;
		}
		if (remote_retrain_started &&
		    responder->call.stage == PCM_CALL_PHASE2 &&
		    responder->call.retrain_active) {
			CHECK(responder->call.round_trip_delay_stored &&
			      responder->call.state.phase2.state.ranging.control
				      .prior_round_trip_delay_samples ==
				      responder_prior_rtd,
			      "law %u role %u lost remote prior RTD",
			      (unsigned int)law,
			      (unsigned int)initiating_role);
			responder_prior_handed_off = true;
		}
		if (remote_retrain_started && pcm_call_is_online(&analog.call) &&
		    pcm_call_is_online(&digital.call) &&
		    analog.downstream_frames > downstream_before &&
		    digital.upstream_frames > upstream_before) {
			reconnected = true;
			break;
		}
	}
	CHECK(remote_retrain_started && initiator_prior_handed_off &&
	      responder_prior_handed_off && reconnected &&
	      !analog.call.retrain_active &&
	      !digital.call.retrain_active &&
	      analog.call.protocol == PCM_PROTOCOL_V90 &&
	      digital.call.protocol == PCM_PROTOCOL_V90 &&
	      analog.call.peer_info0_stored &&
	      digital.call.peer_info0_stored &&
	      !pcm_call_has_failed(&analog.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u role %u did not complete local/remote retrain",
	      (unsigned int)law, (unsigned int)initiating_role);
	return true;
}

static bool test_full_retrain(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(run_full_retrain(law, V90_ANALOG_MODEM) &&
		      run_full_retrain(law, V90_DIGITAL_MODEM),
		      "law %u full retrain matrix failed", (unsigned int)law);
	}
	return true;
}

static bool complete_rate_renegotiation_monitor(
	struct v34_data_rate_renegotiation_monitor *monitor)
{
	struct v34_data_rate_renegotiation_monitor completed = { 0 };

	if (monitor == NULL ||
	    v34_phase3_s_receiver_init_suffix(
		    &completed.rate_renegotiation_marker,
		    V34_PHASE3_S_SYMBOLS) != V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_prefix_init_recurrent(
		    &completed.rate_renegotiation_prefix) !=
		    V34_PHASE3_S_PREFIX_OK)
		return false;
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		unsigned int turns = symbol < V34_PHASE3_S_SYMBOLS ?
			(symbol & 1u) :
			(2u + ((symbol - V34_PHASE3_S_SYMBOLS) & 1u));
		struct qam_point point = v34_rotated_point_zero((int)turns);
		uint8_t quarter_point;
		uint8_t rotation;
		enum v34_phase3_s_receiver_result marker_result;

		if (v34_training_constellation_slice(
			    V34_CONSTELLATION_4_POINT, &point,
			    &quarter_point, &rotation) != V34_CONSTELLATION_OK ||
		    quarter_point != 0u)
			return false;
		marker_result = v34_phase3_s_receiver_receive(
			&completed.rate_renegotiation_marker,
			quarter_point, rotation);
		if (marker_result !=
			    (symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
				     V34_PHASE3_S_RECEIVER_DETECTED :
				     V34_PHASE3_S_RECEIVER_OK))
			return false;
		if (symbol < V34_DATA_RRN_CANDIDATE_PREFIX_SYMBOLS) {
			enum v34_phase3_s_prefix_result prefix_result =
				v34_phase3_s_prefix_receive(
					&completed.rate_renegotiation_prefix,
					&point, 0.25);

			if (prefix_result != V34_PHASE3_S_PREFIX_OK)
				return false;
		}
	}
	completed.rate_renegotiation_candidate_symbols =
		V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS + 1u;
	completed.rate_renegotiation_enabled = true;
	completed.rate_renegotiation_candidate = true;
	*monitor = completed;
	return true;
}

static bool start_error_bounded_rate_renegotiation_candidate(
	struct v34_data_rate_renegotiation_monitor *monitor)
{
	enum {
		CANDIDATE_BOUND =
			V34_DATA_RRN_CANDIDATE_PREFIX_SYMBOLS *
			(V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u),
		LAST_ERROR = CANDIDATE_BOUND -
			V34_DATA_RRN_CANDIDATE_PREFIX_SYMBOLS,
	};
	struct v34_data_rate_renegotiation_monitor candidate = { 0 };

	if (monitor == NULL ||
	    v34_phase3_s_receiver_init_suffix(
		    &candidate.rate_renegotiation_marker,
		    V34_PHASE3_S_SYMBOLS) != V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_s_prefix_init_recurrent(
		    &candidate.rate_renegotiation_prefix) !=
		    V34_PHASE3_S_PREFIX_OK)
		return false;
	for (size_t symbol = 0; symbol < CANDIDATE_BOUND; ++symbol) {
		unsigned int turns = symbol & 1u;
		struct qam_point point;
		uint8_t quarter_point;
		uint8_t rotation;

		if (symbol < LAST_ERROR &&
		    (symbol + 1u) %
			    V34_DATA_RRN_CANDIDATE_PREFIX_SYMBOLS == 0u)
			turns++;
		point = v34_rotated_point_zero((int)turns);
		if (v34_training_constellation_slice(
			    V34_CONSTELLATION_4_POINT, &point,
			    &quarter_point, &rotation) != V34_CONSTELLATION_OK ||
		    quarter_point != 0u ||
		    v34_phase3_s_receiver_receive(
			    &candidate.rate_renegotiation_marker,
			    quarter_point, rotation) != V34_PHASE3_S_RECEIVER_OK ||
		    v34_phase3_s_prefix_receive(
			    &candidate.rate_renegotiation_prefix,
			    &point, 0.25) != V34_PHASE3_S_PREFIX_OK)
			return false;
	}
	if (candidate.rate_renegotiation_prefix.alternating_turns !=
		    V34_PHASE3_S_PREFIX_TURNS ||
	    candidate.rate_renegotiation_prefix.recurrence_hits !=
		    V34_DATA_RRN_CANDIDATE_RECURRENCE_HITS)
		return false;
	candidate.rate_renegotiation_candidate_symbols =
		V34_DATA_RRN_CANDIDATE_SYMBOLS;
	candidate.rate_renegotiation_enabled = true;
	candidate.rate_renegotiation_candidate = true;
	*monitor = candidate;
	return true;
}

enum rrn_peer_upstream_stage {
	RRN_PEER_UPSTREAM_CP,
	RRN_PEER_UPSTREAM_E,
	RRN_PEER_UPSTREAM_DATA,
};

enum rrn_peer_downstream_stage {
	RRN_PEER_DOWNSTREAM_WAIT_TRN2D,
	RRN_PEER_DOWNSTREAM_TRAINING,
	RRN_PEER_DOWNSTREAM_DATA,
};

union rrn_peer_upstream {
	struct v90_phase4_cp_line_tx cp;
	struct v90_phase4_e_line_tx e;
	struct v34_data_stream data;
};

union rrn_peer_downstream {
	struct pcm_downstream_rx training;
	struct v90_downstream_data_rx data;
};

struct rrn_peer {
	union rrn_peer_upstream upstream;
	union rrn_peer_downstream downstream;
	struct v90_cp_parameters data_parameters;
	struct v90_mp_record peer_mp;
	enum g711_law law;
	enum v90_pcm_mapping_plane decision_plane;
	enum rrn_peer_upstream_stage upstream_stage;
	enum rrn_peer_downstream_stage downstream_stage;
	bool peer_mp_received;
	bool acknowledge_pending;
	bool cp_prime_started;
};

static bool rrn_peer_init(
	struct rrn_peer *peer, const struct pcm_call *analog,
	const struct pcm_call *digital)
{
	struct rrn_peer initialized = {
		.law = analog->law,
		.upstream_stage = RRN_PEER_UPSTREAM_CP,
		.downstream_stage = RRN_PEER_DOWNSTREAM_WAIT_TRN2D,
	};
	struct v90_phase4_cp_tx symbols;
	struct v90_cp_parameters training = training_parameters(analog->law);
	const struct v90_analog_path *analog_path;
	const struct v90_digital_path *digital_path;

	if (peer == NULL || analog == NULL || digital == NULL ||
	    analog->stage != PCM_CALL_DATA_PATH ||
	    digital->stage != PCM_CALL_DATA_PATH ||
	    analog->local_role != V90_ANALOG_MODEM ||
	    digital->local_role != V90_DIGITAL_MODEM ||
	    analog->law != digital->law)
		return false;
	analog_path = &analog->state.data_path.analog;
	digital_path = &digital->state.data_path.digital;
	if (!v90_analog_path_is_online(analog_path) ||
	    !digital_path->rate_renegotiation_active ||
	    digital_path->upstream_stage !=
		    V90_DIGITAL_PATH_UPSTREAM_STARTUP ||
	    v90_phase4_cp_tx_init(
		    &symbols,
		    analog_path->upstream.data.line.profile.local_role,
		    digital_path->rate_renegotiation_constellation,
		    &training) != V90_PHASE4_CP_TX_OK)
		return false;
	/* The CP modulator is initialized only before CPt. Recreate its paused
	 * online owner from the matching receiver history, then put the live
	 * upstream carrier/pulse-shaper underneath it. */
	symbols.scrambler = digital_path->control_symbols.descrambler;
	symbols.previous_clockwise_quarter_turns =
		digital_path->control_symbols.previous_clockwise_quarter_turns;
	symbols.bit_index = symbols.frame_bit_count;
	initialized.upstream.cp.line = analog_path->upstream.data.line;
	initialized.upstream.cp.symbols = symbols;
	initialized.data_parameters = analog_path->data_parameters;
	initialized.data_parameters.acknowledges_peer = false;
	initialized.decision_plane = analog_path->downstream.data.stream.decoder
		.decision_plane;
	if (!v90_phase4_cp_line_tx_is_valid(&initialized.upstream.cp) ||
	    v90_phase4_cp_line_tx_start_next_record(
		    &initialized.upstream.cp,
		    &initialized.data_parameters) != V90_PHASE4_CP_LINE_TX_OK)
		return false;
	*peer = initialized;
	return true;
}

static bool rrn_peer_prepare_cp(struct rrn_peer *peer)
{
	struct v90_phase4_cp_line_tx *cp = &peer->upstream.cp;

	if (!v90_phase4_cp_line_tx_record_is_complete(cp))
		return true;
	if (peer->cp_prime_started)
		return true;
	peer->data_parameters.acknowledges_peer =
		peer->acknowledge_pending;
	if (v90_phase4_cp_line_tx_start_next_record(
		    cp, &peer->data_parameters) != V90_PHASE4_CP_LINE_TX_OK)
		return false;
	peer->cp_prime_started = peer->data_parameters.acknowledges_peer;
	return true;
}

static bool rrn_peer_transmit(
	struct rrn_peer *peer, struct upstream_source *source,
	uint8_t *octet)
{
	int16_t sample;

	for (;;) {
		if (peer->upstream_stage == RRN_PEER_UPSTREAM_CP) {
			enum v90_phase4_cp_line_tx_result result;

			if (!rrn_peer_prepare_cp(peer))
				return false;
			result = v90_phase4_cp_line_tx_sample(
				&peer->upstream.cp, &sample);
			if (result == V90_PHASE4_CP_LINE_TX_OK)
				break;
			if (result != V90_PHASE4_CP_LINE_TX_COMPLETE ||
			    !peer->cp_prime_started)
				return false;
			{
				struct v90_phase4_e_line_tx e;

				if (v90_phase4_e_line_tx_init(
					    &e, &peer->upstream.cp) !=
				    V90_PHASE4_E_LINE_TX_OK)
					return false;
				peer->upstream = (union rrn_peer_upstream){ 0 };
				peer->upstream.e = e;
				peer->upstream_stage = RRN_PEER_UPSTREAM_E;
			}
			continue;
		}
		if (peer->upstream_stage == RRN_PEER_UPSTREAM_E) {
			enum v90_phase4_e_line_tx_result result =
				v90_phase4_e_line_tx_sample(
					&peer->upstream.e, &sample);

			if (result == V90_PHASE4_E_LINE_TX_OK)
				break;
			if (result != V90_PHASE4_E_LINE_TX_COMPLETE ||
			    !peer->peer_mp_received)
				return false;
			{
				struct v34_data_stream data;

				if (v90_upstream_data_start_after_e(
					    &data, &peer->upstream.e,
					    &peer->peer_mp) !=
				    V90_UPSTREAM_DATA_OK)
					return false;
				peer->upstream = (union rrn_peer_upstream){ 0 };
				peer->upstream.data = data;
				peer->upstream_stage = RRN_PEER_UPSTREAM_DATA;
			}
			continue;
		}
		{
			bool accepted;

			if (v34_data_stream_sample(
				    &peer->upstream.data, source_upstream_frame,
				    source, &sample, &accepted) !=
			    V34_DATA_STREAM_OK)
				return false;
		}
		break;
	}
	*octet = encode_octet(peer->law, sample);
	return true;
}

static bool rrn_peer_receive(
	struct rrn_peer *peer, const struct v90_digital_path *path,
	uint8_t octet, size_t expected_frame, bool *host_frame_received)
{
	if (peer->downstream_stage == RRN_PEER_DOWNSTREAM_WAIT_TRN2D) {
		struct pcm_downstream_rx training;

		if (path->downstream_stage !=
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING)
			return true;
		if (pcm_downstream_rx_init(
			    &training, PCM_PROTOCOL_V90,
			    &path->control_mapping,
			    peer->decision_plane) != PCM_DOWNSTREAM_RX_OK)
			return false;
		peer->downstream.training = training;
		peer->downstream_stage = RRN_PEER_DOWNSTREAM_TRAINING;
	}
	if (peer->downstream_stage == RRN_PEER_DOWNSTREAM_TRAINING) {
		struct pcm_downstream_event event;
		enum pcm_downstream_rx_result result =
			pcm_downstream_rx_receive(
				&peer->downstream.training, octet, &event);

		if (result == PCM_DOWNSTREAM_RX_OK ||
		    result == PCM_DOWNSTREAM_RX_BUFFERING ||
		    result == PCM_DOWNSTREAM_RX_TRN2D_FRAME ||
		    result == PCM_DOWNSTREAM_RX_MP_REJECTED)
			return true;
		if (result == PCM_DOWNSTREAM_RX_MP) {
			if (v90_mp_record_acknowledges_cp(&event.mp.record))
				return false;
			peer->peer_mp = event.mp.record;
			peer->peer_mp_received = true;
			return true;
		}
		if (result == PCM_DOWNSTREAM_RX_MP_PRIME) {
			if (!peer->peer_mp_received ||
			    !v90_mp_record_acknowledges_cp(&event.mp.record) ||
			    !v90_mp_record_parameters_equal(
				    &peer->peer_mp, &event.mp.record) ||
			    pcm_downstream_rx_expect_ed(
				    &peer->downstream.training) !=
				    PCM_DOWNSTREAM_RX_OK)
				return false;
			peer->acknowledge_pending = true;
			return true;
		}
		if (result == PCM_DOWNSTREAM_RX_ED) {
			struct v90_downstream_data_rx data;

			if (!peer->acknowledge_pending ||
			    v90_downstream_data_rx_start_after_ed(
				    &data, &peer->downstream.training,
				    &path->data_mapping) !=
				    V90_DOWNSTREAM_DATA_RX_OK)
				return false;
			peer->downstream =
				(union rrn_peer_downstream){ 0 };
			peer->downstream.data = data;
			peer->downstream_stage = RRN_PEER_DOWNSTREAM_DATA;
			return true;
		}
		return false;
	}
	{
		struct v90_pcm_stream_frame frame;
		enum v90_downstream_data_rx_result result =
			v90_downstream_data_rx_receive_octet(
				&peer->downstream.data, octet, &frame);

		if (result == V90_DOWNSTREAM_DATA_RX_BUFFERING ||
		    result == V90_DOWNSTREAM_DATA_RX_B1D_FRAME ||
		    result == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE)
			return true;
		if (result != V90_DOWNSTREAM_DATA_RX_DATA_FRAME)
			return false;
		if (!*host_frame_received &&
		    downstream_frame_matches(&frame, expected_frame))
			*host_frame_received = true;
		else if (!*host_frame_received) {
			for (size_t bit = 0; bit < frame.bit_count; ++bit)
				if (frame.bits[bit] != 1u)
					return false;
		}
		return true;
	}
}

static bool run_rate_renegotiation_case(
	enum g711_law law,
	enum v34_constellation_size rate_renegotiation_constellation)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct pcm_call_data_sources sources;
	struct rrn_peer peer;
	struct v90_phase4_cp_rx control_before;
	struct v90_cp_receiver reset_parser;
	struct v90_pcm_mapping old_data_mapping;
	size_t transmitted_before;
	size_t upstream_before_resume;
	size_t received_before;
	size_t downstream_before;
	size_t rd_symbols = 0u;
	uint16_t resumed_upstream_delay = UINT16_MAX;
	uint32_t prior_transmit_rate;
	uint32_t prior_receive_rate;
	uint32_t expected_deadline;
	double online_adaptation;
	bool initially_online = false;
	bool renegotiation_started = false;
	bool deadline_started = false;
	bool timeout_checked = false;
	bool training_started = false;
	bool data_cp_latched_during_control = false;
	bool downstream_host_frame_received = false;
	bool upstream_host_frame_received = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init_with_rate_renegotiation_constellation(
		      &digital, law, V90_JD_RATE_CAPABILITY_MASK,
		      rate_renegotiation_constellation),
	      "law %u rate-renegotiation endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u rate-renegotiation setup stopped at %zu",
		      (unsigned int)law, clock);
		if (pcm_call_is_online(&analog.call) &&
		    pcm_call_is_online(&digital.call) &&
		    analog.downstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		    digital.upstream_frames >= DATA_FRAMES_TO_RECEIVE) {
			initially_online = true;
			break;
		}
	}
	CHECK(initially_online && digital.call.peer_info0_stored &&
	      digital.call.round_trip_delay_stored &&
	      digital.call.state.data_path.digital.pending_upstream_count == 0u,
	      "law %u rate-renegotiation pair did not reach ordinary data",
	      (unsigned int)law);
	transmitted_before = analog.upstream_source.frames;
	received_before = digital.upstream_frames;
	old_data_mapping =
		digital.call.state.data_path.digital.data_mapping;
	online_adaptation = digital.call.state.data_path.digital.upstream.data
		.equalizer_receiver.adaptation;
	control_before =
		digital.call.state.data_path.digital.control_symbols;
	CHECK(pcm_call_get_data_rates(
		      &digital.call, &prior_transmit_rate,
		      &prior_receive_rate),
	      "law %u rate-renegotiation rates were unavailable",
	      (unsigned int)law);
	CHECK(start_error_bounded_rate_renegotiation_candidate(
		      &digital.call.state.data_path.digital.upstream.data
			       .equalizer_receiver.rate_renegotiation) &&
	      pcm_call_is_valid(&digital.call),
	      "law %u bounded rate-renegotiation prefix was invalid",
	      (unsigned int)law);
	for (size_t sample = 0; sample < 96u; ++sample)
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u candidate suppression stopped at %zu",
		      (unsigned int)law, sample);
	CHECK(analog.upstream_source.frames > transmitted_before &&
	      digital.upstream_frames == received_before &&
	      digital.call.state.data_path.digital.pending_upstream_count > 0u,
	      "law %u candidate frames escaped to the host",
	      (unsigned int)law);
	CHECK(complete_rate_renegotiation_monitor(
		      &digital.call.state.data_path.digital.upstream.data
			       .equalizer_receiver.rate_renegotiation) &&
	      pcm_call_is_valid(&digital.call),
	      "law %u completed rate-renegotiation evidence was invalid",
	      (unsigned int)law);
	{
		union resume_alias {
			struct v90_phase4_cp_line_rx output;
			struct v34_data_line_receiver online;
			struct v90_phase4_cp_rx control;
		};
		struct v90_digital_path *path =
			&digital.call.state.data_path.digital;
		struct v90_phase4_cp_line_rx resumed;
		struct v90_phase4_cp_line_rx untouched;
		struct v90_phase4_cp_line_rx saved_untouched;
		struct v90_cp_receiver reset_records;
		union resume_alias online_alias = { 0 };
		union resume_alias saved_online_alias;
		union resume_alias control_alias = { 0 };
		union resume_alias saved_control_alias;

		memset(&untouched, 0xa5, sizeof(untouched));
		saved_untouched = untouched;
		online_alias.online = path->upstream.data;
		saved_online_alias = online_alias;
		control_alias.control = path->control_symbols;
		saved_control_alias = control_alias;
		CHECK(v90_phase4_cp_line_rx_resume_online(
			      NULL, &path->upstream.data,
			      &path->control_symbols,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      v90_phase4_cp_line_rx_resume_online(
			      &untouched, NULL, &path->control_symbols,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      v90_phase4_cp_line_rx_resume_online(
			      &untouched, &path->upstream.data, NULL,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      v90_phase4_cp_line_rx_resume_online(
			      &untouched, &path->upstream.data,
			      &path->control_symbols,
			      (enum v34_constellation_size)0) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&untouched, &saved_untouched,
			     sizeof(untouched)) == 0 &&
		      v90_phase4_cp_line_rx_resume_online(
			      &online_alias.output, &online_alias.online,
			      &path->control_symbols,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&online_alias, &saved_online_alias,
			     sizeof(online_alias)) == 0 &&
		      v90_phase4_cp_line_rx_resume_online(
			      &control_alias.output, &path->upstream.data,
			      &control_alias.control,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&control_alias, &saved_control_alias,
			     sizeof(control_alias)) == 0 &&
		      v90_phase4_cp_line_rx_resume_online(
			      &resumed, &path->upstream.data,
			      &path->control_symbols,
			      rate_renegotiation_constellation) ==
			      V90_PHASE4_CP_LINE_RX_OK &&
		      v90_cp_receiver_init(&reset_records) == V90_CP_OK &&
		      resumed.symbols.constellation ==
			      rate_renegotiation_constellation &&
		      resumed.adaptation == online_adaptation &&
		      resumed.adaptation_is_data_mode &&
		      resumed.tap_adaptation_held ==
			      (rate_renegotiation_constellation ==
			       V34_CONSTELLATION_16_POINT) &&
		      memcmp(&resumed.symbols.descrambler,
			     &path->control_symbols.descrambler,
			     sizeof(resumed.symbols.descrambler)) == 0 &&
		      resumed.symbols.previous_clockwise_quarter_turns ==
			      path->control_symbols
				      .previous_clockwise_quarter_turns &&
		      memcmp(&resumed.symbols.records, &reset_records,
			     sizeof(reset_records)) == 0 &&
		      memcmp(&path->upstream.data,
			     &saved_online_alias.online,
			     sizeof(path->upstream.data)) == 0 &&
		      memcmp(&path->control_symbols,
			     &saved_control_alias.control,
			     sizeof(path->control_symbols)) == 0,
		      "law/constellation %u/%u online CP resume contract changed",
		      (unsigned int)law,
		      (unsigned int)rate_renegotiation_constellation);
	}

	/* The generated-line test owns S/S-bar acquisition. Seed its completed
	 * event here so this boundary tests only host suppression and call-state
	 * recovery, without duplicating the line receiver's waveform fixture.
	 * Keep stepping both endpoints until the seeded event is observed so the
	 * peer transmit line and receiver sample clock remain synchronized. */
	sources = (struct pcm_call_data_sources){
		.upstream = source_upstream_frame,
		.upstream_context = &digital.upstream_source,
		.downstream = source_downstream_frame,
		.downstream_context = &digital.downstream_source,
	};
	for (size_t sample = 0;
	     sample < 64u && !renegotiation_started &&
	     digital.call.stage == PCM_CALL_DATA_PATH; ++sample) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u rate-renegotiation control escaped at sample %zu",
		      (unsigned int)law, sample);
		renegotiation_started =
			digital.call.stage == PCM_CALL_DATA_PATH &&
			digital.call.state.data_path.digital
				.rate_renegotiation_active;
	}
	CHECK(v90_cp_receiver_init(&reset_parser) == V90_CP_OK,
	      "law %u rate-renegotiation parser fixture failed",
	      (unsigned int)law);
	CHECK(renegotiation_started &&
	      digital.call.stage == PCM_CALL_DATA_PATH &&
	      pcm_call_is_online(&digital.call) &&
	      !digital.call.retrain_active &&
	      !pcm_call_has_failed(&digital.call) &&
	      digital.call.state.data_path.digital.downstream_stage ==
		      V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN &&
	      digital.call.state.data_path.digital.upstream_stage ==
		      V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
	      !digital.call.state.data_path.digital.data_mapping_ready &&
	      digital.call.state.data_path.digital.pending_upstream_count == 0u &&
	      digital.call.state.data_path.digital
		      .upstream_rate_2400_bps_units != 0u &&
	      memcmp(&digital.call.state.data_path.digital.control_symbols
			.descrambler,
		     &control_before.descrambler,
		     sizeof(control_before.descrambler)) == 0 &&
	      digital.call.state.data_path.digital.control_symbols
		      .previous_clockwise_quarter_turns ==
		      control_before.previous_clockwise_quarter_turns &&
	      digital.call.state.data_path.digital.control_symbols.constellation ==
		      rate_renegotiation_constellation &&
	      memcmp(&digital.call.state.data_path.digital.control_symbols.records,
		     &reset_parser, sizeof(reset_parser)) == 0,
	      "law %u rate renegotiation did not retain online ownership and CP sync",
	      (unsigned int)law);
	expected_deadline = V34_PHASE2_SAMPLE_RATE * 5u +
		2u * (uint32_t)digital.call.round_trip_delay_samples;
	CHECK(!digital.call.rate_renegotiation_deadline_armed &&
	      digital.call.rate_renegotiation_deadline_remaining_samples == 0u,
	      "law %u rate renegotiation armed its deadline before Rd/Rd-bar",
	      (unsigned int)law);
	{
		struct pcm_call early_deadline = digital.call;

		early_deadline.rate_renegotiation_deadline_armed = true;
		early_deadline.rate_renegotiation_deadline_remaining_samples =
			expected_deadline;
		CHECK(!pcm_call_is_valid(&early_deadline),
		      "law %u accepted a rate-renegotiation deadline before Rd/Rd-bar",
		      (unsigned int)law);
	}
	CHECK(v90_pcm_mapping_equal(
		      &digital.call.state.data_path.digital
			       .downstream.data.transmitter.encoder.mapping,
		      &old_data_mapping) &&
	      digital.call.state.data_path.digital.control_mapping
		      .spectral_lookahead == old_data_mapping.spectral_lookahead &&
	      digital.call.state.data_path.digital.control_mapping
		      .shaping_a1_q1_6 == old_data_mapping.shaping_a1_q1_6 &&
	      digital.call.state.data_path.digital.control_mapping
		      .shaping_a2_q1_6 == old_data_mapping.shaping_a2_q1_6 &&
	      digital.call.state.data_path.digital.control_mapping
		      .shaping_b1_q1_6 == old_data_mapping.shaping_b1_q1_6 &&
	      digital.call.state.data_path.digital.control_mapping
		      .shaping_b2_q1_6 == old_data_mapping.shaping_b2_q1_6,
	      "law %u rate renegotiation did not retain the preceding data shaper",
	      (unsigned int)law);
	CHECK(rrn_peer_init(&peer, &analog.call, &digital.call),
	      "law %u rate-renegotiation peer did not resume its trained line",
	      (unsigned int)law);
	upstream_before_resume = analog.upstream_source.frames;
	downstream_before = digital.downstream_source.frames;
	for (size_t sample = 0;
	     sample < CALL_SAMPLE_LIMIT &&
	     (!downstream_host_frame_received ||
	      !upstream_host_frame_received); ++sample) {
		struct pcm_call_data_event event;
		uint32_t transmit_rate;
		uint32_t receive_rate;
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t peer_transmitted;
		uint8_t transmitted;
		bool was_active = digital.call.state.data_path.digital
			.rate_renegotiation_active;

		channel_receive(
			&channel, &analog_received, &digital_received);
		CHECK(rrn_peer_transmit(
			      &peer, &analog.upstream_source,
			      &peer_transmitted),
		      "law %u rate-renegotiation peer stopped at sample %zu",
		      (unsigned int)law, sample);

		CHECK(pcm_call_data_octet(
			      &digital.call, digital_received,
			      decode_octet(law, digital_received), &sources,
			      &transmitted, &event) == PCM_CALL_OK &&
		      pcm_call_is_online(&digital.call) &&
		      pcm_call_get_data_rates(
			      &digital.call, &transmit_rate,
			      &receive_rate) &&
		      transmit_rate == prior_transmit_rate &&
		      receive_rate == prior_receive_rate &&
		      (!was_active ||
		       (event.kind == PCM_CALL_DATA_NONE &&
			digital.downstream_source.frames == downstream_before)),
		      "law %u rate-renegotiation control consumed host data at sample %zu",
		      (unsigned int)law, sample);
		if (!deadline_started &&
		    digital.call.rate_renegotiation_deadline_armed) {
			struct pcm_call invalid = digital.call;
			struct pcm_call stalled = digital.call;
			struct pcm_call_data_event stalled_event;
			uint8_t stalled_transmitted;

			deadline_started = true;
			invalid.rate_renegotiation_deadline_armed = false;
			invalid.rate_renegotiation_deadline_remaining_samples = 0u;
			stalled.rate_renegotiation_deadline_remaining_samples = 1u;
			CHECK(digital.call.state.data_path.digital.downstream_stage ==
				      V90_DIGITAL_PATH_DOWNSTREAM_RD &&
			      digital.call.state.data_path.digital.downstream.rd
				      .transmitted_symbols == V90_RD_SYMBOLS &&
			      digital.call.rate_renegotiation_deadline_remaining_samples ==
				      expected_deadline &&
			      !pcm_call_is_valid(&invalid) &&
			      pcm_call_is_valid(&stalled) &&
			      pcm_call_data_octet(
				      &stalled, silence_octet(law), 0, NULL,
				      &stalled_transmitted, &stalled_event) ==
				      PCM_CALL_OK &&
			      stalled_event.kind == PCM_CALL_DATA_NONE &&
			      stalled.stage == PCM_CALL_RETRAIN_SILENCE &&
			      stalled.retrain_active &&
			      !stalled.rate_renegotiation_deadline_armed &&
			      stalled.rate_renegotiation_deadline_remaining_samples == 0u &&
			      pcm_call_is_valid(&stalled) &&
			      !pcm_call_has_failed(&stalled),
			      "law %u rate-renegotiation deadline did not own the "
			      "Rd/Rd-bar boundary",
			      (unsigned int)law);
			timeout_checked = true;
		}
		CHECK(rrn_peer_receive(
			      &peer, &digital.call.state.data_path.digital,
			      transmitted, downstream_before,
			      &downstream_host_frame_received),
		      "law %u rate-renegotiation peer rejected downstream sample %zu",
		      (unsigned int)law, sample);
		channel_transmit(
			&channel, peer_transmitted, transmitted);
		if (event.kind == PCM_CALL_UPSTREAM_FRAME) {
			bool matches = upstream_frame_matches(
				&event.frame.upstream,
				&resumed_upstream_delay,
				upstream_before_resume);

			CHECK(!was_active && !upstream_host_frame_received && matches,
			      "law/constellation %u/%u rate-renegotiation replayed "
			      "stale upstream data (active=%u prior=%u match=%u "
			      "pending=%u bits=%u source=%zu/%zu)",
			      (unsigned int)law,
			      (unsigned int)rate_renegotiation_constellation,
			      was_active, upstream_host_frame_received, matches,
			      digital.call.state.data_path.digital
				      .pending_upstream_count,
			      event.frame.upstream.primary_bit_count,
			      analog.upstream_source.frames,
			      upstream_before_resume);
			upstream_host_frame_received = true;
		} else {
			CHECK(event.kind == PCM_CALL_DATA_NONE,
			      "law %u rate-renegotiation returned an unexpected event",
			      (unsigned int)law);
		}
		if (digital.call.state.data_path.digital.downstream_stage ==
		    V90_DIGITAL_PATH_DOWNSTREAM_RD)
			rd_symbols++;
		data_cp_latched_during_control |=
			digital.call.state.data_path.digital.data_mapping_ready &&
			(digital.call.state.data_path.digital.downstream_stage ==
				 V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN ||
			 digital.call.state.data_path.digital.downstream_stage ==
				 V90_DIGITAL_PATH_DOWNSTREAM_RD ||
			 digital.call.state.data_path.digital.downstream_stage ==
				 V90_DIGITAL_PATH_DOWNSTREAM_TRAINING);
		training_started =
			training_started ||
			digital.call.state.data_path.digital.downstream_stage ==
				V90_DIGITAL_PATH_DOWNSTREAM_TRAINING;
	}
	CHECK(training_started && deadline_started && timeout_checked &&
	      rd_symbols == V90_RD_SEQUENCE_SYMBOLS &&
	      data_cp_latched_during_control && peer.peer_mp_received &&
	      peer.cp_prime_started &&
	      peer.upstream_stage == RRN_PEER_UPSTREAM_DATA &&
	      peer.downstream_stage == RRN_PEER_DOWNSTREAM_DATA &&
	      downstream_host_frame_received && upstream_host_frame_received &&
	      !digital.call.state.data_path.digital.rate_renegotiation_active &&
	      !digital.call.rate_renegotiation_deadline_armed &&
	      digital.call.rate_renegotiation_deadline_remaining_samples == 0u &&
	      digital.call.state.data_path.digital.downstream_stage ==
		      V90_DIGITAL_PATH_DOWNSTREAM_DATA &&
	      digital.call.state.data_path.digital.upstream_stage ==
		      V90_DIGITAL_PATH_UPSTREAM_DATA &&
	      digital.call.state.data_path.digital.upstream.data
		      .equalizer_receiver.adaptation == online_adaptation &&
	      digital.downstream_source.frames > downstream_before &&
	      analog.upstream_source.frames > transmitted_before &&
	      digital.upstream_frames == received_before &&
	      pcm_call_is_online(&digital.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u rate renegotiation did not complete CP/E/B1 and resume data "
	      "(train=%u Rd=%zu control-CP=%u MP=%u CP'=%u peer-up=%u peer-down=%u "
	      "down-frame=%u up-frame=%u active=%u digital-down=%u digital-up=%u "
	      "down-source=%zu/%zu up-source=%zu/%zu up-events=%zu/%zu)",
	      (unsigned int)law, training_started, rd_symbols,
	      data_cp_latched_during_control,
	      peer.peer_mp_received, peer.cp_prime_started,
	      (unsigned int)peer.upstream_stage,
	      (unsigned int)peer.downstream_stage,
	      downstream_host_frame_received, upstream_host_frame_received,
	      digital.call.state.data_path.digital.rate_renegotiation_active,
	      (unsigned int)digital.call.state.data_path.digital.downstream_stage,
	      (unsigned int)digital.call.state.data_path.digital.upstream_stage,
	      digital.downstream_source.frames, downstream_before,
	      analog.upstream_source.frames, upstream_before_resume,
	      digital.upstream_frames, received_before);
	return true;
}

static bool test_rate_renegotiation_completion(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(run_rate_renegotiation_case(
			      law, V34_CONSTELLATION_4_POINT),
		      "law %u rate-renegotiation completion failed",
		      (unsigned int)law);
	return true;
}

static bool pcm_frame_is_fill(const uint8_t *bits, size_t bit_count)
{
	if (bits == NULL || bit_count == 0u)
		return false;
	for (size_t bit = 0; bit < bit_count; ++bit)
		if (bits[bit] != 1u)
			return false;
	return true;
}

static bool run_paired_remote_rate_renegotiation(
	enum g711_law law,
	enum v34_constellation_size rate_renegotiation_constellation)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct pcm_call_data_sources analog_sources;
	struct pcm_call_data_sources digital_sources;
	size_t analog_resume_source = 0u;
	size_t digital_resume_source;
	size_t rd_high_water = 0u;
	size_t s_high_water = 0u;
	size_t s_bar_high_water = 0u;
	uint16_t upstream_delay = UINT16_MAX;
	uint32_t analog_rates[2];
	uint32_t digital_rates[2];
	uint32_t analog_deadline;
	uint32_t digital_deadline;
	bool initially_online = false;
	bool digital_started = false;
	bool analog_started = false;
	bool rd_detected = false;
	bool rd_bar_started = false;
	bool rd_bar_complete = false;
	bool analog_deadline_seen = false;
	bool digital_deadline_seen = false;
	bool timeout_checked = false;
	bool cp_seen = false;
	bool e_seen = false;
	bool first_downstream_frame_seen = false;
	bool downstream_resumed = false;
	bool upstream_resumed = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init_with_rate_renegotiation_constellation(
		      &digital, law, V90_JD_RATE_CAPABILITY_MASK,
		      rate_renegotiation_constellation),
	      "law %u constellation %u paired RRN endpoints did not initialize",
	      (unsigned int)law,
	      (unsigned int)rate_renegotiation_constellation);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u paired RRN setup stopped at %zu",
		      (unsigned int)law, clock);
		if (pcm_call_is_online(&analog.call) &&
		    pcm_call_is_online(&digital.call) &&
		    analog.downstream_frames >= DATA_FRAMES_TO_RECEIVE &&
		    digital.upstream_frames >= DATA_FRAMES_TO_RECEIVE) {
			initially_online = true;
			break;
		}
	}
	CHECK(initially_online &&
	      pcm_call_get_data_rates(
		      &analog.call, &analog_rates[0], &analog_rates[1]) &&
	      pcm_call_get_data_rates(
		      &digital.call, &digital_rates[0], &digital_rates[1]) &&
	      analog_rates[0] == digital_rates[1] &&
	      analog_rates[1] == digital_rates[0] &&
	      complete_rate_renegotiation_monitor(
		      &digital.call.state.data_path.digital.upstream.data
			       .equalizer_receiver.rate_renegotiation) &&
	      pcm_call_is_valid(&digital.call),
	      "law %u paired RRN could not seed the digital request",
	      (unsigned int)law);
	for (size_t sample = 0; sample < 64u; ++sample) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u paired RRN trigger stopped at %zu",
		      (unsigned int)law, sample);
		if (digital.call.state.data_path.digital
			    .rate_renegotiation_active) {
			digital_started = true;
			break;
		}
	}
	digital_resume_source = digital.downstream_source.frames;
	analog_deadline = V34_PHASE2_SAMPLE_RATE * 5u +
		2u * (uint32_t)analog.call.round_trip_delay_samples;
	digital_deadline = V34_PHASE2_SAMPLE_RATE * 5u +
		2u * (uint32_t)digital.call.round_trip_delay_samples;
	analog_sources = (struct pcm_call_data_sources){
		.upstream = source_upstream_frame,
		.upstream_context = &analog.upstream_source,
	};
	digital_sources = (struct pcm_call_data_sources){
		.downstream = source_downstream_frame,
		.downstream_context = &digital.downstream_source,
	};
	CHECK(digital_started && pcm_call_is_online(&digital.call),
	      "law %u paired RRN did not enter digital control",
	      (unsigned int)law);

	for (size_t sample = 0; sample < CALL_SAMPLE_LIMIT; ++sample) {
		struct pcm_call_data_event analog_event;
		struct pcm_call_data_event digital_event;
		struct v90_analog_path *analog_path =
			&analog.call.state.data_path.analog;
		struct v90_digital_path *digital_path =
			&digital.call.state.data_path.digital;
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t analog_transmitted;
		uint8_t digital_transmitted;
		size_t analog_source_before = analog.upstream_source.frames;
		size_t digital_source_before = digital.downstream_source.frames;
		bool analog_active_before =
			analog_path->rate_renegotiation_active;
		bool digital_active_before =
			digital_path->rate_renegotiation_active;

		channel_receive(
			&channel, &analog_received, &digital_received);
		CHECK(pcm_call_data_octet(
			      &analog.call, analog_received,
			      decode_octet(law, analog_received), &analog_sources,
			      &analog_transmitted, &analog_event) == PCM_CALL_OK &&
		      pcm_call_data_octet(
			      &digital.call, digital_received,
			      decode_octet(law, digital_received), &digital_sources,
			      &digital_transmitted, &digital_event) == PCM_CALL_OK &&
		      analog.call.stage == PCM_CALL_DATA_PATH &&
		      digital.call.stage == PCM_CALL_DATA_PATH,
		      "law %u constellation %u paired RRN stopped at %zu",
		      (unsigned int)law,
		      (unsigned int)rate_renegotiation_constellation, sample);
		analog_path = &analog.call.state.data_path.analog;
		digital_path = &digital.call.state.data_path.digital;
		if (analog_active_before)
			CHECK(analog.upstream_source.frames == analog_source_before,
			      "law %u paired RRN consumed analogue host input",
			      (unsigned int)law);
		if (digital_active_before)
			CHECK(digital.downstream_source.frames ==
				      digital_source_before,
			      "law %u paired RRN consumed digital host input",
			      (unsigned int)law);

		rd_detected |= v90_downstream_rrn_rx_rd_detected(
			&analog_path->rate_renegotiation);
		rd_bar_started |= analog_path->rate_renegotiation.stage ==
			V90_DOWNSTREAM_RRN_RX_RD_BAR;
		rd_bar_complete |= v90_downstream_rrn_rx_is_complete(
			&analog_path->rate_renegotiation);
		if (!analog_started && analog_path->rate_renegotiation_active) {
			analog_started = true;
			analog_resume_source = analog.upstream_source.frames;
			CHECK(analog_path->upstream_stage ==
				      V90_ANALOG_PATH_UPSTREAM_RRN_RETURN &&
			      analog.call.rate_renegotiation_deadline_armed &&
			      analog.call
				      .rate_renegotiation_deadline_remaining_samples ==
				      analog_deadline,
			      "law %u paired RRN missed the Rd-bar boundary",
			      (unsigned int)law);
		}
		if (analog_path->rate_renegotiation_active) {
			uint32_t rates[2];

			CHECK(pcm_call_is_online(&analog.call) &&
			      pcm_call_get_data_rates(
				      &analog.call, &rates[0], &rates[1]) &&
			      rates[0] == analog_rates[0] &&
			      rates[1] == analog_rates[1] &&
			      analog_event.kind == PCM_CALL_DATA_NONE,
			      "law %u analogue RRN lost online ownership",
			      (unsigned int)law);
		}
		if (analog_path->upstream_stage ==
		    V90_ANALOG_PATH_UPSTREAM_RRN_RETURN) {
			if (analog_path->upstream.rrn_return.points.s_symbols >
			    s_high_water)
				s_high_water = analog_path->upstream.rrn_return
					.points.s_symbols;
			if (analog_path->upstream.rrn_return.points.s_bar_symbols >
			    s_bar_high_water)
				s_bar_high_water = analog_path->upstream.rrn_return
					.points.s_bar_symbols;
		} else if (analog_started && analog_path->upstream_stage ==
					   V90_ANALOG_PATH_UPSTREAM_CP) {
			cp_seen = true;
		} else if (analog_started && analog_path->upstream_stage ==
					   V90_ANALOG_PATH_UPSTREAM_E) {
			e_seen = true;
		}
		if (analog_started && !timeout_checked && analog_active_before) {
			struct pcm_call stalled = analog.call;
			struct pcm_call_data_event stalled_event;
			uint8_t stalled_octet;

			stalled.rate_renegotiation_deadline_remaining_samples = 1u;
			CHECK(pcm_call_data_octet(
				      &stalled, analog_received,
				      decode_octet(law, analog_received), NULL,
				      &stalled_octet, &stalled_event) == PCM_CALL_OK &&
			      stalled.stage == PCM_CALL_RETRAIN_SILENCE &&
			      stalled.retrain_active && pcm_call_is_valid(&stalled) &&
			      !pcm_call_has_failed(&stalled),
			      "law %u analogue RRN timeout was not bounded",
			      (unsigned int)law);
			timeout_checked = true;
		}

		if (digital_path->downstream_stage ==
		    V90_DIGITAL_PATH_DOWNSTREAM_RD &&
		    digital_path->downstream.rd.transmitted_symbols >
			    rd_high_water)
			rd_high_water =
				digital_path->downstream.rd.transmitted_symbols;
		if (!digital_deadline_seen &&
		    digital.call.rate_renegotiation_deadline_armed) {
			digital_deadline_seen = true;
			CHECK(digital.call
				      .rate_renegotiation_deadline_remaining_samples ==
				      digital_deadline,
			      "law %u digital RRN deadline began off boundary",
			      (unsigned int)law);
		}
		analog_deadline_seen |=
			analog.call.rate_renegotiation_deadline_armed;
		if (digital_path->rate_renegotiation_active)
			CHECK(pcm_call_is_online(&digital.call) &&
			      digital_event.kind == PCM_CALL_DATA_NONE,
			      "law %u digital RRN lost online ownership",
			      (unsigned int)law);

		if (analog_event.kind == PCM_CALL_DOWNSTREAM_FRAME &&
		    analog_started && !analog_path->rate_renegotiation_active) {
			bool matches = downstream_frame_matches(
				&analog_event.frame.downstream,
				digital_resume_source);

			if (!first_downstream_frame_seen) {
				CHECK(matches || pcm_frame_is_fill(
					      analog_event.frame.downstream.bits,
					      analog_event.frame.downstream.bit_count),
				      "law %u first post-RRN downstream frame was stale",
				      (unsigned int)law);
				first_downstream_frame_seen = true;
			}
			downstream_resumed |= matches;
		} else if (analog_path->rate_renegotiation_active) {
			CHECK(analog_event.kind == PCM_CALL_DATA_NONE,
			      "law %u analogue RRN exposed a host frame",
			      (unsigned int)law);
		}
		if (digital_event.kind == PCM_CALL_UPSTREAM_FRAME &&
		    analog_started && !digital_path->rate_renegotiation_active) {
			bool matches = upstream_frame_matches(
				&digital_event.frame.upstream, &upstream_delay,
				analog_resume_source);

			CHECK(matches || pcm_frame_is_fill(
				      digital_event.frame.upstream.primary_bits,
				      digital_event.frame.upstream.primary_bit_count),
			      "law %u post-RRN upstream frame was stale",
			      (unsigned int)law);
			upstream_resumed |= matches;
		} else if (digital_path->rate_renegotiation_active) {
			CHECK(digital_event.kind == PCM_CALL_DATA_NONE,
			      "law %u digital RRN exposed a host frame",
			      (unsigned int)law);
		}
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		if (analog_started && !analog_path->rate_renegotiation_active &&
		    !digital_path->rate_renegotiation_active &&
		    downstream_resumed && upstream_resumed)
			break;
	}
	CHECK(analog_started && rd_detected && rd_bar_started &&
	      rd_bar_complete && analog_deadline_seen &&
	      digital_deadline_seen && timeout_checked && cp_seen && e_seen &&
	      rd_high_water == V90_RD_SEQUENCE_SYMBOLS &&
	      s_high_water == V34_PHASE3_S_SYMBOLS &&
	      s_bar_high_water == V34_PHASE3_S_BAR_SYMBOLS &&
	      first_downstream_frame_seen && downstream_resumed &&
	      upstream_resumed && pcm_call_is_online(&analog.call) &&
	      pcm_call_is_online(&digital.call) &&
	      !analog.call.state.data_path.analog.rate_renegotiation_active &&
	      !digital.call.state.data_path.digital.rate_renegotiation_active &&
	      !pcm_call_has_failed(&analog.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u paired RRN did not resume (Rd=%zu S=%zu/%zu "
	      "detect=%u/%u/%u CP/E=%u/%u data=%u/%u)",
	      (unsigned int)law, rd_high_water, s_high_water,
	      s_bar_high_water, rd_detected, rd_bar_started, rd_bar_complete,
	      cp_seen, e_seen, downstream_resumed, upstream_resumed);
	return true;
}

static bool test_paired_remote_rate_renegotiation(void)
{
	static const enum v34_constellation_size CONSTELLATIONS[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0;
		     index < ARRAY_LENGTH(CONSTELLATIONS); ++index) {
			enum v34_constellation_size constellation =
				CONSTELLATIONS[index];

			CHECK(run_paired_remote_rate_renegotiation(
				      law, constellation),
			      "law %u constellation %u paired remote RRN failed",
			      (unsigned int)law,
			      (unsigned int)constellation);
		}
	}
	return true;
}

static bool test_rate_renegotiation_16_point_completion(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(run_rate_renegotiation_case(
			      law, V34_CONSTELLATION_16_POINT),
		      "law %u 16-point rate-renegotiation completion failed",
		      (unsigned int)law);
	return true;
}

static bool run_unusable_retrain_selection(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool initially_online = false;
	bool remote_retrain_started = false;
	bool selection_invalidated = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u unusable-selection endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u unusable-selection setup stopped at %zu",
		      (unsigned int)law, clock);
		if (pcm_call_is_online(&analog.call) &&
		    pcm_call_is_online(&digital.call)) {
			initially_online = true;
			break;
		}
	}
	CHECK(initially_online &&
	      pcm_call_start_retrain(&analog.call) == PCM_CALL_OK,
	      "law %u unusable-selection retrain did not start",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_phase2_info1_exchange *analog_info1;
		struct pcm_phase2_info1_exchange *digital_info1;

		if (!remote_retrain_started) {
			analog.resume_frame_index =
				digital.downstream_source.frames;
			digital.resume_frame_index = analog.upstream_source.frames;
			analog.resync_data_frame = true;
			digital.resync_data_frame = true;
		}
		if (!selection_invalidated &&
		    call_is_in_info1_exchange(&analog.call) &&
		    call_is_in_info1_exchange(&digital.call)) {
			analog_info1 = &analog.call.state.phase2.state.info1;
			digital_info1 = &digital.call.state.phase2.state.info1;
			if (analog_info1->digital_record_stored &&
			    !digital_info1->analog_record_stored) {
				/* Keep the offer already decoded by the peer, but make
				 * that peer's returning 3000-symbol/s choice unusable to
				 * the digital call owner at Phase 3 entry. */
				digital_info1->digital.record.v90
					.answer_transmit[V34_SYMBOL_RATE_3000]
					.projected_data_rate_2400_bps_units = 0u;
				CHECK(pcm_call_is_valid(&digital.call),
				      "law %u invalidated INFO1d damaged call state",
				      (unsigned int)law);
				selection_invalidated = true;
			}
		}
		if (!selection_invalidated) {
			CHECK(step_endpoints(&channel, &analog, &digital, law),
			      "law %u retrain stopped before INFO1 at %zu",
			      (unsigned int)law, clock);
			if (!remote_retrain_started &&
			    digital.call.stage == PCM_CALL_RETRAIN_SILENCE)
				remote_retrain_started = true;
			continue;
		}
		{
			struct pcm_call strict;
			struct pcm_call_observation strict_observation;
			struct pcm_call_observation observation;
			uint8_t analog_received;
			uint8_t digital_received;
			uint8_t analog_transmitted;
			uint8_t digital_transmitted;
			uint8_t strict_transmitted;
			enum pcm_call_result strict_result;
			enum pcm_call_result result;

			channel_receive(
				&channel, &analog_received, &digital_received);
			CHECK(endpoint_octet(
				      &analog, analog_received,
				      decode_octet(law, analog_received), law,
				      &analog_transmitted),
			      "law %u analogue peer stopped after INFO1 mutation",
			      (unsigned int)law);
			strict = digital.call;
			/* Replaying the same sample outside a retrain proves that
			 * initial negotiation remains strict. */
			strict.retrain_active = false;
			strict_result = pcm_call_octet(
				&strict, digital_received,
				decode_octet(law, digital_received),
				&strict_transmitted, &strict_observation);
			result = pcm_call_octet(
				&digital.call, digital_received,
				decode_octet(law, digital_received),
				&digital_transmitted, &observation);
			if (digital.call.stage == PCM_CALL_RETRAIN_SILENCE) {
				CHECK(strict_result == PCM_CALL_UNUSABLE_SELECTION &&
				      pcm_call_has_failed(&strict) &&
				      result == PCM_CALL_OK &&
				      strict_transmitted == digital_transmitted &&
				      observation.processed == PCM_CALL_PHASE2 &&
				      observation.phase2.kind ==
					      PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED &&
				      observation.next == PCM_CALL_RETRAIN_SILENCE &&
				      digital.call.retrain_active &&
				      digital.call.peer_info0_stored &&
				      digital.call.protocol == PCM_PROTOCOL_V90 &&
				      !pcm_call_has_failed(&digital.call),
				      "law %u unusable retrain selection returned %s/%s",
				      (unsigned int)law,
				      pcm_call_result_name(strict_result),
				      pcm_call_result_name(result));
				return true;
			}
			CHECK(strict_result == PCM_CALL_OK &&
			      result == PCM_CALL_OK &&
			      strict_transmitted == digital_transmitted,
			      "law %u mutated INFO1 exchange diverged at %zu: %s/%s",
			      (unsigned int)law, clock,
			      pcm_call_result_name(strict_result),
			      pcm_call_result_name(result));
			channel_transmit(
				&channel, analog_transmitted, digital_transmitted);
		}
	}
	CHECK(false, "law %u unusable retrain selection was not received",
	      (unsigned int)law);
	return false;
}

static bool test_unusable_retrain_selection(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(run_unusable_retrain_selection(law),
		      "law %u unusable retrain selection did not restart",
		      (unsigned int)law);
	return true;
}

static bool retrain_preserves_protocol(
	const struct pcm_call *completed, enum g711_law law)
{
	struct pcm_call retraining = *completed;
	uint16_t prior_round_trip_delay = completed->round_trip_delay_samples;
	uint8_t silence = silence_octet(law);

	if (pcm_call_start_retrain(&retraining) != PCM_CALL_OK ||
	    retraining.stage != PCM_CALL_RETRAIN_SILENCE ||
	    !retraining.round_trip_delay_stored ||
	    retraining.round_trip_delay_samples != prior_round_trip_delay)
		return false;
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
		struct pcm_call_observation observation;
		uint8_t transmitted;

		if (pcm_call_octet(
			    &retraining, silence, decode_octet(law, silence),
			    &transmitted,
			    &observation) != PCM_CALL_OK ||
		    transmitted != silence)
			return false;
	}
	return retraining.stage == PCM_CALL_PHASE2 &&
	       retraining.protocol == PCM_PROTOCOL_V92 &&
	       retraining.state.phase2.info0_protocol == PCM_PROTOCOL_V92 &&
	       retraining.state.phase2.peer_info0_stored &&
	       retraining.round_trip_delay_stored &&
	       retraining.round_trip_delay_samples == prior_round_trip_delay &&
	       retraining.state.phase2.state.ranging.control
		       .prior_round_trip_delay_samples == prior_round_trip_delay &&
	       retraining.peer_info0_stored &&
	       !retraining.phase3_entry_stored &&
	       pcm_call_is_valid(&retraining);
}

static bool run_v92_phase4_control(
	enum g711_law law, size_t delay)
{
	struct pcm_call_configuration analog_configuration =
		v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		v92_configuration(V90_DIGITAL_MODEM, law);
	struct channel channel;
	struct pcm_call analog;
	struct pcm_call digital;
	struct pcm_call_v92_analog_phase4_control analog_control = { 0 };
	struct v92_phase4_digital_station digital_control = { 0 };
	bool analog_entered_phase3 = false;
	bool digital_entered_phase3 = false;
	bool analog_entered_phase4 = false;
	bool digital_entered_phase4 = false;
	bool analog_control_reached = false;
	bool digital_control_reached = false;
	bool analog_e2u_started = false;
	bool digital_e2u_started = false;
	bool analog_handoff_reached = false;
	bool digital_handoff_reached = false;
	bool completion_failure_checked = false;
	size_t first_cpu_sent = SIZE_MAX;
	size_t first_cpu_received = SIZE_MAX;

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
		uint8_t analog_transmitted = silence_octet(law);
		uint8_t digital_transmitted = silence_octet(law);
		enum pcm_call_result analog_result;
		enum pcm_call_result digital_result;
		bool analog_ready = pcm_call_phase4_is_ready(&analog);
		bool digital_ready = pcm_call_phase4_is_ready(&digital);
		bool analog_control_ready =
			analog.stage == PCM_CALL_PHASE4 &&
			analog.state.phase4.v92.child ==
				PCM_CALL_V92_PHASE4_CONTROL &&
			v92_phase4_analog_station_is_e2u_ready(
				&analog.state.phase4.v92.owner.analog.station);
		bool digital_control_active =
			digital.stage == PCM_CALL_PHASE4 &&
			digital.state.phase4.v92.child ==
				PCM_CALL_V92_PHASE4_CONTROL;

		if (analog_control_ready && !analog_control_reached) {
			analog_control = analog.state.phase4.v92.owner.analog;
			analog_control_reached = true;
		}
		if (digital_control_active && !digital_control_reached)
			digital_control = digital.state.phase4.v92.owner.digital;

		if (analog_ready && !analog_handoff_reached) {
			CHECK(phase4_boundary_is_transactional(&analog, law),
			      "law %u analogue V.92 handoff consumed input",
			      (unsigned int)law);
			analog_handoff_reached = true;
		}
		if (digital_ready && !digital_handoff_reached) {
			CHECK(phase4_boundary_is_transactional(&digital, law),
			      "law %u digital V.92 handoff consumed input",
			      (unsigned int)law);
			digital_handoff_reached = true;
		}
		channel_receive(
			&channel, &analog_received, &digital_received);
		if (!completion_failure_checked && law == G711_LAW_PCMU &&
		    delay == 1u && !analog_ready &&
		    analog.stage == PCM_CALL_PHASE4 &&
		    analog.state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_CONTROL &&
		    analog.state.phase4.v92.owner.analog.station.cpd_received) {
			struct v92_phase4_analog_station predicted =
				analog.state.phase4.v92.owner.analog.station;
			uint64_t completed =
				predicted.transmit.completed_cpu_records;
			int16_t predicted_sample;

			if (v92_phase4_analog_station_transmit(
				    &predicted, &predicted_sample) ==
				    V92_PHASE4_ANALOG_STATION_OK &&
			    predicted.transmit.completed_cpu_records ==
				    completed + 1u) {
				struct pcm_call collision = analog;
				struct pcm_call_observation collision_observation;
				uint8_t collision_output;
				uint8_t rate = collision.state.phase4.v92.owner
					.analog.station.cpd.data_rate_number;

				collision.phase3_configuration.v92_analog
					.upstream_rate_mask &=
					~(UINT32_C(1) << (rate - 1u));
				CHECK(pcm_call_octet(
					      &collision, analog_received,
					      decode_octet(law, analog_received),
					      &collision_output,
					      &collision_observation) ==
					      PCM_CALL_UNUSABLE_SELECTION &&
				      pcm_call_is_valid(&collision) &&
				      pcm_call_has_failed(&collision) &&
				      collision.state.phase4.v92.owner.analog
					      .observed_cpu_records ==
					      collision.state.phase4.v92.owner.analog
						      .station.transmit
						      .completed_cpu_records,
				      "V.92 CPu completion hid a receive failure");
				completion_failure_checked = true;
			}
		}
		analog_observation = (struct pcm_call_observation){ 0 };
		digital_observation = (struct pcm_call_observation){ 0 };
		analog_result = analog_ready ? PCM_CALL_PHASE4_READY :
			pcm_call_octet(
				&analog, analog_received,
				decode_octet(law, analog_received),
				&analog_transmitted,
				&analog_observation);
		digital_result = digital_ready ? PCM_CALL_PHASE4_READY :
			pcm_call_octet(
				&digital, digital_received,
				decode_octet(law, digital_received),
				&digital_transmitted,
				&digital_observation);
		CHECK(analog_result == (analog_ready ? PCM_CALL_PHASE4_READY :
						       PCM_CALL_OK) &&
		      digital_result == (digital_ready ? PCM_CALL_PHASE4_READY :
						         PCM_CALL_OK),
		      "law %u delay %zu V.92 call stopped at character %zu: "
		      "%s/%s stages %u/%u children %u/%u",
		      (unsigned int)law, delay, clock,
		      pcm_call_result_name(analog_result),
		      pcm_call_result_name(digital_result),
		      (unsigned int)analog.stage,
		      (unsigned int)digital.stage,
		      (unsigned int)(analog.stage == PCM_CALL_PHASE4 ?
			      analog.state.phase4.v92.child : 0u),
		      (unsigned int)(digital.stage == PCM_CALL_PHASE4 ?
			      digital.state.phase4.v92.child : 0u));
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		analog_entered_phase3 |=
			analog_observation.next == PCM_CALL_PHASE3;
		digital_entered_phase3 |=
			digital_observation.next == PCM_CALL_PHASE3;
		analog_entered_phase4 |=
			analog_observation.next == PCM_CALL_PHASE4;
		digital_entered_phase4 |=
			digital_observation.next == PCM_CALL_PHASE4;
		analog_e2u_started |= analog.stage == PCM_CALL_PHASE4 &&
			analog.state.phase4.v92.child ==
				PCM_CALL_V92_PHASE4_E2U;
		digital_e2u_started |= digital.stage == PCM_CALL_PHASE4 &&
			digital.state.phase4.v92.child ==
				PCM_CALL_V92_PHASE4_E2U;
		if (digital_control_active && digital.stage == PCM_CALL_PHASE4 &&
		    digital.state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U)
			digital_control_reached = true;
		if (analog.stage == PCM_CALL_PHASE4 &&
		    analog.state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_CONTROL &&
		    analog.state.phase4.v92.owner.analog.station.transmit.cpu_sent &&
		    first_cpu_sent == SIZE_MAX)
			first_cpu_sent = clock;
		if (digital.stage == PCM_CALL_PHASE4 &&
		    digital.state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_CONTROL &&
		    digital.state.phase4.v92.owner.digital.cpu_received &&
		    first_cpu_received == SIZE_MAX)
			first_cpu_received = clock;
		if (analog_handoff_reached && digital_handoff_reached)
			break;
	}
	CHECK(analog_handoff_reached && digital_handoff_reached &&
	      analog_control_reached && digital_control_reached &&
	      analog_e2u_started && digital_e2u_started &&
	      (law != G711_LAW_PCMU || delay != 1u ||
	       completion_failure_checked) &&
	      analog_entered_phase3 && digital_entered_phase3 &&
	      analog_entered_phase4 && digital_entered_phase4 &&
	      first_cpu_sent != SIZE_MAX && first_cpu_received != SIZE_MAX &&
	      first_cpu_received >= first_cpu_sent &&
	      first_cpu_received - first_cpu_sent >=
		      analog_control.cpu_repeat_interval_samples &&
	      pcm_call_is_valid(&analog) && pcm_call_is_valid(&digital) &&
	      !pcm_call_has_failed(&analog) && !pcm_call_has_failed(&digital) &&
	      analog.phase3_entry_stored && digital.phase3_entry_stored &&
	      analog.v92_peer_downstream_rate_stored &&
	      analog.v92_peer_downstream_rate_mask ==
		      digital_configuration.phase3.v92_digital.jd
			      .data_rate_capability_mask &&
	      !digital.v92_peer_downstream_rate_stored &&
	      digital.v92_peer_downstream_rate_mask == 0u &&
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
		      &analog_configuration.local_info1.v92_analog) &&
	      analog.stage == PCM_CALL_PHASE4 &&
	      digital.stage == PCM_CALL_PHASE4 &&
	      analog.state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U &&
	      digital.state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U &&
	      analog.state.phase4.v92.owner.e2u.local_role ==
		      V90_ANALOG_MODEM &&
	      digital.state.phase4.v92.owner.e2u.local_role ==
		      V90_DIGITAL_MODEM &&
	      v92_phase4_e2u_is_complete(
		      &analog.state.phase4.v92.owner.e2u) &&
	      v92_phase4_e2u_is_complete(
		      &digital.state.phase4.v92.owner.e2u) &&
	      analog.round_trip_delay_stored &&
	      analog.round_trip_delay_samples != 0u &&
	      digital.round_trip_delay_stored &&
	      digital.round_trip_delay_samples != 0u &&
	      analog_control.cpu_repeat_interval_samples ==
		      800u + analog.round_trip_delay_samples &&
	      analog_control.cpu_repeat_remaining_samples == 0u &&
	      !analog_control.awaiting_cpu_completion &&
	      analog_control.observed_cpu_records ==
		      analog_control.station.transmit.completed_cpu_records &&
	      analog_control.observed_cpu_records >= 2u &&
	      v92_cpu_frame_bits(
		      &analog_configuration.v92_analog_phase4.cpu, 2u) /
		      2u >
		      analog_control.cpu_repeat_interval_samples &&
	      v92_phase4_analog_station_is_e2u_ready(
		      &analog_control.station) &&
	      v92_cpu_parameters_are_valid(
		      &digital.state.phase4.v92.owner.e2u.state.digital.cpu) &&
	      v92_cpd_base_parameters_are_valid(
		      &digital.state.phase4.v92.owner.e2u.state.digital.cpd) &&
	      analog_control.station.cpd_received &&
	      analog_control.station.suvd.request_silence ==
		      digital_configuration.v92_digital_phase4.suvd
			      .request_silence &&
	      analog_control.station.cpd.data_rate_number ==
		      digital_configuration.v92_digital_phase4.cpd
			      .data_rate_number &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpu
		      .data_rate_number ==
		      analog_configuration.v92_analog_phase4.cpu
			      .data_rate_number &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpu.law == law &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpd
		      .data_rate_number ==
		      digital_configuration.v92_digital_phase4.cpd
			      .data_rate_number &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpd.trellis ==
		      digital_configuration.v92_digital_phase4.cpd.trellis &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpd
		      .four_times_gain_q0_16 ==
		      digital_configuration.v92_digital_phase4.cpd
			      .four_times_gain_q0_16 &&
	      digital.state.phase4.v92.owner.e2u.state.digital.cpd.extend_e2u ==
		      digital_configuration.v92_digital_phase4.cpd.extend_e2u &&
	      v90_pcm_mapping_equal(
		      &analog_control.station.receive.stream.decoder.mapping,
		      &digital_control.transmit.encoder.mapping),
	      "law %u delay %zu V.92 handoff failed: "
	      "handoff=%u/%u control=%u/%u phase=%u/%u stage=%u/%u "
	      "cpu=%zu/%zu records=%llu/%llu timer=%u/%u rtd=%u",
	      (unsigned int)law, delay,
	      analog_handoff_reached, digital_handoff_reached,
	      analog_control_reached, digital_control_reached,
	      analog_entered_phase4, digital_entered_phase4,
	      (unsigned int)analog.stage, (unsigned int)digital.stage,
	      first_cpu_sent, first_cpu_received,
	      (unsigned long long)analog_control.observed_cpu_records,
	      (unsigned long long)analog_control.station
		      .transmit.completed_cpu_records,
	      analog_control.cpu_repeat_interval_samples,
	      analog_control.cpu_repeat_remaining_samples,
	      analog.round_trip_delay_samples);
	CHECK(phase4_boundary_is_transactional(&analog, law) &&
	      phase4_boundary_is_transactional(&digital, law) &&
	      retrain_preserves_protocol(&analog, law) &&
	      retrain_preserves_protocol(&digital, law) &&
	      !pcm_call_is_online(&analog) && !pcm_call_is_online(&digital) &&
	      pcm_call_start_data_path(&analog) == PCM_CALL_INVALID_STATE &&
	      pcm_call_start_data_path(&digital) == PCM_CALL_INVALID_STATE,
	      "law %u V.92 control was misreported as online",
	      (unsigned int)law);
	return true;
}

static bool run_v92_unavailable_selection(bool downstream)
{
	const enum g711_law law = G711_LAW_PCMU;
	struct pcm_call_configuration analog_configuration =
		v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		v92_configuration(V90_DIGITAL_MODEM, law);
	struct channel channel;
	struct pcm_call analog;
	struct pcm_call digital;

	if (downstream) {
		uint32_t selected = UINT32_C(1) <<
			(analog_configuration.v92_analog_phase4.cpu
				 .data_rate_number - 1u);

		digital_configuration.enabled_downstream_rate_mask &= ~selected;
		digital_configuration.phase3.v92_digital.jd
			.data_rate_capability_mask &= ~selected;
	} else {
		uint32_t selected = UINT32_C(1) <<
			(digital_configuration.v92_digital_phase4.cpd
				 .data_rate_number - 1u);

		analog_configuration.phase3.v92_analog.upstream_rate_mask &=
			~selected;
	}
	CHECK(channel_init(&channel, law, 1u) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "V.92 unavailable-rate calls did not initialize");
	for (size_t clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
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
			&analog, analog_received, decode_octet(law, analog_received),
			&analog_transmitted,
			&analog_observation);
		digital_result = pcm_call_octet(
			&digital, digital_received,
			decode_octet(law, digital_received), &digital_transmitted,
			&digital_observation);
		if (analog_result != PCM_CALL_OK ||
		    digital_result != PCM_CALL_OK) {
			enum pcm_call_result expected_analog = downstream ?
				PCM_CALL_UNUSABLE_SELECTION : PCM_CALL_OK;
			enum pcm_call_result expected_digital = downstream ?
				PCM_CALL_OK : PCM_CALL_UNUSABLE_SELECTION;

			CHECK(analog_result == expected_analog &&
			      digital_result == expected_digital &&
			      pcm_call_is_valid(&analog) &&
			      pcm_call_is_valid(&digital) &&
			      pcm_call_has_failed(
				      downstream ? &analog : &digital) &&
			      !pcm_call_has_failed(
				      downstream ? &digital : &analog),
			      "V.92 accepted an unadvertised %s rate: %s/%s",
			      downstream ? "downstream" : "upstream",
			      pcm_call_result_name(analog_result),
			      pcm_call_result_name(digital_result));
			return true;
		}
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
	}
	CHECK(false, "V.92 unadvertised %s rate did not terminate",
	      downstream ? "downstream" : "upstream");
	return false;
}

static bool test_v92_phase4_control(void)
{
	for (size_t delay = 1; delay <= 3; ++delay)
		CHECK(run_v92_phase4_control(G711_LAW_PCMU, delay) &&
		      run_v92_phase4_control(G711_LAW_PCMA, delay),
		      "delay %zu V.92 role-local Phase 4 handoff failed",
		      delay);
	return run_v92_unavailable_selection(true) &&
	       run_v92_unavailable_selection(false);
}

static bool test_downstream_rate_negotiation(void)
{
	CHECK(run_configured_call(
		      G711_LAW_PCMU, 1, CALL_GOAL_ONLINE_DATA,
		      UINT32_C(3), 2, UINT32_C(1), DATA_DOWNSTREAM_RATE,
		      false),
	      "call did not select the highest mutually enabled rate");
	return true;
}

static uint16_t current_dil_segment_symbols(
	const struct v90_dil_sequence *sequence)
{
	uint8_t chord =
		sequence->parameters.training_ucodes[sequence->segment] >> 4;

	return (uint16_t)(
		(sequence->parameters.segment_length_codes[chord] + 1u) *
		V90_DIL_SEGMENT_INTERVALS);
}

static bool check_completed_dil_return_drains(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool drain_seen = false;
	bool boundary_seen = false;
	bool online_data_seen = false;
	size_t drain_octets = 0u;

	CHECK(channel_init(&channel, law, 3u) &&
	      endpoint_init_with_dil(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u,
		      V90_DIL_PROFILE_CONVENTIONAL) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u DIL-drain endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		const struct v90_dil_sequence *dil = NULL;
		uint16_t segment_symbols = 0u;
		uint16_t segment_symbol = 0u;
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t analog_transmitted;
		uint8_t digital_transmitted;
		bool draining = false;

		channel_receive(
			&channel, &analog_received, &digital_received);
		CHECK(endpoint_octet(
			      &analog, analog_received,
			      decode_octet(law, analog_received), law,
			      &analog_transmitted),
		      "law %u DIL-drain analogue endpoint stopped at %zu",
		      (unsigned int)law, clock);
		if (digital.call.stage == PCM_CALL_PHASE3 &&
		    v90_phase3_digital_station_receive_complete(
			    &digital.call.state.phase3.digital) &&
		    !v90_phase3_digital_station_transmit_complete(
			    &digital.call.state.phase3.digital)) {
			dil = &digital.call.state.phase3.digital.transmit.dil;
			segment_symbols = current_dil_segment_symbols(dil);
			segment_symbol = dil->segment_symbol;
			draining = true;
			drain_seen = true;
			drain_octets++;
			CHECK(digital.call.state.phase3.digital.transmit.stage ==
				      PCM_PHASE3_DIGITAL_TX_DIL &&
			      dil->finish_requested && !dil->complete &&
			      segment_symbol < segment_symbols,
			      "law %u invalid DIL drain state at %zu: %u/%u",
			      (unsigned int)law, clock, segment_symbol,
			      segment_symbols);
		}
		CHECK(endpoint_octet(
			      &digital, digital_received,
			      decode_octet(law, digital_received), law,
			      &digital_transmitted),
		      "law %u completed return was reused at %zu",
		      (unsigned int)law, clock);
		if (draining && digital.entered_phase4) {
			boundary_seen = true;
			CHECK((uint16_t)(segment_symbol + 1u) == segment_symbols,
			      "law %u DIL left before its boundary: %u/%u",
			      (unsigned int)law, segment_symbol, segment_symbols);
		}
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		if (goal_is_reached(
			    CALL_GOAL_ONLINE_DATA, &analog, &digital)) {
			online_data_seen = true;
			break;
		}
	}
	CHECK(drain_seen && drain_octets > 0u && boundary_seen &&
	      analog.entered_phase4 && digital.entered_phase4 &&
	      online_data_seen &&
	      !pcm_call_has_failed(&analog.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u did not drain completed-return DIL into online data: "
	      "drain=%zu boundary=%d online=%d stages=%u/%u",
	      (unsigned int)law, drain_octets, boundary_seen, online_data_seen,
	      (unsigned int)analog.call.stage,
	      (unsigned int)digital.call.stage);
	return true;
}

static bool test_completed_dil_return_drains(void)
{
	return check_completed_dil_return_drains(G711_LAW_PCMU) &&
	       check_completed_dil_return_drains(G711_LAW_PCMA);
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
	CHECK(run_configured_call(
		      G711_LAW_PCMA, 1, CALL_GOAL_ONLINE_DATA,
		      UINT32_C(1), 1, V90_JD_RATE_CAPABILITY_MASK,
		      DATA_DOWNSTREAM_RATE, true),
	      "digital Phase 4 depended on a reconstructed codeword");
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
	struct pcm_call invalid;
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
	      !pcm_call_phase4_is_ready(&call) &&
	      !pcm_call_is_online(&call),
	      "valid call did not initialize");
	invalid = call;
	invalid.b1_deadline_armed = true;
	invalid.b1_deadline_remaining_samples =
		V90_B1_BASE_DEADLINE_SAMPLES;
	CHECK(!pcm_call_is_valid(&invalid),
	      "call accepted a B1 deadline without timing evidence");
	invalid = call;
	invalid.b1_deadline_remaining_samples = 1u;
	CHECK(!pcm_call_is_valid(&invalid),
	      "call accepted an unowned B1 deadline");
	invalid = call;
	invalid.rate_renegotiation_deadline_remaining_samples = 1u;
	CHECK(!pcm_call_is_valid(&invalid),
	      "call accepted an unowned rate-renegotiation deadline");
	invalid = call;
	invalid.rate_renegotiation_deadline_armed = true;
	invalid.rate_renegotiation_deadline_remaining_samples = 1u;
	CHECK(!pcm_call_is_valid(&invalid),
	      "call accepted a rate-renegotiation deadline outside online data");
	invalid = call;
	invalid.retrain_active = true;
	CHECK(!pcm_call_is_valid(&invalid),
	      "call accepted retrain ownership without peer INFO0");
	saved = call;
	memset(&observation, 0x5a, sizeof(observation));
	saved_observation = observation;
	memset(&data_event, 0x3c, sizeof(data_event));
	saved_data_event = data_event;
	CHECK(pcm_call_octet(
		      NULL, silence_octet(G711_LAW_PCMU), 0,
		      &output, &observation) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_start_retrain(NULL) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_start_retrain(&call) == PCM_CALL_INVALID_STATE &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU), 0,
		      NULL, &observation) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU), 0,
		      &output, NULL) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU), 0,
		      (uint8_t *)&call, &observation) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_octet(
		      &call, silence_octet(G711_LAW_PCMU), 0,
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
		      &call, silence_octet(G711_LAW_PCMU), 0, NULL,
		      &output, &data_event) == PCM_CALL_INVALID_STATE &&
	      output == 0xa5 &&
	      memcmp(&observation, &saved_observation,
		     sizeof(observation)) == 0 &&
	      memcmp(&data_event, &saved_data_event,
		     sizeof(data_event)) == 0 &&
	      memcmp(&call, &saved, sizeof(call)) == 0 &&
	      !pcm_call_is_valid(NULL) && !pcm_call_has_failed(NULL) &&
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
	selected.v92_analog_phase4.cpu.law = G711_LAW_PCMA;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a mismatched V.92 CPu law");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.v92_analog_phase4.cpu.data_rate_number = 0u;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a V.92 CPu cleardown as startup");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.enabled_downstream_rate_mask &=
		~(UINT32_C(1) <<
		  (selected.v92_analog_phase4.cpu.data_rate_number - 1u));
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a locally disabled V.92 CPu rate");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.v92_digital_phase4.cpd.acknowledges_cpu = true;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an acknowledging initial V.92 CPd");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.v92_digital_phase4.cpd.data_rate_number = 0u;
	CHECK(pcm_call_init(&untouched, &selected) ==
		      PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a V.92 CPd cleardown as startup");
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
		"phase 4 ready",
		"V.34 fallback ready",
		"cleardown complete",
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
		{ "initial INFO1 retrain-tone response",
		  test_initial_info1_retrain_tone },
		{ "remote retrain during final PCM training",
		  test_final_training_remote_retrain },
		{ "completed V.90 return drains DIL",
		  test_completed_dil_return_drains },
		{ "generated PCMU and PCMA calls through online data",
		  test_generated_calls },
		{ "local and remote PCM full retrain", test_full_retrain },
		{ "V.90 in-place rate-renegotiation completion and resume",
		  test_rate_renegotiation_completion },
		{ "paired V.90 remote rate renegotiation",
		  test_paired_remote_rate_renegotiation },
		{ "16-point rate renegotiation completes in place",
		  test_rate_renegotiation_16_point_completion },
		{ "unusable retrain selection restarts full training",
		  test_unusable_retrain_selection },
		{ "standards-timed V.90 B1 retrain", test_b1_timeout },
		{ "downstream rate negotiation",
		  test_downstream_rate_negotiation },
		{ "V.92 role-local Phase 4 handoffs",
		  test_v92_phase4_control },
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
