// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_call.h>

#include <g711.h>
#include <v34_constellation.h>
#include <v34_phase2_selection.h>
#include <v34_phase3_s_receiver.h>
#include <v90_downstream_data_handoff.h>
#include <v90_phase3_sd.h>
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
		configuration.phase3.digital.jd = jd_parameters();
		configuration.digital_phase4.initial_mp = initial_mp();
	}
	return configuration;
}

static bool check_phase2_input_ownership(
	enum pcm_protocol protocol, enum v90_modem_role role, enum g711_law law)
{
	struct pcm_call_configuration selected = protocol == PCM_PROTOCOL_V90 ?
		configuration(role, law) : v92_configuration(role, law);
	struct pcm_call call;
	struct pcm_phase2 expected;
	struct v34_phase2_tone_transmitter peer;
	bool physical = protocol == PCM_PROTOCOL_V90 && role == V90_DIGITAL_MODEM;
	size_t distinct_inputs = 0u;
	size_t noncodeword_samples = 0u;

	CHECK(pcm_call_init(&call, &selected) == PCM_CALL_OK &&
	      v34_phase2_tone_transmitter_init(
		      &peer, role == V90_DIGITAL_MODEM ?
			      V34_ANSWERING_STATION : V34_CALLING_STATION,
		      PHASE2_NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "Phase 2 input owners did not initialize");
	expected = call.state.phase2;
	for (size_t index = 0u; index < 320u; ++index) {
		struct pcm_phase2_event phase2;
		struct pcm_call_observation observed;
		int16_t sample;
		int16_t generated;
		int16_t owned_input;
		uint8_t octet;
		uint8_t transmitted;

		CHECK(v34_phase2_tone_transmitter_sample(&peer, &sample) ==
			      V34_PHASE2_TONE_OK,
		      "Phase 2 ownership tone stopped");
		noncodeword_samples += sample !=
			decode_octet(law, encode_octet(law, sample));
		/* Deliberately disagree at the two inputs. The physical owner must
		 * neither reconstruct nor requantize its conventional sample. */
		octet = physical ? silence_octet(law) : encode_octet(law, sample);
		sample = physical ? sample : 0;
		distinct_inputs += sample != decode_octet(law, octet);
		owned_input = physical ? sample : decode_octet(law, octet);
		CHECK(pcm_phase2_sample(
			      &expected, owned_input, &generated, &phase2) ==
			      PCM_PHASE2_OK &&
		      pcm_call_octet(
			      &call, octet, sample, &transmitted, &observed) ==
			      PCM_CALL_OK &&
		      observed.processed == PCM_CALL_PHASE2 &&
		      observed.next == PCM_CALL_PHASE2 &&
		      transmitted == encode_octet(law, generated) &&
		      memcmp(&phase2, &observed.phase2, sizeof(phase2)) == 0 &&
		      memcmp(&expected, &call.state.phase2, sizeof(expected)) == 0,
		      "protocol %u role %u law %u Phase 2 changed its input "
		      "or consumed it twice at %zu",
		      (unsigned int)protocol, (unsigned int)role,
		      (unsigned int)law, index);
	}
	CHECK(distinct_inputs != 0u && noncodeword_samples != 0u,
	      "Phase 2 ownership inputs did not diverge");
	return true;
}

static bool test_phase2_input_ownership(void)
{
	for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
	     protocol <= PCM_PROTOCOL_V92; ++protocol)
		for (enum v90_modem_role role = V90_ANALOG_MODEM;
		     role <= V90_DIGITAL_MODEM; ++role)
			for (enum g711_law law = G711_LAW_PCMU;
			     law <= G711_LAW_PCMA; ++law)
				CHECK(check_phase2_input_ownership(protocol, role, law),
				      "Phase 2 input ownership failed");
	return true;
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

static bool check_peer_cleardown(
	const struct pcm_call *analog, const struct pcm_call *digital,
	const struct channel *line, unsigned int initiator)
{
	struct pcm_call calls[2] = { *analog, *digital };
	struct channel channel = *line;
	bool complete[2] = { false, false };
	enum pcm_call_result started = pcm_call_start_cleardown(&calls[initiator]);

	CHECK(started == PCM_CALL_OK,
	      "law %u side %u paired cleardown did not start: %s",
	      (unsigned int)analog->law, initiator, pcm_call_result_name(started));
	CHECK(!pcm_call_is_online(&calls[initiator]) &&
	      !calls[initiator].b1_deadline_armed &&
	      !calls[initiator].rate_renegotiation_deadline_armed,
	      "local cleardown retained CONNECT or training deadlines");
	for (size_t clock = 0; clock < 16000u && !(complete[0] && complete[1]); ++clock) {
		uint8_t received[2];
		uint8_t transmitted[2];

		channel_receive(&channel, &received[0], &received[1]);
		for (unsigned int side = 0; side < 2u; ++side) {
			struct pcm_call_data_event event;
			enum pcm_call_result result = pcm_call_data_octet(
				&calls[side], received[side],
				decode_octet(calls[side].law, received[side]), NULL,
				&transmitted[side], &event);

			CHECK((result == PCM_CALL_OK || result == PCM_CALL_CLEARDOWN_COMPLETE) &&
			      !calls[side].failed && !calls[side].retrain_active &&
			      pcm_call_is_valid(&calls[side]),
			      "law %u initiator %u side %u sample %zu cleardown returned %s stage %u",
			      (unsigned int)calls[side].law, initiator, side, clock,
			      pcm_call_result_name(result), (unsigned int)calls[side].stage);
			if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
				CHECK(event.kind == PCM_CALL_DATA_NONE &&
				      calls[side].stage == PCM_CALL_CLOSED &&
				      !pcm_call_is_online(&calls[side]) &&
				      !calls[side].b1_deadline_armed &&
				      !calls[side].rate_renegotiation_deadline_armed,
				      "cleardown retained online data or an armed deadline");
				complete[side] = true;
			}
		}
		channel_transmit(&channel, transmitted[0], transmitted[1]);
	}
	CHECK(complete[0] && complete[1],
	      "law %u initiator %u did not close both calls: %u/%u",
	      (unsigned int)analog->law, initiator, complete[0], complete[1]);
	for (unsigned int side = 0; side < 2u; ++side) {
		struct pcm_call before;
		struct pcm_call_data_event data_event;
		struct pcm_call_observation sample_event;
		const struct pcm_call_data_sources sources = {
			.downstream = source_downstream_frame,
			.upstream = source_upstream_frame,
		}; /* NULL contexts fail if a closed call attempts to source data. */
		uint8_t output = 0u;

		memcpy(&before, &calls[side], sizeof(before));
		CHECK(pcm_call_data_octet(&calls[side], 0u, INT16_MAX, &sources,
					 &output, &data_event) == PCM_CALL_CLEARDOWN_COMPLETE &&
		      output == silence_octet(calls[side].law) &&
		      data_event.kind == PCM_CALL_DATA_NONE &&
		      pcm_call_octet(&calls[side], 0u, INT16_MAX, &output,
				     &sample_event) == PCM_CALL_CLEARDOWN_COMPLETE &&
		      output == silence_octet(calls[side].law) &&
		      sample_event.processed == PCM_CALL_CLOSED &&
		      sample_event.next == PCM_CALL_CLOSED &&
		      pcm_call_start_retrain(&calls[side]) == PCM_CALL_INVALID_STATE &&
		      pcm_call_start_cleardown(&calls[side]) == PCM_CALL_OK &&
		      memcmp(&before, &calls[side], sizeof(before)) == 0,
		      "closed call consumed input, advanced state, or restarted");
	}
	return true;
}

static bool check_training_cleardown(
	const struct pcm_call *analog, const struct pcm_call *digital,
	const struct channel *line)
{
	struct pcm_call calls[2] = { *analog, *digital };
	struct channel channel = *line;
	unsigned int checked = 0u;

	CHECK(check_peer_cleardown(analog, digital, line, 0u) &&
	      check_peer_cleardown(analog, digital, line, 1u),
	      "initial control cancellation failed");
	for (size_t clock = 0; clock < 16000u && checked != 3u; ++clock) {
		uint8_t received[2], transmitted[2];
		const struct v90_digital_path *path = &calls[1].state.data_path.digital;
		const struct pcm_downstream_tx *tx = &path->downstream.training;

		CHECK(path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING,
		      "control cancellation fixture left training too early");
		unsigned int bit = tx->stage == PCM_DOWNSTREAM_TX_MP ? 1u :
			tx->stage == PCM_DOWNSTREAM_TX_MP_PRIME ? 2u : 0u;

		if (bit != 0u && !(checked & bit) && tx->control_bit_index != 0u) {
			CHECK(check_peer_cleardown(&calls[0], &calls[1], &channel, 1u),
			      "mid-MP cancellation failed at stage %u", (unsigned int)tx->stage);
			if (bit == 1u)
				CHECK(check_peer_cleardown(&calls[0], &calls[1], &channel, 0u),
				      "mid-CP cancellation failed");
			checked |= bit;
		}
		channel_receive(&channel, &received[0], &received[1]);
		for (unsigned int side = 0; side < 2u; ++side) {
			struct pcm_call_data_event event;
			CHECK(pcm_call_data_octet(&calls[side], received[side],
				      decode_octet(calls[side].law, received[side]), NULL,
				      &transmitted[side], &event) == PCM_CALL_OK,
			      "control cancellation fixture stopped at %zu", clock);
		}
		channel_transmit(&channel, transmitted[0], transmitted[1]);
	}
	CHECK(checked == 3u, "control cancellation missed MP or MP-prime");
	return true;
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
						&cp_receive, sample, &received, NULL);

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
				struct v90_mp_record zero = path->local_mp;
				struct v90_mp_parameters *parameters =
					zero.type == V90_MP_RECORD_TYPE0 ?
					&zero.parameters.type0 : &zero.parameters.type1.common;

				parameters->maximum_upstream_rate_2400_bps_units = 0u;
				CHECK(v90_mp_record_equal(
					      &received.mp.record, &zero),
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

enum call_impairment {
	CALL_CHANNEL_CLEAN,
	CALL_CODEWORD_REPLACED,
	CALL_INITIAL_TONE_B_LOST,
	CALL_INITIAL_TONE_A_LOST,
	CALL_INITIAL_INFO1D_LOST,
	CALL_INITIAL_L1_LOST,
	CALL_RECIPROCAL_L1_LOST,
};

static bool run_configured_call(
	enum g711_law law, size_t delay, enum call_goal goal,
	uint32_t analog_rate_mask, uint8_t analog_selected_rate_index,
	uint32_t digital_rate_mask, uint32_t expected_downstream_rate,
	enum call_impairment impairment)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool milestone_reached = false;
	bool probe_interrupted = false;
	bool ranging_recovered = false;
	bool reversal_recovered = false;
	bool info1_interrupted = false;
	bool info1_retrained = false;
	bool probe_retrained = false;
	size_t lost_a_samples = 0;
	unsigned int replacement_stages = 0;

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
		if (impairment == CALL_INITIAL_TONE_B_LOST &&
		    analog.call.stage == PCM_CALL_PHASE2) {
			if (analog.call.state.phase2.stage == PCM_PHASE2_INITIAL_PROBE)
				probe_interrupted = true;
			else if (probe_interrupted &&
				 analog.call.state.phase2.stage == PCM_PHASE2_RANGING)
				ranging_recovered = true;
			if (probe_interrupted && !ranging_recovered)
				analog_received = silence_octet(law);
		}
		if (impairment == CALL_INITIAL_TONE_A_LOST) {
			if (digital.call.stage == PCM_CALL_PHASE2 &&
			    digital.call.state.phase2.stage == PCM_PHASE2_PROBE_TURNAROUND &&
			    digital.call.state.phase2.state.turnaround.control.phase ==
				    PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY &&
			    digital.call.state.phase2.state.turnaround.control.phase_samples_remaining == 320u)
				reversal_recovered = true;
			if (digital.call.stage == PCM_CALL_PHASE2 &&
			    (digital.call.state.phase2.stage == PCM_PHASE2_PROBE_TURNAROUND ||
			     (digital.call.state.phase2.stage == PCM_PHASE2_INITIAL_PROBE &&
			      digital.call.state.phase2.state.initial_probe.control.phase ==
				      PHASE2_INITIAL_PROBE_SIGNAL_B_TONE)))
				probe_interrupted = true;
			if (probe_interrupted && lost_a_samples < 8000u) {
				digital_received = silence_octet(law);
				lost_a_samples++;
			}
		}
		if (impairment == CALL_INITIAL_INFO1D_LOST &&
		    analog.call.stage == PCM_CALL_PHASE2 &&
		    analog.call.state.phase2.stage == PCM_PHASE2_INFO1 &&
		    !analog.call.retrain_active) {
			analog_received = silence_octet(law);
			info1_interrupted = true;
		}
		if (impairment == CALL_INITIAL_L1_LOST ||
		    impairment == CALL_RECIPROCAL_L1_LOST) {
			bool initial = impairment == CALL_INITIAL_L1_LOST;
			const struct pcm_call *receiver = initial ?
				&digital.call : &analog.call;
			enum pcm_phase2_stage stage = initial ?
				PCM_PHASE2_INITIAL_PROBE : PCM_PHASE2_RECIPROCAL_PROBE;

			if (receiver->stage == PCM_CALL_PHASE2 &&
			    receiver->state.phase2.stage == stage && !probe_retrained) {
				if (initial)
					digital_received = silence_octet(law);
				else
					analog_received = silence_octet(law);
				probe_interrupted = true;
			}
		}
		analog_received_sample = decode_octet(law, analog_received);
		digital_received_sample = decode_octet(law, digital_received);
		if (impairment == CALL_CODEWORD_REPLACED &&
		    (pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE2 ||
		     pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE3 ||
		     pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE3_MIXED ||
		     pcm_call_current_stage(&digital.call) == PCM_CALL_PHASE4 ||
		     pcm_call_current_stage(&digital.call) == PCM_CALL_DATA_PATH)) {
			uint8_t replacement = silence_octet(law);

			if (replacement != digital_received)
				replacement_stages |= 1u << digital.call.stage;
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
		info1_retrained |= info1_interrupted &&
			analog.call.stage == PCM_CALL_RETRAIN_SILENCE;
		if (impairment == CALL_INITIAL_L1_LOST ||
		    impairment == CALL_RECIPROCAL_L1_LOST)
			probe_retrained |= probe_interrupted &&
				(impairment == CALL_INITIAL_L1_LOST ?
					digital.call.stage == PCM_CALL_RETRAIN_SILENCE :
					analog.call.stage == PCM_CALL_RETRAIN_SILENCE);
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		if (goal_is_reached(goal, &analog, &digital)) {
			milestone_reached = true;
			break;
		}
	}
	CHECK(impairment != CALL_INITIAL_TONE_B_LOST || ranging_recovered,
	      "lost Tone B did not exercise initial-probe ranging recovery");
	CHECK(impairment != CALL_INITIAL_TONE_A_LOST ||
	      (lost_a_samples == 8000u && reversal_recovered),
	      "lost Tone A did not exercise the one-second loss and timed reversal recovery");
	CHECK(impairment != CALL_INITIAL_INFO1D_LOST ||
	      (info1_interrupted && info1_retrained),
	      "lost INFO1d did not exercise analogue timeout retraining");
	CHECK((impairment != CALL_INITIAL_L1_LOST &&
	       impairment != CALL_RECIPROCAL_L1_LOST) ||
	      (probe_interrupted && probe_retrained),
	      "lost L1 did not exercise full-retrain recovery");
	CHECK(impairment != CALL_CODEWORD_REPLACED ||
	      replacement_stages == ((1u << PCM_CALL_PHASE2) |
				     (1u << PCM_CALL_PHASE3) |
				     (1u << PCM_CALL_PHASE3_MIXED) |
				     (1u << PCM_CALL_PHASE4) |
				     (1u << PCM_CALL_DATA_PATH)),
	      "law %u physical receive did not exercise every owned stage: %x",
	      (unsigned int)law, replacement_stages);
	CHECK(milestone_reached &&
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
	if (goal == CALL_GOAL_DATA_PATH) {
		CHECK(!pcm_call_is_online(&analog.call) &&
		      !pcm_call_is_online(&digital.call) &&
		      check_training_cleardown(&analog.call, &digital.call, &channel),
		      "initial rate exchange did not honor local cleardown");
	}
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
		CHECK(check_peer_cleardown(&analog.call, &digital.call, &channel, 0u) &&
		      check_peer_cleardown(&analog.call, &digital.call, &channel, 1u),
		      "online V.90 endpoints did not accept peer zero-rate cleardown");
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
		V90_JD_RATE_CAPABILITY_MASK, DATA_DOWNSTREAM_RATE, CALL_CHANNEL_CLEAN);
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

static bool test_negotiated_zero_symbols(void)
{
	static const struct {
		enum v34_symbol_rate baud;
		uint8_t rate_index;
		bool training;
	} cases[] = {
		{ V34_SYMBOL_RATE_3200, 3u, false },
		{ V34_SYMBOL_RATE_3200, 1u, true },
		{ V34_SYMBOL_RATE_3200, 3u, true },
		{ V34_SYMBOL_RATE_3000, 1u, true },
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t trial = 0u; trial < ARRAY_LENGTH(cases); ++trial) {
			const bool training = cases[trial].training;
			struct channel channel;
			struct endpoint analog = { .upstream_received_delay = UINT16_MAX };
			struct endpoint digital;
			struct pcm_call_configuration selected =
				configuration(V90_ANALOG_MODEM, law);
			struct v90_cp_parameters *parameters[] = {
				&selected.analog_phase4.data_parameters,
				&selected.analog_phase4.training_parameters,
			};
			size_t zero_symbols[2][2] = { { 0 } };
			size_t zero_frame_ends[2] = { 0 };

			selected.local_info1.analog.upstream_symbol_rate = cases[trial].baud;
			/* 30,667 bit/s uses all 8^6 modulus tuples, including zero
			 * in the last interval, where host frames are emitted. Also
			 * cover zero in training with the default 28,000-bit/s data map. */
			selected.enabled_downstream_rate_mask =
				UINT32_C(1) << (cases[trial].rate_index - 1u);
			selected.analog_phase4.data_parameters.selected_rate_index =
				cases[trial].rate_index;
			/* Ucode zero is a legal CP constellation point, not necessarily
			 * silence. Keep the cardinality and rate unchanged. */
			for (unsigned int index = 0u; index <= training; ++index) {
				struct pcm_constellation *set =
					&parameters[index]->transmit_constellations[0];

				set->ucode_mask[24u / 8u] &= (uint8_t)~1u;
				add_ucode(set, 0u);
			}
			CHECK(channel_init(&channel, law, 3u) &&
			      pcm_call_init(&analog.call, &selected) == PCM_CALL_OK &&
			      endpoint_init(&digital, V90_DIGITAL_MODEM, law,
					    V90_JD_RATE_CAPABILITY_MASK, 0u),
			      "zero-symbol endpoints did not initialize");
			for (size_t clock = 0u; clock < CALL_SAMPLE_LIMIT &&
			     analog.downstream_frames < 64u; ++clock) {
				if (analog.call.stage == PCM_CALL_DATA_PATH) {
					struct v90_pcm_symbol symbol;
					bool data = analog.call.state.data_path.analog
						.downstream_stage == V90_ANALOG_PATH_DOWNSTREAM_DATA;

					CHECK(v90_pcm_symbol_parse(&symbol, law,
						      channel.digital_to_analog[channel.cursor]),
					      "zero-symbol input did not parse");
					if (symbol.ucode == 0u) {
						zero_symbols[data][symbol.positive]++;
						if (data && pcm_call_is_online(&analog.call) &&
						    analog.call.state.data_path.analog.downstream.data
							    .stream.pending_count == V90_CP_FRAME_INTERVALS - 1u)
							zero_frame_ends[symbol.positive]++;
					}
				}
				CHECK(step_endpoints(&channel, &analog, &digital, law),
				      "law %u case %zu zero-symbol call stopped at %zu "
				      "stages %u/%u frames %zu/%zu failed %u/%u",
				      (unsigned int)law, trial, clock,
				      analog.call.stage, digital.call.stage,
				      analog.downstream_frames, digital.upstream_frames,
				      analog.call.failed, digital.call.failed);
			}
			CHECK(goal_is_reached(CALL_GOAL_ONLINE_DATA, &analog, &digital) &&
			      analog.downstream_frames == 64u &&
			      zero_symbols[1][0] != 0u && zero_symbols[1][1] != 0u &&
			      (training || (zero_frame_ends[0] != 0u && zero_frame_ends[1] != 0u)) &&
			      (!training || zero_symbols[0][0] + zero_symbols[0][1] != 0u),
			      "law %u case %zu lost negotiated zero symbols or ordered data: "
			      "stages %u/%u frames %zu/%zu zero %zu/%zu/%zu/%zu",
			      (unsigned int)law, trial, analog.call.stage, digital.call.stage,
			      analog.downstream_frames, digital.upstream_frames,
			      zero_symbols[0][0], zero_symbols[0][1],
			      zero_symbols[1][0], zero_symbols[1][1]);
		}
	}
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

	if (!call_is_in_info1_exchange(call) || call->retrain_active ||
	    call->local_role != V90_DIGITAL_MODEM)
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

static bool info1_timeout_restarts_analog(
	const struct pcm_call *initial, enum g711_law law)
{
	struct pcm_call call = *initial;
	const struct phase2_info1_exchange *exchange =
		&initial->state.phase2.state.info1.control;
	struct v34_retrain_detector detector;
	bool detected;
	bool qualified = false;
	uint32_t remaining = PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES +
		2u * exchange->round_trip_delay_samples -
		exchange->peer_deadline_elapsed_samples;

	CHECK(initial->local_role == V90_ANALOG_MODEM &&
	      exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER && remaining > 0,
	      "analogue INFO1 timeout fixture was not ready");
	for (uint32_t sample = 0; sample < remaining; ++sample) {
		struct pcm_call_observation observation;
		uint8_t received = silence_octet(law);
		uint8_t transmitted;
		bool expired = sample + 1u == remaining;

		CHECK(pcm_call_octet(
			      &call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      observation.processed == PCM_CALL_PHASE2 &&
		      observation.next == (expired ? PCM_CALL_RETRAIN_SILENCE :
					     PCM_CALL_PHASE2) &&
		      (observation.phase2.kind ==
			       PCM_PHASE2_EVENT_INFO1_RECOVERY_STARTED) == expired,
		      "analogue INFO1 timeout missed its deadline at %u/%u",
		      sample + 1u, remaining);
	}
	CHECK(call.retrain_active && !call.retrain.peer_tone_retained &&
	      call.peer_info0_stored &&
	      memcmp(&call.peer_info0, &initial->peer_info0,
		     sizeof(call.peer_info0)) == 0 &&
	      call.round_trip_delay_samples == initial->round_trip_delay_samples &&
	      v34_retrain_detector_init(&detector, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "analogue INFO1 timeout lost its retrain agreement");
	for (size_t sample = 0;
	     sample < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++sample) {
		struct pcm_call_observation observation;
		uint8_t received = silence_octet(law);
		uint8_t transmitted;

		CHECK(pcm_call_octet(
			      &call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      observation.processed == PCM_CALL_RETRAIN_SILENCE &&
		      transmitted == received &&
		      v34_retrain_detector_sample(
			      &detector, decode_octet(law, transmitted), &detected) ==
			      V34_PHASE2_TONE_OK && !detected,
		      "analogue INFO1 timeout silence failed at %zu", sample);
	}
	CHECK(call.stage == PCM_CALL_PHASE2 &&
	      call.state.phase2.stage == PCM_PHASE2_RANGING &&
	      call.protocol == initial->protocol &&
	      call.state.phase2.info0_protocol == initial->protocol,
	      "analogue INFO1 timeout did not restart ranging");
	for (size_t sample = 0;
	     sample < V34_RETRAIN_TONE_SAMPLES + 2u * V34_PHASE2_TONE_WINDOW_SAMPLES;
	     ++sample) {
		struct pcm_call_observation observation;
		uint8_t received = silence_octet(law);
		uint8_t transmitted;

		CHECK(pcm_call_octet(
			      &call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      v34_retrain_detector_sample(
			      &detector, decode_octet(law, transmitted), &detected) ==
			      V34_PHASE2_TONE_OK,
		      "analogue INFO1 timeout Tone A failed at %zu", sample);
		qualified |= detected;
	}
	CHECK(qualified, "analogue INFO1 timeout sent no qualifying retrain");
	return true;
}

static bool info1_retrain_preamble(
	const struct pcm_call *initial, enum g711_law law,
	uint32_t remaining_samples)
{
	struct pcm_call call = *initial;
	struct v34_phase2_tone_transmitter peer_tone;
	bool before_deadline = false;
	bool recovered = false;
	bool responded = false;

	CHECK(v34_phase2_tone_transmitter_init(
		      &peer_tone, call.local_role == V90_DIGITAL_MODEM ?
			      V34_ANSWERING_STATION : V34_CALLING_STATION,
		      PHASE2_NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "INFO1 boundary peer tone did not initialize");
	/* The peer's silence arrives while INFO1 is still outstanding. Begin
	 * its tone before the local deadline, without seeding a detector. */
	for (size_t sample = 0; sample < CALL_SAMPLE_LIMIT; ++sample) {
		const struct phase2_info1_exchange *exchange =
			&call.state.phase2.state.info1.control;
		uint32_t deadline = call.local_role == V90_DIGITAL_MODEM ?
			(uint32_t)PHASE2_INFO1_SIGNAL_B_WAIT_BASE_SAMPLES +
				exchange->round_trip_delay_samples :
			PHASE2_INFO1_SIGNAL_A_WAIT_BASE_SAMPLES +
				2u * exchange->round_trip_delay_samples;
		struct pcm_call_observation observation;
		uint8_t received = silence_octet(law);
		uint8_t transmitted;

		if (exchange->phase == PHASE2_INFO1_WAITING_FOR_PEER &&
		    deadline - exchange->peer_deadline_elapsed_samples ==
			    remaining_samples) {
			before_deadline = true;
			break;
		}
		CHECK(pcm_call_octet(
			      &call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      call_is_in_info1_exchange(&call),
		      "INFO1 boundary silence failed at %zu", sample);
	}
	CHECK(before_deadline, "INFO1 boundary deadline was not reached");
	for (size_t sample = 0;
	     sample < V34_RETRAIN_TONE_SAMPLES +
			      2u * V34_PHASE2_TONE_WINDOW_SAMPLES; ++sample) {
		struct pcm_call_observation observation;
		int16_t remote_sample;
		uint8_t received;
		uint8_t transmitted;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &remote_sample) == V34_PHASE2_TONE_OK,
		      "INFO1 boundary tone failed at %zu", sample);
		received = encode_octet(law, remote_sample);
		CHECK(pcm_call_octet(
			      &call, received, decode_octet(law, received),
			      &transmitted, &observation) == PCM_CALL_OK,
		      "INFO1 boundary call failed at %zu", sample);
		recovered |= observation.phase2.kind ==
			PCM_PHASE2_EVENT_INFO1_RECOVERY_STARTED;
		if (call.stage == PCM_CALL_RETRAIN_SILENCE) {
			responded = true;
			break;
		}
	}
	CHECK(recovered == (remaining_samples < V34_RETRAIN_TONE_SAMPLES) &&
	      responded && call.retrain_active &&
	      call.retrain.peer_tone_retained ==
		      !(recovered && initial->local_role == V90_ANALOG_MODEM) &&
	      pcm_call_is_valid(&call),
	      "law %u role %u lost pre-timeout silence: recovery=%d response=%d",
	      (unsigned int)law, (unsigned int)initial->local_role,
	      recovered, responded);
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
	CHECK(info1_timeout_restarts_analog(&retained[0], law),
	      "law %u analogue INFO1 timeout failed", (unsigned int)law);
	for (size_t role = 0; role < ARRAY_LENGTH(retained); ++role)
		CHECK(info1_retrain_preamble(
			      &retained[role], law, 2u * V34_RETRAIN_TONE_SAMPLES) &&
		      info1_retrain_preamble(
			      &retained[role], law,
			      V34_PHASE2_TONE_ACQUIRE_SAMPLES / 2u),
		      "law %u role %zu INFO1 preamble failed",
		      (unsigned int)law, role);
	marks = retained[1];
	CHECK(initial_info1_marks_request_retrain(&marks, law) &&
	      force_initial_info1_recovery(&retained[1], law) &&
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

static bool test_v92_info1_timeout(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_call_configuration analog_configuration =
			v92_configuration(V90_ANALOG_MODEM, law);
		struct pcm_call_configuration digital_configuration =
			v92_configuration(V90_DIGITAL_MODEM, law);
		struct pcm_call analog;
		struct pcm_call digital;
		struct channel channel;
		bool waiting = false;

		CHECK(channel_init(&channel, law, 3u) &&
		      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
		      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
		      "V.92 INFO1 timeout calls did not initialize");
		for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
			struct pcm_call_observation observation;
			uint8_t analog_received, digital_received;
			uint8_t analog_transmitted, digital_transmitted;

			channel_receive(&channel, &analog_received, &digital_received);
			CHECK(pcm_call_octet(
				      &analog, analog_received, decode_octet(law, analog_received),
				      &analog_transmitted, &observation) == PCM_CALL_OK &&
			      pcm_call_octet(
				      &digital, digital_received, decode_octet(law, digital_received),
				      &digital_transmitted, &observation) == PCM_CALL_OK,
			      "V.92 INFO1 timeout setup failed at %zu", clock);
			channel_transmit(&channel, analog_transmitted, digital_transmitted);
			if (call_is_in_info1_exchange(&analog)) {
				waiting = true;
				break;
			}
		}
		CHECK(waiting && info1_timeout_restarts_analog(&analog, law),
		      "law %u V.92 full INFO1 timeout failed", (unsigned int)law);
	}
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
	       expected_stage == PCM_CALL_PHASE4 ||
	       expected_stage == PCM_CALL_DATA_PATH) &&
	      call->stage == expected_stage && !call->retrain_active &&
	      call->peer_info0_stored && call->round_trip_delay_stored &&
	      pcm_call_is_valid(call) && !pcm_call_is_online(call),
	      "law %u role %u stage %u is not final training",
	      (unsigned int)law, (unsigned int)call->local_role,
	      (unsigned int)expected_stage);
	if (expected_stage == PCM_CALL_DATA_PATH) {
		enum v34_station_role peer_role =
			call->local_role == V90_ANALOG_MODEM ?
				V34_CALLING_STATION : V34_ANSWERING_STATION;
		const size_t quiet = V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES;
		const size_t limit = quiet + V34_RETRAIN_TONE_SAMPLES +
				     V34_PHASE2_TONE_WINDOW_SAMPLES;

		CHECK(v34_phase2_tone_transmitter_init(
			      &remote_tone, peer_role, PHASE2_NOMINAL_AMPLITUDE) ==
			      V34_PHASE2_TONE_OK,
		      "could not initialize final-training peer tone");
		/* Feed the complete physical request through the live call owner;
		 * do not seed its detector or skip the CP/E/B1 receive path. */
		for (size_t index = 0; index < limit; ++index) {
			struct pcm_call_data_event event;
			int16_t sample = 0;
			uint8_t received;

			if (index >= quiet)
				CHECK(v34_phase2_tone_transmitter_sample(
					      &remote_tone, &sample) == V34_PHASE2_TONE_OK,
				      "final-training peer tone stopped");
			received = encode_octet(law, sample);
			CHECK(pcm_call_data_octet(
				      call, received, decode_octet(law, received),
				      NULL, &transmitted, &event) == PCM_CALL_OK,
			      "law %u role %u retrain input failed at %zu",
			      (unsigned int)law, (unsigned int)call->local_role, index);
			if (call->stage == PCM_CALL_RETRAIN_SILENCE) {
				CHECK(index >= quiet + V34_RETRAIN_TONE_SAMPLES - 1u,
				      "final training retrained before the complete guard");
				break;
			}
		}
	} else {
		CHECK(form_remote_retrain_trigger(
			      call, law, &remote_tone, &trigger_octet, &trigger_sample) &&
		      pcm_call_octet(
			      call, trigger_octet, trigger_sample, &transmitted,
			      &observation) == PCM_CALL_OK &&
		      observation.processed == expected_stage &&
		      observation.next == PCM_CALL_RETRAIN_SILENCE,
		      "could not deliver final-training retrain trigger");
	}
	CHECK(call->stage == PCM_CALL_RETRAIN_SILENCE &&
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

static int tracked_final_training_stage(const struct pcm_call *call)
{
	if (call->stage == PCM_CALL_PHASE3)
		return 0;
	if (call->stage == PCM_CALL_PHASE3_MIXED)
		return 1;
	if (call->stage == PCM_CALL_PHASE4)
		return 2;
	if (call->stage != PCM_CALL_DATA_PATH)
		return -1;
	if (call->local_role == V90_ANALOG_MODEM) {
		const struct v90_analog_path *path = &call->state.data_path.analog;

		if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_CP)
			return 3;
		if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_E)
			return 4;
		if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_DATA &&
		    path->upstream.data.b1_active)
			return 5;
	} else {
		const struct v90_digital_path *path = &call->state.data_path.digital;

		if (path->upstream_stage != V90_DIGITAL_PATH_UPSTREAM_STARTUP)
			return -1;
		if (path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_CP)
			return 3;
		if (path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_E)
			return 4;
		if (path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_B1)
			return 5;
	}
	return -1;
}

static bool check_final_training_remote_retrain(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool checked[2][6] = { { false } };
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
	     clock < CALL_SAMPLE_LIMIT && checked_count != 12u; ++clock) {
		struct endpoint *endpoints[] = { &analog, &digital };

		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u final-training retrain setup stopped at %zu",
		      (unsigned int)law, clock);
		for (size_t role = 0; role < ARRAY_LENGTH(endpoints); ++role) {
			struct pcm_call candidate;
			int stage = tracked_final_training_stage(
				&endpoints[role]->call);

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
	CHECK(checked_count == 12u,
	      "law %u did not exercise every final-training stage (%zu/12)",
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

static bool digital_is_sending_ri_bar(const struct pcm_call *call)
{
	const struct v90_phase4_digital_startup *startup;
	const struct v90_phase4_ri_tx *transmit;

	if (call->local_role != V90_DIGITAL_MODEM ||
	    call->stage != PCM_CALL_PHASE4)
		return false;
	startup = &call->state.phase4.digital;
	if (startup->stage != V90_PHASE4_STARTUP_INITIAL &&
	    startup->stage != V90_PHASE4_STARTUP_CONTINUING)
		return false;
	transmit = startup->stage == V90_PHASE4_STARTUP_INITIAL ?
		&startup->state.initial.transmit :
		&startup->state.continuing.station.transmit;
	return transmit->stage == V90_PHASE4_RI_BAR ||
	       (transmit->stage == V90_PHASE4_RI &&
		transmit->finish_requested &&
		transmit->ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
		transmit->phase == 0u);
}

static bool missing_ri_bar_recovers_data(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	size_t erased = 0u;
	bool analog_timed_out = false;
	bool digital_timed_out = false;

	CHECK(channel_init(&channel, law, 13u) &&
	      endpoint_init(&analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(&digital, V90_DIGITAL_MODEM, law,
			    V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "lost Ri-bar endpoints did not initialize");
	/* Lose only the first 24T reversal, then allow real audio time and both
	 * reacting call owners to recover. Do not seed detectors, shorten a
	 * deadline, restart either peer, or skip their second handshake. */
	for (size_t clock = 0u; clock < 3u * CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t analog_received, digital_received;
		uint8_t analog_transmitted, digital_transmitted;
		uint32_t analog_remaining = analog.call.b1_deadline_remaining_samples;
		uint32_t digital_remaining = digital.call.b1_deadline_remaining_samples;
		bool erase = erased < V90_PHASE4_RI_BAR_SYMBOLS &&
			digital_is_sending_ri_bar(&digital.call);

		channel_receive(&channel, &analog_received, &digital_received);
		CHECK(endpoint_octet(
			      &analog, analog_received, decode_octet(law, analog_received),
			      law, &analog_transmitted) &&
		      endpoint_octet(
			      &digital, digital_received, decode_octet(law, digital_received),
			      law, &digital_transmitted),
		      "law %u lost Ri-bar call stopped at %zu", (unsigned int)law, clock);
		if (erase) {
			/* Both G.711 laws put the sign in bit 7. Turn Ri-bar back into
			 * uninterrupted Ri without changing its amplitude or duration. */
			digital_transmitted ^= 0x80u;
			erased++;
		}
		if (!analog_timed_out && analog.call.stage == PCM_CALL_RETRAIN_SILENCE) {
			CHECK(analog_remaining == 1u && analog.call.peer_info0_stored &&
			      !analog.call.b1_deadline_armed,
			      "analogue missed the natural B1 timeout or lost INFO0");
			analog_timed_out = true;
		}
		if (!digital_timed_out && digital.call.stage == PCM_CALL_RETRAIN_SILENCE) {
			CHECK(digital_remaining == 1u && digital.call.peer_info0_stored &&
			      !digital.call.b1_deadline_armed,
			      "digital missed the natural B1 timeout or lost INFO0");
			digital_timed_out = true;
		}
		channel_transmit(&channel, analog_transmitted, digital_transmitted);
		if (goal_is_reached(CALL_GOAL_ONLINE_DATA, &analog, &digital)) {
			CHECK(erased == V90_PHASE4_RI_BAR_SYMBOLS &&
			      analog_timed_out && digital_timed_out &&
			      pcm_call_is_online(&analog.call) &&
			      pcm_call_is_online(&digital.call) &&
			      !analog.call.retrain_active && !digital.call.retrain_active &&
			      !analog.call.b1_deadline_armed && !digital.call.b1_deadline_armed,
			      "lost Ri-bar did not retrain and resume both data directions");
			return true;
		}
	}
	CHECK(false, "law %u lost Ri-bar did not recover: erased=%zu stages=%u/%u",
	      (unsigned int)law, erased, analog.call.stage, digital.call.stage);
}

static bool test_missing_ri_bar(void)
{
	return missing_ri_bar_recovers_data(G711_LAW_PCMU) &&
	       missing_ri_bar_recovers_data(G711_LAW_PCMA);
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

static bool run_probe_response_timeout(
	enum pcm_protocol protocol, enum g711_law law, size_t delay,
	enum v90_modem_role role)
{
	struct pcm_call_configuration analog_configuration =
		protocol == PCM_PROTOCOL_V90 ?
			configuration(V90_ANALOG_MODEM, law) :
			v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		protocol == PCM_PROTOCOL_V90 ?
			configuration(V90_DIGITAL_MODEM, law) :
			v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call analog;
	struct pcm_call digital;
	struct pcm_call waiting;
	const struct pcm_call *source =
		role == V90_ANALOG_MODEM ? &analog : &digital;
	struct channel channel;
	uint8_t silence = silence_octet(law);
	uint32_t deadline;
	bool probe_started = false;

	CHECK(channel_init(&channel, law, delay) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "probe-timeout endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_call_observation observed;
		uint8_t analog_received, digital_received;
		uint8_t analog_transmitted, digital_transmitted;

		channel_receive(&channel, &analog_received, &digital_received);
		CHECK(pcm_call_octet(
			      &analog, analog_received,
			      decode_octet(law, analog_received),
			      &analog_transmitted, &observed) == PCM_CALL_OK &&
		      pcm_call_octet(
			      &digital, digital_received,
			      decode_octet(law, digital_received),
			      &digital_transmitted, &observed) == PCM_CALL_OK,
		      "probe-timeout setup stopped at %zu", clock);
		channel_transmit(&channel, analog_transmitted, digital_transmitted);
		if (source->stage == PCM_CALL_PHASE2 &&
		    source->state.phase2.stage == (role == V90_ANALOG_MODEM ?
			    PCM_PHASE2_INITIAL_PROBE : PCM_PHASE2_RECIPROCAL_PROBE)) {
			probe_started = true;
			break;
		}
	}
	CHECK(probe_started && source->round_trip_delay_stored &&
	      (role == V90_ANALOG_MODEM ?
		      source->state.phase2.state.initial_probe.control.path.transmit
			      .samples_emitted :
		      source->state.phase2.state.reciprocal_probe.control.path.transmit
			      .samples_emitted) == 0u,
	      "probe-timeout test missed the first L1 sample");
	waiting = *source;
	/* V.90 9.2.1.2.5 / 9.2.2.2.3: 650 / 600 ms plus RTD after L1. The
	 * shared PCM Phase 2 must expire naturally, without rebasing counters. */
	deadline = (160u + (role == V90_ANALOG_MODEM ? 600u : 650u)) * 8u +
		waiting.round_trip_delay_samples;
	for (uint32_t elapsed = 1u; elapsed <= deadline; ++elapsed) {
		struct pcm_call_observation observed;
		uint8_t transmitted;
		enum pcm_call_result result = pcm_call_octet(
			&waiting, silence, decode_octet(law, silence),
			&transmitted, &observed);

		CHECK(result == PCM_CALL_OK &&
		      observed.processed == PCM_CALL_PHASE2 &&
		      observed.next == (elapsed < deadline || role == V90_ANALOG_MODEM ?
			      PCM_CALL_PHASE2 : PCM_CALL_RETRAIN_SILENCE),
		      "protocol %u role %u law %u delay %zu probe deadline %u/%u: %s",
		      (unsigned int)protocol, (unsigned int)role, (unsigned int)law, delay,
		      elapsed, deadline, pcm_call_result_name(result));
		if (elapsed > (160u + 550u) * 8u + waiting.round_trip_delay_samples)
			CHECK(transmitted == silence,
			      "probe exceeded its L2 wire limit");
		if (elapsed == deadline && role == V90_DIGITAL_MODEM)
			CHECK(observed.phase2.kind == PCM_PHASE2_EVENT_FAILED &&
			      observed.phase2.failure ==
				      PCM_PHASE2_FAILURE_LOCAL_PROBE_LIMIT,
			      "probe deadline lost its failure observation");
	}
	if (role == V90_ANALOG_MODEM) {
		const struct v90_phase2_ranging *ranging =
			&waiting.state.phase2.state.ranging;
		struct v34_phase2_tone_transmitter expected =
			ranging->info0_exchange.transport.tone_transmitter;

		CHECK(waiting.stage == PCM_CALL_PHASE2 &&
		      waiting.state.phase2.stage == PCM_PHASE2_RANGING &&
		      ranging->control.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		      ranging->control.local_tone_samples == 0u &&
		      ranging->control.prior_round_trip_delay_samples ==
			      source->round_trip_delay_samples &&
		      !ranging->control.round_trip_delay_valid &&
		      !ranging->info0_exchange.transport.tone_detector.carrier.tone_present &&
		      !waiting.retrain_active && !waiting.failed &&
		      waiting.protocol == protocol && waiting.peer_info0_stored &&
		      memcmp(&waiting.peer_info0, &source->peer_info0,
			     sizeof(waiting.peer_info0)) == 0,
		      "unanswered initial probe did not restart a fresh ranging epoch");
		for (size_t index = 0; index < 400u; ++index) {
			struct pcm_call_observation observed;
			uint8_t transmitted;
			int16_t tone;

			CHECK(v34_phase2_tone_transmitter_sample(&expected, &tone) ==
				      V34_PHASE2_TONE_OK &&
			      pcm_call_octet(&waiting, silence, decode_octet(law, silence),
				      &transmitted, &observed) == PCM_CALL_OK &&
			      transmitted == encode_octet(law, tone),
			      "initial-probe recovery did not send Tone A immediately");
		}
		struct v34_phase2_tone_transmitter tone_b;
		bool reversed = false;

		CHECK(v34_phase2_tone_transmitter_init(
			      &tone_b, V34_CALLING_STATION, 12000) == V34_PHASE2_TONE_OK,
		      "recovery Tone B did not initialize");
		for (size_t index = 0; index < 400u; ++index) {
			struct pcm_call_observation observed;
			uint8_t transmitted;
			int16_t tone;

			CHECK(v34_phase2_tone_transmitter_sample(&tone_b, &tone) == V34_PHASE2_TONE_OK,
			      "recovery Tone B generation failed");
			uint8_t received = encode_octet(law, tone);
			CHECK(pcm_call_octet(&waiting, received, decode_octet(law, received),
				      &transmitted, &observed) == PCM_CALL_OK,
			      "initial-probe recovery did not accept fresh Tone B");
			if (ranging->control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
				reversed = true;
				break;
			}
		}
		CHECK(reversed && ranging->control.local_tone_samples == 400u &&
		      ranging->control.measurement_elapsed_samples != 0u,
		      "initial-probe recovery did not begin first ranging");
		return true;
	}
	CHECK(waiting.retrain_active && !waiting.retrain.peer_tone_retained &&
	      !waiting.failed && waiting.peer_info0_stored &&
	      waiting.round_trip_delay_samples == digital.round_trip_delay_samples &&
	      memcmp(&waiting.peer_info0, &digital.peer_info0,
		     sizeof(waiting.peer_info0)) == 0,
	      "probe timeout lost the established call agreement");
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
		struct pcm_call_observation observed;
		uint8_t transmitted;

		CHECK(pcm_call_octet(
			      &waiting, silence, decode_octet(law, silence),
			      &transmitted, &observed) == PCM_CALL_OK &&
		      transmitted == silence,
		      "probe timeout did not send full-retrain silence");
	}
	CHECK(waiting.stage == PCM_CALL_PHASE2 && waiting.retrain_active &&
	      waiting.protocol == protocol && waiting.peer_info0_stored &&
	      waiting.state.phase2.info0_protocol == protocol &&
	      waiting.state.phase2.state.ranging.control.prior_round_trip_delay_samples ==
		      digital.round_trip_delay_samples &&
	      pcm_call_is_valid(&waiting),
	      "probe timeout did not restart the selected PCM ranging");
	return true;
}

static bool test_probe_response_timeout(void)
{
	for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
	     protocol <= PCM_PROTOCOL_V92; ++protocol)
		for (enum g711_law law = G711_LAW_PCMU;
		     law <= G711_LAW_PCMA; ++law)
			for (enum v90_modem_role role = V90_ANALOG_MODEM;
			     role <= V90_DIGITAL_MODEM; ++role)
				CHECK(run_probe_response_timeout(protocol, law, 3u, role) &&
				      run_probe_response_timeout(protocol, law, 13u, role),
				      "PCM probe-timeout recovery failed");
	return true;
}

static bool run_phase3_return_timeout(enum g711_law law, size_t delay)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct pcm_call waiting;
	uint8_t silence = silence_octet(law);
	uint32_t deadline;
	bool trn1d_started = false;
	bool return_seen = false;

	CHECK(channel_init(&channel, law, delay) &&
	      endpoint_init(
		      &analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
	      endpoint_init(
		      &digital, V90_DIGITAL_MODEM, law,
		      V90_JD_RATE_CAPABILITY_MASK, 0u),
	      "law %u return-timeout endpoints did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		const struct pcm_phase3_digital_tx *tx;

		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u return-timeout setup stopped at %zu",
		      (unsigned int)law, clock);
		if (digital.call.stage != PCM_CALL_PHASE3)
			continue;
		tx = &digital.call.state.phase3.digital.transmit;
		if (digital.call.state.phase3.digital.transmit_started &&
		    tx->stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
		    tx->sequence.prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_TRN1D &&
		    tx->sequence.prefix.sequence.signs.trn1d_symbols == 0u) {
			trn1d_started = true;
			break;
		}
	}
	CHECK(trn1d_started && digital.call.round_trip_delay_stored,
	      "law %u return-timeout test missed the TRN1d boundary",
	      (unsigned int)law);
	waiting = digital.call;
	/* V.90 9.3.1.5 starts at TRN1d, not at Ja or the first Jd record. */
	deadline = 40800u + waiting.round_trip_delay_samples;
	for (uint32_t elapsed = 1u; elapsed <= deadline; ++elapsed) {
		struct pcm_call_observation observation;
		uint8_t transmitted;
		enum pcm_call_result result = pcm_call_octet(
			&waiting, silence, decode_octet(law, silence),
			&transmitted, &observation);

		CHECK(result == PCM_CALL_OK &&
		      observation.processed == PCM_CALL_PHASE3 &&
		      observation.next == (elapsed < deadline ?
			      PCM_CALL_PHASE3 : PCM_CALL_RETRAIN_SILENCE),
		      "law %u delay %zu return deadline at %u/%u: %s stage %u",
		      (unsigned int)law, delay, elapsed, deadline,
		      pcm_call_result_name(result), (unsigned int)waiting.stage);
	}
	{
		struct pcm_call_observation observation;
		uint8_t transmitted;

		CHECK(waiting.retrain_active && waiting.peer_info0_stored &&
		      !waiting.b1_deadline_armed && !pcm_call_has_failed(&waiting) &&
		      pcm_call_octet(
			      &waiting, silence, decode_octet(law, silence),
			      &transmitted, &observation) == PCM_CALL_OK &&
		      transmitted == silence &&
		      observation.processed == PCM_CALL_RETRAIN_SILENCE &&
		      pcm_call_is_valid(&waiting),
		      "law %u return timeout did not start protocol retrain silence",
		      (unsigned int)law);
	}
	/* The original, un-stalled pair supplies real S detection. A latched
	 * response must suppress expiry while the final Jd record drains. */
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_phase3_digital_tx *tx;
		uint8_t received = channel.analog_to_digital[channel.cursor];

		waiting = digital.call;

		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u timely return stopped at %zu",
		      (unsigned int)law, clock);
		if (digital.call.stage != PCM_CALL_PHASE3)
			continue;
		tx = &digital.call.state.phase3.digital.transmit;
		if (tx->stage == PCM_PHASE3_DIGITAL_TX_JD &&
		    tx->finish_jd_requested) {
			struct pcm_call_observation observation;
			struct pcm_call detected = digital.call;
			uint8_t transmitted;
			uint32_t jd_symbols;

			CHECK(waiting.stage == PCM_CALL_PHASE3 &&
			      waiting.state.phase3.digital.transmit.stage ==
				      PCM_PHASE3_DIGITAL_TX_JD &&
			      !waiting.state.phase3.digital.transmit.finish_jd_requested,
			      "law %u timely-return control missed the detection edge",
			      (unsigned int)law);
			tx = &waiting.state.phase3.digital.transmit;
			/* Put the generated S-detection sample on the last allowed
			 * tick. Only transmitter counts move; receive history is exact. */
			jd_symbols = deadline - 1u - tx->sequence.signs.trn1d_symbols;
			tx->jd_records = jd_symbols / PCM_J_FRAME_BITS;
			tx->bit_index = (uint8_t)(jd_symbols % PCM_J_FRAME_BITS);
			CHECK(pcm_call_is_valid(&waiting) &&
			      pcm_call_octet(
				      &waiting, received, decode_octet(law, received),
				      &transmitted, &observation) == PCM_CALL_OK &&
			      waiting.stage == PCM_CALL_PHASE3 &&
			      !waiting.retrain_active &&
			      waiting.state.phase3.digital.transmit.finish_jd_requested,
			      "law %u S on the deadline lost to timeout",
			      (unsigned int)law);
			waiting = detected;
			/* Rebase only the completed-record count past the deadline;
			 * keep the generated receive state and pending Jd tail. */
			waiting.state.phase3.digital.transmit.jd_records +=
				deadline / PCM_J_FRAME_BITS + 1u;
			CHECK(pcm_call_is_valid(&waiting) &&
			      pcm_call_octet(
				      &waiting, silence, decode_octet(law, silence),
				      &transmitted, &observation) == PCM_CALL_OK &&
			      waiting.stage == PCM_CALL_PHASE3 &&
			      !waiting.retrain_active &&
			      waiting.state.phase3.digital.transmit.finish_jd_requested,
			      "law %u detected S did not cancel the return deadline",
			      (unsigned int)law);
			return_seen = true;
			break;
		}
	}
	CHECK(return_seen, "law %u return-timeout control did not detect S",
	      (unsigned int)law);
	return true;
}

static bool test_phase3_return_timeout(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(run_phase3_return_timeout(law, 3u) &&
		      run_phase3_return_timeout(law, 13u),
		      "law %u V.90 return timeout failed", (unsigned int)law);
	}
	return true;
}

static bool check_phase3_sd_deadline(
	const struct pcm_call *started, const uint8_t *response,
	uint32_t response_delay)
{
	struct pcm_call waiting = *started;
	enum g711_law law = started->law;
	uint8_t silence = silence_octet(law);
	const uint32_t deadline = 12000u;
	const uint32_t first_sd = deadline - V90_PHASE3_SD_SYMBOLS -
		V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS + 1u + response_delay;
	const uint32_t tail = V90_PHASE3_SD_BAR_SYMBOLS -
		V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS;
	bool timeout = response_delay != 0u;

	/* The source has emitted exactly the first Ja sample. A full generated
	 * Sd followed by 12 Sd-bar characters qualifies on the final allowed
	 * sample, one sample late, or not at all within the deadline. */
	for (uint32_t elapsed = 2u;
	     elapsed <= deadline + (timeout ? 0u : tail); ++elapsed) {
		struct pcm_call_observation observed;
		uint8_t transmitted;
		uint8_t received = elapsed < first_sd ? silence :
			response[elapsed - first_sd];
		enum pcm_call_result result = pcm_call_octet(
			&waiting, received, decode_octet(law, received),
			&transmitted, &observed);

		CHECK(result == PCM_CALL_OK &&
		      observed.processed == PCM_CALL_PHASE3 &&
		      observed.next == (timeout && elapsed == deadline ?
			      PCM_CALL_RETRAIN_SILENCE : PCM_CALL_PHASE3),
		      "law %u Sd delay %u at sample %u: %s stage %u",
		      (unsigned int)law, response_delay, elapsed,
		      pcm_call_result_name(result), (unsigned int)waiting.stage);
		if (!timeout && elapsed == deadline)
			CHECK(waiting.state.phase3.analog.receive_stage ==
				      V90_PHASE3_ANALOG_RECEIVE_SD &&
			      waiting.state.phase3.analog.receive.sd.stage ==
				      V90_PHASE3_SD_RECEIVER_SD_BAR &&
			      !waiting.retrain_active,
			      "Sd/Sd-bar detection did not win on the deadline");
	}
	if (!timeout) {
		CHECK(!waiting.retrain_active &&
		      waiting.state.phase3.analog.receive_stage ==
			      V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
		      waiting.state.phase3.analog.transmit_stage ==
			      V90_PHASE3_ANALOG_TRANSMIT_RETURN &&
		      pcm_call_is_valid(&waiting),
		      "timely Sd/Sd-bar did not cancel expiry through the Ja tail");
		return true;
	}
	CHECK(waiting.retrain_active && waiting.peer_info0_stored &&
	      !waiting.b1_deadline_armed && !pcm_call_has_failed(&waiting) &&
	      waiting.round_trip_delay_samples == started->round_trip_delay_samples &&
	      memcmp(&waiting.peer_info0, &started->peer_info0,
		     sizeof(waiting.peer_info0)) == 0,
	      "missing Sd lost the established call agreement");
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
		struct pcm_call_observation observed;
		uint8_t transmitted;

		CHECK(pcm_call_octet(
			      &waiting, silence, decode_octet(law, silence),
			      &transmitted, &observed) == PCM_CALL_OK &&
		      observed.processed == PCM_CALL_RETRAIN_SILENCE &&
		      transmitted == silence,
		      "Sd timeout did not send the complete retrain silence");
	}
	CHECK(waiting.stage == PCM_CALL_PHASE2 && waiting.retrain_active &&
	      waiting.peer_info0_stored && pcm_call_is_valid(&waiting),
	      "Sd timeout did not restart shared ranging");
	return true;
}

static bool run_phase3_sd_timeout(
	enum g711_law law, size_t delay, enum v34_symbol_rate rate)
{
	struct channel channel;
	struct endpoint analog = { .upstream_received_delay = UINT16_MAX };
	struct endpoint digital = { .upstream_received_delay = UINT16_MAX };
	struct pcm_call_configuration analog_config =
		configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_config =
		configuration(V90_DIGITAL_MODEM, law);
	uint8_t response[V90_PHASE3_SD_SYMBOLS + V90_PHASE3_SD_BAR_SYMBOLS];
	bool ja_started = false;
	bool sd_bar_seen = false;
	size_t position = 0u;

	analog_config.local_info1.analog.upstream_symbol_rate = rate;
	/* Cover both integral and fractional baud, and both Ja descriptor forms. */
	if (rate == V34_SYMBOL_RATE_3429) {
		analog_config.phase3.analog.dil_profile = V90_DIL_PROFILE_CONVENTIONAL;
		digital_config.local_info1.digital.answer_transmit[rate].high_carrier = false;
	}
	CHECK(channel_init(&channel, law, delay) &&
	      pcm_call_init(&analog.call, &analog_config) == PCM_CALL_OK &&
	      pcm_call_init(&digital.call, &digital_config) == PCM_CALL_OK,
	      "Sd-timeout endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		const struct v90_phase3_analog_station *station;

		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "Sd-timeout setup stopped at %zu", clock);
		if (analog.call.stage != PCM_CALL_PHASE3)
			continue;
		station = &analog.call.state.phase3.analog;
		if (station->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL &&
		    station->transmit.initial.symbols.stage == V90_PHASE3_ANALOG_TX_JA &&
		    station->transmit.initial.symbols.ja_symbols == 1u) {
			ja_started = true;
			break;
		}
	}
	CHECK(ja_started && analog.call.peer_info0_stored,
	      "Sd-timeout test missed the first Ja sample");
	for (enum v90_phase3_sd_kind kind = V90_PHASE3_SD;
	     kind <= V90_PHASE3_SD_BAR; ++kind) {
		struct v90_phase3_sd sequence;

		CHECK(v90_phase3_sd_init(&sequence, law,
			      analog_config.local_info1.analog.training_ucode, kind) ==
			      V90_PHASE3_SD_OK,
		      "Sd-timeout response did not initialize");
		while (v90_phase3_sd_remaining(&sequence) != 0u) {
			struct v90_pcm_symbol symbol;

			CHECK(position < sizeof(response) &&
			      v90_phase3_sd_next(&sequence, &symbol) == V90_PHASE3_SD_OK,
			      "Sd-timeout response stopped at %zu", position);
			response[position++] = symbol.octet;
		}
	}
	CHECK(position == sizeof(response) &&
	      check_phase3_sd_deadline(&analog.call, response, 12000u) &&
	      check_phase3_sd_deadline(&analog.call, response, 1u) &&
	      check_phase3_sd_deadline(&analog.call, response, 0u),
	      "law %u delay %zu baud %u Sd deadline failed",
	      (unsigned int)law, delay, (unsigned int)rate);
	/* The original pair must also take its ordinary, timely response. */
	for (size_t clock = 0; clock < 12000u; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "Sd-timeout control stopped at %zu", clock);
		CHECK(analog.call.stage == PCM_CALL_PHASE3 && !analog.call.retrain_active,
		      "timely Sd control unexpectedly left Phase 3");
		if (analog.call.state.phase3.analog.receive_stage ==
			    V90_PHASE3_ANALOG_RECEIVE_SIGNS ||
		    analog.call.state.phase3.analog.receive.sd.stage >=
			    V90_PHASE3_SD_RECEIVER_SD_BAR) {
			sd_bar_seen = true;
			break;
		}
	}
	CHECK(sd_bar_seen, "Sd-timeout control did not detect Sd/Sd-bar");
	return true;
}

static bool test_phase3_sd_timeout(void)
{
	const enum v34_symbol_rate rates[] = {
		V34_SYMBOL_RATE_3000, V34_SYMBOL_RATE_3429,
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t rate = 0; rate < ARRAY_LENGTH(rates); ++rate)
			CHECK(run_phase3_sd_timeout(law, 3u, rates[rate]) &&
			      run_phase3_sd_timeout(law, 13u, rates[rate]),
			      "V.90 Sd timeout failed");
	return true;
}

static bool idle_retrain_octet(
	struct pcm_call *call, enum g711_law law, uint8_t received,
	uint8_t *transmitted, struct pcm_call_data_event *data)
{
	struct pcm_call_observation observation;

	*data = (struct pcm_call_data_event){ 0 };
	if (call->stage == PCM_CALL_DATA_PATH)
		return pcm_call_data_octet(
			       call, received, decode_octet(law, received),
			       NULL, transmitted, data) == PCM_CALL_OK;
	return pcm_call_octet(
		       call, received, decode_octet(law, received),
		       transmitted, &observation) == PCM_CALL_OK;
}

static bool check_idle_retrain_receive(
	const struct endpoint *analog, const struct endpoint *digital,
	const struct channel *line, enum g711_law law,
	enum v90_modem_role initiating_role)
{
	enum {
		DRAIN_SAMPLES = V34_PHASE2_SAMPLE_RATE / 10,
		IDLE_SAMPLES = V34_PHASE2_SAMPLE_RATE / 5,
		RETRAIN_LIMIT_SAMPLES = V34_PHASE2_SAMPLE_RATE / 2,
	};
	struct pcm_call calls[] = { analog->call, digital->call };
	struct channel channel = *line;
	const size_t initiator = initiating_role == V90_ANALOG_MODEM ? 0u : 1u;
	const size_t responder = 1u - initiator;
	size_t idle_frames[2] = { 0u, 0u };
	bool detected = false;

	/* Drain the supplied payload and verify continuous idle before retrain.
	 * Raw decisions while its preamble is being qualified are not payload;
	 * the complete call test verifies ordered data after recovery. */
	for (size_t clock = 0;
	     clock < IDLE_SAMPLES + RETRAIN_LIMIT_SAMPLES; ++clock) {
		uint8_t received[2];
		uint8_t transmitted[2];

		if (clock == IDLE_SAMPLES)
			CHECK(pcm_call_start_retrain(&calls[initiator]) == PCM_CALL_OK,
			      "idle retrain did not start");
		channel_receive(&channel, &received[0], &received[1]);
		for (size_t role = 0; role < ARRAY_LENGTH(calls); ++role) {
			struct pcm_call_data_event event;
			const uint8_t *bits = NULL;
			size_t count = 0u;

			CHECK(idle_retrain_octet(
				      &calls[role], law, received[role],
				      &transmitted[role], &event),
			      "law %u role %zu idle retrain stopped at %zu",
			      (unsigned int)law, role, clock);
			if (event.kind == PCM_CALL_UPSTREAM_FRAME) {
				bits = event.frame.upstream.primary_bits;
				count = event.frame.upstream.primary_bit_count;
			} else if (event.kind == PCM_CALL_DOWNSTREAM_FRAME) {
				bits = event.frame.downstream.bits;
				count = event.frame.downstream.bit_count;
			}
			if (clock < DRAIN_SAMPLES || clock >= IDLE_SAMPLES || count == 0u)
				continue;
			for (size_t bit = 0; bit < count; ++bit)
				CHECK(bits[bit] == 1u,
				      "law %u initiator %zu receiver %zu lost "
				      "idle data before retrain at sample %zu bit %zu",
				      (unsigned int)law, initiator, role, clock, bit);
			idle_frames[role]++;
		}
		channel_transmit(&channel, transmitted[0], transmitted[1]);
		if (calls[responder].stage == PCM_CALL_RETRAIN_SILENCE) {
			detected = true;
			break;
		}
	}
	CHECK(detected && idle_frames[0] != 0u && idle_frames[1] != 0u,
	      "idle receive check did not exercise both data paths and retrain");
	return true;
}

static bool check_short_quiet_receive(
	const struct endpoint *analog, const struct endpoint *digital,
	const struct channel *line, enum g711_law law, size_t receiver)
{
	enum {
		IDLE_SAMPLES = V34_PHASE2_SAMPLE_RATE / 5,
	};
	const size_t quiet_lengths[] = { 80u, 160u, 320u };
	const size_t phases = receiver == 0u ? V90_CP_FRAME_INTERVALS : 1u;
	const size_t signs = receiver == 0u ? 2u : 1u;
	const size_t recovery_samples = receiver == 0u ? 64u : V34_PHASE2_SAMPLE_RATE / 10;

	for (size_t trial = 0; trial < signs * phases * ARRAY_LENGTH(quiet_lengths); ++trial) {
		struct pcm_call calls[] = { analog->call, digital->call };
		struct channel channel = *line;
		const size_t quiet_start = IDLE_SAMPLES +
			(trial / ARRAY_LENGTH(quiet_lengths)) % phases;
		const size_t quiet_length = quiet_lengths[trial % ARRAY_LENGTH(quiet_lengths)];
		const size_t quiet_end = quiet_start + quiet_length;
		const bool negative = trial >= phases * ARRAY_LENGTH(quiet_lengths);
		uint8_t quiet;
		size_t recovered_frames = 0u;

		CHECK(g711_ucode_octet(law, 0u, !negative, &quiet),
		      "signed silence did not encode");
		/* Resume the same signal after erasure, without ATO1 or a full
		 * silence/tone preamble. Neither zero sign is in the constellation;
		 * exercise them at all six downstream mapper phases. */
		for (size_t clock = 0;
		     clock < quiet_end + 2u * recovery_samples; ++clock) {
			struct pcm_call_data_event events[2];
			struct v34_line_receiver expected =
				calls[1].state.data_path.digital.upstream.data.line;
			uint64_t downstream_symbols = calls[0].state.data_path.analog
				.downstream.data.stream.received_symbols;
			struct v34_adaptive_equalizer_observation observation;
			enum v34_line_receiver_result line_result;
			uint8_t received[2];
			uint8_t transmitted[2];

			channel_receive(&channel, &received[0], &received[1]);
			if (clock >= quiet_start && clock < quiet_end)
				received[receiver] = quiet;
			line_result = v34_line_receiver_sample(
				&expected, decode_octet(law, received[1]),
				&observation);
			CHECK(line_result == V34_LINE_RECEIVER_OK ||
			      line_result == V34_LINE_RECEIVER_OBSERVATION_READY,
			      "short quiet reference sample failed");
			for (size_t role = 0; role < ARRAY_LENGTH(calls); ++role)
				CHECK(idle_retrain_octet(
					      &calls[role], law, received[role],
					      &transmitted[role], &events[role]) &&
				      pcm_call_is_online(&calls[role]) &&
				      !calls[role].retrain_active,
				      "law %u role %zu quiet %zu stopped at %zu",
				      (unsigned int)law, role, quiet_length, clock);
			CHECK(memcmp(
				      &expected.timing,
				      &calls[1].state.data_path.digital.upstream.data
					       .line.timing,
				      sizeof(expected.timing)) == 0,
			      "law %u quiet %zu dropped receive sample %zu",
			      (unsigned int)law, quiet_length, clock);
			CHECK(calls[0].state.data_path.analog.downstream.data.stream
				      .received_symbols == downstream_symbols + 1u,
			      "law %u quiet %zu dropped downstream sample %zu",
			      (unsigned int)law, quiet_length, clock);
			channel_transmit(&channel, transmitted[0], transmitted[1]);
			if (clock < quiet_end + recovery_samples ||
			    events[receiver].kind != (receiver == 0u ?
				    PCM_CALL_DOWNSTREAM_FRAME : PCM_CALL_UPSTREAM_FRAME))
				continue;
			const uint8_t *bits = receiver == 0u ? events[0].frame.downstream.bits :
				events[1].frame.upstream.primary_bits;
			size_t count = receiver == 0u ? events[0].frame.downstream.bit_count :
				events[1].frame.upstream.primary_bit_count;
			for (size_t bit = 0; bit < count; ++bit)
				CHECK(bits[bit] == 1u,
				      "law %u quiet %zu did not recover idle data at %zu",
				      (unsigned int)law, quiet_length, clock);
			recovered_frames++;
		}
		CHECK(recovered_frames >= DATA_FRAMES_TO_RECEIVE,
		      "short quiet receive check did not recover data");
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
	double initiator_receive_floor;
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
	CHECK(check_idle_retrain_receive(
		      &analog, &digital, &channel, law, initiating_role),
	      "idle retrain receive boundary failed");
	if (initiating_role == V90_ANALOG_MODEM)
		CHECK(check_short_quiet_receive(&analog, &digital, &channel, law, 0u) &&
		      check_short_quiet_receive(&analog, &digital, &channel, law, 1u),
		      "short quiet receive boundary failed");
	initiator = initiating_role == V90_ANALOG_MODEM ? &analog : &digital;
	responder = initiating_role == V90_ANALOG_MODEM ? &digital : &analog;
	initiator_prior_rtd = initiator->call.round_trip_delay_samples;
	responder_prior_rtd = responder->call.round_trip_delay_samples;
	/* Pin a distinct trained level so the local handoff cannot pass by
	 * accidentally restoring the initial-acquisition default. */
	if (initiating_role == V90_DIGITAL_MODEM)
		initiator->call.retrain.detector.tone.carrier.profile
			.minimum_line_power = 4096.0;
	initiator_receive_floor = initiator->call.retrain.detector.tone
		.carrier.profile.minimum_line_power;
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
			if (!initiator_prior_handed_off) {
				const struct v34_phase2_tone_detector *tone =
					&initiator->call.state.phase2.state.ranging
						.info0_exchange.transport.tone_detector;

				CHECK(tone->carrier.profile.minimum_line_power ==
					      initiator_receive_floor &&
				      tone->carrier.history_count == 0u &&
				      !tone->carrier.tone_present &&
				      tone->carrier.reference_in_phase == 0.0 &&
				      tone->carrier.reference_quadrature == 0.0,
				      "local retrain lost sensitivity or retained stale peer phase");
			}
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
	    v34_phase3_s_hunt_init(&completed.s) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return false;
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		unsigned int turns = symbol < V34_PHASE3_S_SYMBOLS ?
			(symbol & 1u) :
			(2u + ((symbol - V34_PHASE3_S_SYMBOLS) & 1u));
		struct v34_analytic_sample fixed_point;
		struct v34_phase3_s_adaptation adaptation;
		bool control;
		enum v34_phase3_s_receiver_result marker_result;

		if (v34_training_constellation_fixed_point(
			    V34_CONSTELLATION_4_POINT, 0u,
			    (uint8_t)((4u - turns) & 3u),
			    &fixed_point) != V34_CONSTELLATION_OK)
			return false;
		marker_result = v34_phase3_s_hunt_receive(
			&completed.s, fixed_point, &adaptation, &control);
		if (marker_result !=
			    (symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
				     V34_PHASE3_S_RECEIVER_DETECTED :
				     V34_PHASE3_S_RECEIVER_OK))
			return false;
	}
	completed.enabled = true;
	*monitor = completed;
	return true;
}

static bool start_error_bounded_candidate(
	struct v34_data_rate_renegotiation_monitor *monitor)
{
	enum {
		CANDIDATE_BOUND =
			V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS *
			(V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u),
		LAST_ERROR = CANDIDATE_BOUND -
			V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS,
	};
	struct v34_data_rate_renegotiation_monitor candidate = { 0 };

	if (monitor == NULL ||
	    v34_phase3_s_hunt_init(&candidate.s) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return false;
	for (size_t symbol = 0; symbol < CANDIDATE_BOUND; ++symbol) {
		unsigned int turns = symbol & 1u;
		struct v34_analytic_sample fixed_point;
		struct v34_phase3_s_adaptation adaptation;
		bool control;

		if (symbol < LAST_ERROR &&
		    (symbol + 1u) %
			    V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS == 0u)
			turns++;
		if (v34_training_constellation_fixed_point(
			    V34_CONSTELLATION_4_POINT, 0u,
			    (uint8_t)((4u - turns) & 3u),
			    &fixed_point) != V34_CONSTELLATION_OK ||
		    v34_phase3_s_hunt_receive(
			    &candidate.s, fixed_point,
			    &adaptation, &control) != V34_PHASE3_S_RECEIVER_OK)
			return false;
	}
	if (candidate.s.prefix.alternating_turns !=
		    V34_PHASE3_S_PREFIX_TURNS ||
	    candidate.s.prefix.recurrence_hits !=
		    V34_PHASE3_S_HUNT_CANDIDATE_RECURRENCE_HITS ||
	    !candidate.s.candidate)
		return false;
	candidate.enabled = true;
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
	RRN_PEER_DOWNSTREAM_SILENCE,
	RRN_PEER_DOWNSTREAM_RT,
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
	size_t silence_samples;
	size_t scr_samples;
	size_t rt_symbols;
};

static bool rrn_peer_init(
	struct rrn_peer *peer, const struct pcm_call *analog,
	const struct pcm_call *digital, bool recondition)
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
	initialized.data_parameters.request_silence = recondition;
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
	if (peer->cp_prime_started && !peer->data_parameters.request_silence)
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

			if (peer->data_parameters.request_silence &&
			    peer->downstream_stage == RRN_PEER_DOWNSTREAM_SILENCE &&
			    v90_phase4_cp_line_tx_record_is_complete(&peer->upstream.cp)) {
				result = v90_phase4_cp_line_tx_scr_sample(
					&peer->upstream.cp, peer->scr_samples >= 1601u, &sample);
				if (result == V90_PHASE4_CP_LINE_TX_OK) {
					peer->scr_samples++;
					break;
				}
				if (result != V90_PHASE4_CP_LINE_TX_COMPLETE)
					return false;
				peer->data_parameters.request_silence = false;
				peer->acknowledge_pending = false;
				peer->cp_prime_started = false;
			}
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
	if (peer->downstream_stage == RRN_PEER_DOWNSTREAM_SILENCE) {
		struct v90_pcm_symbol silence;

		if (!v90_pcm_symbol_make(&silence, peer->law, 0u, true))
			return false;
		if (octet == silence.octet) {
			peer->silence_samples++;
			return true;
		}
		if (peer->silence_samples == 0u ||
		    peer->silence_samples % V90_CP_FRAME_INTERVALS != 0u)
			return false;
		peer->downstream_stage = RRN_PEER_DOWNSTREAM_RT;
	}
	if (peer->downstream_stage == RRN_PEER_DOWNSTREAM_RT) {
		struct v90_pcm_symbol expected;
		uint8_t ucode;
		size_t interval = peer->rt_symbols % V90_CP_FRAME_INTERVALS;
		bool positive = interval < 3u;

		if (peer->rt_symbols >= V90_RD_SYMBOLS)
			positive = !positive;
		if (v90_pcm_mapping_ucode(&path->training_mapping, peer->decision_plane,
					 interval, 0u, &ucode) != V90_PCM_MAPPING_OK ||
		    !v90_pcm_symbol_make(&expected, peer->law, ucode, positive) ||
		    octet != expected.octet)
			return false;
		if (++peer->rt_symbols == V90_RD_SEQUENCE_SYMBOLS) {
			if (pcm_downstream_rx_resume_after_rt(&peer->downstream.training) != PCM_DOWNSTREAM_RX_OK)
				return false;
			peer->downstream_stage = RRN_PEER_DOWNSTREAM_TRAINING;
		}
		return true;
	}
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

			if (peer->data_parameters.request_silence) {
				peer->downstream_stage = RRN_PEER_DOWNSTREAM_SILENCE;
				peer->peer_mp_received = false;
				return true;
			}
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
	enum v34_constellation_size rate_renegotiation_constellation,
	bool recondition)
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
	bool initially_online = false;
	bool renegotiation_started = false;
	bool deadline_started = false;
	bool timeout_checked = false;
	bool training_started = false;
	bool data_cp_latched_during_control = false;
	bool recondition_timeout_checked = false;
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
	control_before =
		digital.call.state.data_path.digital.control_symbols;
	CHECK(pcm_call_get_data_rates(
		      &digital.call, &prior_transmit_rate,
		      &prior_receive_rate),
	      "law %u rate-renegotiation rates were unavailable",
	      (unsigned int)law);
	CHECK(start_error_bounded_candidate(
		      &digital.call.state.data_path.digital.upstream.data
			       .rate_renegotiation) &&
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
			       .rate_renegotiation) &&
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
		struct v34_line_receiver expected_line = path->upstream.data.line;
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
		CHECK(v34_adaptive_equalizer_set_data_carrier_tracking(
			      &expected_line.equalizer,
			      rate_renegotiation_constellation ==
				      V34_CONSTELLATION_16_POINT) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "online CP carrier mode did not resume");
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
		      resumed.boundary_complete &&
		      !v34_phase3_s_receiver_is_valid(&resumed.boundary) &&
		      resumed.adaptation ==
			      (rate_renegotiation_constellation ==
				       V34_CONSTELLATION_16_POINT ?
				       V34_ADAPTIVE_EQUALIZER_CARRIER :
				       V34_ADAPTIVE_EQUALIZER_FULL) &&
		      memcmp(&resumed.line, &expected_line,
			     sizeof(resumed.line)) == 0 &&
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
	CHECK(rrn_peer_init(&peer, &analog.call, &digital.call, recondition),
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
		      "law %u rate renegotiation lost online ownership or host gating "
		      "at sample %zu (stage %u)",
		      (unsigned int)law, sample, (unsigned int)digital.call.stage);
		if (recondition && !recondition_timeout_checked &&
		    digital.call.state.data_path.digital.downstream_stage ==
			    V90_DIGITAL_PATH_DOWNSTREAM_SILENCE) {
			struct pcm_call stalled = digital.call;
			struct pcm_call_data_event ignored;
			uint8_t output;

			CHECK(stalled.rate_renegotiation_deadline_armed &&
			      stalled.rate_renegotiation_deadline_remaining_samples < expected_deadline,
			      "CPs retired or restarted the original response deadline");
			stalled.rate_renegotiation_deadline_remaining_samples = 1u;
			CHECK(pcm_call_data_octet(&stalled, digital_received,
				      decode_octet(law, digital_received), NULL, &output, &ignored) == PCM_CALL_OK &&
			      stalled.stage == PCM_CALL_RETRAIN_SILENCE && !stalled.failed &&
			      pcm_call_is_valid(&stalled), "missing post-silence CP did not time out");
			recondition_timeout_checked = true;
		}
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
	CHECK(!recondition || (recondition_timeout_checked &&
			      peer.silence_samples > 0u && peer.scr_samples >= 1601u &&
			      peer.rt_symbols == V90_RD_SEQUENCE_SYMBOLS),
	      "CPs did not complete silence/SCR/Rt: %zu/%zu/%zu",
	      peer.silence_samples, peer.scr_samples, peer.rt_symbols);
	return true;
}

static bool test_rate_renegotiation_completion(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(run_rate_renegotiation_case(
			      law, V34_CONSTELLATION_4_POINT, false),
		      "law %u rate-renegotiation completion failed",
		      (unsigned int)law);
	return true;
}

static bool test_echo_reconditioning(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int index = 0u; index < 2u; ++index)
			CHECK(run_rate_renegotiation_case(law, index == 0u ?
				V34_CONSTELLATION_4_POINT : V34_CONSTELLATION_16_POINT, true),
			      "law %u constellation %u echo reconditioning failed", law, index);
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

static bool check_rrn_response_deadline(
	const struct pcm_call *before, struct pcm_call *after,
	uint8_t received, uint8_t expected_transmitted)
{
	struct pcm_call last_sample = *before;
	struct pcm_call stale_deadline = *after;
	struct pcm_call_data_event event;
	uint8_t transmitted;

	CHECK(before->rate_renegotiation_deadline_armed,
	      "role %u retired its RRN response deadline before E/Ed",
	      (unsigned int)before->local_role);
	/* Replay the actual final E/Ed input character with only one deadline
	 * sample left. This is a timer-boundary check, not a forced DSP event. */
	last_sample.rate_renegotiation_deadline_remaining_samples = 1u;
	CHECK(pcm_call_is_valid(&last_sample) &&
	      pcm_call_data_octet(
		      &last_sample, received, decode_octet(before->law, received),
		      NULL, &transmitted, &event) == PCM_CALL_OK &&
	      last_sample.stage == PCM_CALL_DATA_PATH &&
	      event.kind == PCM_CALL_DATA_NONE &&
	      transmitted == expected_transmitted &&
	      pcm_call_is_valid(&last_sample),
	      "role %u retrained on an E/Ed response at the deadline",
	      (unsigned int)after->local_role);
	CHECK(!last_sample.rate_renegotiation_deadline_armed &&
	      last_sample.rate_renegotiation_deadline_remaining_samples == 0u &&
	      !after->rate_renegotiation_deadline_armed &&
	      after->rate_renegotiation_deadline_remaining_samples == 0u,
	      "role %u retained its RRN response deadline after E/Ed",
	      (unsigned int)after->local_role);
	stale_deadline.rate_renegotiation_deadline_armed = true;
	stale_deadline.rate_renegotiation_deadline_remaining_samples = 1u;
	CHECK(!pcm_call_is_valid(&stale_deadline),
	      "role %u accepted an armed RRN response deadline during B1",
	      (unsigned int)after->local_role);
	/* Continue the paired exchange with the deadline-edge receiver through
	 * exact fresh data, rather than comparing reinitialized struct padding. */
	*after = last_sample;
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
	struct pcm_call analog_response_before;
	struct pcm_call digital_response_before;
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
	bool analog_response_seen = false;
	bool digital_response_seen = false;
	bool cleardown_checked = false;
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
			       .rate_renegotiation) &&
	      pcm_call_is_valid(&digital.call),
	      "law %u paired RRN could not seed the digital request",
	      (unsigned int)law);
	for (size_t sample = 0; sample < 64u; ++sample) {
		struct v34_line_receiver expected =
			digital.call.state.data_path.digital.upstream.data.line;
		struct v34_adaptive_equalizer_observation observation;
		uint8_t ignored_analog;
		uint8_t digital_received;
		enum v34_line_receiver_result expected_result;

		channel_receive(&channel, &ignored_analog, &digital_received);
		expected_result = v34_line_receiver_sample(
			&expected, decode_octet(law, digital_received), &observation);
		CHECK(expected_result == V34_LINE_RECEIVER_OK ||
		      expected_result == V34_LINE_RECEIVER_OBSERVATION_READY,
		      "law %u RRN handoff could not advance one physical sample",
		      (unsigned int)law);
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u paired RRN trigger stopped at %zu",
		      (unsigned int)law, sample);
		if (digital.call.state.data_path.digital
			    .rate_renegotiation_active) {
			CHECK(memcmp(
				      &expected.timing,
				      &digital.call.state.data_path.digital.upstream
					       .startup.state.cp.line.timing,
				      sizeof(expected.timing)) == 0,
			      "law %u latched RRN marker dropped its input sample",
			      (unsigned int)law);
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
		bool analog_receiving_ed = analog_active_before &&
			analog_path->downstream_stage ==
				V90_ANALOG_PATH_DOWNSTREAM_TRAINING &&
			analog_path->downstream.training.stage ==
				PCM_DOWNSTREAM_RX_ED_SEARCH;
		bool digital_receiving_e = digital_active_before &&
			digital_path->upstream_stage ==
				V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
			digital_path->upstream.startup.stage ==
				V90_UPSTREAM_RX_STAGE_E;

		channel_receive(
			&channel, &analog_received, &digital_received);
		if (analog_receiving_ed)
			analog_response_before = analog.call;
		if (digital_receiving_e)
			digital_response_before = digital.call;
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
		if (analog_receiving_ed && analog_path->downstream_stage ==
					 V90_ANALOG_PATH_DOWNSTREAM_DATA) {
			CHECK(!analog_response_seen &&
			      check_rrn_response_deadline(
				      &analog_response_before, &analog.call,
				      analog_received, analog_transmitted),
			      "law %u analogue RRN response deadline failed",
			      (unsigned int)law);
			analog_response_seen = true;
		}
		if (digital_receiving_e && digital_path->upstream_stage ==
					  V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
		    digital_path->upstream.startup.stage ==
			    V90_UPSTREAM_RX_STAGE_B1) {
			CHECK(!digital_response_seen &&
			      check_rrn_response_deadline(
				      &digital_response_before, &digital.call,
				      digital_received, digital_transmitted),
			      "law %u digital RRN response deadline failed",
			      (unsigned int)law);
			digital_response_seen = true;
		}
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
			struct pcm_call early_deadline = analog.call;

			analog_started = true;
			analog_resume_source = analog.upstream_source.frames;
			CHECK(analog_path->upstream_stage ==
				      V90_ANALOG_PATH_UPSTREAM_RRN_RETURN &&
			      !analog.call.rate_renegotiation_deadline_armed &&
			      analog.call
				      .rate_renegotiation_deadline_remaining_samples ==
				      0u,
			      "law %u analogue deadline began at incoming Rd-bar",
			      (unsigned int)law);
			early_deadline.rate_renegotiation_deadline_armed = true;
			early_deadline.rate_renegotiation_deadline_remaining_samples =
				analog_deadline;
			CHECK(!pcm_call_is_valid(&early_deadline),
			      "law %u analogue RRN accepted a deadline before S-bar",
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
			/* Accepting the last S point advances the point generator to
			 * S-bar before the line has accepted its first S-bar point. */
			CHECK(analog.call.rate_renegotiation_deadline_armed ==
				      (analog_path->upstream.rrn_return
					       .points.s_bar_symbols != 0u),
			      "law %u analogue deadline missed the transmitted S-bar boundary",
			      (unsigned int)law);
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
		if (!analog_deadline_seen &&
		    analog.call.rate_renegotiation_deadline_armed) {
			struct pcm_call missing_deadline = analog.call;

			CHECK(analog_path->upstream_stage ==
				      V90_ANALOG_PATH_UPSTREAM_RRN_RETURN &&
			      analog_path->upstream.rrn_return.points.s_bar_symbols == 1u &&
			      analog.call
				      .rate_renegotiation_deadline_remaining_samples ==
				      analog_deadline,
			      "law %u analogue RRN deadline began off boundary",
			      (unsigned int)law);
			missing_deadline.rate_renegotiation_deadline_armed = false;
			missing_deadline.rate_renegotiation_deadline_remaining_samples = 0u;
			CHECK(!pcm_call_is_valid(&missing_deadline),
			      "law %u analogue RRN accepted no deadline after S-bar",
			      (unsigned int)law);
			analog_deadline_seen = true;
		}
		if (!timeout_checked &&
		    analog.call.rate_renegotiation_deadline_armed) {
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
		if (!cleardown_checked && analog.call.rate_renegotiation_deadline_armed &&
		    digital.call.rate_renegotiation_deadline_armed &&
		    analog_path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_CP &&
		    !analog_path->finishing_cp && digital_path->downstream_stage ==
			V90_DIGITAL_PATH_DOWNSTREAM_TRAINING &&
		    digital_path->downstream.training.stage == PCM_DOWNSTREAM_TX_MP &&
		    digital_path->downstream.training.control_bit_index != 0u) {
			CHECK(check_peer_cleardown(&analog.call, &digital.call, &channel, 0u) &&
			      check_peer_cleardown(&analog.call, &digital.call, &channel, 1u),
			      "active rate exchange did not honor local cleardown");
			cleardown_checked = true;
		}
		if (analog_started && !analog_path->rate_renegotiation_active &&
		    !digital_path->rate_renegotiation_active &&
		    downstream_resumed && upstream_resumed)
			break;
	}
	CHECK(cleardown_checked, "paired RRN never exercised control cancellation");
	CHECK(analog_started && rd_detected && rd_bar_started &&
	      rd_bar_complete && analog_deadline_seen &&
	      analog_response_seen && digital_response_seen &&
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
			      law, V34_CONSTELLATION_16_POINT, false),
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
		      CALL_CHANNEL_CLEAN),
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
	size_t distinct_drain_samples = 0u;
	size_t verified_drain_samples = 0u;

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
		struct v90_upstream_training_rx expected;
		const struct v90_dil_sequence *dil = NULL;
		uint16_t segment_symbols = 0u;
		uint16_t segment_symbol = 0u;
		uint8_t analog_received;
		uint8_t digital_received;
		uint8_t analog_transmitted;
		uint8_t digital_transmitted;
		int16_t digital_sample;
		bool draining = false;

		channel_receive(
			&channel, &analog_received, &digital_received);
		digital_sample = decode_octet(law, digital_received);
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
			/* After final S-bar, only the conventional sample belongs to
			 * the fixed upstream owner. A divergent PCM codeword must not
			 * replace or double-consume the inherited CP sample stream. */
			digital_received = silence_octet(law);
			if (digital_sample != decode_octet(law, digital_received))
				distinct_drain_samples++;
			expected = digital.call.state.phase3.digital.receive.
				returning.upstream;
			CHECK(v90_upstream_training_rx_advance(
				      &expected, digital_sample) ==
				      V90_UPSTREAM_TRAINING_RX_OK,
			      "law %u expected DIL drain stopped at %zu",
			      (unsigned int)law, clock);
		}
		CHECK(endpoint_octet(
			      &digital, digital_received,
			      digital_sample, law,
			      &digital_transmitted),
		      "law %u completed return was reused at %zu",
		      (unsigned int)law, clock);
		if (draining && !digital.entered_phase4) {
			CHECK(memcmp(&expected,
				     &digital.call.state.phase3.digital.receive.
					     returning.upstream,
				     sizeof(expected)) == 0,
			      "law %u DIL drain did not consume its physical sample "
			      "exactly once at %zu", (unsigned int)law, clock);
			verified_drain_samples++;
		}
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
	CHECK(drain_seen && drain_octets > 0u && distinct_drain_samples > 0u &&
	      verified_drain_samples > 0u && boundary_seen &&
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

static bool test_initial_probe_loss(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum call_impairment loss = CALL_INITIAL_TONE_B_LOST;
		     loss <= CALL_INITIAL_TONE_A_LOST; ++loss)
			CHECK(run_configured_call(law, 13u, CALL_GOAL_ONLINE_DATA,
				      UINT32_C(1), 1, V90_JD_RATE_CAPABILITY_MASK,
				      DATA_DOWNSTREAM_RATE, loss),
			      "law %u loss %u did not recover online V.90 data",
			      (unsigned int)law, (unsigned int)loss);
	return true;
}

static bool test_initial_info1_loss(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		CHECK(run_configured_call(law, 13u, CALL_GOAL_ONLINE_DATA,
			      UINT32_C(1), 1, V90_JD_RATE_CAPABILITY_MASK,
			      DATA_DOWNSTREAM_RATE, CALL_INITIAL_INFO1D_LOST),
		      "law %u lost INFO1d did not recover online V.90 data",
		      (unsigned int)law);
	return true;
}

static bool check_missing_probe_boundary(
	enum pcm_protocol protocol, enum g711_law law, enum v90_modem_role role)
{
	struct pcm_call_configuration analog_configuration =
		protocol == PCM_PROTOCOL_V90 ? configuration(V90_ANALOG_MODEM, law) :
			v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		protocol == PCM_PROTOCOL_V90 ? configuration(V90_DIGITAL_MODEM, law) :
			v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call analog, digital, waiting;
	struct pcm_call *receiver = role == V90_DIGITAL_MODEM ? &digital : &analog;
	struct channel channel;
	bool probe_started = false;
	bool restarted = false;
	unsigned int missing_probe_limit;
	uint8_t silence = silence_octet(law);

	CHECK(channel_init(&channel, law, 13u) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "missing-probe boundary endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_call_observation observed;
		uint8_t analog_received, digital_received;
		uint8_t analog_transmitted, digital_transmitted;

		channel_receive(&channel, &analog_received, &digital_received);
		CHECK(pcm_call_octet(&analog, analog_received,
			      decode_octet(law, analog_received), &analog_transmitted,
			      &observed) == PCM_CALL_OK &&
		      pcm_call_octet(&digital, digital_received,
			      decode_octet(law, digital_received), &digital_transmitted,
			      &observed) == PCM_CALL_OK,
		      "missing-probe setup stopped at %zu", clock);
		channel_transmit(&channel, analog_transmitted, digital_transmitted);
		if (receiver->stage == PCM_CALL_PHASE2 &&
		    receiver->state.phase2.stage == (role == V90_DIGITAL_MODEM ?
			    PCM_PHASE2_INITIAL_PROBE : PCM_PHASE2_RECIPROCAL_PROBE)) {
			probe_started = true;
			break;
		}
	}
	CHECK(probe_started && receiver->round_trip_delay_stored &&
	      receiver->peer_info0_stored, "missing-probe test missed the receive boundary");
	waiting = *receiver;
	/* The reciprocal receiver searches successive L1 capture windows up to
	 * the reversal deadline; the initial receiver uses one capture window. */
	missing_probe_limit = role == V90_DIGITAL_MODEM ?
		V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES :
		PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES -
			waiting.state.phase2.state.reciprocal_probe.control.
				signal_a_reversal_elapsed_samples;
	for (unsigned int clock = 0; clock < missing_probe_limit; ++clock) {
		struct pcm_call_observation observed;
		uint8_t transmitted;

		CHECK(pcm_call_octet(&waiting, silence, decode_octet(law, silence),
			      &transmitted, &observed) == PCM_CALL_OK &&
		      pcm_call_is_valid(&waiting) && !waiting.failed,
		      "missing probe terminated protocol %u role %u law %u at %u",
		      (unsigned int)protocol, (unsigned int)role, (unsigned int)law, clock);
		if (observed.phase2.kind == PCM_PHASE2_EVENT_FAILED) {
			CHECK(observed.phase2.failure == PCM_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND &&
			      clock + 1u == missing_probe_limit &&
			      waiting.stage == PCM_CALL_RETRAIN_SILENCE && waiting.retrain_active &&
			      waiting.protocol == protocol && waiting.local_role == role &&
			      waiting.round_trip_delay_stored && waiting.peer_info0_stored &&
			      waiting.round_trip_delay_samples == receiver->round_trip_delay_samples &&
			      memcmp(&waiting.peer_info0, &receiver->peer_info0,
				     sizeof(waiting.peer_info0)) == 0,
			      "missing-probe restart lost the call agreement or failure observation");
			restarted = true;
			break;
		}
	}
	CHECK(restarted, "protocol %u role %u law %u missed its L1 acquisition deadline",
	      (unsigned int)protocol, (unsigned int)role, (unsigned int)law);
	for (unsigned int clock = 0; clock < 70u * 8u; ++clock) {
		struct pcm_call_observation observed;
		uint8_t transmitted;

		CHECK(pcm_call_octet(&waiting, silence, decode_octet(law, silence),
			      &transmitted, &observed) == PCM_CALL_OK &&
		      transmitted == silence && observed.processed == PCM_CALL_RETRAIN_SILENCE,
		      "missing-probe retrain did not send its complete silent interval");
	}
	CHECK(waiting.stage == PCM_CALL_PHASE2 && waiting.retrain_active &&
	      !waiting.failed, "missing-probe retrain did not return to Phase 2");
	return true;
}

static bool test_missing_probe_recovery(void)
{
	for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
	     protocol <= PCM_PROTOCOL_V92; ++protocol)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (enum v90_modem_role role = V90_ANALOG_MODEM;
			     role <= V90_DIGITAL_MODEM; ++role)
				CHECK(check_missing_probe_boundary(protocol, law, role),
				      "missing-probe retrain boundary failed");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum call_impairment loss = CALL_INITIAL_L1_LOST;
		     loss <= CALL_RECIPROCAL_L1_LOST; ++loss)
			CHECK(run_configured_call(law, 13u, CALL_GOAL_ONLINE_DATA,
				      UINT32_C(1), 1, V90_JD_RATE_CAPABILITY_MASK,
				      DATA_DOWNSTREAM_RATE, loss),
			      "law %u loss %u did not recover online V.90 data",
			      (unsigned int)law, (unsigned int)loss);
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
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		CHECK(run_configured_call(
			      law, 1, CALL_GOAL_ONLINE_DATA,
			      UINT32_C(1), 1, V90_JD_RATE_CAPABILITY_MASK,
			      DATA_DOWNSTREAM_RATE, CALL_CODEWORD_REPLACED),
		      "law %u digital training/data depended on a reconstructed "
		      "codeword", (unsigned int)law);
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

static bool check_v34_restart(enum g711_law law, bool remote)
{
	const struct pcm_call_configuration selected =
		configuration(V90_DIGITAL_MODEM, law);
	struct v90_fallback_restart restart = {
		.local_info0 = selected.local_info0.digital,
		.peer_info0 = analog_info0(),
		.local_info1 = selected.local_info1.digital,
		.phase3 = selected.phase3.digital,
		.phase4 = selected.digital_phase4,
		.round_trip_delay_samples = 800u,
		.phase2_nominal_amplitude = selected.phase2_nominal_amplitude,
		.law = law,
	};
	struct v34_full_retrain response;
	struct v34_phase2_tone_transmitter peer;
	struct pcm_call call;
	struct pcm_call before;
	bool detected = false;

	CHECK(v34_full_retrain_init(&response, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &peer, V34_ANSWERING_STATION, PHASE2_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "fallback retrain response did not initialize");
	if (remote) {
		for (size_t index = 0; index < 2000u && !detected; ++index) {
			int16_t received = 0;

			if (index >= V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES)
				CHECK(v34_phase2_tone_transmitter_sample(&peer, &received) ==
					      V34_PHASE2_TONE_OK,
				      "fallback peer tone failed");
			CHECK(v34_retrain_detector_sample(
				      &response.detector, received, &detected) ==
				      V34_PHASE2_TONE_OK,
			      "fallback peer request failed");
		}
		CHECK(detected, "fallback peer request was not detected");
	}
	CHECK(v34_full_retrain_begin_response(&response, remote) ==
		      V34_PHASE2_TONE_OK,
	      "fallback retrain did not begin");
	/* A datapump switch happens at the next media-block boundary, not at
	 * the exact sample that recognizes the peer or starts local ATO1. */
	for (size_t index = 0; index < 79u; ++index) {
		enum v34_full_retrain_response_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&peer, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_full_retrain_sample_response(&response, sample, &event) ==
			      V34_PHASE2_TONE_OK &&
		      event == V34_FULL_RETRAIN_RESPONSE_CONTINUES,
		      "fallback block consumed the entire response");
	}
	CHECK(pcm_call_init_from_v34(&call, &restart, &response) == PCM_CALL_OK &&
	      call.stage == PCM_CALL_RETRAIN_SILENCE && call.retrain_active &&
	      memcmp(&call.retrain, &response, sizeof(response)) == 0 &&
	      memcmp(&call.local_info0.digital, &restart.local_info0,
		     sizeof(restart.local_info0)) == 0 &&
	      memcmp(&call.peer_info0.analog, &restart.peer_info0,
		     sizeof(restart.peer_info0)) == 0,
	      "fallback lost its V.90 agreement or restarted the silence");
	before = call;
	restart.law = (enum g711_law)99;
	CHECK(pcm_call_init_from_v34(&call, &restart, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &before, sizeof(call)) == 0,
	      "invalid fallback media changed the call");
	restart.law = law;
	restart.round_trip_delay_samples = UINT16_MAX;
	CHECK(pcm_call_init_from_v34(&call, &restart, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_init_from_v34(NULL, &restart, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_init_from_v34(&call, NULL, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_init_from_v34(&call, &restart, NULL) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_init_from_v34(&call, &restart, &call.retrain) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &before, sizeof(call)) == 0,
	      "invalid fallback ranging or alias changed the call");
	for (size_t index = 0; index < 481u; ++index) {
		struct pcm_call_observation observed;
		enum v34_full_retrain_response_event event;
		uint8_t transmitted;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&peer, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_full_retrain_sample_response(&response, sample, &event) ==
			      V34_PHASE2_TONE_OK &&
		      pcm_call_octet(&call, encode_octet(law, sample), sample,
				     &transmitted, &observed) == PCM_CALL_OK &&
		      transmitted == silence_octet(law) &&
		      (call.stage == PCM_CALL_PHASE2) == (index == 480u),
		      "fallback silence changed at sample %zu", index);
	}
	CHECK(call.protocol == PCM_PROTOCOL_V90 &&
	      call.state.phase2.info0_protocol == PCM_PROTOCOL_V90 &&
	      call.state.phase2.local_role == V90_DIGITAL_MODEM &&
	      call.round_trip_delay_samples == 800u &&
	      (!remote || memcmp(
		      &call.state.phase2.state.ranging.info0_exchange.transport.tone_detector,
		      &response.detector.tone, sizeof(response.detector.tone)) == 0),
	      "fallback retrained as V.34 or lost the peer tone");
	return true;
}

static bool test_v34_restart(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		CHECK(check_v34_restart(law, false) && check_v34_restart(law, true),
		      "fallback restart failed for law %u", (unsigned int)law);
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
		{ "V.90 peer CPs echo reconditioning", test_echo_reconditioning },
		{ "V.34 fallback retains V.90 retrain agreement", test_v34_restart },
		{ "Phase 2 input ownership", test_phase2_input_ownership },
		{ "PCM probe-response timeout recovery", test_probe_response_timeout },
		{ "lost initial probe responses recover online data", test_initial_probe_loss },
		{ "missing L1 probes recover online data", test_missing_probe_recovery },
		{ "initial INFO1 retrain-tone response",
		  test_initial_info1_retrain_tone },
		{ "lost INFO1d recovers online data", test_initial_info1_loss },
		{ "V.92 full INFO1 timeout", test_v92_info1_timeout },
		{ "remote retrain during final PCM training",
		  test_final_training_remote_retrain },
		{ "completed V.90 return drains DIL",
		  test_completed_dil_return_drains },
		{ "generated PCMU and PCMA calls through online data",
		  test_generated_calls },
		{ "negotiated zero symbols retain training and ordered data",
		  test_negotiated_zero_symbols },
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
		{ "lost Ri-bar recovers online data", test_missing_ri_bar },
		{ "standards-timed V.90 Phase 3 return", test_phase3_return_timeout },
		{ "standards-timed V.90 Sd response", test_phase3_sd_timeout },
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
