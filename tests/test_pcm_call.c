// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_call.h>

#include <g711.h>
#include <v34_constellation.h>
#include <v34_phase2_selection.h>
#include <v34_phase3_s_receiver.h>
#include <v90_downstream_data_handoff.h>
#include <v90_phase3_sd.h>
#include <v90_profile.h>
#include <v90_upstream_data.h>
#include <v92_cpd_direct.h>
#include <v92_profile.h>
#include <packed_bits.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum {
	CALL_SAMPLE_LIMIT = 120000,
	V90_B1_BASE_DEADLINE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 15,
	MAX_LINE_DELAY_CHARACTERS = 801,
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
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
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

static struct v92_cpd_parameters v92_cpd_parameters(enum g711_law law)
{
	struct v92_cpd_parameters full;

	if (v92_cpd_direct_init(&full, law, 14u, true) != V92_CPD_OK)
		return (struct v92_cpd_parameters){ 0 };
	return full;
}

static struct pcm_call_configuration v92_configuration(
	enum v90_modem_role role, enum g711_law law)
{
	const uint32_t rate_mask = UINT32_C(0x52491);
	struct pcm_call_configuration conventional;

	if (v90_profile_init(&conventional, role, law, 28000, 56000, NULL) !=
	    V90_PROFILE_OK)
		return (struct pcm_call_configuration){ 0 };
	struct pcm_call_configuration configuration = {
		.protocol = PCM_PROTOCOL_V92,
		.local_role = role,
		.law = law,
		.enabled_downstream_rate_mask = V92_JD_RATE_CAPABILITY_MASK,
		.phase2_nominal_amplitude = PHASE2_NOMINAL_AMPLITUDE,
		.local_info1 = conventional.local_info1,
		.phase3 = conventional.phase3,
		.analog_phase4 = conventional.analog_phase4,
		.digital_phase4 = conventional.digital_phase4,
	};
	/* This fixture supplies explicit maps; only native profiles adapt them. */
	configuration.analog_phase4.generate_constellations = false;

	if (role == V90_ANALOG_MODEM) {
		configuration.local_info0.v92_analog = (struct v92_info0a){
			.v90 = analog_info0(),
			.v92_capable = true,
		};
		configuration.local_info1.v92_analog = v92_analog_info1();
		configuration.local_info1.pcm_upstream_enabled = true;
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
	bool physical = role == V90_DIGITAL_MODEM;
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

static bool v34_capabilities(const struct v90_info0 *info0, struct v34_info0 *capabilities)
{
	uint8_t payload[V90_INFO0A_PAYLOAD_BYTES];

	/* INFO0a shares V.34's capability bits; reserved clock bits encode
	 * internal timing. Decode independently of the call's field mapping. */
	return v90_info0a_encode(info0, payload, sizeof(payload)) == V90_INFO0_OK &&
	       v34_info0_decode(payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
				capabilities) == V34_INFO0_OK;
}

static bool check_v92_measured_info1d(
	const struct pcm_call *digital, struct v92_info1d *selected)
{
	const struct pcm_phase2 *phase2 = &digital->state.phase2;
	const struct v92_info1d *configured = &digital->local_info1.v92_digital;
	struct v92_info1d expected = *configured;
	struct v34_info0 peer;

	CHECK(digital->protocol == PCM_PROTOCOL_V92 &&
	      digital->local_role == V90_DIGITAL_MODEM &&
	      digital->stage == PCM_CALL_PHASE2 &&
	      phase2->stage == PCM_PHASE2_INFO1 &&
	      v34_capabilities(&digital->peer_info0.v92_analog.v90, &peer) &&
	      v34_phase2_select_v90_info1d(
		      &phase2->remote_probe, &configured->v34, &peer,
		      &expected.v34) == V34_PHASE2_SELECTION_OK,
	      "V.92 call did not retain a measured INFO1d boundary");
	expected.v34.common.local_md_length_35_ms_units =
		configured->v34.common.local_md_length_35_ms_units;
	expected.v34.common.frequency_offset_1_50_hz_units =
		configured->v34.common.frequency_offset_1_50_hz_units;
	expected.v34.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
	CHECK(v92_digital_info1_equal(
		      &expected, &phase2->state.info1.digital.record.v92),
	      "V.92 INFO1d ignored the probe or changed an owned field");
	*selected = expected;
	return true;
}

static bool run_v92_measured_info1d(
	enum g711_law law, bool pcm_upstream, bool reduce_power,
	bool shaped, struct v92_info1d *selected)
{
	struct pcm_call_configuration analog_configuration =
		v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call analog;
	struct pcm_call digital;
	struct channel channel;
	int16_t previous = 0;
	bool measured = false;

	analog_configuration.local_info0.v92_analog.v90
		.transmit_power_reduction_supported = reduce_power;
	digital_configuration.local_info1.v92_digital.pcm_upstream_supported =
		pcm_upstream;
	digital_configuration.local_info1.v92_digital.v34
		.answer_transmit[V34_SYMBOL_RATE_2800]
		.projected_data_rate_2400_bps_units = 0;
	CHECK(channel_init(&channel, law, 3u) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "V.92 measured-offer endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_call_observation analog_observation;
		struct pcm_call_observation digital_observation;
		uint8_t analog_received, digital_received;
		uint8_t analog_transmitted, digital_transmitted;

		channel_receive(&channel, &analog_received, &digital_received);
		CHECK(pcm_call_octet(&analog, analog_received,
			      decode_octet(law, analog_received), &analog_transmitted,
			      &analog_observation) == PCM_CALL_OK &&
		      pcm_call_octet(&digital, digital_received,
			      decode_octet(law, digital_received), &digital_transmitted,
			      &digital_observation) == PCM_CALL_OK,
		      "V.92 measured-offer call stopped at %zu", clock);
		if (shaped) {
			int16_t sample = decode_octet(law, analog_transmitted);

			analog_transmitted = encode_octet(law,
				(int16_t)(((int32_t)sample + previous) / 2));
			previous = sample;
		}
		channel_transmit(&channel, analog_transmitted, digital_transmitted);
		if (digital_observation.phase2.kind ==
		    PCM_PHASE2_EVENT_READY_FOR_INFO1) {
			CHECK(check_v92_measured_info1d(&digital, selected),
			      "V.92 measured offer failed");
			measured = true;
		}
		if (analog_observation.phase2.kind ==
		    PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED) {
			CHECK(measured && v92_digital_info1_equal(selected,
				      &analog.state.phase2.state.info1.digital.record.v92) &&
			      !v92_digital_info1_equal(selected,
				      &digital_configuration.local_info1.v92_digital) &&
			      v92_digital_info1_equal(&digital.local_info1.v92_digital,
				      &digital_configuration.local_info1.v92_digital),
			      "V.92 wire offer or configured policy changed");
			CHECK(selected->pcm_upstream_supported == pcm_upstream &&
			      selected->v34.common.minimum_peer_power_reduction_db == 0 &&
			      selected->v34.common.additional_peer_power_reduction_db ==
				      (reduce_power ? 3 : 0) &&
			      selected->v34.answer_transmit[V34_SYMBOL_RATE_2800]
				      .projected_data_rate_2400_bps_units == 0,
			      "V.92 offer ignored PCM permission, power or disabled rates");
			return true;
		}
	}
	CHECK(false, "V.92 measured offer did not reach its peer");
}

static bool test_v92_measured_info1d(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int pcm = 0; pcm < 2u; ++pcm)
			for (unsigned int power = 0; power < 2u; ++power) {
				struct v92_info1d flat, shaped;

				CHECK(run_v92_measured_info1d(law, pcm != 0u,
					      power != 0u, false, &flat) &&
				      run_v92_measured_info1d(law, pcm != 0u,
					      power != 0u, true, &shaped) &&
				      !v92_digital_info1_equal(&flat, &shaped),
				      "law %u PCM %u power %u ignored the changed channel",
				      (unsigned int)law, pcm, power);
			}
	return true;
}

static bool run_info1d_peer_capabilities(enum pcm_protocol protocol, enum g711_law law)
{
	bool v92 = protocol == PCM_PROTOCOL_V92;
	struct pcm_call_configuration analog_configuration = v92 ?
		v92_configuration(V90_ANALOG_MODEM, law) :
		configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration = v92 ?
		v92_configuration(V90_DIGITAL_MODEM, law) :
		configuration(V90_DIGITAL_MODEM, law);
	struct v90_info0 *peer = v92 ? &analog_configuration.local_info0.v92_analog.v90 :
		&analog_configuration.local_info0.analog;
	struct pcm_call analog, digital;
	struct channel channel;

	peer->low_carrier_3000_supported = true;
	peer->high_carrier_3000_supported = false;
	peer->low_carrier_3200_supported = true;
	peer->high_carrier_3200_supported = false;
	peer->symbol_rate_3429_supported = false;
	peer->symbol_rate_3429_transmit_permitted = false;
	CHECK(channel_init(&channel, law, 3u) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "peer-limited INFO1d endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		struct pcm_call_observation analog_observation, digital_observation;
		uint8_t analog_received, digital_received, analog_transmitted, digital_transmitted;

		channel_receive(&channel, &analog_received, &digital_received);
		CHECK(pcm_call_octet(&analog, analog_received, decode_octet(law, analog_received),
			      &analog_transmitted, &analog_observation) == PCM_CALL_OK &&
		      pcm_call_octet(&digital, digital_received, decode_octet(law, digital_received),
			      &digital_transmitted, &digital_observation) == PCM_CALL_OK,
		      "peer-limited INFO1d call stopped at %zu", clock);
		channel_transmit(&channel, analog_transmitted, digital_transmitted);
		if (analog_observation.phase2.kind == PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED) {
			const struct pcm_info1d *wire = &analog.state.phase2.state.info1.digital;
			const struct v34_info1c *offer = v92 ? &wire->record.v92.v34 : &wire->record.v90;

			CHECK(!offer->answer_transmit[V34_SYMBOL_RATE_3000].high_carrier &&
			      !offer->answer_transmit[V34_SYMBOL_RATE_3200].high_carrier &&
			      offer->answer_transmit[V34_SYMBOL_RATE_3000]
				      .projected_data_rate_2400_bps_units != 0 &&
			      offer->answer_transmit[V34_SYMBOL_RATE_3200]
				      .projected_data_rate_2400_bps_units != 0 &&
			      offer->answer_transmit[V34_SYMBOL_RATE_3429]
				      .projected_data_rate_2400_bps_units == 0,
			      "INFO1d wire offer ignored peer carrier or symbol-rate restrictions");
			CHECK(!v92 || wire->record.v92.pcm_upstream_supported,
			      "removing 3429 symbols/s also removed V.92 PCM permission");
			return true;
		}
	}
	CHECK(false, "peer-limited INFO1d did not reach its peer");
}

static bool test_info1d_peer_capabilities(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		CHECK(run_info1d_peer_capabilities(PCM_PROTOCOL_V90, law) &&
		      run_info1d_peer_capabilities(PCM_PROTOCOL_V92, law),
		      "law %u ignored peer INFO0a capabilities", (unsigned int)law);
	return true;
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

static bool endpoint_init_conventional_v92(
	struct endpoint *endpoint, enum v90_modem_role role, enum g711_law law)
{
	struct pcm_call_configuration selected = v92_configuration(role, law);
	const struct pcm_call_configuration conventional = configuration(role, law);
	struct endpoint initialized = { .upstream_received_delay = UINT16_MAX };

	selected.enabled_downstream_rate_mask = conventional.enabled_downstream_rate_mask;
	if (role == V90_ANALOG_MODEM) {
		selected.local_info1.analog = conventional.local_info1.analog;
		selected.phase3.analog = conventional.phase3.analog;
		selected.analog_phase4 = conventional.analog_phase4;
		selected.v92_analog_phase4.cpu.data_rate_number = 1u;
	} else {
		selected.local_info1.v92_digital.pcm_upstream_supported = false;
		selected.phase3.digital = conventional.phase3.digital;
		selected.digital_phase4 = conventional.digital_phase4;
	}
	if (pcm_call_init(&initialized.call, &selected) != PCM_CALL_OK)
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
		&endpoint->call, received, received_sample, true, selected_sources,
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
			&endpoint->call, received, received_sample, true, &sources,
			&generated, &event);
	}
	CHECK(result == PCM_CALL_OK, "data role %u stage %u: %s",
	      endpoint->call.local_role, endpoint->call.stage, pcm_call_result_name(result));
	endpoint->idle_source_checked = true;
	if (event.kind == PCM_CALL_DOWNSTREAM_FRAME) {
		size_t expected = endpoint->resync_data_frame ?
			endpoint->resume_frame_index : endpoint->downstream_frames;

		if (endpoint->call.local_role != V90_ANALOG_MODEM)
			return false;
		if (!downstream_frame_matches(&event.frame.downstream, expected)) {
			CHECK(endpoint->resync_data_frame, "downstream frame %zu (%u bits) differs",
			      expected, event.frame.downstream.bit_count);
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
			CHECK(endpoint->resync_data_frame, "upstream frame %zu differs", expected);
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
		struct pcm_downstream_equalizer channel = endpoint->call.downstream_equalizer;

		if (!phase4_boundary_is_transactional(&endpoint->call, law) ||
		    pcm_call_start_data_path(&endpoint->call) != PCM_CALL_OK ||
		    memcmp(&channel, &endpoint->call.downstream_equalizer, sizeof(channel)) != 0)
			return false;
		if (endpoint->call.local_role == V90_DIGITAL_MODEM)
			endpoint->upstream_received_delay = UINT16_MAX;
		endpoint->phase4_boundary_checked = true;
		endpoint->data_path_started = true;
	}
	if (pcm_call_current_stage(&endpoint->call) == PCM_CALL_DATA_PATH)
		return endpoint_data_octet(
			endpoint, received, received_sample, transmitted);
	enum pcm_call_result result = pcm_call_octet(
		    &endpoint->call, received, received_sample, transmitted,
		    &observation);
	CHECK(result == PCM_CALL_OK, "training role %u stage %u: %s",
	      endpoint->call.local_role, endpoint->call.stage, pcm_call_result_name(result));
	if (endpoint->call.protocol == PCM_PROTOCOL_V90 &&
	    endpoint->call.local_role == V90_DIGITAL_MODEM &&
	    observation.phase2.kind == PCM_PHASE2_EVENT_READY_FOR_INFO1) {
		struct v34_info1c expected;
		struct v34_info0 peer;
		const struct pcm_phase2 *phase2 = &endpoint->call.state.phase2;

		if (phase2->stage != PCM_PHASE2_INFO1 ||
		    !v34_capabilities(&endpoint->call.peer_info0.analog, &peer) ||
		    v34_phase2_select_v90_info1d(
			    &phase2->remote_probe,
			    &endpoint->call.local_info1.digital, &peer,
			    &expected) != V34_PHASE2_SELECTION_OK ||
		    !v90_info1d_equal(
			    &expected,
			    &phase2->state.info1.digital.record.v90))
			return false;
		endpoint->measured_info1d_checked = true;
	}
	if (pcm_call_uses_conventional_upstream(&endpoint->call) &&
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
	CHECK(!pcm_call_is_disconnecting(analog) && !pcm_call_is_disconnecting(digital) &&
	      pcm_call_is_disconnecting(&calls[initiator]) &&
	      !pcm_call_is_disconnecting(&calls[1u - initiator]) &&
	      !pcm_call_is_online(&calls[initiator]) &&
	      !calls[initiator].b1_deadline_armed &&
	      !calls[initiator].rate_renegotiation_deadline_armed,
	      "local cleardown retained CONNECT or training deadlines");
	for (size_t clock = 0; clock < 16000u && !(complete[0] && complete[1]); ++clock) {
		uint8_t received[2];
		uint8_t transmitted[2];

		channel_receive(&channel, &received[0], &received[1]);
		for (unsigned int side = 0; side < 2u; ++side) {
			const struct v34_full_retrain retrain_before = calls[side].retrain;
			bool closing_control = calls[side].stage == PCM_CALL_PHASE4 &&
				(side == 0u ? calls[side].state.phase4.v92.owner.analog.station.cleardown.active :
				 calls[side].state.phase4.v92.owner.digital.control.cleardown.active);
			struct pcm_call_data_event event = { 0 };
			struct pcm_call_observation observation;
			enum pcm_call_result result = calls[side].stage == PCM_CALL_PHASE4 ?
				pcm_call_octet(&calls[side], received[side],
					decode_octet(calls[side].law, received[side]),
					&transmitted[side], &observation) :
				pcm_call_data_octet(&calls[side], received[side],
					decode_octet(calls[side].law, received[side]), true, NULL,
					&transmitted[side], &event);

			CHECK((result == PCM_CALL_OK || result == PCM_CALL_CLEARDOWN_COMPLETE) &&
			      !calls[side].failed && !calls[side].retrain_active &&
			      pcm_call_is_valid(&calls[side]),
			      "law %u initiator %u side %u sample %zu cleardown returned %s stage %u",
			      (unsigned int)calls[side].law, initiator, side, clock,
			      pcm_call_result_name(result), (unsigned int)calls[side].stage);
			CHECK(!closing_control || result == PCM_CALL_CLEARDOWN_COMPLETE ||
			      memcmp(&retrain_before, &calls[side].retrain, sizeof(retrain_before)) == 0,
			      "closing control continued to hunt for retrain");
			CHECK(!closing_control || pcm_call_is_disconnecting(&calls[side]),
			      "active local or peer control lost its shutdown ownership");
			if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
				CHECK(event.kind == PCM_CALL_DATA_NONE &&
				      calls[side].stage == PCM_CALL_CLOSED &&
				      pcm_call_is_disconnecting(&calls[side]) &&
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
		CHECK(pcm_call_data_octet(&calls[side], 0u, INT16_MAX, true, &sources,
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
				      decode_octet(calls[side].law, received[side]), true, NULL,
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
			call, silence_octet(call->law), 0, true, NULL, &output, &event);

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
		      analog.call.downstream_equalizer.training_level == 0 &&
		      digital.call.downstream_equalizer.training_level == 0 &&
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
	enum pcm_protocol protocol = call->protocol;
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
	      call->protocol == protocol &&
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

static bool check_initial_info1_retrain_tone(
	enum g711_law law, enum pcm_protocol protocol)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	struct pcm_call retained[2];
	struct pcm_call marks;
	bool stored[2] = { false, false };
	bool initialized;

	if (protocol == PCM_PROTOCOL_V92) {
		struct pcm_call_configuration ac =
			v92_configuration(V90_ANALOG_MODEM, law);
		struct pcm_call_configuration dc =
			v92_configuration(V90_DIGITAL_MODEM, law);

		analog = (struct endpoint){ .upstream_received_delay = UINT16_MAX };
		digital = (struct endpoint){ .upstream_received_delay = UINT16_MAX };
		initialized = pcm_call_init(&analog.call, &ac) == PCM_CALL_OK &&
			pcm_call_init(&digital.call, &dc) == PCM_CALL_OK;
	} else
		initialized = endpoint_init(
			&analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
			endpoint_init(&digital, V90_DIGITAL_MODEM, law,
				      V90_JD_RATE_CAPABILITY_MASK, 0u);
	CHECK(channel_init(&channel, law, 3u) && initialized,
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
	CHECK(stored[0] && stored[1] &&
	      retained[0].protocol == protocol && retained[1].protocol == protocol,
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
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(check_initial_info1_retrain_tone(law, protocol),
			      "law %u protocol %u initial INFO1 retrain matrix failed",
			      (unsigned int)law, (unsigned int)protocol);
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
				      call, received, decode_octet(law, received), true,
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
			&stalled->call, stall_octet, 0, true, NULL,
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
			      &stalled->call, stall_octet, 0, true, NULL,
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

/* V.92 9.5.1.2: Ja is timed from INFO1a; both Su waits are timed
 * from TRN1d. Capture real receive edges before testing stalled copies. */
static uint32_t v92_phase3_response_elapsed(const struct pcm_call *call, unsigned kind)
{
	const struct pcm_phase3_digital_tx *tx = &call->state.phase3.v92_digital.downstream;
	if (kind == 0u)
		return 160000u + 6u * call->round_trip_delay_samples - call->b1_deadline_remaining_samples;
	if (tx->stage == PCM_PHASE3_DIGITAL_TX_PREFIX)
		return tx->sequence.prefix.sequence.signs.trn1d_symbols;
	return tx->sequence.signs.trn1d_symbols +
		(tx->jd_records + tx->jp_records) * PCM_J_FRAME_BITS + tx->bit_index;
}

static bool v92_phase3_missing_response(const struct pcm_call *boundary, unsigned kind)
{
	struct pcm_call call = *boundary;
	uint32_t deadline = (kind == 0u ? 36000u : 40800u) + call.round_trip_delay_samples;

	for (uint32_t elapsed = v92_phase3_response_elapsed(&call, kind) + 1u; elapsed <= deadline; ++elapsed) {
		struct pcm_call_observation event;
		uint8_t output, silence = silence_octet(call.law);
		enum pcm_call_result result = pcm_call_octet(&call, silence, 0, &output, &event);
		CHECK(result == PCM_CALL_OK && call.stage ==
			(elapsed < deadline ? PCM_CALL_PHASE3 : PCM_CALL_RETRAIN_SILENCE),
			"law %u kind %u elapsed %u/%u result %s stage %u",
			call.law, kind, elapsed, deadline, pcm_call_result_name(result), call.stage);
	}
	CHECK(call.retrain_active && call.protocol == PCM_PROTOCOL_V92 && call.peer_info0_stored &&
		!call.b1_deadline_armed && !call.failed && pcm_call_is_valid(&call),
		"timeout lost the retained V.92 call");
	{
		struct pcm_call_observation event;
		uint8_t output, silence = silence_octet(call.law);
		CHECK(pcm_call_octet(&call, silence, 0, &output, &event) == PCM_CALL_OK &&
		      event.processed == PCM_CALL_RETRAIN_SILENCE && output == silence,
		      "missing response did not begin protocol retrain silence");
	}
	return true;
}

static bool v92_phase3_timely_response(const struct pcm_call *before, uint8_t received, bool ja)
{
	struct pcm_call call = *before;
	struct pcm_call_observation event;
	uint8_t transmitted, silence = silence_octet(call.law);
	uint32_t deadline = (ja ? 36000u : 40800u) + call.round_trip_delay_samples;

	/* Move only the elapsed counts: the generated receive history and
	 * detection sample remain exact. A buffered record may still complete
	 * if just its last input sample is erased; test absence separately. */
	if (ja) {
		call.b1_deadline_remaining_samples =
			160000u + 6u * call.round_trip_delay_samples - deadline + 1u;
	} else {
		struct pcm_phase3_digital_tx *tx = &call.state.phase3.v92_digital.downstream;
		CHECK(tx->stage == PCM_PHASE3_DIGITAL_TX_JP, "final Su did not arrive during Jp");
		uint32_t tail = deadline - 1u - tx->sequence.signs.trn1d_symbols - PCM_J_FRAME_BITS;
		tx->jd_records = 1u;
		tx->jp_records = tail / PCM_J_FRAME_BITS;
		tx->bit_index = tail % PCM_J_FRAME_BITS;
	}
	CHECK(pcm_call_is_valid(&call), "rebased response is invalid");
	CHECK(pcm_call_octet(&call, received, decode_octet(call.law, received), &transmitted, &event) ==
		PCM_CALL_OK && call.stage == PCM_CALL_PHASE3 && !call.retrain_active &&
		call.state.phase3.v92_digital.receive_stage == (ja ? V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE :
			V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U), "last-tick response lost to timeout");
	if (!ja) {
		call.state.phase3.v92_digital.downstream.jd_records += 1000u;
		CHECK(pcm_call_is_valid(&call) && pcm_call_octet(&call, silence, 0, &transmitted, &event) ==
			PCM_CALL_OK && call.stage == PCM_CALL_PHASE3 && !call.retrain_active,
			"accepted Su did not cancel its deadline while Jp drains");
	}
	return true;
}

static bool run_v92_phase3_deadlines(enum g711_law law, size_t delay)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2], boundaries[3];
	struct channel wire;
	bool captured[3] = { false };
	bool timely[2] = { false };
	CHECK(pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
		pcm_call_init(&calls[1], &dc) == PCM_CALL_OK && channel_init(&wire, law, delay),
		"V.92 deadline endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT && !timely[1]; ++clock) {
		uint8_t rx[2], tx[2];
		struct pcm_call before = calls[1];
		channel_receive(&wire, &rx[0], &rx[1]);
		for (unsigned side = 0; side < 2; ++side) {
			struct pcm_call_observation event;
			CHECK(pcm_call_octet(&calls[side], rx[side], decode_octet(law, rx[side]),
				&tx[side], &event) == PCM_CALL_OK, "V.92 deadline setup failed at %zu side %u", clock, side);
		}
		channel_transmit(&wire, tx[0], tx[1]);
		if (calls[1].stage != PCM_CALL_PHASE3)
			continue;
		const struct v92_phase3_digital_station *station = &calls[1].state.phase3.v92_digital;
		const struct pcm_phase3_digital_tx *txp = &station->downstream;
		if (before.stage == PCM_CALL_PHASE3) {
			enum v92_phase3_digital_receive_stage previous = before.state.phase3.v92_digital.receive_stage;
			if (previous == V92_PHASE3_DIGITAL_RECEIVE_INITIAL &&
			    station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SU_PHASE) {
				CHECK(v92_phase3_timely_response(&before, rx[1], true), "timely Ja failed");
				timely[0] = true;
			}
			if (previous == V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE &&
			    station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SECOND_TRN1U) {
				CHECK(v92_phase3_timely_response(&before, rx[1], false), "timely Su failed");
				timely[1] = true;
			}
		}
		bool reached[] = {
			station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_INITIAL,
			station->transmit_stage == V92_PHASE3_DIGITAL_TRANSMIT_DOWNSTREAM &&
				txp->stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
				txp->sequence.prefix.stage == PCM_PHASE3_DIGITAL_PREFIX_TRN1D &&
				txp->sequence.prefix.sequence.signs.trn1d_symbols == 0u,
			station->receive_stage == V92_PHASE3_DIGITAL_RECEIVE_SU_RESPONSE,
		};
		for (unsigned kind = 0; kind < 3; ++kind)
			if (!captured[kind] && reached[kind]) {
				boundaries[kind] = calls[1];
				captured[kind] = true;
			}
	}
	CHECK(captured[0] && captured[1] && captured[2] && timely[0] && timely[1],
		"did not capture all three response boundaries");
	bool passed = true;
	for (unsigned kind = 0; kind < 3; ++kind)
		passed = v92_phase3_missing_response(&boundaries[kind], kind) && passed;
	return passed;
}

static bool test_v92_phase3_deadlines(void)
{
	bool passed = true;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned delay = 3; delay <= 13; delay += 10)
			passed = run_v92_phase3_deadlines(law, delay) && passed;
	return passed;
}

static bool v92_analog_missing_response(const struct pcm_call *boundary, unsigned kind)
{
	struct pcm_call call = *boundary;
	struct v90_phase3_sd sd;
	struct v90_phase3_signs trn1d;
	uint8_t ucode = call.state.phase3.v92_analog.downstream_training_ucode;
	unsigned deadline = kind == 2u ? 36000u : 12000u;
	unsigned elapsed = kind == 2u ? call.state.phase3.v92_analog.post_ja_silence_samples :
		(unsigned)call.state.phase3.v92_analog.transmit.initial.sequence.ja.emitted_symbols;
	CHECK(v90_phase3_sd_init(&sd, call.law, ucode, V90_PHASE3_SD) == V90_PHASE3_SD_OK &&
		v90_phase3_signs_init(&trn1d, call.law, ucode) == V90_PHASE3_SIGNS_OK,
		"analogue response generators did not initialize");
	while (++elapsed <= deadline) {
		struct v90_pcm_symbol input = { .octet = silence_octet(call.law) };
		struct pcm_call_observation event;
		uint8_t output;
		if (kind == 1u) {
			if (v90_phase3_sd_remaining(&sd) == 0u)
				CHECK(v90_phase3_sd_init(&sd, call.law, ucode, V90_PHASE3_SD) ==
					V90_PHASE3_SD_OK, "repeated Sd failed");
			CHECK(v90_phase3_sd_next(&sd, &input) == V90_PHASE3_SD_OK, "Sd generation failed");
		} else if (kind == 2u) {
			CHECK(v90_phase3_signs_next_trn1d(&trn1d, &input) == V90_PHASE3_SIGNS_OK,
				"TRN1d generation failed");
		}
		CHECK(pcm_call_octet(&call, input.octet, input.sample, &output, &event) == PCM_CALL_OK &&
			call.stage == (elapsed < deadline ? PCM_CALL_PHASE3 : PCM_CALL_RETRAIN_SILENCE),
			"law %u kind %u missed analogue deadline %u/%u, stage %u",
			call.law, kind, elapsed, deadline, call.stage);
	}
	CHECK(call.retrain_active && call.protocol == PCM_PROTOCOL_V92 && call.peer_info0_stored &&
		!call.b1_deadline_armed && !call.failed && pcm_call_is_valid(&call),
		"analogue timeout lost the retained V.92 call");
	return true;
}

static bool v92_analog_timely_response(const struct pcm_call *before, uint8_t input,
	uint8_t next_input, bool sd)
{
	struct pcm_call call = *before;
	struct v92_phase3_analog_station *station = &call.state.phase3.v92_analog;
	struct pcm_call_observation event;
	uint8_t output;
	/* Keep the real receiver history and detection sample, changing only
	 * the elapsed counter. Receive must win on the last allowed tick. */
	if (sd)
		station->transmit.initial.sequence.ja.emitted_symbols = 11999u;
	else
		station->post_ja_silence_samples = 35999u;
	CHECK(pcm_call_is_valid(&call) &&
		pcm_call_octet(&call, input, decode_octet(call.law, input), &output, &event) == PCM_CALL_OK &&
		call.stage == PCM_CALL_PHASE3 && !call.retrain_active,
		"last-tick analogue response lost to timeout");
	if (sd) {
		CHECK(station->receive.sd.stage == V90_PHASE3_SD_RECEIVER_SD_BAR &&
			station->transmit.initial.stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE &&
			station->post_ja_silence_samples == 0u,
			"last-tick Sd reversal did not end Ja on its twelve-symbol boundary");
		int16_t sample = 1;
		CHECK(v92_phase3_analog_station_transmit(station, NULL, &sample) ==
			V92_PHASE3_ANALOG_STATION_OK && sample == 0 && station->post_ja_silence_samples == 1u,
			"silence clock did not start after the last Ja symbol");
		station->post_ja_silence_samples = UINT32_MAX;
		CHECK(v92_phase3_analog_station_transmit(station, NULL, &sample) ==
			V92_PHASE3_ANALOG_STATION_OK && sample == 0 && station->post_ja_silence_samples == UINT32_MAX,
			"unattended station silence counter wrapped");
	} else {
		CHECK(station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU &&
			station->post_ja_silence_samples == 36000u,
			"accepted Jd has transmit stage %u and silence count %u",
			station->transmit_stage, station->post_ja_silence_samples);
		enum pcm_call_result result = pcm_call_octet(&call, next_input,
			decode_octet(call.law, next_input), &output, &event);
		CHECK(result == PCM_CALL_OK &&
			call.stage == PCM_CALL_PHASE3 && station->post_ja_silence_samples == 36000u,
			"accepted Jd next input returned %s, stage %u, silence %u",
			pcm_call_result_name(result), call.stage, station->post_ja_silence_samples);
	}
	return true;
}

static bool run_v92_analog_phase3_deadlines(enum g711_law law, size_t delay)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2], boundaries[2];
	struct channel wire;
	bool captured[2] = { false }, timely[2] = { false }, plain_sd_seen = false;
	CHECK(pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
		pcm_call_init(&calls[1], &dc) == PCM_CALL_OK && channel_init(&wire, law, delay),
		"analogue deadline endpoints did not initialize");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT && !timely[1]; ++clock) {
		struct pcm_call before = calls[0];
		uint8_t rx[2], tx[2];
		channel_receive(&wire, &rx[0], &rx[1]);
		for (unsigned side = 0; side < 2; ++side) {
			struct pcm_call_observation event;
			CHECK(pcm_call_octet(&calls[side], rx[side], decode_octet(law, rx[side]),
				&tx[side], &event) == PCM_CALL_OK, "analogue deadline setup failed at %zu", clock);
		}
		channel_transmit(&wire, tx[0], tx[1]);
		if (calls[0].stage != PCM_CALL_PHASE3)
			continue;
		const struct v92_phase3_analog_station *station = &calls[0].state.phase3.v92_analog;
		const struct v92_pcm_phase3_initial_tx *initial = &station->transmit.initial;
		if (station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_INITIAL) {
			if (station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_SD &&
			    station->receive.sd.stage == V90_PHASE3_SD_RECEIVER_SD) {
				CHECK(initial->stage == V92_PCM_PHASE3_INITIAL_TX_JA && !initial->finish_requested &&
					station->post_ja_silence_samples == 0u, "plain Sd ended Ja before its reversal");
				plain_sd_seen = true;
			}
			if (!captured[0] && initial->stage == V92_PCM_PHASE3_INITIAL_TX_JA &&
			    initial->sequence.ja.emitted_symbols == 1u) {
				boundaries[0] = calls[0];
				captured[0] = true;
			}
			if (!captured[1] && station->receive_stage == V92_PHASE3_ANALOG_RECEIVE_J) {
				CHECK(initial->stage == V92_PCM_PHASE3_INITIAL_TX_COMPLETE &&
					initial->sequence.ja.emitted_symbols % 12u == 0u &&
					station->receive.j.stage == V92_PHASE3_J_RECEIVER_TRN1D,
					"Jd wait did not follow an aligned Ja and complete Sd-bar");
				boundaries[1] = calls[0];
				captured[1] = true;
			}
		}
		if (before.stage != PCM_CALL_PHASE3)
			continue;
		const struct v92_phase3_analog_station *previous = &before.state.phase3.v92_analog;
		if (!timely[0] && previous->receive_stage == V92_PHASE3_ANALOG_RECEIVE_SD &&
		    previous->receive.sd.stage == V90_PHASE3_SD_RECEIVER_SD &&
		    station->receive.sd.stage == V90_PHASE3_SD_RECEIVER_SD_BAR) {
			CHECK(v92_analog_timely_response(&before, rx[0], 0u, true), "timely Sd failed");
			timely[0] = true;
		}
		if (!timely[1] && previous->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_INITIAL &&
		    station->transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU) {
			uint8_t following[2];
			channel_receive(&wire, &following[0], &following[1]);
			CHECK(v92_analog_timely_response(&before, rx[0], following[0], false), "timely Jd failed");
			timely[1] = true;
		}
	}
	CHECK(captured[0] && captured[1] && timely[0] && timely[1] && plain_sd_seen,
		"did not capture both analogue response boundaries");
	bool passed = true;
	for (unsigned kind = 0; kind < 3; ++kind)
		passed = v92_analog_missing_response(&boundaries[kind == 2u], kind) && passed;
	return passed;
}

static bool test_v92_analog_phase3_deadlines(void)
{
	bool passed = true;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned delay = 3; delay <= 13; delay += 10)
			passed = run_v92_analog_phase3_deadlines(law, delay) && passed;
	return passed;
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
			      analog.call.phase3_entry.selection.analog.conventional.training_ucode, kind) ==
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
			       call, received, decode_octet(law, received), true,
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
	enum pcm_protocol protocol, enum g711_law law,
	enum v90_modem_role initiating_role, bool generated)
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
	      (protocol == PCM_PROTOCOL_V92 ?
		      endpoint_init_conventional_v92(&analog, V90_ANALOG_MODEM, law) &&
		      endpoint_init_conventional_v92(&digital, V90_DIGITAL_MODEM, law) :
		      endpoint_init(&analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
		      endpoint_init(&digital, V90_DIGITAL_MODEM, law,
			      V90_JD_RATE_CAPABILITY_MASK, 0u)),
	      "law %u retrain endpoints did not initialize",
	      (unsigned int)law);
	if (generated) {
		struct pcm_call_configuration profile;

		CHECK(v90_profile_init(&profile, V90_ANALOG_MODEM, law, 28000, 28000, NULL) ==
			      V90_PROFILE_OK, "generated conventional profile failed");
		analog.call.analog_phase4_configuration = profile.analog_phase4;
		/* This variant exercises V.92's conventional child, not the fixture's
		 * separately supplied multi-plane PCM-upstream CPt/CPu records. */
		if (protocol == PCM_PROTOCOL_V92)
			analog.call.local_info1.pcm_upstream_enabled = false;
	}
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
	CHECK(pcm_call_uses_conventional_upstream(&analog.call) &&
	      pcm_call_uses_conventional_upstream(&digital.call) &&
	      analog.call.protocol == protocol && digital.call.protocol == protocol &&
	      analog.info1_b1_deadline_seen && digital.info1_b1_deadline_seen,
	      "conventional selection lost session/deadline: mode %u/%u "
	      "protocol %u/%u expected %u deadline %d/%d",
	      analog.call.phase3_entry.selection.upstream,
	      digital.call.phase3_entry.selection.upstream,
	      analog.call.protocol, digital.call.protocol, protocol,
	      analog.info1_b1_deadline_seen, digital.info1_b1_deadline_seen);
	if (protocol == PCM_PROTOCOL_V92) {
		uint32_t at, ar, dt, dr;

		CHECK(pcm_call_get_data_rates(&analog.call, &at, &ar) &&
		      pcm_call_get_data_rates(&digital.call, &dt, &dr) &&
		      at == dr && ar == dt && ar == 28000u && at > 0u && at <= 33600u &&
		      check_peer_cleardown(&analog.call, &digital.call, &channel, 0u) &&
		      check_peer_cleardown(&analog.call, &digital.call, &channel, 1u),
		      "V.92 conventional rates or bidirectional cleardown failed");
	}
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
	      initiator->call.downstream_equalizer.training_level == 0 &&
	      initiator->call.retrain_active &&
	      initiator->call.round_trip_delay_stored &&
	      initiator->call.round_trip_delay_samples == initiator_prior_rtd &&
	      initiator->call.protocol == protocol &&
	      !pcm_call_uses_conventional_upstream(&initiator->call),
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
			CHECK(initiator->call.state.phase2.info0_protocol == protocol &&
			      initiator->call.round_trip_delay_stored &&
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
			      responder->call.downstream_equalizer.training_level == 0 &&
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
			CHECK(responder->call.state.phase2.info0_protocol == protocol &&
			      responder->call.round_trip_delay_stored &&
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
	      analog.call.protocol == protocol && digital.call.protocol == protocol &&
	      pcm_call_uses_conventional_upstream(&analog.call) &&
	      pcm_call_uses_conventional_upstream(&digital.call) &&
	      analog.call.peer_info0_stored &&
	      digital.call.peer_info0_stored &&
	      !pcm_call_has_failed(&analog.call) &&
	      !pcm_call_has_failed(&digital.call),
	      "law %u role %u did not complete local/remote retrain",
	      (unsigned int)law, (unsigned int)initiating_role);
	if (generated) {
		const struct pcm_call_analog_phase4_configuration *profile = &analog.call.analog_phase4_configuration;
		struct v90_pcm_mapping training, data;
		uint32_t tf, tc, df, dc;

		CHECK(v90_pcm_mapping_init(&training, &profile->training_parameters) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_init(&data, &profile->data_parameters) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_power(&training, V90_PCM_MAPPING_CODEC, law, &tf, &tc) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_power(&data, V90_PCM_MAPPING_CODEC, law, &df, &dc) == V90_PCM_MAPPING_OK &&
		      tc <= 5368u * 5368u && dc <= 5368u * 5368u && dc <= tf * pow(10.0, 0.3),
		      "generated conventional call exceeded peer power after retrain");
	}
	return true;
}

static bool test_full_retrain(void)
{
	for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
	     protocol <= PCM_PROTOCOL_V92; ++protocol)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			CHECK(run_full_retrain(protocol, law, V90_ANALOG_MODEM, false) &&
			      run_full_retrain(protocol, law, V90_DIGITAL_MODEM, false) &&
			      run_full_retrain(protocol, law,
				      law == G711_LAW_PCMU ? V90_ANALOG_MODEM : V90_DIGITAL_MODEM, true),
			      "protocol %u law %u full retrain matrix failed", protocol, law);
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

		if (peer->rt_symbols >= PCM_R_SYMBOLS)
			positive = !positive;
		if (v90_pcm_mapping_ucode(&path->training_mapping, peer->decision_plane,
					 interval, 0u, &ucode) != V90_PCM_MAPPING_OK ||
		    !v90_pcm_symbol_make(&expected, peer->law, ucode, positive) ||
		    octet != expected.octet)
			return false;
		if (++peer->rt_symbols == PCM_R_SEQUENCE_SYMBOLS) {
			if (pcm_downstream_rx_resume_after_rt(&peer->downstream.training, positive) != PCM_DOWNSTREAM_RX_OK)
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
			      decode_octet(law, digital_received), true, &sources,
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
				      decode_octet(law, digital_received), true, NULL, &output, &ignored) == PCM_CALL_OK &&
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

			invalid.state.data_path.digital.downstream.rd.signal = PCM_R_RF;
			CHECK(pcm_r_tx_is_valid(&invalid.state.data_path.digital.downstream.rd) &&
			      !pcm_call_is_valid(&invalid),
			      "V.90 rate renegotiation accepted a V.92 fast request");
			invalid = digital.call;
			deadline_started = true;
			invalid.rate_renegotiation_deadline_armed = false;
			invalid.rate_renegotiation_deadline_remaining_samples = 0u;
			stalled.rate_renegotiation_deadline_remaining_samples = 1u;
			CHECK(digital.call.state.data_path.digital.downstream_stage ==
				      V90_DIGITAL_PATH_DOWNSTREAM_RD &&
			      digital.call.state.data_path.digital.downstream.rd
				      .transmitted_symbols == PCM_R_SYMBOLS &&
			      digital.call.rate_renegotiation_deadline_remaining_samples ==
				      expected_deadline &&
			      !pcm_call_is_valid(&invalid) &&
			      pcm_call_is_valid(&stalled) &&
			      pcm_call_data_octet(
				      &stalled, silence_octet(law), 0, true, NULL,
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
	      rd_symbols == PCM_R_SEQUENCE_SYMBOLS &&
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
			      peer.rt_symbols == PCM_R_SEQUENCE_SYMBOLS),
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
		      &last_sample, received, decode_octet(before->law, received), true,
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
			      decode_octet(law, analog_received), true, &analog_sources,
			      &analog_transmitted, &analog_event) == PCM_CALL_OK &&
		      pcm_call_data_octet(
			      &digital.call, digital_received,
			      decode_octet(law, digital_received), true, &digital_sources,
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

		rd_detected |= pcm_r_rx_signal_detected(
			&analog_path->rate_renegotiation);
		rd_bar_started |= analog_path->rate_renegotiation.stage ==
			PCM_R_RX_BAR;
		rd_bar_complete |= pcm_r_rx_is_complete(
			&analog_path->rate_renegotiation);
		if (!analog_started && analog_path->rate_renegotiation_active) {
			struct pcm_call early_deadline = analog.call;

			early_deadline.state.data_path.analog.rate_renegotiation.signal = PCM_R_RF;
			CHECK(pcm_r_rx_is_valid(
				      &early_deadline.state.data_path.analog.rate_renegotiation) &&
			      !pcm_call_is_valid(&early_deadline),
			      "V.90 analogue path accepted a V.92 fast detector");
			early_deadline = analog.call;
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
				      decode_octet(law, analog_received), true, NULL,
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
	      rd_high_water == PCM_R_SEQUENCE_SYMBOLS &&
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

static bool run_unusable_retrain_selection(
	enum pcm_protocol protocol, enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool initially_online = false;
	bool remote_retrain_started = false;
	bool selection_invalidated = false;
	bool recovery_started = false;

	CHECK(channel_init(&channel, law, 3u) &&
	      (protocol == PCM_PROTOCOL_V92 ?
		      endpoint_init_conventional_v92(&analog, V90_ANALOG_MODEM, law) &&
		      endpoint_init_conventional_v92(&digital, V90_DIGITAL_MODEM, law) :
		      endpoint_init(&analog, V90_ANALOG_MODEM, law, UINT32_C(1), 1u) &&
		      endpoint_init(&digital, V90_DIGITAL_MODEM, law,
			      V90_JD_RATE_CAPABILITY_MASK, 0u)),
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
				struct v34_info1c *offer = protocol == PCM_PROTOCOL_V92 ?
					&digital_info1->digital.record.v92.v34 :
					&digital_info1->digital.record.v90;

				/* Keep the offer already decoded by the peer, but make
				 * that peer's returning 3000-symbol/s choice unusable to
				 * the digital call owner at Phase 3 entry. */
				offer->answer_transmit[V34_SYMBOL_RATE_3000]
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
				      digital.call.protocol == protocol &&
				      !pcm_call_has_failed(&digital.call),
				      "law %u unusable retrain selection returned %s/%s",
				      (unsigned int)law,
				      pcm_call_result_name(strict_result),
				      pcm_call_result_name(result));
				recovery_started = true;
				channel_transmit(
					&channel, analog_transmitted, digital_transmitted);
				break;
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
	CHECK(recovery_started, "law %u unusable retrain selection was not received",
	      (unsigned int)law);
	/* Prove the new epoch retains the negotiated protocol and returns data,
	 * not merely that the unusable selection was converted into silence. */
	analog.resume_frame_index = digital.downstream_source.frames;
	digital.resume_frame_index = analog.upstream_source.frames;
	analog.resync_data_frame = true;
	digital.resync_data_frame = true;
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		CHECK(step_endpoints(&channel, &analog, &digital, law),
		      "law %u recovery stopped at %zu", (unsigned int)law, clock);
		if (pcm_call_is_online(&analog.call) && pcm_call_is_online(&digital.call) &&
		    analog.downstream_frames >=
			    analog.resume_frame_index + DATA_FRAMES_TO_RECEIVE &&
		    digital.upstream_frames >=
			    digital.resume_frame_index + DATA_FRAMES_TO_RECEIVE) {
			CHECK(analog.call.protocol == protocol &&
			      digital.call.protocol == protocol &&
			      !analog.call.retrain_active && !digital.call.retrain_active,
			      "law %u recovery lost its protocol or retrain boundary",
			      (unsigned int)law);
			return true;
		}
	}
	CHECK(false, "law %u recovery did not resume bidirectional data",
	      (unsigned int)law);
	return false;
}

static bool test_unusable_retrain_selection(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(run_unusable_retrain_selection(protocol, law),
			      "protocol %u law %u unusable retrain selection did not restart",
			      (unsigned int)protocol, (unsigned int)law);
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
	    retraining.downstream_equalizer.training_level != 0 ||
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
	       retraining.v92_upstream_retry_ceiling == completed->v92_upstream_retry_ceiling &&
	       memcmp(&retraining.v92_digital_phase4_configuration,
		      &completed->v92_digital_phase4_configuration,
		      sizeof(retraining.v92_digital_phase4_configuration)) == 0 &&
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
	struct v92_info1d selected_info1d = { 0 };
	bool measured_info1d = false;
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
	bool cleardown_checked = false;
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
			v92_phase4_analog_station_can_handoff(
				&analog.state.phase4.v92.owner.analog.station);
		bool digital_control_active =
			digital.stage == PCM_CALL_PHASE4 &&
			digital.state.phase4.v92.child ==
				PCM_CALL_V92_PHASE4_CONTROL;

		if (!cleardown_checked && digital_control_active && analog.stage == PCM_CALL_PHASE4 &&
		    analog.state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
		    analog.state.phase4.v92.owner.analog.station.transmit.stage == V92_PHASE4_ANALOG_TX_SUVU &&
		    digital.state.phase4.v92.owner.digital.control.transmit_stage == V92_DOWNSTREAM_CONTROL_CPD &&
		    digital.state.phase4.v92.owner.digital.control.initial_cpd.bit_index != 0u) {
			CHECK(check_peer_cleardown(&analog, &digital, &channel, 0u) &&
			      check_peer_cleardown(&analog, &digital, &channel, 1u),
			      "PCM-upstream control cleardown failed");
			cleardown_checked = true;
		}
		if (analog_control_ready && !analog_control_reached) {
			analog_control = analog.state.phase4.v92.owner.analog;
			analog_control_reached = true;
		}
		if (digital_control_active && !digital_control_reached)
			digital_control = digital.state.phase4.v92.owner.digital;

		if (analog_ready && !analog_handoff_reached) {
			CHECK(cleardown_checked, "PCM-upstream control missed cleardown boundary");
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
				predicted.transmit.control.completed_cpu_records;
			int16_t predicted_sample;

			if (v92_phase4_analog_station_transmit(
				    &predicted, &predicted_sample) ==
				    V92_PHASE4_ANALOG_STATION_OK &&
			    predicted.transmit.control.completed_cpu_records ==
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
						      .control.completed_cpu_records,
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
		/* Lose the first CPu on the wire, not by announcing digital readiness
		 * before the receiver listens for control. Keep SUVu intact so its
		 * repeated peer status can qualify the 100 ms + RTD retry. */
		if (analog.stage == PCM_CALL_PHASE4 &&
		    analog.state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
		    analog.state.phase4.v92.owner.analog.station.transmit.stage == V92_PHASE4_ANALOG_TX_CPU &&
		    !analog.state.phase4.v92.owner.analog.station.transmit.control.cpu_sent)
			analog_transmitted = silence_octet(law);
		if (analog_control_ready) {
			struct v92_phase4_record_tx expected =
				analog_control.station.transmit.signal.record;
			struct v92_phase4_modulator modulator = expected.modulator;
			int16_t sample;

			CHECK((!v92_phase4_record_tx_is_complete(&expected) ||
			       v92_phase4_record_tx_init_e2u(&expected, &modulator,
				       analog_control.station.mapping.cpd.extend_e2u) ==
				       V92_PHASE4_RECORD_TX_OK) &&
			      v92_phase4_record_tx_next(&expected, &sample) ==
				      V92_PHASE4_RECORD_TX_OK &&
			      analog_transmitted == encode_octet(law, sample) &&
			      analog.stage == PCM_CALL_PHASE4 &&
			      analog.state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U,
			      "mutual acknowledgement lost a pending control/E2u symbol");
		}
		digital_result = digital_ready ? PCM_CALL_PHASE4_READY :
			pcm_call_octet(
				&digital, digital_received,
				decode_octet(law, digital_received),
				&digital_transmitted,
				&digital_observation);
		if (digital_observation.phase2.kind == PCM_PHASE2_EVENT_READY_FOR_INFO1) {
			CHECK(check_v92_measured_info1d(&digital, &selected_info1d),
			      "V.92 handoff ignored the measured offer");
			measured_info1d = true;
		}
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
		    analog.state.phase4.v92.owner.analog.station.transmit.control.cpu_sent &&
		    first_cpu_sent == SIZE_MAX)
			first_cpu_sent = clock;
		if (digital.stage == PCM_CALL_PHASE4 &&
		    digital.state.phase4.v92.child ==
			    PCM_CALL_V92_PHASE4_CONTROL &&
		    digital.state.phase4.v92.owner.digital.control.cpu_received &&
		    first_cpu_received == SIZE_MAX)
			first_cpu_received = clock;
		if (analog_handoff_reached && digital_handoff_reached)
			break;
	}
	struct v92_pcm_info1a expected_info1a = analog_configuration.local_info1.v92_analog;
	expected_info1a.training_ucode = analog.phase3_entry.selection.analog.pcm.training_ucode;
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
	      analog.v92_peer_jd_stored &&
	      analog.v92_peer_jd.data_rate_capability_mask ==
		      digital_configuration.phase3.v92_digital.jd
			      .data_rate_capability_mask &&
	      analog.v92_peer_jd.maximum_spectral_shaping_lookahead ==
		      digital_configuration.phase3.v92_digital.jd.maximum_spectral_shaping_lookahead &&
	      !digital.v92_peer_jd_stored &&
	      digital.v92_peer_jd.data_rate_capability_mask == 0u &&
	      digital.v92_peer_jd.maximum_spectral_shaping_lookahead == 0u &&
	      analog.phase3_entry.selection.startup.protocol ==
		      PCM_PROTOCOL_V92 &&
	      digital.phase3_entry.selection.startup.protocol ==
		      PCM_PROTOCOL_V92 &&
	      analog.phase3_entry.selection.upstream == PCM_UPSTREAM_PCM &&
	      digital.phase3_entry.selection.upstream == PCM_UPSTREAM_PCM &&
	      measured_info1d && v92_digital_info1_equal(
		      &analog.phase3_entry.selection.digital.record.v92,
		      &selected_info1d) &&
	      v92_digital_info1_equal(
		      &digital.phase3_entry.selection.digital.record.v92,
		      &selected_info1d) &&
	      v92_pcm_info1a_equal(
		      &analog.phase3_entry.selection.analog.pcm,
		      &expected_info1a) &&
	      v92_pcm_info1a_equal(
		      &digital.phase3_entry.selection.analog.pcm,
		      &expected_info1a) &&
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
	      analog_control.station.transmit.control.retry == V92_UPSTREAM_RETRY_IDLE &&
	      analog_control.observed_cpu_records ==
		      analog_control.station.transmit.control.completed_cpu_records &&
	      analog_control.observed_cpu_records >= 2u &&
	      v92_cpu_frame_bits(
		      &analog_configuration.v92_analog_phase4.cpu, 24u) /
		      2u >
		      analog_control.cpu_repeat_interval_samples &&
	      v92_phase4_analog_station_can_handoff(
		      &analog_control.station) &&
	      v92_cpu_parameters_are_valid(
		      &digital.state.phase4.v92.owner.e2u.state.digital.cpu) &&
	      v92_pcm_mapping_equal(
		      &digital.state.phase4.v92.owner.e2u.state.digital.mapping,
		      &analog.state.phase4.v92.owner.e2u.state.analog.mapping) &&
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
	      digital.state.phase4.v92.owner.e2u.state.digital.mapping.cpd
		      .data_rate_number ==
		      digital_configuration.v92_digital_phase4.cpd
			      .data_rate_number &&
	      digital.state.phase4.v92.owner.e2u.state.digital.mapping.cpd.trellis ==
		      digital_configuration.v92_digital_phase4.cpd.trellis &&
	      digital.state.phase4.v92.owner.e2u.state.digital.mapping.cpd
		      .four_times_gain_q0_16 ==
		      digital_configuration.v92_digital_phase4.cpd
			      .four_times_gain_q0_16 &&
	      digital.state.phase4.v92.owner.e2u.state.digital.mapping.cpd.extend_e2u ==
		      digital_configuration.v92_digital_phase4.cpd.extend_e2u &&
	      v90_pcm_mapping_equal(
		      &analog_control.station.receive.stream.decoder.mapping,
		      &digital_control.control.transmit.encoder.mapping),
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
		      .transmit.control.completed_cpu_records,
	      analog_control.cpu_repeat_interval_samples,
	      analog_control.cpu_repeat_remaining_samples,
	      analog.round_trip_delay_samples);
	CHECK(phase4_boundary_is_transactional(&analog, law) &&
	      phase4_boundary_is_transactional(&digital, law) &&
	      retrain_preserves_protocol(&analog, law) &&
	      retrain_preserves_protocol(&digital, law) &&
	      !pcm_call_is_online(&analog) && !pcm_call_is_online(&digital) &&
	      pcm_call_start_data_path(&analog) == PCM_CALL_OK &&
	      pcm_call_start_data_path(&digital) == PCM_CALL_OK &&
	      !pcm_call_is_online(&analog) && !pcm_call_is_online(&digital),
	      "law %u V.92 control was misreported as online",
	      (unsigned int)law);
	return true;
}

struct pcm_payload_source {
	uint32_t frames;
	bool reject;
	bool rejection_checked;
};

static void pcm_payload_bits(uint8_t *bits, size_t count, uint32_t frame)
{
	uint32_t state = UINT32_C(0x8712563b) ^ frame;

	for (size_t bit = 0u; bit < count; ++bit) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = (uint8_t)(state >> 31u);
	}
}

static bool pcm_payload(void *context, uint8_t *bits, size_t count)
{
	struct pcm_payload_source *source = context;

	if (source->reject) {
		source->reject = false;
		return false;
	}
	pcm_payload_bits(bits, count, source->frames++);
	return true;
}

static bool v92_data_boundary(struct pcm_call *call)
{
	struct v92_phase4_e2u_handoff handoff;
	struct pcm_call snapshot = *call;

	CHECK(phase4_boundary_is_transactional(call, call->law) &&
	      v92_phase4_e2u_handoff(&call->state.phase4.v92.owner.e2u, &handoff) ==
		      V92_PHASE4_E2U_OK &&
	      pcm_call_start_data_path(call) == PCM_CALL_OK &&
	      memcmp(&call->downstream_equalizer, &snapshot.downstream_equalizer,
		      sizeof(call->downstream_equalizer)) == 0 &&
	      call->stage == PCM_CALL_DATA_PATH && pcm_call_is_valid(call) &&
	      !pcm_call_is_online(call) && call->b1_deadline_armed &&
	      call->b1_deadline_remaining_samples == snapshot.b1_deadline_remaining_samples,
	      "V.92 data entry changed the boundary or B1 deadline");
	if (call->local_role == V90_ANALOG_MODEM) {
		struct pcm_call_v92_analog_data *path = &call->state.data_path.v92_analog;
		struct v92_phase4_e2u_analog_handoff *from = &handoff.state.analog;

		CHECK(v92_pcm_mapping_equal(&path->upstream.encoder.mapping, &from->mapping) &&
		      path->upstream.upstream_level == from->transmit.lu &&
		      path->upstream.b1u_symbols_remaining == V92_B1U_SYMBOLS &&
		      path->downstream_stage == from->downstream_stage &&
		      memcmp(&path->downstream, &from->downstream, sizeof(path->downstream)) == 0,
		      "V.92 analogue data entry lost its continuous downstream");
	} else {
		struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
		struct v92_phase4_e2u_digital_handoff *from = &handoff.state.digital;

		CHECK(v92_pcm_mapping_equal(&path->upstream.decoder.mapping, &from->mapping) &&
		      path->upstream.line.input_samples == from->receive.line.input_samples &&
		      path->upstream.line.polarity == from->receive.line.polarity &&
		      memcmp(&path->upstream.line.plan, &from->upstream_plan, sizeof(from->upstream_plan)) == 0 &&
		      (from->upstream_plan.observation == V92_PCM_FRAME_CONSTELLATION ?
			path->upstream.line.delay_remaining == from->upstream_plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY :
			memcmp(&path->upstream.line.equalizer, &from->receive.line.equalizer,
			       sizeof(path->upstream.line.equalizer)) == 0) &&
		      path->downstream_stage == from->downstream_stage &&
		      memcmp(&path->downstream, &from->downstream, sizeof(path->downstream)) == 0,
		      "V.92 digital data entry reset trained line or downstream history");
	}
	snapshot = *call;
	CHECK(pcm_call_start_data_path(call) == PCM_CALL_INVALID_STATE &&
	      pcm_call_start_cleardown(call) == PCM_CALL_INVALID_STATE &&
	      memcmp(call, &snapshot, sizeof(snapshot)) == 0,
	      "V.92 duplicate data entry or unsupported cleardown changed the call");
	return true;
}

static bool v92_payload_sample_with_idle(
	struct pcm_call *call, uint8_t received, int16_t sample, struct pcm_payload_source *source,
	uint32_t *received_frames, uint8_t *transmitted, uint64_t *idle_frames)
{
	struct pcm_call_data_sources sources = {
		.pcm_upstream = pcm_payload, .pcm_upstream_context = source,
		.downstream = pcm_payload, .downstream_context = source,
	};
	struct pcm_call_data_event event = { 0 };
	struct pcm_call_data_event sentinel = event;
	struct pcm_call snapshot;
	uint8_t generated = 0xa5u;
	enum pcm_call_result result;

	if (!source->rejection_checked)
		snapshot = *call;
	result = pcm_call_data_octet(call, received, sample, true,
				    &sources, &generated, &event);
	if (result == PCM_CALL_SOURCE_FAILURE) {
		CHECK(!source->rejection_checked && !source->reject &&
		      generated == 0xa5u && memcmp(&event, &sentinel, sizeof(event)) == 0 &&
		      memcmp(call, &snapshot, sizeof(snapshot)) == 0,
		      "V.92 source rejection consumed media or changed output");
		source->reject = true;
		CHECK(pcm_call_data_octet(call, received, sample, false, &sources,
			&generated, &event) == PCM_CALL_SOURCE_FAILURE && !source->reject &&
		      generated == 0xa5u && memcmp(&event, &sentinel, sizeof(event)) == 0 &&
		      memcmp(call, &snapshot, sizeof(snapshot)) == 0,
		      "V.92 source rejection consumed a media loss notification");
		source->rejection_checked = true;
		result = pcm_call_data_octet(call, received, sample, true,
					    &sources, &generated, &event);
	}
	CHECK(result == PCM_CALL_OK, "V.92 data returned %s", pcm_call_result_name(result));
	if (event.kind != PCM_CALL_DATA_NONE) {
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
		const uint8_t *bits;
		size_t count;

		if (call->local_role == V90_ANALOG_MODEM) {
			CHECK(event.kind == PCM_CALL_DOWNSTREAM_FRAME,
			      "analogue received the wrong PCM event");
			bits = event.frame.downstream.bits;
			count = event.frame.downstream.bit_count;
		} else {
			CHECK(event.kind == PCM_CALL_PCM_UPSTREAM_FRAME &&
			      !event.frame.pcm_upstream.b1u && !event.frame.pcm_upstream.ambiguous,
			      "digital exposed B1u or an ambiguous clean frame");
			bits = event.frame.pcm_upstream.bits;
			count = event.frame.pcm_upstream.bit_count;
		}
		CHECK(count != 0u && count <= sizeof(expected), "invalid PCM payload size");
		/* Ed/B1d can finish before the digital peer receives E2u. Its
		 * source is still absent then, so account for the transmitted idle
		 * prefix separately. The caller verifies its exact frame count;
		 * neither arbitrary bytes nor idle inside the payload are skipped. */
		if (idle_frames != NULL && *received_frames == 0u) {
			bool idle = true;
			for (size_t bit = 0u; bit < count; ++bit)
				idle &= bits[bit] == 1u;
			if (idle) {
				(*idle_frames)++;
				*transmitted = generated;
				return true;
			}
		}
		pcm_payload_bits(expected, count, *received_frames);
		CHECK(memcmp(bits, expected, count) == 0,
		      "role %u PCM payload changed at frame %u", call->local_role, *received_frames);
		(*received_frames)++;
	}
	*transmitted = generated;
	return true;
}

static bool v92_payload_sample(
	struct pcm_call *call, uint8_t received, int16_t sample, struct pcm_payload_source *source,
	uint32_t *received_frames, uint8_t *transmitted)
{
	return v92_payload_sample_with_idle(call, received, sample, source,
		received_frames, transmitted, NULL);
}

static bool v92_bad_b1_stays_clamped(const struct pcm_call *boundary)
{
	struct pcm_call damaged = *boundary;
	struct pcm_call erased = *boundary;
	struct pcm_call_data_event event;
	uint8_t transmitted;
	uint8_t silence = silence_octet(boundary->law);

	while (erased.state.data_path.v92_digital.upstream.line.delay_remaining != 0u)
		CHECK(pcm_call_data_octet(&erased, silence, 0, false, NULL, &transmitted, &event) ==
			      PCM_CALL_OK && !erased.state.data_path.v92_digital.b1u_decode_failed,
		      "media loss in the unused input delay rejected B1u");
	CHECK(pcm_call_data_octet(&erased, silence, 0, false, NULL, &transmitted, &event) ==
		      PCM_CALL_OK && pcm_call_is_valid(&erased) && !pcm_call_is_online(&erased) &&
	      event.kind == PCM_CALL_DATA_NONE && erased.state.data_path.v92_digital.b1u_decode_failed &&
	      erased.state.data_path.v92_digital.quality_guard_samples == 0,
	      "first missing B1u sample did not reject training before delayed decisions");

	for (size_t sample = 0u; sample < (size_t)V92_B1U_SYMBOLS +
	     boundary->state.data_path.v92_digital.upstream.line.delay_remaining; ++sample) {
		CHECK(pcm_call_data_octet(&damaged, silence, 0, true, NULL, &transmitted, &event) ==
			      PCM_CALL_OK &&
		      event.kind == PCM_CALL_DATA_NONE && !pcm_call_is_online(&damaged),
		      "missing B1u opened the host or failed before its deadline");
	}
	CHECK(damaged.state.data_path.v92_digital.upstream.line.received_frames == V92_B1U_FRAMES &&
	      damaged.state.data_path.v92_digital.b1u_squared_error > 0.0 &&
	      damaged.b1_deadline_armed, "known B1u digits hid missing waveform evidence");
	damaged.b1_deadline_remaining_samples = 1u;
	CHECK(pcm_call_is_valid(&damaged) &&
	      pcm_call_data_octet(&damaged, silence, 0, true, NULL, &transmitted, &event) == PCM_CALL_OK &&
	      damaged.stage == PCM_CALL_RETRAIN_SILENCE && !damaged.failed &&
	      damaged.protocol == PCM_PROTOCOL_V92 && !damaged.b1_deadline_armed,
	      "missing V.92 B1 did not start a bounded retrain");
	return true;
}

static bool v92_wrong_b1_stays_clamped(const struct pcm_call *boundary)
{
	struct pcm_call damaged = *boundary;
	struct pcm_call_v92_digital_data *path = &damaged.state.data_path.v92_digital;
	struct v92_pcm_frame_tx tx;
	struct pcm_call_data_event event;
	uint8_t transmitted;
	uint8_t silence = silence_octet(boundary->law);

	if (path->upstream.line.plan.observation != V92_PCM_FRAME_CONSTELLATION)
		return true;
	CHECK(v92_pcm_frame_tx_init(&tx, &path->upstream.decoder.mapping) == V92_PCM_FRAME_OK,
	      "wrong-B1u transmitter failed");
	while (path->upstream.line.delay_remaining != 0u)
		CHECK(pcm_call_data_octet(&damaged, silence, 0, true, NULL, &transmitted, &event) == PCM_CALL_OK,
		      "wrong-B1u delay failed");
	for (unsigned frame = 0u; frame < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES + 2u; ++frame) {
		struct v92_pcm_frame encoded;
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

		memset(bits, 1, tx.mapping.data_bits_per_frame);
		if (frame == V92_B1U_FRAMES - 1u)
			bits[0] = 0u;
		CHECK(v92_pcm_frame_encode(&tx, bits, tx.mapping.data_bits_per_frame, &encoded) == V92_PCM_FRAME_OK,
		      "wrong-B1u encoding failed");
		for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			/* Feed the trained raw-coordinate contract directly. This
			 * isolates bit validation from a second channel acquisition:
			 * the bad bit is validly coded and has negligible residual. */
			long sample = lround(path->upstream.line.polarity * encoded.u[symbol] /
				path->upstream.line.plan.gain);
			enum pcm_call_result result;

			CHECK(sample >= INT16_MIN && sample <= INT16_MAX, "wrong-B1u sample overflowed");
			result = pcm_call_data_octet(&damaged,
				encode_octet(boundary->law, (int16_t)sample), (int16_t)sample, true,
				NULL, &transmitted, &event);

			CHECK(event.kind == PCM_CALL_DATA_NONE && !pcm_call_is_online(&damaged),
			      "wrong B1u or its undecided tail opened the host at frame %u", frame);
			if (result == PCM_CALL_UNUSABLE_SELECTION || damaged.stage == PCM_CALL_RETRAIN_SILENCE) {
				CHECK(boundary->v92_digital_phase4_configuration.upstream_selection ==
					      PCM_CALL_V92_UPSTREAM_DIRECT &&
				      frame == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES - 1u &&
				      symbol == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u && pcm_call_is_valid(&damaged) &&
				      (tx.mapping.cpd.data_rate_number == 1u ?
					(result == PCM_CALL_UNUSABLE_SELECTION && damaged.failed) :
					(result == PCM_CALL_OK && !damaged.failed && !damaged.b1_deadline_armed &&
					 damaged.v92_upstream_retry_ceiling == tx.mapping.cpd.data_rate_number - 1u)) &&
				      memcmp(&damaged.v92_digital_phase4_configuration,
					&boundary->v92_digital_phase4_configuration,
					sizeof(damaged.v92_digital_phase4_configuration)) == 0,
				      "wrong B1u did not bound rate recovery at the final decision");
				return true;
			}
			CHECK(result == PCM_CALL_OK, "wrong B1u recovery failed");
		}
	}
	CHECK(path->b1u_decode_failed &&
	      path->b1u_squared_error < V92_B1U_SYMBOLS &&
	      damaged.b1_deadline_armed && pcm_call_is_valid(&damaged),
	      "small residual concealed a wrong B1u bit");
	damaged.b1_deadline_remaining_samples = 1u;
	CHECK(pcm_call_data_octet(&damaged, silence, 0, true, NULL, &transmitted, &event) == PCM_CALL_OK &&
	      damaged.stage == PCM_CALL_RETRAIN_SILENCE && !damaged.failed &&
	      damaged.protocol == PCM_PROTOCOL_V92 && !damaged.b1_deadline_armed,
	      "wrong B1u did not retain bounded retrain recovery");
	return true;
}

static bool run_wrong_direct_b1(enum g711_law law, bool fault)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call analog, digital;
	struct channel wire;
	bool injected = false;
	double b1_error = 0.0;
	unsigned received_frames = 0u;

	/* Keep the validly coded wrong bit below the validation-only residual
	 * ceiling, so this test requires bit validation rather than a power alarm. */
	ac.phase3.v92_analog.upstream_lu_ucode = 60u;
	ac.phase3.v92_analog.upstream_rate_mask = UINT32_C(1) << 12u;
	CHECK(v92_profile_init(&dc, V90_DIGITAL_MODEM, law, 28000u, 56000u, 13u, NULL) == V92_PROFILE_OK &&
	      channel_init(&wire, law, 3u) && pcm_call_init(&analog, &ac) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &dc) == PCM_CALL_OK, "wrong direct B1 setup failed");
	for (unsigned clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t ar, dr, at, dt;
		struct pcm_call *calls[] = { &analog, &digital };

		channel_receive(&wire, &ar, &dr);
		for (unsigned role = 0u; role < 2u; ++role) {
			struct pcm_call *call = calls[role];
			uint8_t received = role == 0u ? ar : dr;
			uint8_t *transmitted = role == 0u ? &at : &dt;

			if (pcm_call_phase4_is_ready(call))
				CHECK(v92_data_boundary(call), "wrong direct B1 handoff failed");
			if (call->stage != PCM_CALL_DATA_PATH) {
				struct pcm_call_observation observation;
				CHECK(pcm_call_octet(call, received, decode_octet(law, received),
					transmitted, &observation) == PCM_CALL_OK, "wrong direct B1 training failed");
				continue;
			}
			struct v92_pcm_frame_tx encoder;
			bool inject = role == 0u && fault && !injected &&
				call->state.data_path.v92_analog.upstream.b1u_symbols_remaining == V92_PCM_MAPPING_FRAME_SYMBOLS;

			if (inject)
				encoder = call->state.data_path.v92_analog.upstream.encoder;
			if (role == 1u) {
				const struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
				CHECK(path->upstream.line.plan.observation == V92_PCM_FRAME_FILTER_OUTPUT,
				      "wrong B1 fixture lost direct observation");
				b1_error = path->b1u_squared_error;
				received_frames = path->upstream.line.received_frames;
			}
			struct pcm_call_data_event event;
			CHECK(pcm_call_data_octet(call, received, decode_octet(law, received), true,
				NULL, transmitted, &event) == PCM_CALL_OK, "wrong direct B1 data clock failed");
			if (inject) {
				struct v92_upstream_data_tx *tx = &call->state.data_path.v92_analog.upstream;
				struct v92_pcm_frame encoded;
				uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
				int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

				memset(bits, 1, encoder.mapping.data_bits_per_frame);
				/* One late source bit changes the wire, but remains below
				 * the aggregate B1u error ceiling. Carry the changed
				 * coding state into the following idle frames as well. */
				bits[55] = 0u;
				CHECK(v92_pcm_frame_encode(&encoder, bits, encoder.mapping.data_bits_per_frame, &encoded) ==
					V92_PCM_FRAME_OK, "wrong direct B1 encoding failed");
				for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
					CHECK(v92_pcm_frame_scale(&encoder.mapping, tx->upstream_level, encoded.v[i], &samples[i]) ==
						V92_PCM_FRAME_OK, "wrong direct B1 scale failed");
				CHECK(tx->queue_count == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u, "wrong B1 missed frame boundary");
				tx->encoder = encoder;
				memcpy(tx->queue, samples + 1u, tx->queue_count * sizeof(*samples));
				*transmitted = encode_octet(law, samples[0]);
				injected = true;
			}
			if (role == 1u) {
				CHECK(!fault || event.kind == PCM_CALL_DATA_NONE, "bad B1 delivered payload");
				if (call->stage == PCM_CALL_RETRAIN_SILENCE) {
					CHECK(fault && injected && received_frames == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES - 1u &&
					      b1_error < (double)V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR &&
					      call->v92_upstream_retry_ceiling == 12u && !pcm_call_is_online(call) &&
					      event.kind == PCM_CALL_DATA_NONE && pcm_call_is_valid(call),
					      "bad direct B1 recovery failed: law=%u fault=%u injected=%u frame=%u error=%g retry_ceiling=%u",
					      law, fault, injected, received_frames, b1_error, call->v92_upstream_retry_ceiling);
					return true;
				}
				if (pcm_call_is_online(call)) {
					CHECK(!fault, "wrong direct B1 opened data path");
					return true;
				}
			}
		}
		channel_transmit(&wire, at, dt);
	}
	CHECK(false, "wrong direct B1 test timed out");
}

/* Faults: none, one bad epoch, every epoch, or a fixed caller-owned map. */
static bool run_downstream_b1_validation(unsigned mode, enum g711_law law, unsigned fault)
{
	struct pcm_call_configuration ac, dc;
	struct pcm_call calls[2];
	struct channel wire;
	bool injected = false;
	bool recovered = false;
	bool payload_enabled = false;
	struct pcm_payload_source payload = { 0 };
	unsigned received_frames = 0u;

	if (mode == 0u) {
		CHECK(v90_profile_init(&ac, V90_ANALOG_MODEM, law, 28000u, 56000u, NULL) == V90_PROFILE_OK &&
		      v90_profile_init(&dc, V90_DIGITAL_MODEM, law, 28000u, 56000u, NULL) == V90_PROFILE_OK,
		      "downstream B1 profile failed");
	} else {
		CHECK(v92_profile_init(&ac, V90_ANALOG_MODEM, law, 28000u, 56000u, 19u, NULL) == V92_PROFILE_OK &&
		      v92_profile_init(&dc, V90_DIGITAL_MODEM, law, 28000u, 56000u, 19u, NULL) == V92_PROFILE_OK,
		      "downstream B1 V.92 profile failed");
		if (mode == 1u)
			ac.local_info1.pcm_upstream_enabled = false;
	}
	/* Each side has a different hole. After 56k fails, only 28k is common;
	 * neither the local 41.3k nor the peer's 33.3k offer can be selected. */
	ac.enabled_downstream_rate_mask = UINT32_C(1) | (UINT32_C(1) << 10u) | (UINT32_C(1) << 21u);
	dc.enabled_downstream_rate_mask = UINT32_C(1) | (UINT32_C(1) << 4u) | (UINT32_C(1) << 21u);
	dc.phase3.digital.jd.data_rate_capability_mask = dc.enabled_downstream_rate_mask;
	if (mode != 0u)
		dc.phase3.v92_digital.jd.data_rate_capability_mask = dc.enabled_downstream_rate_mask;
	if (fault == 3u)
		ac.analog_phase4.generate_constellations = false;
	CHECK(channel_init(&wire, law, 3u) && pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
	      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK, "downstream B1 setup failed");
	for (unsigned clock = 0u; clock < 3u * CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t rx[2], tx[2];
		channel_receive(&wire, &rx[0], &rx[1]);
		for (unsigned side = 0u; side < 2u; ++side) {
			struct pcm_call *call = &calls[side];
			if (pcm_call_phase4_is_ready(call))
				CHECK(pcm_call_start_data_path(call) == PCM_CALL_OK, "downstream B1 handoff failed");
			if (call->stage != PCM_CALL_DATA_PATH) {
				struct pcm_call_observation event;
				CHECK(pcm_call_octet(call, rx[side], decode_octet(law, rx[side]), &tx[side], &event) ==
				      PCM_CALL_OK, "downstream B1 training failed: mode=%u law=%u clock=%u", mode, law, clock);
				continue;
			}
			if (side == 1u && fault && !injected) {
				struct v90_downstream_data_stream *stream = NULL;
				if (mode == 2u && call->state.data_path.v92_digital.downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA)
					stream = &call->state.data_path.v92_digital.downstream.data;
				if (mode != 2u && call->state.data_path.digital.downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_DATA)
					stream = &call->state.data_path.digital.downstream.data;
				if (stream && stream->transmitter.b1d_frames == V90_PHASE4_B1D_FRAMES - 1u && !stream->queue_count) {
					/* A valid waveform with a wrong late scrambled bit must
					 * not qualify merely because all 48 frames elapsed. */
					stream->transmitter.scrambler.history ^= UINT32_C(1) << 22u;
					injected = true;
				}
			}
			struct pcm_call_data_event event;
			const struct pcm_call_data_sources sources = {
				.downstream = pcm_payload, .downstream_context = &payload,
			};
			enum pcm_call_result result = pcm_call_data_octet(call, rx[side], decode_octet(law, rx[side]), true,
				payload_enabled ? &sources : NULL, &tx[side], &event);
			if (side == 0u && result == PCM_CALL_UNUSABLE_SELECTION) {
				CHECK(injected && (fault == 3u || (fault == 2u && recovered)) &&
				      !pcm_call_is_online(call) && event.kind == PCM_CALL_DATA_NONE &&
				      call->enabled_downstream_rate_mask == ac.enabled_downstream_rate_mask &&
				      (fault != 3u || (!call->downstream_retry_ceiling &&
					memcmp(&call->analog_phase4_configuration, &ac.analog_phase4, sizeof(ac.analog_phase4)) == 0)),
				      "failed B1d did not preserve explicit or exhausted policy");
				return true;
			}
			CHECK(result == PCM_CALL_OK, "downstream B1 data clock failed: mode=%u law=%u fault=%u result=%s",
			      mode, law, fault, pcm_call_result_name(result));
			if (side == 0u) {
				CHECK(!fault || recovered || event.kind == PCM_CALL_DATA_NONE, "bad B1d delivered payload");
				if (call->stage == PCM_CALL_RETRAIN_SILENCE) {
					CHECK(fault && injected && !recovered && !pcm_call_is_online(call) &&
					      call->downstream_retry_ceiling == 21u && pcm_call_is_valid(call) &&
					      call->enabled_downstream_rate_mask == ac.enabled_downstream_rate_mask,
					      "unexpected B1d retrain");
					recovered = true;
					if (fault == 2u)
						injected = false;
				}
				if (pcm_call_is_online(call)) {
					uint32_t upstream, downstream;
					CHECK(!fault || (fault == 1u && recovered),
					      "wrong B1d opened data path: mode=%u law=%u injected=%u", mode, law, injected);
					CHECK(pcm_call_get_data_rates(call, &upstream, &downstream) &&
					      downstream == (fault ? 28000u : 56000u), "B1d recovery ignored rate masks");
				}
				if (payload_enabled && event.kind == PCM_CALL_DOWNSTREAM_FRAME) {
					const struct v90_pcm_stream_frame *frame = &event.frame.downstream;
					bool idle = true;
					for (size_t bit = 0u; bit < frame->bit_count; ++bit)
						idle &= frame->bits[bit] == 1u;
					if (received_frames || !idle) {
						uint8_t expected[V90_PCM_FRAME_MAX_BITS];
						pcm_payload_bits(expected, frame->bit_count, received_frames++);
						CHECK(memcmp(expected, frame->bits, frame->bit_count) == 0, "B1d recovery corrupted payload");
						if (received_frames == 100u)
							return true;
					}
				}
			}
		}
		channel_transmit(&wire, tx[0], tx[1]);
		payload_enabled |= pcm_call_is_online(&calls[0]) && pcm_call_is_online(&calls[1]);
	}
	CHECK(false, "downstream B1 timed out: mode=%u law=%u fault=%u injected=%u recovered=%u stages=%u/%u online=%u/%u payload=%u/%u",
		mode, law, fault, injected, recovered, calls[0].stage, calls[1].stage,
		pcm_call_is_online(&calls[0]), pcm_call_is_online(&calls[1]), payload.frames, received_frames);
}

static bool test_downstream_b1_validation(void)
{
	for (unsigned mode = 0u; mode < 3u; ++mode)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned fault = 0u; fault < 4u; ++fault)
				CHECK(run_downstream_b1_validation(mode, law, fault), "downstream B1 validation failed");
	return true;
}

static bool test_v92_direct_b1_validation(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned fault = 0u; fault < 2u; ++fault)
			if (!run_wrong_direct_b1(law, fault))
				return false;
	return true;
}

static bool run_v92_b1_quality_window(enum g711_law law)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2];
	struct channel wire;
	int16_t previous = 0;
	double acquisition = 0.0, validation = 0.0;
	unsigned checked = 0u;

	ac.phase3.v92_analog.upstream_rate_mask = UINT32_C(1) << 18u;
	CHECK(v92_profile_init(&dc, V90_DIGITAL_MODEM, law, 28000u, 56000u, 19u, NULL) == V92_PROFILE_OK &&
	      channel_init(&wire, law, 3u) && pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
	      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK, "B1 quality window setup failed");
	for (unsigned clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t received[2], transmitted[2];
		channel_receive(&wire, &received[0], &received[1]);
		/* Keep the ordinary training level through a channel with memory
		 * and a second codec. Acquisition has a large correctable residual;
		 * validation uses the fitted model without changing the error limit. */
		int16_t current = decode_octet(law, received[1]);
		received[1] = encode_octet(law, (int16_t)lround(.9 * current + .1 * previous));
		previous = current;
		for (unsigned side = 0u; side < 2u; ++side) {
			struct pcm_call *call = &calls[side];
			int16_t sample = decode_octet(law, received[side]);
			if (pcm_call_phase4_is_ready(call))
				CHECK(v92_data_boundary(call), "B1 quality window handoff failed");
			if (call->stage != PCM_CALL_DATA_PATH) {
				struct pcm_call_observation observation;
				CHECK(pcm_call_octet(call, received[side], sample, &transmitted[side], &observation) == PCM_CALL_OK,
				      "B1 quality window training failed");
				continue;
			}
			struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
			bool boundary = side == 1u &&
				path->upstream.line.symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u &&
				path->upstream.line.received_frames < V92_B1U_FRAMES;
			if (boundary) {
				/* Independently account for each receiver observation, so
				 * the call cannot silently include or skip a boundary frame. */
				struct v92_upstream_data_rx probe = path->upstream;
				struct v92_upstream_data_frame frame;
				CHECK(v92_upstream_data_rx_octet(&probe, received[side], sample, true, &frame) == V92_UPSTREAM_DATA_RX_FRAME,
				      "B1 quality frame probe failed");
				if (probe.line.received_frames <= V92_B1U_ACQUISITION_FRAMES)
					acquisition += frame.squared_error;
				else
					validation += probe.line.host.gain > 0.0 ?
						frame.squared_error / probe.line.host.gain / probe.line.host.gain :
						frame.squared_error;
				++checked;
			}
			struct pcm_call_data_event event;
			CHECK(pcm_call_data_octet(call, received[side], sample, true, NULL, &transmitted[side], &event) == PCM_CALL_OK &&
			      (side == 0u || pcm_call_is_online(call) || event.kind == PCM_CALL_DATA_NONE),
			      "B1 quality window failed or released undecided payload");
			if (boundary)
				CHECK(path->b1u_squared_error == validation,
				      "B1 acquisition error contaminated validation: law=%u frame=%u got=%g wanted=%g",
				      law, checked, path->b1u_squared_error, validation);
			if (boundary && checked == V92_B1U_ACQUISITION_FRAMES) {
				struct pcm_call invalid = *call;
				invalid.state.data_path.v92_digital.b1u_squared_error = 1.0;
				CHECK(!pcm_call_is_valid(&invalid), "B1 accepted validation error before validation began");
			}
		}
		channel_transmit(&wire, transmitted[0], transmitted[1]);
		if (pcm_call_is_online(&calls[0]) && pcm_call_is_online(&calls[1])) {
			const double symbols = (V92_B1U_FRAMES - V92_B1U_ACQUISITION_FRAMES) * V92_PCM_MAPPING_FRAME_SYMBOLS;
			const double ceiling = symbols * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR;
			CHECK(checked == V92_B1U_FRAMES && acquisition > ceiling && validation < ceiling,
			      "B1 fixture missed acquisition transient: law=%u frames=%u acquisition=%g validation=%g",
			      law, checked, acquisition, validation);
			struct pcm_call limit = calls[1];
			limit.state.data_path.v92_digital.b1u_squared_error = ceiling;
			CHECK(pcm_call_is_valid(&limit) && pcm_call_is_online(&limit), "B1 rejected exact validation bound");
			limit.state.data_path.v92_digital.b1u_squared_error++;
			CHECK(pcm_call_is_valid(&limit) && !pcm_call_is_online(&limit),
			      "B1 used acquisition symbols in its validation bound");
			return true;
		}
	}
	CHECK(false, "B1 quality window did not connect");
}

static bool test_v92_b1_quality_window(void)
{
	return run_v92_b1_quality_window(G711_LAW_PCMU) && run_v92_b1_quality_window(G711_LAW_PCMA);
}

static bool v92_data_error_contracts(const struct pcm_call *online)
{
	struct pcm_call checked = *online;
	struct pcm_call owned = *online;
	struct pcm_call_data_event checked_event, owned_event;
	uint8_t checked_octet, owned_octet;
	uint8_t silence = silence_octet(online->law);

	CHECK(pcm_call_data_octet(&checked, silence, 0, true, NULL,
				 (uint8_t *)&checked, &checked_event) == PCM_CALL_INVALID_ARGUMENT &&
	      pcm_call_data_octet_owned(&checked, silence, 0, 0, true, NULL,
				       &checked_octet, (struct pcm_call_data_event *)&checked) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&checked, online, sizeof(checked)) == 0,
	      "V.92 accepted aliased media outputs");
	CHECK(pcm_call_data_octet(&checked, silence, 0, true, NULL, &checked_octet, &checked_event) ==
		      PCM_CALL_OK &&
	      pcm_call_data_octet_owned(&owned, silence, 0, 0, true, NULL, &owned_octet, &owned_event) ==
		      PCM_CALL_OK &&
	      pcm_call_is_valid(&owned) && checked_octet == owned_octet &&
	      memcmp(&checked_event, &owned_event, sizeof(checked_event)) == 0 &&
	      memcmp(&checked, &owned, sizeof(checked)) == 0,
	      "validated-block and checked V.92 clocks diverged");
	if (online->local_role == V90_DIGITAL_MODEM) {
		CHECK(checked.state.data_path.v92_digital.quality_guard_samples == 0,
		      "a received zero was treated as an erasure");
		checked = *online;
		owned = *online;
		CHECK(pcm_call_data_octet(&checked, silence, 0, false, NULL, &checked_octet, &checked_event) ==
			      PCM_CALL_OK &&
		      pcm_call_data_octet_owned(&owned, silence, 0, 0, false, NULL, &owned_octet, &owned_event) ==
			      PCM_CALL_OK &&
		      memcmp(&checked, &owned, sizeof(checked)) == 0 &&
		      checked_event.kind == PCM_CALL_DATA_NONE && owned_event.kind == PCM_CALL_DATA_NONE &&
		      checked.state.data_path.v92_digital.pending_ambiguous &&
		      checked.state.data_path.v92_digital.quality_guard_samples != 0 &&
		      pcm_call_is_valid(&checked), "known loss did not clamp delayed PCM payload");
	}
	if (online->local_role == V90_DIGITAL_MODEM && online->state.data_path.v92_digital
	    .upstream.line.plan.observation == V92_PCM_FRAME_FILTER_OUTPUT) {
		checked = *online;
		checked.state.data_path.v92_digital.upstream.line.equalizer.history[0].in_phase =
			DBL_MAX;
		checked.state.data_path.v92_digital.upstream.line.equalizer.coefficients[1].in_phase =
			2.0;
		owned = checked;
		CHECK(pcm_call_is_valid(&checked) &&
		      v92_downstream_transmit_owned(
			      &owned.state.data_path.v92_digital.downstream,
			      &owned.state.data_path.v92_digital.downstream_stage,
			      &owned.state.data_path.v92_digital.cpu,
			      NULL, NULL, &owned_octet) == V90_DOWNSTREAM_DATA_STREAM_OK,
		      "late V.92 receive-failure fixture was invalid");
		owned.state.data_path.v92_digital.upstream.failed = true;
		owned.failed = true;
		CHECK(pcm_call_data_octet(&checked, silence, INT16_MAX, true, NULL,
					 &checked_octet, &checked_event) == PCM_CALL_NUMERIC_FAILURE &&
		      pcm_call_has_failed(&checked) && checked_octet == owned_octet &&
		      checked_event.kind == PCM_CALL_DATA_NONE &&
		      memcmp(&checked, &owned, sizeof(checked)) == 0,
		      "late V.92 receive failure hid output or advanced receive state");
	}
	return true;
}

struct v92_upstream_offer {
	uint32_t peer_mask;
	uint8_t local_maximum;
	/* Zero expects exhaustion after a failed B1u and fresh peer offer. */
	uint8_t expected;
	double postcursor;
	/* Zero keeps the received sample unchanged; scaling follows G.711. */
	double receive_gain;
	unsigned expected_retrains;
	/* Noise precedes the final codec; zero frames keeps ordinary coverage. */
	unsigned noise_peak, payload_frames;
	/* Replace the TRN2u tail with silence in this many initial epochs. */
	unsigned damaged_trn2u_epochs;
	/* A second delayed sample distinguishes a two-postcursor channel. */
	double second_postcursor;
};

struct v92_online_pair {
	struct pcm_call analog, digital;
	struct channel wire;
};

static bool v92_quiet_downstream_training(const struct pcm_call *boundary)
{
	struct pcm_call call = *boundary;
	uint8_t quiet = silence_octet(call.law), transmitted;
	struct pcm_call_observation observation;

	for (unsigned n = 0; n < 16; ++n)
		CHECK(pcm_call_octet(&call, quiet, decode_octet(call.law, quiet),
			&transmitted, &observation) == PCM_CALL_OK, "quiet TRN1d stopped the call");
	CHECK(memcmp(call.downstream_equalizer.filter.coefficients,
		boundary->downstream_equalizer.filter.coefficients,
		sizeof(call.downstream_equalizer.filter.coefficients)) == 0 &&
	      call.state.phase3.v92_analog.receive.j.trn1d_symbols ==
		boundary->state.phase3.v92_analog.receive.j.trn1d_symbols + 16u,
	      "quiet TRN1d adapted the channel or failed to advance synchronization");
	return true;
}

static bool capture_v92_duplex_data(
	enum g711_law law, size_t delay, bool long_cpd,
	const struct v92_upstream_offer *offer, const struct v92_jp *jp,
	unsigned damaged_b1_epochs, struct v92_online_pair *capture)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call analog, digital;
	struct channel wire;
	struct pcm_payload_source upstream = { .reject = true };
	struct pcm_payload_source downstream = { .reject = true };
	uint32_t upstream_received = 0u, downstream_received = 0u;
	uint64_t downstream_idle = 0u, expected_downstream_idle = 0u;
	size_t analog_boundary = SIZE_MAX, digital_boundary = SIZE_MAX;
	bool deadline_checked[2] = { false, false };
	bool early_downstream_training_checked = false;
	unsigned retrains[2] = { 0u, 0u };
	uint8_t previous_rate = 0u;
	size_t sample_limit = CALL_SAMPLE_LIMIT *
		(offer == NULL ? 1u : offer->expected_retrains + 1u);
	int16_t selected_lu = 0;
	int16_t previous_upstream = 0;
	int16_t second_upstream = 0;
	bool channel_memory = offer != NULL &&
		(offer->postcursor != 0.0 || offer->second_postcursor != 0.0);
	uint32_t noise_state = UINT32_C(0x713812ad);
	/* Use a longer payload for compensated 40k and balanced direct maps to
	 * catch rare errors not exercised by the all-ones B1u sequence. */
	unsigned payload_frames = offer != NULL && ((offer->expected >= 15u && offer->expected <= 17u) ||
		(channel_memory && offer->expected == 13u)) ? 2000u : 200u;
	if (offer != NULL && offer->payload_frames != 0u)
		payload_frames = offer->payload_frames;

	if (offer != NULL) {
		CHECK(!long_cpd && v92_profile_init(
			      &dc, V90_DIGITAL_MODEM, law, 28000u, 56000u,
			      offer->local_maximum, NULL) == V92_PROFILE_OK,
		      "V.92 negotiating profile failed");
		ac.phase3.v92_analog.upstream_rate_mask = offer->peer_mask;
		/* The impaired fixture adds memory and a second G.711 conversion
		 * to the octet transport. Use a roughly 2k training level so this
		 * deliberate tandem quantization stays inside the B1 quality bound. */
		if (channel_memory)
			ac.phase3.v92_analog.upstream_lu_ucode = 60u;
	}
	if (jp != NULL)
		dc.phase3.v92_digital.jp = *jp;
	if (long_cpd) {
		struct v92_cpd_parameters *cpd = &dc.v92_digital_phase4.cpd;

		/* A longer CPd moves the final receive acknowledgment away from the
		 * downstream transmit boundary. Keep the active filter taps small so
		 * this call isolates negotiation and the continuous data handoff. */
		memset(cpd->filter, 0, sizeof(cpd->filter));
		cpd->filter[V92_CPD_PRECODER_FEEDBACK].count = 90u;
		cpd->filter[V92_CPD_PRECODER_FEEDBACK].coefficient[89] = 1;
		cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 85u;
		cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] = INT16_MAX;
		cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[84] = -1;
	}
	CHECK(channel_init(&wire, law, delay) && pcm_call_init(&analog, &ac) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &dc) == PCM_CALL_OK, "V.92 duplex initialization failed");
	for (size_t clock = 0u; clock < sample_limit; ++clock) {
		uint8_t ar, dr, at, dt;
		struct pcm_call *calls[] = { &analog, &digital };

		channel_receive(&wire, &ar, &dr);
		if (offer != NULL && (channel_memory || offer->noise_peak != 0u)) {
			int16_t current = decode_octet(law, dr);
			noise_state = noise_state * UINT32_C(1664525) + UINT32_C(1013904223);
			int noise = (int)((noise_state >> 16) % (2u * offer->noise_peak + 1u)) -
				(int)offer->noise_peak;
			double impaired = (1.0 - offer->postcursor - offer->second_postcursor) * current +
				offer->postcursor * previous_upstream + offer->second_postcursor * second_upstream + noise;

			dr = encode_octet(law, (int16_t)lround(fmax(INT16_MIN, fmin(INT16_MAX, impaired))));
			second_upstream = previous_upstream;
			previous_upstream = current;
		}
		for (size_t role = 0u; role < ARRAY_LENGTH(calls); ++role) {
			struct pcm_call *call = calls[role];
			enum pcm_call_stage previous_stage = call->stage;
			uint8_t received = role == 0u ? ar : dr;
			uint8_t *transmitted = role == 0u ? &at : &dt;
			int16_t sample = decode_octet(law, received);

			if (role == 0u && call->stage == PCM_CALL_PHASE3 &&
			    call->state.phase3.v92_analog.receive_stage == V92_PHASE3_ANALOG_RECEIVE_J &&
			    call->state.phase3.v92_analog.receive.j.stage == V92_PHASE3_J_RECEIVER_TRN1D &&
			    call->state.phase3.v92_analog.receive.j.trn1d_symbols >= 512u) {
				CHECK(call->downstream_equalizer.training_level > 0,
				      "V.92 waited for Ri before training its downstream equalizer");
				if (!early_downstream_training_checked)
					CHECK(v92_quiet_downstream_training(call), "quiet training recovery failed");
				early_downstream_training_checked = true;
			}

			if (role == 1u && offer != NULL && offer->receive_gain != 0.0) {
				double scaled = round(sample * offer->receive_gain);
				CHECK(scaled >= INT16_MIN && scaled <= INT16_MAX, "host-gain fixture clipped its input");
				sample = (int16_t)scaled;
			}

			if (pcm_call_phase4_is_ready(call)) {
				CHECK(v92_data_boundary(call), "V.92 handoff failed");
				/* Each retrain creates fresh coding and payload epochs. */
				if (role == 0u) {
					upstream = (struct pcm_payload_source){ .reject = true };
					downstream_received = 0u;
					downstream_idle = 0u;
				} else {
					downstream = (struct pcm_payload_source){ .reject = true };
					upstream_received = 0u;
				}
				if (role == 0u)
					analog_boundary = clock;
				else {
					uint8_t rate = call->state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number;

					CHECK(previous_rate == 0u ? call->v92_upstream_retry_ceiling == 0u :
						      (rate < previous_rate && call->v92_upstream_retry_ceiling == previous_rate - 1u),
					      "B1 recovery repeated or increased the failed upstream rate");
					previous_rate = rate;
					digital_boundary = clock;
					const struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
					uint64_t accepted = path->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA ?
						path->downstream.data.accepted_frames : 0u;
					expected_downstream_idle = accepted > V90_PHASE4_B1D_FRAMES ?
						accepted - V90_PHASE4_B1D_FRAMES : 0u;
					CHECK(v92_bad_b1_stays_clamped(call) && v92_wrong_b1_stays_clamped(call),
					      "V.92 B1 quality gate failed");
				}
			}
			if (call->stage == PCM_CALL_DATA_PATH) {
				/* Damage held-out B1u explicitly. A valid tandem-codec
				 * channel is no longer an appropriate failure injector:
				 * a better decoder should carry it at the offered rate. */
				if (role == 1u && retrains[role] < damaged_b1_epochs &&
				    call->state.data_path.v92_digital.upstream.line.received_frames >=
					V92_B1U_ACQUISITION_FRAMES &&
				    call->state.data_path.v92_digital.upstream.line.received_frames < V92_B1U_FRAMES) {
					received = silence_octet(law);
					sample = 0;
				}
				if (role == 0u && retrains[1] > retrains[0]) {
					struct pcm_call_data_event event;

					/* The digital peer has stopped its data transmitter.
					 * Raw transition bytes before tone qualification are
					 * not payload; keep clocking the analogue receiver. */
					CHECK(pcm_call_data_octet(call, received, sample, true, NULL, transmitted, &event) ==
						      PCM_CALL_OK, "V.92 peer did not accept training recovery");
				} else {
					CHECK(v92_payload_sample_with_idle(call, received, sample, role == 0u ? &upstream : &downstream,
							      role == 0u ? &downstream_received : &upstream_received,
							      transmitted, role == 0u ? &downstream_idle : NULL),
					      "V.92 payload failed at clock %zu", clock);
				}
			} else {
				struct pcm_call_observation observation;
				bool entering_v92_phase4 = role == 1u && call->stage == PCM_CALL_PHASE3_MIXED;
				bool damaged_training = role == 1u && offer != NULL &&
					retrains[role] < offer->damaged_trn2u_epochs &&
					call->stage == PCM_CALL_PHASE4 &&
					call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
					call->state.phase4.v92.owner.digital.receive.channel.symbols >= 3000u;

				if (damaged_training) {
					received = silence_octet(law);
					sample = 0;
				}

				enum pcm_call_result result = pcm_call_octet(call, received, sample,
					transmitted, &observation);

				if (damaged_training && result == PCM_CALL_UNUSABLE_SELECTION && offer->expected == 0u) {
					CHECK(retrains[0] == V92_DATA_QUALITY_MAX_RETRAINS &&
					      retrains[1] == V92_DATA_QUALITY_MAX_RETRAINS &&
					      call->pcm_upstream_quality.automatic_retrains == V92_DATA_QUALITY_MAX_RETRAINS &&
					      call->failed && pcm_call_is_valid(call) && !pcm_call_is_online(call) &&
					      call->v92_upstream_retry_ceiling == 0u &&
					      analog_boundary == SIZE_MAX && digital_boundary == SIZE_MAX,
					      "persistent TRN2u failure escaped its budget or entered data");
					return true;
				}
				if (offer != NULL && offer->expected == 0u && result == PCM_CALL_UNUSABLE_SELECTION) {
					CHECK(role == 1u && retrains[0] == offer->expected_retrains &&
					      retrains[1] == offer->expected_retrains && call->failed &&
					      call->v92_upstream_retry_ceiling == offer->local_maximum - 1u &&
					      call->v92_digital_phase4_configuration.direct_maximum_data_rate_number ==
						      offer->local_maximum && pcm_call_is_valid(call) && !pcm_call_is_online(call),
					      "V.92 exhausted peer rates without bounded recovery");
					return true;
				}
				CHECK(result == PCM_CALL_OK, "V.92 role %zu stage %u training failed at clock %zu: %s",
				      role, call->stage, clock, pcm_call_result_name(result));
				if (entering_v92_phase4 && call->stage == PCM_CALL_PHASE4)
					selected_lu = call->state.phase4.v92.owner.digital.receive
						.signal.trn2u.demodulator.lu;
				if (observation.phase2.kind == PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT ||
				    observation.phase2.kind == PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED) {
					CHECK(call->b1_deadline_armed && call->b1_deadline_remaining_samples ==
						      20u * V34_PHASE2_SAMPLE_RATE +
							      6u * call->round_trip_delay_samples,
					      "V.92 B1 deadline did not start at INFO1a");
					deadline_checked[role] = true;
				}
			}
			if (previous_stage != PCM_CALL_RETRAIN_SILENCE && call->stage == PCM_CALL_RETRAIN_SILENCE)
				retrains[role]++;
		}
		channel_transmit(&wire, at, dt);
		if (upstream_received >= payload_frames && downstream_received >= payload_frames)
			break;
	}
	CHECK(downstream_idle == expected_downstream_idle,
	      "V.92 downstream idle prefix changed: received %llu, sent %llu",
	      (unsigned long long)downstream_idle, (unsigned long long)expected_downstream_idle);
	CHECK(upstream_received >= payload_frames && downstream_received >= payload_frames &&
	      early_downstream_training_checked &&
	      analog.downstream_equalizer.training_level > 0 &&
	      digital.downstream_equalizer.training_level == 0 &&
	      upstream.rejection_checked && downstream.rejection_checked &&
	      analog_boundary < digital_boundary && deadline_checked[0] && deadline_checked[1] &&
	      pcm_call_is_online(&analog) && pcm_call_is_online(&digital) &&
	      !analog.b1_deadline_armed && !digital.b1_deadline_armed &&
	      retrain_preserves_protocol(&analog, law) && retrain_preserves_protocol(&digital, law) &&
	      v92_data_error_contracts(&analog) && v92_data_error_contracts(&digital),
	      "law %u delay %zu V.92 did not complete ordered duplex data (%u/%u)",
	      law, delay, upstream_received, downstream_received);
	CHECK(retrains[0] == retrains[1] && retrains[1] == (offer == NULL ? 0u : offer->expected_retrains),
	      "V.92 did not recover only failed training (%u/%u)", retrains[0], retrains[1]);
	{
		uint32_t at = 0u, ar = 0u, dt = 0u, dr = 0u;
		unsigned int expected_rate = offer == NULL ? 41333u :
			((offer->expected + 17u) * 8000u + 3u) / 6u;

		CHECK(pcm_call_get_data_rates(&analog, &at, &ar) &&
		      pcm_call_get_data_rates(&digital, &dt, &dr) &&
		      at == expected_rate && at == dr && ar == dt && ar == 41333u,
		      "V.92 call reported incorrect PCM frame rates: %u/%u %u/%u",
		      at, ar, dt, dr);
	}
	if (offer != NULL) {
		struct v92_cpd_parameters expected;
		struct v92_pcm_mapping mapping;

		CHECK(v92_cpd_direct_select(&expected, law, offer->expected, selected_lu, false) == V92_CPD_OK,
		      "V.92 expected upstream selection did not initialize");
		const struct v92_upstream_data_rx *rx = &digital.state.data_path.v92_digital.upstream;

		CHECK(rx->line.plan.observation == (!channel_memory ?
			V92_PCM_FRAME_FILTER_OUTPUT : V92_PCM_FRAME_CONSTELLATION),
		      "V.92 selected the wrong receive coordinates for its measured channel");
		if (channel_memory) {
			CHECK(rx->decoder.mapping.cpd.filter[V92_CPD_PRECODER_FEEDBACK].count == 90u &&
			      rx->decoder.mapping.cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count == 85u,
			      "V.92 distorted channel did not receive compensating CPd");
			memcpy(expected.filter, rx->decoder.mapping.cpd.filter, sizeof(expected.filter));
			expected.four_times_gain_q0_16 = rx->decoder.mapping.cpd.four_times_gain_q0_16;
			if (offer->expected >= 16u) {
				static const uint8_t dense_modulus[] = { 27u, 31u, 34u, 39u };
				unsigned int modulus = dense_modulus[offer->expected - 16u];

				for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
					expected.modulus[symbol] = symbol % 4u == 3u ? modulus : 2u * modulus;
			}
			if ((expected.trellis == V92_CPD_TRELLIS_32_STATE && expected.constellation[0].count == 32u) ||
			    (offer->expected == 16u && expected.constellation[0].count == 27u))
				expected.constellation[0].count = 64u;
			/* The focused channel test pins the dense spacing. Here verify
			 * that both peers actually exchanged that complete design. */
			if (expected.trellis == V92_CPD_TRELLIS_16_STATE && expected.constellation[0].count == 64u)
				expected.constellation[0] = rx->decoder.mapping.cpd.constellation[0];
			else if (expected.constellation[0].count == 32u || expected.constellation[0].count == 64u)
				for (unsigned point = 0; point < expected.constellation[0].count; ++point)
					CHECK(g711_ucode_sample(law, (uint8_t)(2u * point + 1u), true,
						&expected.constellation[0].positive_linear[point]), "expected spacing failed");
		}
		/* CPd-prime repeats the same selection with CPu acknowledged. The
		 * acknowledgement may now be in CPd rather than a later SUVd. */
		expected.acknowledges_cpu = analog.state.data_path.v92_analog
			.upstream.encoder.mapping.cpd.acknowledges_cpu;
		CHECK(v92_pcm_mapping_init(&mapping, law, &expected) == V92_PCM_MAPPING_OK &&
		      v92_pcm_mapping_equal(&mapping, &analog.state.data_path.v92_analog
						       .upstream.encoder.mapping) &&
		      v92_pcm_mapping_equal(&mapping, &digital.state.data_path.v92_digital
						       .upstream.decoder.mapping) &&
		      digital.v92_digital_phase4_configuration.upstream_selection ==
			      PCM_CALL_V92_UPSTREAM_DIRECT &&
		      digital.v92_digital_phase4_configuration.direct_maximum_data_rate_number ==
			      offer->local_maximum,
		      "V.92 did not exchange the complete selected mapping or lost its rate policy");
	}
	if (capture != NULL)
		*capture = (struct v92_online_pair){ .analog = analog, .digital = digital, .wire = wire };
	return true;
}

static bool run_v92_duplex_data(enum g711_law law, size_t delay, bool long_cpd,
	const struct v92_upstream_offer *offer)
{
	return capture_v92_duplex_data(law, delay, long_cpd, offer, NULL, 0u, NULL);
}

static bool test_v92_duplex_data(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t delay = 1u; delay <= 3u; delay += 2u)
			CHECK(run_v92_duplex_data(law, delay, false, NULL), "V.92 duplex data failed");
		CHECK(run_v92_duplex_data(law, 1u, true, NULL), "V.92 long-CPd duplex data failed");
	}
	return true;
}

static bool test_v92_trn2u_recovery(void)
{
	/* Real channel samples fail the measured inverse; no failure flag or
	 * retry counter is injected. Both modems must exchange full-retrain
	 * tones before they can send a new, independent training sequence. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned epochs = 1u; epochs <= V92_DATA_QUALITY_MAX_RETRAINS + 1u; ++epochs) {
			bool exhausted = epochs > V92_DATA_QUALITY_MAX_RETRAINS;
			struct v92_upstream_offer offer = {
				.peer_mask = UINT32_C(1) << 18, .local_maximum = 19u,
				.expected = exhausted ? 0u : 19u,
				.expected_retrains = exhausted ? V92_DATA_QUALITY_MAX_RETRAINS : epochs,
				.damaged_trn2u_epochs = epochs,
			};
			struct v92_online_pair pair;

			CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, 0u, &pair),
			      "law %u damaged TRN2u epochs %u failed recovery", law, epochs);
			if (!exhausted)
				CHECK(pair.digital.pcm_upstream_quality.automatic_retrains == epochs &&
				      pair.digital.v92_upstream_retry_ceiling == 0u,
				      "successful training reset its budget or downshifted an unmeasured rate");
		}
	return true;
}

static bool test_v92_two_postcursor_data(void)
{
	/* Exercise both trellis families through a unity-DC-gain channel with
	 * two delayed samples and tandem quantization. This protects the whole
	 * negotiated CPd/B1u/data path, not just the channel designer's fit. */
	const uint8_t rates[] = { 13u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t r = 0u; r < ARRAY_LENGTH(rates); ++r) {
			const struct v92_upstream_offer offer = {
				.peer_mask = UINT32_C(1) << (rates[r] - 1u),
				.local_maximum = rates[r], .expected = rates[r],
				.postcursor = .08, .second_postcursor = .02,
				.receive_gain = .75, .payload_frames = 4000u,
			};
			struct v92_online_pair pair;
			CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, 0u, &pair),
			      "law %u rate %u failed two-postcursor PCM data", law, rates[r]);
			const struct v92_cpd_parameters *cpd =
				&pair.digital.state.data_path.v92_digital.upstream.decoder.mapping.cpd;
			CHECK(cpd->filter[V92_CPD_PRECODER_FEEDBACK].coefficient[1] != 0,
			      "two-postcursor call lost its second feedback tap");
		}
	return true;
}

static bool test_v92_codec_noise_recovery(void)
{
	/* A short all-ones B1u or 200-frame data check misses rare dense-map
	 * disagreements. Keep clean data through 4000 frames, then inject bounded
	 * noise before the tandem codec throughout real training and payload.
	 * Never arm quality state or tolerate a wrong delivered payload here. */
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++) {
		for (unsigned noisy = 0; noisy <= 1; noisy++) {
			bool recovery = noisy && law == G711_LAW_PCMU;
			struct v92_upstream_offer offer = {
				.peer_mask = (UINT32_C(1) << 18) | (recovery ? UINT32_C(1) << 17 : 0u),
				.local_maximum = 19u, .expected = recovery ? 18u : 19u,
				.postcursor = .10, .receive_gain = .75,
				.expected_retrains = recovery ? 1u : 0u,
				.noise_peak = noisy ? 8u : 0u, .payload_frames = 4000u,
			};
			struct v92_online_pair pair;
			CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, 0u, &pair),
			      "codec %u noise %u failed sustained PCM data or recovery", law, offer.noise_peak);
			CHECK(pair.digital.pcm_upstream_quality.automatic_retrains == offer.expected_retrains,
			      "codec noise did not trigger the expected online quality recovery");
		}
		/* Sparse 24k maps should carry a noisier channel without inheriting
		 * the dense-map recovery expectation. Exercise real training and
		 * exact payloads; leave the B1 and online quality limits intact. */
		for (unsigned boosted = 0u; boosted <= 1u; ++boosted) {
			const struct v92_upstream_offer sparse = {
				.peer_mask = UINT32_C(1), .local_maximum = 1u, .expected = 1u,
				.postcursor = .10, .noise_peak = 64u, .payload_frames = 4000u,
				/* Host gain after the wire codec must not change quality units. */
				.receive_gain = boosted ? 1.25 : 1.0,
			};
			CHECK(run_v92_duplex_data(law, 3u, false, &sparse),
			      "codec %u failed noisy sparse PCM-upstream data at gain %g", law, sparse.receive_gain);
		}
	}
	return true;
}

static bool test_v92_delayed_duplex_data(void)
{
	static const uint8_t rates[] = { 1u, 19u };
	static const size_t delays[] = { 400u, 801u };

	/* 50 and 100.125 ms each way separate the two data handoffs and
	 * exercise a nonzero six-symbol phase, with actual PCM upstream. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t i = 0u; i < ARRAY_LENGTH(rates); ++i) {
			const struct v92_upstream_offer offer = {
				.peer_mask = UINT32_C(1) << (rates[i] - 1u),
				.local_maximum = rates[i], .expected = rates[i],
			};
			for (size_t j = 0u; j < ARRAY_LENGTH(delays); ++j)
				CHECK(run_v92_duplex_data(law, delays[j], false, &offer),
				      "V.92 delayed duplex failed: law %u rate %u delay %zu",
				      law, rates[i], delays[j]);
		}
	return true;
}

static void arm_v92_quality_failure(struct pcm_call *call, bool ambiguous)
{
	/* Isolate the policy boundary from the frame scorer: a qualified bad
	 * observation is waiting for the last sample of the control guard. */
	struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
	path->quality_guard_samples = 1u;
	path->pending_ambiguous = ambiguous;
	call->pcm_upstream_quality.good_windows = 0u;
	if (!ambiguous) {
		call->pcm_upstream_quality.frames = V92_DATA_QUALITY_WINDOW_FRAMES - 1u;
		call->pcm_upstream_quality.bad_windows = V92_DATA_QUALITY_BAD_WINDOWS - 1u;
		path->pending_squared_error = 2.0 * V92_DATA_QUALITY_WINDOW_FRAMES *
			V92_PCM_MAPPING_FRAME_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR;
	}
}

static bool v92_quality_backoff_contracts(const struct pcm_call *online)
{
	const struct pcm_call_v92_digital_phase4_configuration *configuration =
		&online->v92_digital_phase4_configuration;
	uint8_t rate = online->state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number;
	uint8_t ceiling = online->v92_upstream_retry_ceiling;

	if (configuration->upstream_selection == PCM_CALL_V92_UPSTREAM_DIRECT &&
	    rate > 1u && (ceiling == 0u || rate <= ceiling))
		ceiling = rate - 1u;
	CHECK(retrain_preserves_protocol(online, online->law), "manual retrain changed rate policy");
	for (unsigned budget = 0; budget <= V92_DATA_QUALITY_MAX_RETRAINS; ++budget)
		for (unsigned reason = 0; reason < 3u; ++reason) {
			struct pcm_call call = *online;
			struct pcm_call_data_event event;
			uint8_t octet;

			call.pcm_upstream_quality.automatic_retrains = budget;
			arm_v92_quality_failure(&call, reason == 1u);
			/* A pending observation is already in quality units. Applying
			 * host gain again here would incorrectly forgive the error. */
			if (call.state.data_path.v92_digital.upstream.line.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
				call.state.data_path.v92_digital.upstream.line.host.gain = 2.0;
				call.state.data_path.v92_digital.upstream.line.host.lower = 2.0;
				call.state.data_path.v92_digital.upstream.line.host.upper = 2.0;
			}
			if (reason == 2u) {
				call.state.data_path.v92_digital.pending_squared_error = 0.0;
				call.state.data_path.v92_digital.pending_uncertain = true;
			}
			CHECK(pcm_call_is_valid(&call), "quality boundary fixture is invalid");
			enum pcm_call_result result = pcm_call_data_octet(&call,
				silence_octet(call.law), 0, true, NULL, &octet, &event);
			bool available = budget < V92_DATA_QUALITY_MAX_RETRAINS;

			CHECK(result == (available ? PCM_CALL_OK : PCM_CALL_FAILED) &&
			      pcm_call_is_valid(&call) && event.kind == PCM_CALL_DATA_NONE &&
			      call.v92_upstream_retry_ceiling == (available ? ceiling : online->v92_upstream_retry_ceiling) &&
			      call.pcm_upstream_quality.automatic_retrains == budget + (available ? 1u : 0u) &&
			      (available ? call.stage == PCM_CALL_RETRAIN_SILENCE : call.failed) &&
			      memcmp(&call.v92_digital_phase4_configuration, configuration, sizeof(*configuration)) == 0,
			      "quality recovery lost its rate ceiling, fixed mapping, or retry budget");
		}
	return true;
}

static bool v92_no_path_recovery_contracts(const struct pcm_call *before, uint8_t received)
{
	unsigned rate = before->state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number;
	bool training = before->b1_deadline_armed;

	for (unsigned fixed = 0u; fixed < 2u; ++fixed)
		for (unsigned fast = 0u; fast < (training ? 1u : 3u); ++fast)
			for (unsigned budget = 0u; budget <= V92_DATA_QUALITY_MAX_RETRAINS; ++budget) {
				struct pcm_call checked = *before;
				struct pcm_call_data_event event, owned_event;
				uint8_t transmitted, owned_transmitted;
				bool closing = fast == 2u;
				bool available = !closing && ((!fixed && training) || budget < V92_DATA_QUALITY_MAX_RETRAINS);

				/* Preserve the negotiated mapping, but exercise ownership by
				 * the caller as well as automatic rate-selection policy. */
				if (fixed) {
					checked.v92_digital_phase4_configuration.upstream_selection = PCM_CALL_V92_UPSTREAM_CONFIGURED;
					checked.v92_digital_phase4_configuration.cpd =
						before->state.data_path.v92_digital.upstream.decoder.mapping.cpd;
					checked.v92_digital_phase4_configuration.cpd.acknowledges_cpu = false;
				}
				checked.pcm_upstream_quality.automatic_retrains = budget;
				CHECK(pcm_call_is_valid(&checked) && (!fast || pcm_call_start_fast_exchange(&checked) == PCM_CALL_OK) &&
				      (!closing || pcm_call_start_cleardown(&checked) == PCM_CALL_OK),
				      "no-path policy setup failed");
				struct pcm_call owned = checked;
				struct pcm_call_v92_digital_phase4_configuration configuration = checked.v92_digital_phase4_configuration;
				int16_t linear = decode_octet(before->law, received);
				enum pcm_call_result result = pcm_call_data_octet(
					&checked, received, linear, true, NULL, &transmitted, &event);
				enum pcm_call_result owned_result = pcm_call_data_octet_owned(
					&owned, received, linear, linear, true, NULL, &owned_transmitted, &owned_event);
				if (!training && !fast) {
					CHECK(result == PCM_CALL_OK && owned_result == result &&
					      pcm_call_is_valid(&checked) && checked.state.data_path.v92_digital.upstream.failed &&
					      checked.state.data_path.v92_digital.quality_guard_samples &&
					      checked.state.data_path.v92_digital.pending_ambiguous &&
					      checked.pcm_upstream_quality.automatic_retrains == budget &&
					      checked.v92_upstream_retry_ceiling == before->v92_upstream_retry_ceiling,
					      "online lost paths bypassed the control guard");
					struct v92_upstream_data_rx stopped = checked.state.data_path.v92_digital.upstream;
					unsigned samples = 0;
					while (result == PCM_CALL_OK && checked.stage == PCM_CALL_DATA_PATH && samples++ < 1600u) {
						CHECK(transmitted == owned_transmitted && event.kind == PCM_CALL_DATA_NONE &&
						      memcmp(&event, &owned_event, sizeof(event)) == 0 &&
						      memcmp(&checked, &owned, sizeof(checked)) == 0 && pcm_call_is_valid(&checked) &&
						      memcmp(&checked.state.data_path.v92_digital.upstream, &stopped, sizeof(stopped)) == 0,
						      "lost decoder advanced or emitted data during its guard");
						result = pcm_call_data_octet(&checked, received, linear, true, NULL, &transmitted, &event);
						owned_result = pcm_call_data_octet_owned(&owned, received, linear, linear, true, NULL,
							&owned_transmitted, &owned_event);
					}
					CHECK(samples <= 1600u, "lost paths extended the control guard");
				}

				CHECK(result == (closing ? PCM_CALL_CLEARDOWN_COMPLETE : available ? PCM_CALL_OK : PCM_CALL_FAILED) &&
				      result == owned_result && transmitted == owned_transmitted &&
				      memcmp(&checked, &owned, sizeof(checked)) == 0 &&
				      memcmp(&event, &owned_event, sizeof(event)) == 0 &&
				      pcm_call_is_valid(&checked) && event.kind == PCM_CALL_DATA_NONE &&
				      checked.protocol == PCM_PROTOCOL_V92 &&
				      checked.pcm_upstream_quality.automatic_retrains == budget + (available && (fixed || !training)) &&
				      checked.v92_upstream_retry_ceiling == (available && !fixed ? rate - 1u : 0u) &&
				      (closing ? checked.stage == PCM_CALL_CLOSED && !checked.failed :
				       available ? checked.stage == PCM_CALL_RETRAIN_SILENCE && !checked.failed : checked.failed) &&
				      memcmp(&checked.v92_digital_phase4_configuration, &configuration, sizeof(configuration)) == 0,
				      "no-path recovery lost budget, selection, or payload clamp: law=%u training=%u fixed=%u fast=%u budget=%u result=%s",
				      before->law, training, fixed, fast, budget, pcm_call_result_name(result));
			}
	return true;
}

static bool run_v92_no_path_recovery(enum g711_law law)
{
	const struct v92_upstream_offer offer = {
		.peer_mask = (UINT32_C(1) << 19u) - 1u,
		.local_maximum = 19u, .expected = 19u,
	};
	struct v92_online_pair pair;
	uint32_t noise = UINT32_C(0x81374aef);

	CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, 0u, &pair),
	      "no-path recovery initial call failed");
	struct pcm_call call = pair.digital;

	/* Damage real media without an erasure notification. Probe an owned
	 * receiver copy to identify the exact sample that eliminates all paths,
	 * then feed that same sample through the complete call at each budget.
	 * This distinguishes decoder failure from a later quality-window alarm. */
	for (unsigned sample = 0; sample < 64u; ++sample) {
		noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
		int32_t signed_noise = (int32_t)(noise >> 16);
		int16_t linear = (int16_t)(signed_noise > INT16_MAX ? signed_noise - 65536 : signed_noise);
		uint8_t received = encode_octet(law, linear), transmitted;
		struct v92_upstream_data_rx probe = call.state.data_path.v92_digital.upstream;
		struct v92_upstream_data_frame frame;
		struct pcm_call_data_event event;

		linear = decode_octet(law, received);
		enum v92_upstream_data_rx_result rx = v92_upstream_data_rx_octet(
			&probe, received, linear, true, &frame);
		if (rx == V92_UPSTREAM_DATA_RX_NO_PATH) {
			return v92_no_path_recovery_contracts(&call, received);
		}
		CHECK(pcm_call_data_octet(&call, received, linear, true, NULL, &transmitted, &event) == PCM_CALL_OK &&
		      call.stage == PCM_CALL_DATA_PATH && pcm_call_is_valid(&call),
		      "no-path fixture recovered before losing its decoder paths");
	}
	CHECK(false, "no-path fixture did not eliminate the decoder paths");
}

static bool test_v92_no_path_recovery(void)
{
	return run_v92_no_path_recovery(G711_LAW_PCMU) && run_v92_no_path_recovery(G711_LAW_PCMA);
}

static bool run_v92_b1_no_path_recovery(enum g711_law law)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2];
	struct channel wire;
	uint32_t noise = UINT32_C(0x81374aef);
	unsigned injected = 0u;
	bool recovered = false;

	ac.phase3.v92_analog.upstream_rate_mask = (UINT32_C(1) << 19u) - 1u;
	CHECK(v92_profile_init(&dc, V90_DIGITAL_MODEM, law, 28000u, 56000u, 19u, NULL) == V92_PROFILE_OK &&
	      channel_init(&wire, law, 3u) && pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
	      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK, "B1 no-path setup failed");
	for (unsigned clock = 0u; clock < 2u * CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t received[2], transmitted[2];
		channel_receive(&wire, &received[0], &received[1]);
		for (unsigned side = 0u; side < 2u; ++side) {
			struct pcm_call *call = &calls[side];
			if (pcm_call_phase4_is_ready(call))
				CHECK(v92_data_boundary(call), "B1 no-path handoff failed");
			if (call->stage != PCM_CALL_DATA_PATH) {
				struct pcm_call_observation observation;
				CHECK(pcm_call_octet(call, received[side], decode_octet(law, received[side]),
					&transmitted[side], &observation) == PCM_CALL_OK, "B1 no-path training failed");
				continue;
			}
			struct pcm_call_v92_digital_data *path = &call->state.data_path.v92_digital;
			bool damage = side == 1u && !recovered &&
				path->upstream.line.received_frames >= V92_B1U_ACQUISITION_FRAMES;
			if (damage) {
				/* A bounded unannounced noise burst, not injected decoder
				 * state: the delayed B1 decisions must stay clamped. */
				noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
				int32_t n = (int32_t)(noise >> 16);
				received[side] = encode_octet(law, (int16_t)(n > INT16_MAX ? n - 65536 : n));
				CHECK(++injected <= 512u && !pcm_call_is_online(call), "B1 burst escaped training");
				struct v92_upstream_data_rx probe = path->upstream;
				struct v92_upstream_data_frame frame;
				enum v92_upstream_data_rx_result rx = v92_upstream_data_rx_octet(
					&probe, received[side], decode_octet(law, received[side]), true, &frame);
				if (rx == V92_UPSTREAM_DATA_RX_NO_PATH) {
					CHECK(v92_no_path_recovery_contracts(call, received[side]), "B1 no-path recovery contract failed");
					recovered = true;
				}
			}
			struct pcm_call_data_event event;
			CHECK(pcm_call_data_octet(call, received[side], decode_octet(law, received[side]), true,
				NULL, &transmitted[side], &event) == PCM_CALL_OK && pcm_call_is_valid(call) &&
			      (!damage || event.kind == PCM_CALL_DATA_NONE), "B1 no-path call failed or released payload");
		}
		channel_transmit(&wire, transmitted[0], transmitted[1]);
		if (recovered && pcm_call_is_online(&calls[0]) && pcm_call_is_online(&calls[1])) {
			CHECK(calls[1].v92_upstream_retry_ceiling == 18u && calls[1].pcm_upstream_quality.automatic_retrains == 0u &&
			      calls[1].state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number == 18u &&
			      calls[0].state.data_path.v92_analog.upstream.encoder.mapping.cpd.data_rate_number == 18u,
			      "B1 no-path recovery did not negotiate the lower rate: law=%u ceiling=%u budget=%u digital=%u analog=%u",
			      law, calls[1].v92_upstream_retry_ceiling, calls[1].pcm_upstream_quality.automatic_retrains,
			      calls[1].state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number,
			      calls[0].state.data_path.v92_analog.upstream.encoder.mapping.cpd.data_rate_number);
			return true;
		}
	}
	CHECK(false, "B1 no-path recovery did not return online");
}

static bool test_v92_b1_no_path_recovery(void)
{
	return run_v92_b1_no_path_recovery(G711_LAW_PCMU) && run_v92_b1_no_path_recovery(G711_LAW_PCMA);
}

static bool v92_peer_retrain_quality_guard(const struct pcm_call *online, int16_t amplitude)
{
	/* The real silence-to-tone waveform must outrank a PCM quality failure,
	 * even when its recovery budget is exhausted. Exercise both silence
	 * tolerance edges, retaining the online decoder's pending data history. */
	for (unsigned silence_ms = 65u; silence_ms <= 75u; silence_ms += 5u)
		for (unsigned budget = 0; budget <= V92_DATA_QUALITY_MAX_RETRAINS; ++budget) {
			struct pcm_call call = *online;
			struct v34_phase2_tone_transmitter peer;
			enum v34_station_role role = call.retrain.detector.tone.local_role ==
				V34_CALLING_STATION ? V34_ANSWERING_STATION : V34_CALLING_STATION;
			bool guarded = false;

			/* Put the measurement one frame before recovery so even a
			 * retrain beginning at the worst quality boundary can qualify. */
			call.pcm_upstream_quality = (struct v92_data_quality){
				.frames = V92_DATA_QUALITY_WINDOW_FRAMES - 1u,
				.bad_windows = V92_DATA_QUALITY_BAD_WINDOWS - 1u,
				.automatic_retrains = budget,
				.squared_error = 2.0 * V92_DATA_QUALITY_WINDOW_FRAMES *
					V92_PCM_MAPPING_FRAME_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
			};
			CHECK(v34_phase2_tone_transmitter_init(&peer, role, amplitude) ==
				V34_PHASE2_TONE_OK, "peer retrain tone did not initialize");
			for (unsigned clock = 0; clock < 1600u && call.stage == PCM_CALL_DATA_PATH; ++clock) {
				struct pcm_call_data_event event;
				int16_t sample = 0;
				uint8_t octet;

				if (clock >= silence_ms * 8u)
					CHECK(v34_phase2_tone_transmitter_sample(&peer, &sample) == V34_PHASE2_TONE_OK,
					      "peer retrain tone stopped");
				uint8_t input = encode_octet(call.law, sample);
				CHECK(pcm_call_data_octet(&call, input, decode_octet(call.law, input), true, NULL,
					&octet, &event) == PCM_CALL_OK && pcm_call_is_valid(&call) &&
				      call.pcm_upstream_quality.automatic_retrains == budget &&
				      call.v92_upstream_retry_ceiling == online->v92_upstream_retry_ceiling,
				      "peer full retrain consumed quality recovery at sample %u", clock);
				if (call.stage == PCM_CALL_DATA_PATH)
					guarded |= call.state.data_path.v92_digital.quality_guard_samples != 0u;
				CHECK(!guarded || event.kind == PCM_CALL_DATA_NONE,
				      "pending retrain quality failure exposed host data");
			}
			CHECK(guarded && call.stage == PCM_CALL_RETRAIN_SILENCE && call.retrain.peer_tone_retained,
			      "peer full retrain did not qualify: law %u silence %u budget %u guard %u stage %u tone %u",
			      call.law, silence_ms, budget, guarded, call.stage, call.retrain.peer_tone_retained);
		}
	return true;
}

static bool test_v92_peer_retrain_lost_paths(void)
{
	/* A lower-amplitude full-retrain onset can eliminate every dense PCM
	 * path before its tone qualifies. Retain control priority at all retry
	 * budgets, not just when the receiver returns a low-margin frame. */
	const struct v92_upstream_offer offer = {
		.peer_mask = (UINT32_C(1) << 19u) - 1u,
		.local_maximum = 19u, .expected = 19u,
	};
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v92_online_pair pair;
		CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, 0u, &pair) &&
		      v92_peer_retrain_quality_guard(&pair.digital, 4800),
		      "dense PCM decoder rejected a peer full retrain");
	}
	return true;
}

static bool test_v92_quality_rate_recovery(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned fixed = 0; fixed < 2u; ++fixed) {
			const struct v92_upstream_offer offer = {
				.peer_mask = (UINT32_C(1) << 12u) | (UINT32_C(1) << 6u) | UINT32_C(1),
				.local_maximum = 19u, .expected = 13u,
			};
			const uint8_t rates[] = { 13u, 7u, 1u };
			struct v92_online_pair pair;

			CHECK(capture_v92_duplex_data(law, 3u, false, fixed ? NULL : &offer, NULL, 0u, &pair),
			      "quality recovery initial call failed");
			CHECK(v92_peer_retrain_quality_guard(&pair.digital, PHASE2_NOMINAL_AMPLITUDE), "peer full retrain quality guard failed");
			uint8_t fixed_rate = pair.digital.state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number;
			for (unsigned epoch = 0; epoch < 3u; ++epoch) {
				CHECK(v92_quality_backoff_contracts(&pair.digital), "quality boundary contracts failed");
				if (epoch == 2u)
					break;
				if (epoch != 0u)
					arm_v92_quality_failure(&pair.digital, false);
				struct pcm_payload_source sources[2] = { { .reject = true }, { .reject = true } };
				uint32_t frames[2] = { 0u, 0u };
				bool started[2] = { false, false };
				unsigned loss_guard = 0u;
				for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
					uint8_t rx[2], tx[2];
					struct pcm_call *calls[] = { &pair.analog, &pair.digital };

					channel_receive(&pair.wire, &rx[0], &rx[1]);
					for (unsigned side = 0; side < 2u; ++side) {
						struct pcm_call *call = calls[side];
						if (pcm_call_phase4_is_ready(call)) {
							CHECK(pcm_call_start_data_path(call) == PCM_CALL_OK, "quality recovery handoff failed");
							started[side] = true;
						}
						if (call->stage == PCM_CALL_DATA_PATH && started[side]) {
							CHECK(v92_payload_sample(call, rx[side], decode_octet(law, rx[side]),
								&sources[side], &frames[side], &tx[side]), "quality recovery payload failed");
						} else if (call->stage == PCM_CALL_DATA_PATH) {
							struct pcm_call_data_event event;
							bool lost = epoch == 0u && side == 1u && clock < 80u;
							uint8_t input = lost ? silence_octet(law) : rx[side];
							CHECK(pcm_call_data_octet(call, input, decode_octet(law, input), !lost,
								NULL, &tx[side], &event) == PCM_CALL_OK, "quality retrain detection failed");
							if (lost) {
								if (clock == 0u)
									loss_guard = call->state.data_path.v92_digital.quality_guard_samples;
								CHECK(event.kind == PCM_CALL_DATA_NONE &&
								      loss_guard >= 80u && loss_guard < 8000u &&
								      call->state.data_path.v92_digital.quality_guard_samples ==
									      loss_guard - clock,
								      "repeated media loss exposed payload or extended recovery");
							}
						} else {
							struct pcm_call_observation event;
							CHECK(pcm_call_octet(call, rx[side], decode_octet(law, rx[side]),
								&tx[side], &event) == PCM_CALL_OK, "quality retrain negotiation failed");
						}
					}
					channel_transmit(&pair.wire, tx[0], tx[1]);
					if (frames[0] >= 200u && frames[1] >= 200u)
						break;
				}
				uint8_t expected = fixed ? fixed_rate : rates[epoch + 1u];
				CHECK(frames[0] >= 200u && frames[1] >= 200u &&
				      pcm_call_is_online(&pair.analog) && pcm_call_is_online(&pair.digital) &&
				      pair.digital.pcm_upstream_quality.automatic_retrains == epoch + 1u &&
				      pair.digital.state.data_path.v92_digital.upstream.decoder.mapping.cpd.data_rate_number == expected &&
				      pair.analog.state.data_path.v92_analog.upstream.encoder.mapping.cpd.data_rate_number == expected &&
				      pair.digital.v92_upstream_retry_ceiling == (fixed ? 0u : rates[epoch] - 1u),
				      "quality recovery did not negotiate the next usable upstream rate and exchange data");
			}
		}
	return true;
}

struct v92_rate_call {
	struct pcm_call call;
	struct v92_phase4_analog_station *analog;
	struct pcm_downstream_rx peer;
	struct v90_downstream_data_rx data_peer;
	struct v92_cpu_parameters cpu;
	struct pcm_payload_source downstream_source;
	uint32_t upstream_frames, downstream_frames;
	struct pcm_r_tx rd;
	unsigned rd_symbols, zero_acks, quality_retrains;
	uint64_t transmitted;
	bool control_started, complete, data_started, peer_data_started, peer_acknowledged;
};

static bool v92_rate_payload(const uint8_t *bits, size_t count, uint32_t *frames)
{
	uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

	CHECK(count && count <= sizeof(expected), "invalid resumed payload width");
	pcm_payload_bits(expected, count, *frames);
	CHECK(memcmp(bits, expected, count) == 0, "resumed payload changed at frame %u", *frames);
	(*frames)++;
	return true;
}

static bool v92_rate_call_sample(struct v92_rate_call *test, int16_t sample)
{
	struct pcm_call *call = &test->call;
	struct pcm_call_data_event data = { 0 };
	struct pcm_call_observation observation;
	uint8_t octet;
	enum pcm_call_result result;
	const struct pcm_call_data_sources sources = {
		.downstream = pcm_payload, .downstream_context = &test->downstream_source,
	};

	if (pcm_call_phase4_is_ready(call)) {
		CHECK(pcm_call_start_data_path(call) == PCM_CALL_OK, "post-Ru data entry failed");
		test->data_started = true;
	}
	if (call->stage == PCM_CALL_DATA_PATH) {
		struct pcm_call checked = *call;
		struct pcm_payload_source checked_source = test->downstream_source;
		struct pcm_call_data_sources checked_sources = sources;
		struct pcm_call_data_event checked_data;
		uint8_t checked_octet;
		checked_sources.downstream_context = &checked_source;
		result = pcm_call_data_octet_owned(call, encode_octet(call->law, sample), sample, sample, true,
			test->data_started ? &sources : NULL, &octet, &data);
		CHECK(pcm_call_data_octet(&checked, encode_octet(checked.law, sample), sample, true,
			test->data_started ? &checked_sources : NULL, &checked_octet, &checked_data) == result &&
		      checked_octet == octet && memcmp(&checked_data, &data, sizeof(data)) == 0 &&
		      memcmp(&checked, call, sizeof(checked)) == 0 &&
		      memcmp(&checked_source, &test->downstream_source, sizeof(checked_source)) == 0,
		      "checked/owned PCM rate exchange diverged");
	} else
		result = pcm_call_octet(call, encode_octet(call->law, sample), sample, &octet, &observation);
	CHECK(result == PCM_CALL_OK || result == PCM_CALL_CLEARDOWN_COMPLETE,
	      "PCM rate call failed in stage %u: %s", call->stage, pcm_call_result_name(result));
	CHECK(data.kind != PCM_CALL_PCM_UPSTREAM_FRAME || !data.frame.pcm_upstream.ambiguous,
	      "PCM rate request exposed ambiguous bits");
	if (test->data_started && data.kind == PCM_CALL_PCM_UPSTREAM_FRAME)
		CHECK(v92_rate_payload(data.frame.pcm_upstream.bits, data.frame.pcm_upstream.bit_count,
			&test->upstream_frames), "upstream payload did not resume");
	CHECK(call->pcm_upstream_quality.automatic_retrains == test->quality_retrains,
	      "legitimate Ru consumed quality recovery budget");
	uint64_t transmitted = test->transmitted++;
	if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
		test->complete = true;
		CHECK(test->zero_acks && !pcm_call_is_online(call), "cleardown omitted wire acknowledgement");
		return true;
	}
	if (!test->control_started && call->stage != PCM_CALL_PHASE4)
		return true;
	CHECK(call->stage != PCM_CALL_PHASE4 ||
	      (call->retrain_active && !pcm_call_is_online(call) && data.kind == PCM_CALL_DATA_NONE),
	      "Ru did not pause the existing host session");
	struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
	if (!test->control_started && state->prefix_stage == PCM_CALL_V92_PREFIX_RD &&
	    test->rd_symbols < PCM_R_SEQUENCE_SYMBOLS) {
		struct v90_pcm_symbol expected;
		CHECK(test->rd_symbols || transmitted % V90_CP_FRAME_INTERVALS == 0u,
		      "Rd did not start on the existing downstream frame boundary");
		CHECK(pcm_r_tx_next(&test->rd, &expected) == PCM_R_TX_OK && octet == expected.octet,
		      "Rd waveform changed at symbol %u", test->rd_symbols);
		test->rd_symbols++;
	}
	if (!test->control_started && state->prefix_stage != PCM_CALL_V92_PREFIX_NONE)
		return true;
	CHECK(test->rd_symbols == PCM_R_SEQUENCE_SYMBOLS, "TRN2d started before complete Rd-bar");
	test->control_started = true;
	if (test->analog != NULL) {
		enum v92_phase4_analog_station_result result =
			v92_phase4_analog_station_receive(test->analog, octet);
		CHECK(result == V92_PHASE4_ANALOG_STATION_OK || result == V92_PHASE4_ANALOG_STATION_EVENT,
		      "analogue rate control rejected the downstream: %s",
		      v92_phase4_analog_station_result_name(result));
	}
	if (test->peer_data_started) {
		struct v90_pcm_stream_frame frame;
		enum v90_downstream_data_rx_result received = v90_downstream_data_rx_receive_octet(
			&test->data_peer, octet, &frame);
		if (received == V90_DOWNSTREAM_DATA_RX_DATA_FRAME)
			return v92_rate_payload(frame.bits, frame.bit_count, &test->downstream_frames);
		CHECK(received == V90_DOWNSTREAM_DATA_RX_BUFFERING || received == V90_DOWNSTREAM_DATA_RX_B1D_FRAME ||
		      received == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE, "resumed downstream decode failed");
		return true;
	}
	struct pcm_downstream_event event;
	enum pcm_downstream_rx_result received = pcm_downstream_rx_receive(&test->peer, octet, &event);
	CHECK(received == PCM_DOWNSTREAM_RX_BUFFERING || received == PCM_DOWNSTREAM_RX_TRN2D_FRAME ||
	      received == PCM_DOWNSTREAM_RX_SUVD || received == PCM_DOWNSTREAM_RX_CPD || received == PCM_DOWNSTREAM_RX_ED,
	      "post-Rd control decode failed: %s", pcm_downstream_rx_result_name(received));
	if (received == PCM_DOWNSTREAM_RX_CPD && !event.control.record.cpd.parameters.data_rate_number) {
		CHECK(event.control.record.cpd.frame_result == V92_CPD_OK &&
		      event.control.record.cpd.parameters.acknowledges_cpu, "invalid zero-rate acknowledgement");
		test->zero_acks++;
	}
	if ((received == PCM_DOWNSTREAM_RX_CPD && event.control.record.cpd.parameters.acknowledges_cpu &&
	     event.control.record.cpd.parameters.data_rate_number) ||
	    (received == PCM_DOWNSTREAM_RX_SUVD && event.control.record.suvd.parameters.acknowledges_cpu)) {
		test->peer_acknowledged = true;
		CHECK(test->peer.ed_requested || pcm_downstream_rx_expect_ed(&test->peer) == PCM_DOWNSTREAM_RX_OK,
		      "peer could not arm Ed at acknowledgement boundary");
	}
	if (received == PCM_DOWNSTREAM_RX_ED) {
		struct v90_pcm_mapping mapping;
		CHECK(v90_pcm_mapping_init_from_v92_cpu(&mapping, &test->cpu) == V90_PCM_MAPPING_OK &&
		      v90_downstream_data_rx_start_after_ed(&test->data_peer, &test->peer, &mapping) == V90_DOWNSTREAM_DATA_RX_OK,
		      "peer could not enter post-Ru B1d");
		test->peer_data_started = true;
	}
	return true;
}

static bool v92_rejected_control_recovery(const struct pcm_call *online, int16_t lu, bool quiet)
{
	/* A short Ru-like run followed by unrelated energy is not a rate request.
	 * Neither is indefinite silence. Do not deliver ambiguous frames or
	 * repeatedly defer recovery while waiting for missing control. */
	for (unsigned budget = 0; budget <= V92_DATA_QUALITY_MAX_RETRAINS; ++budget) {
		struct pcm_call call = *online;
		unsigned guard_started = 0;
		bool recovered = false;
		call.pcm_upstream_quality.automatic_retrains = budget;
		for (unsigned clock = 0; clock < 8000u; ++clock) {
			struct pcm_call_data_event event;
			uint8_t octet;
			int16_t sample = quiet ? 0 : clock < 180u ?
				(clock % 6u < 3u ? lu : -lu) : (clock % 2u ? lu / 3 : -lu / 3);
			enum pcm_call_result result = pcm_call_data_octet(&call,
				encode_octet(call.law, sample), decode_octet(call.law, encode_octet(call.law, sample)), true,
				NULL, &octet, &event);
			if (call.stage == PCM_CALL_DATA_PATH &&
			    call.state.data_path.v92_digital.quality_guard_samples && !guard_started)
				guard_started = clock + 1u;
			CHECK(!call.v92_rate_renegotiation_active &&
			      (event.kind != PCM_CALL_PCM_UPSTREAM_FRAME || !event.frame.pcm_upstream.ambiguous),
			      "false Ru entered control or delivered ambiguous data");
			if (result != PCM_CALL_OK || call.stage == PCM_CALL_RETRAIN_SILENCE) {
				/* Allow full-retrain qualification as well as Ru, but never
				 * let a false candidate defer recovery beyond 200 ms. */
				CHECK(guard_started && clock + 1u - guard_started <= 1600u &&
				      pcm_call_is_valid(&call) &&
				      (budget < V92_DATA_QUALITY_MAX_RETRAINS ?
					(result == PCM_CALL_OK && call.stage == PCM_CALL_RETRAIN_SILENCE &&
					 call.pcm_upstream_quality.automatic_retrains == budget + 1u) :
					(result == PCM_CALL_FAILED && call.failed)),
				      "false Ru lost its bounded quality recovery/budget");
				recovered = true;
				break;
			}
		}
		CHECK(recovered, "false Ru indefinitely postponed quality recovery");
	}
	return true;
}

static bool v92_interrupted_rate_recovery(const struct v92_rate_call *test,
	const struct v92_phase4_record_tx *record, unsigned mapping_bits)
{
	/* A valid record that contradicts the committed exchange must not be
	 * mistaken for an internal DSP failure or silently applied to data. */
	for (unsigned rollback = 0; rollback < 2u; ++rollback) {
		struct pcm_call call = test->call;
		struct v92_phase4_record_tx changed = *record;
		struct v92_cpus_parameters cpus = {
			.data_rate_number = test->cpu.data_rate_number + (rollback ? 0u : 1u),
			.acknowledges_cpd = !rollback,
		};
		uint8_t frame[V92_CPUS_MAX_FRAME_BYTES];
		size_t bits;
		CHECK(v92_cpus_encode(&cpus, mapping_bits, frame, sizeof(frame), &bits) == V92_CPU_OK,
		      "contradictory CPus did not encode");
		/* Repeat once so the first record's CRC crosses the causal FIR. */
		unsigned limit = 2u * bits / v92_phase4_modulator_bits_per_symbol(changed.modulator.constellation);
		for (unsigned i = 0; i < limit && call.stage == PCM_CALL_PHASE4; ++i) {
			struct pcm_call_observation observation;
			int16_t sample;
			uint8_t generated;
			if (v92_phase4_record_tx_is_complete(&changed))
				CHECK(v92_phase4_record_tx_start_next(&changed, frame, sizeof(frame), bits) ==
					V92_PHASE4_RECORD_TX_OK, "contradictory CPus did not restart");
			CHECK(v92_phase4_record_tx_next(&changed, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "contradictory CPus transmitter failed");
			uint8_t octet = encode_octet(call.law, sample);
			CHECK(pcm_call_octet(&call, octet, decode_octet(call.law, octet), &generated,
				&observation) == PCM_CALL_OK, "contradictory CPus aborted the call (rollback=%u)", rollback);
			CHECK(call.stage != PCM_CALL_RETRAIN_SILENCE || generated == encode_octet(call.law, 0),
			      "failed rate exchange did not start with retrain silence");
		}
		CHECK(call.stage == PCM_CALL_RETRAIN_SILENCE && call.retrain_active &&
		      !call.v92_rate_renegotiation_active && !call.rate_renegotiation_deadline_armed &&
		      !call.b1_deadline_armed && !call.failed && pcm_call_is_valid(&call) &&
		      call.protocol == PCM_PROTOCOL_V92 && !pcm_call_is_online(&call) &&
		      call.pcm_upstream_quality.automatic_retrains == test->quality_retrains &&
		      memcmp(&call.local_info0, &test->call.local_info0, sizeof(call.local_info0)) == 0 &&
		      memcmp(&call.peer_info0, &test->call.peer_info0, sizeof(call.peer_info0)) == 0,
		      "failed rate exchange did not preserve the call through full retrain");
	}
	return true;
}

static bool v92_resume_rate_call(struct v92_rate_call *test, struct v92_phase4_record_tx *record,
	const struct v92_pcm_mapping *mapping, int16_t lu, unsigned mapping_bits,
	struct v92_upstream_data_tx *resumed)
{
	enum g711_law law = test->call.law;
	const struct v92_cpus_parameters cpus = {
		.data_rate_number = test->cpu.data_rate_number, .acknowledges_cpd = true,
	};
	uint8_t frame[V92_CPUS_MAX_FRAME_BYTES];
	size_t bits;
	unsigned trailing_records = 0;

	CHECK(v92_cpus_encode(&cpus, mapping_bits, frame, sizeof(frame), &bits) == V92_CPU_OK,
	      "acknowledged CPus encoding failed");
	for (unsigned repeat = 0; repeat < 100u && trailing_records < 3u; ++repeat) {
		bool in_e2u = test->call.stage == PCM_CALL_PHASE4 &&
			test->call.state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U;
		CHECK(v92_phase4_record_tx_start_next(record, frame, sizeof(frame), bits) ==
			V92_PHASE4_RECORD_TX_OK, "CPus repeat did not start");
		while (!v92_phase4_record_tx_is_complete(record)) {
			int16_t sample;
			CHECK(v92_phase4_record_tx_next(record, &sample) == V92_PHASE4_RECORD_TX_OK &&
			      v92_rate_call_sample(test, decode_octet(law, encode_octet(law, sample))),
			      "CPus repeat did not cross the E2u boundary");
		}
		if (in_e2u && test->peer_acknowledged)
			trailing_records++;
	}
	CHECK(trailing_records == 3u && !test->data_started &&
	      test->call.state.phase4.v92.owner.e2u.state.digital.cpu.data_rate_number == cpus.data_rate_number,
	      "rate exchange did not reach E2u with the selected rate");
	CHECK(v92_interrupted_rate_recovery(test, record, mapping_bits), "interrupted rate recovery failed");
	struct v92_phase4_modulator modulator;
	CHECK(v92_phase4_record_tx_handoff(record, &modulator) == V92_PHASE4_RECORD_TX_OK &&
	      v92_phase4_record_tx_init_e2u(record, &modulator, mapping->cpd.extend_e2u) == V92_PHASE4_RECORD_TX_OK,
	      "rate exchange E2u generation failed");
	while (!v92_phase4_record_tx_is_complete(record)) {
		int16_t sample;
		CHECK(v92_phase4_record_tx_next(record, &sample) == V92_PHASE4_RECORD_TX_OK &&
		      v92_rate_call_sample(test, decode_octet(law, encode_octet(law, sample))), "E2u failed");
	}
	struct v92_upstream_data_tx upstream;
	struct pcm_payload_source source = { 0 };
	CHECK(v92_upstream_data_tx_init(&upstream, mapping, lu) == V92_UPSTREAM_DATA_TX_OK,
	      "post-Ru B1u did not initialize");
	for (unsigned i = 0; i < 20000u && (test->upstream_frames < 200u || test->downstream_frames < 200u); ++i) {
		int16_t sample;
		CHECK(v92_upstream_data_tx_next(&upstream, pcm_payload, &source, &sample) == V92_UPSTREAM_DATA_TX_OK &&
		      v92_rate_call_sample(test, decode_octet(law, encode_octet(law, sample))), "resumed data failed");
	}
	uint32_t tx, rx;
	CHECK(test->upstream_frames >= 200u && test->downstream_frames >= 200u &&
	      test->data_peer.b1d_bit_errors == 0u && pcm_call_is_online(&test->call) &&
	      !test->call.retrain_active && !test->call.v92_rate_renegotiation_active &&
	      !test->call.rate_renegotiation_deadline_armed &&
	      pcm_call_get_data_rates(&test->call, &tx, &rx) &&
	      tx == ((cpus.data_rate_number + 20u) * 8000u + 3u) / 6u &&
	      rx == ((mapping->cpd.data_rate_number + 17u) * 8000u + 3u) / 6u,
	      "post-Ru call did not resume ordered data at its new rate (%u/%u frames)",
	      test->upstream_frames, test->downstream_frames);
	*resumed = upstream;
	return true;
}

static bool run_v92_rate_request(struct pcm_call *online,
	struct v92_upstream_data_tx *upstream, unsigned epoch, bool local)
{
	const enum g711_law law = online->law;
	struct v92_rate_call test = {
		.call = *online, .quality_retrains = online->pcm_upstream_quality.automatic_retrains,
	};
	const struct pcm_call_v92_digital_data *path = &online->state.data_path.v92_digital;
	test.cpu = path->cpu;
	test.cpu.data_rate_number--;
	test.transmitted = path->downstream.data.transmitted_symbols;
	struct v90_pcm_mapping control;
	struct v92_pcm_phase4_trn2u training;
	const unsigned mapping_bits = V92_PCM_MAPPING_FRAME_SYMBOLS *
		v92_phase4_modulator_bits_per_symbol(path->ru.constellation);
	CHECK(v90_pcm_mapping_init_rate_renegotiation(&control, &path->training_mapping,
		&path->downstream.data.transmitter.encoder.mapping) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_rx_init(&test.peer, PCM_PROTOCOL_V92, &control,
		V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      pcm_r_tx_init(&test.rd, &path->downstream.data.transmitter.encoder.mapping, PCM_R_RD) == PCM_R_TX_OK &&
	      v92_pcm_phase4_trn2u_init(&training, path->ru.constellation, 0,
		upstream->upstream_level) == V92_PCM_PHASE4_TRN2U_OK, "rate exchange setup failed");
	if (local) {
		CHECK(pcm_call_start_rate_renegotiation(&test.call) == PCM_CALL_OK,
		      "digital delayed-response request failed");
		/* Keep transmitting peer data until our Rd and TRN2d have ended.
		 * Ru then overlaps repeated SUVd, not just the first TRN2d frame. */
		for (unsigned i = 0; i < 3000u; ++i) {
			int16_t sample;
			CHECK(v92_upstream_data_tx_next(upstream, NULL, NULL, &sample) == V92_UPSTREAM_DATA_TX_OK &&
			      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))),
			      "digital request did not retain its line while awaiting Ru");
		}
		CHECK(test.call.state.phase4.v92.child == PCM_CALL_V92_PHASE4_RU &&
		      test.call.state.phase4.v92.owner.ru.transmit.stage == PCM_DOWNSTREAM_TX_V92_CONTROL,
		      "delayed Ru did not overlap repeated SUVd");
	}
	unsigned drain = upstream->queue_count;
	for (unsigned i = 0; i < drain; ++i) {
		int16_t sample;
		CHECK(v92_upstream_data_tx_next(upstream, NULL, NULL, &sample) == V92_UPSTREAM_DATA_TX_OK &&
		      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))), "data queue drain failed");
	}
	bool broken_checked = false;
	for (unsigned i = 0; i < PCM_R_SEQUENCE_SYMBOLS; ++i) {
		int sign = i % 6u < 3u ? 1 : -1;
		if (i >= PCM_R_SYMBOLS)
			sign = -sign;
		CHECK(v92_rate_call_sample(&test, decode_octet(law,
			encode_octet(law, (int16_t)(sign * upstream->upstream_level)))), "Ru/bar failed");
		if (local && test.call.state.phase4.v92.child == PCM_CALL_V92_PHASE4_RU &&
		    test.call.state.phase4.v92.owner.ru.receive.signal.bar_symbols == 1u) {
			struct pcm_call broken = test.call;
			for (unsigned n = 0; n < 64u && broken.stage == PCM_CALL_PHASE4; ++n) {
				struct pcm_call_observation observation;
				uint8_t octet;
				CHECK(pcm_call_octet(&broken, silence_octet(law), 0, &octet, &observation) == PCM_CALL_OK,
				      "corrupt delayed Ru aborted its owner");
			}
			CHECK(broken.stage == PCM_CALL_RETRAIN_SILENCE && !broken.failed &&
			      !broken.v92_rate_renegotiation_active, "corrupt delayed Ru did not use full retrain");
			broken_checked = true;
		}
	}
	CHECK(!local || broken_checked, "delayed Ru did not reach the corruption checkpoint");
	if (epoch == 0u) {
		struct v92_phase4_analog_station analog;
		struct v92_phase4_analog_handoff handoff;
		struct v92_pcm_mapping expected = upstream->encoder.mapping;
		expected.cpd.extend_e2u = false;
		CHECK(v92_phase4_analog_station_init_after_ru(&analog, path->ru.constellation,
			upstream->upstream_level, &test.peer, &test.cpu, &upstream->encoder.mapping,
			false, online->round_trip_delay_samples) == V92_PHASE4_ANALOG_STATION_OK,
		      "analogue rate control did not reenter");
		test.analog = &analog;
		for (unsigned clock = 0; clock < 16008u && !v92_phase4_analog_station_can_handoff(&analog); ++clock) {
			int16_t sample;
			CHECK(v92_phase4_analog_station_transmit(&analog, &sample) == V92_PHASE4_ANALOG_STATION_OK &&
			      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))),
			      "analogue rate exchange failed at %u", clock);
		}
		test.analog = NULL;
		CHECK(v92_phase4_analog_station_handoff(&analog, &handoff) == V92_PHASE4_ANALOG_STATION_OK &&
		      memcmp(&handoff.receive, &test.peer, sizeof(test.peer)) == 0 &&
		      v92_pcm_mapping_equal(&handoff.mapping, &expected),
		      "analogue rate exchange lost its mapping or downstream boundary");
		while (!v92_phase4_record_tx_is_complete(&handoff.transmit)) {
			int16_t sample;
			CHECK(v92_phase4_record_tx_next(&handoff.transmit, &sample) == V92_PHASE4_RECORD_TX_OK &&
			      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))),
			      "analogue rate control tail did not finish");
		}
		CHECK(v92_resume_rate_call(&test, &handoff.transmit, &handoff.mapping,
			upstream->upstream_level, mapping_bits, upstream), "analogue rate exchange did not resume");
		*online = test.call;
		return true;
	}
	for (unsigned i = 0; i < 2400u; ++i) {
		int16_t sample;
		CHECK(v92_pcm_phase4_trn2u_next(&training, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
		      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))), "TRN2u failed");
	}
	CHECK(test.control_started && test.call.state.phase4.v92.owner.digital.receive.channel.symbols == 12u &&
	      !test.call.state.phase4.v92.owner.digital.control.cpu_received &&
	      !test.call.state.phase4.v92.owner.digital.control.peer_confirmation_received,
	      "rate exchange reused initial training or acknowledgement state");
	{
		struct pcm_call expired = test.call;
		struct pcm_call_observation observation;
		struct v92_pcm_phase4_trn2u next_training = training;
		int16_t sample;
		uint8_t octet;
		expired.rate_renegotiation_deadline_remaining_samples = 1u;
		CHECK(v92_pcm_phase4_trn2u_next(&next_training, &sample) == V92_PCM_PHASE4_TRN2U_OK &&
		      pcm_call_octet(&expired, encode_octet(law, sample), decode_octet(law, encode_octet(law, sample)),
			&octet, &observation) == PCM_CALL_OK && expired.stage == PCM_CALL_RETRAIN_SILENCE &&
		      !expired.v92_rate_renegotiation_active && !expired.rate_renegotiation_deadline_armed &&
		      expired.protocol == PCM_PROTOCOL_V92, "stalled PCM control did not leave through full retrain");
	}
	struct v92_phase4_record_tx record;
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	size_t bits;
	const struct v92_suvu_parameters suvu = { .prefilter_level_db_q2_2 = -16 };
	CHECK(v92_suvu_encode(&suvu, mapping_bits, frame, sizeof(frame), &bits) == V92_SUVU_OK &&
	      v92_phase4_record_tx_init(&record, &training.modulator, frame, sizeof(frame), bits) ==
		V92_PHASE4_RECORD_TX_OK, "SUVu setup failed");
	/* A short CPus retains the old constellation. Then either acknowledge
	 * the new rate and resume data, or request zero-rate cleardown. */
	for (unsigned which = 0; which < 6u; ++which) {
		if (which == 3u && epoch == 1u) {
			CHECK(v92_resume_rate_call(&test, &record, &upstream->encoder.mapping,
				upstream->upstream_level, mapping_bits, upstream), "rate exchange did not resume");
			*online = test.call;
			return true;
		}
		if (which) {
			const struct v92_cpus_parameters cpus = {
				.data_rate_number = which < 3u ? path->cpu.data_rate_number - 1u : 0u,
			};
			CHECK(v92_cpus_encode(&cpus, mapping_bits, frame, sizeof(frame), &bits) == V92_CPU_OK &&
			      v92_phase4_record_tx_start_next(&record, frame, sizeof(frame), bits) ==
				V92_PHASE4_RECORD_TX_OK, "CPus setup failed");
		}
		while (!v92_phase4_record_tx_is_complete(&record)) {
			int16_t sample;
			CHECK(v92_phase4_record_tx_next(&record, &sample) == V92_PHASE4_RECORD_TX_OK &&
			      v92_rate_call_sample(&test, decode_octet(law, encode_octet(law, sample))), "control record failed");
		}
		if (which == 2u) {
			struct v92_cpu_parameters cpu = path->cpu;
			cpu.data_rate_number--;
			cpu.acknowledges_cpd = false;
			CHECK(test.call.state.phase4.v92.owner.digital.control.cpu_received &&
			      memcmp(&cpu, &test.call.state.phase4.v92.owner.digital.control.cpu, sizeof(cpu)) == 0,
			      "CPus did not retain the negotiated mapping");
		}
	}
	for (unsigned i = 0; i < 4000u && !test.complete; ++i)
		CHECK(v92_rate_call_sample(&test, upstream->upstream_level), "cleardown failed");
	CHECK(test.complete, "rate exchange did not complete bounded cleardown");
	*online = test.call;
	return true;
}

static bool test_v92_remote_rate_request(void)
{
	const unsigned rates[] = { 1, 7, 13, 19 };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned i = 0; i < ARRAY_LENGTH(rates); ++i) {
			struct v92_online_pair pair;
			const struct v92_upstream_offer offer = {
				.peer_mask = UINT32_C(1) << (rates[i] - 1u),
				.local_maximum = rates[i], .expected = rates[i],
			};
			CHECK(capture_v92_duplex_data(law, 1u, false, &offer, NULL, 0u, &pair), "rate request call did not train");
			struct v92_upstream_data_tx *upstream = &pair.analog.state.data_path.v92_analog.upstream;
			if (rates[i] == 7u)
				CHECK(v92_rejected_control_recovery(&pair.digital, upstream->upstream_level, false) &&
				      v92_rejected_control_recovery(&pair.digital, upstream->upstream_level, true),
				      "false control recovery failed");
			uint8_t ar, dr, transmitted;
			struct pcm_call_data_event event;
			channel_receive(&pair.wire, &ar, &dr);
			CHECK(pcm_call_data_octet(&pair.digital, dr, decode_octet(law, dr), true, NULL,
				&transmitted, &event) == PCM_CALL_OK, "pending data sample failed");
			struct pcm_call local = pair.digital;
			struct v92_upstream_data_tx delayed = *upstream;
			CHECK(run_v92_rate_request(&local, &delayed, 1u, true),
			      "delayed digital rate request failed");
			/* Retained initial-training metadata must not extend a new E2u.
			 * This flag no longer affects the already-running data coders. */
			upstream->encoder.mapping.cpd.extend_e2u = true;
			pair.digital.state.data_path.v92_digital.upstream.decoder.mapping.cpd.extend_e2u = true;
			for (unsigned epoch = 0; epoch < 3u; ++epoch) {
				/* A legitimate exchange must work even after automatic
				 * recovery attempts have been exhausted, without resetting them. */
				if (epoch == 1u)
					pair.digital.pcm_upstream_quality.automatic_retrains = V92_DATA_QUALITY_MAX_RETRAINS;
				CHECK(run_v92_rate_request(&pair.digital, upstream, epoch, false),
				      "law %u rate %u Ru epoch %u failed", law, rates[i], epoch);
			}
		}
	return true;
}

struct v92_fast_peer_source {
	struct v92_upstream_control control;
	uint8_t record[V92_CPU_MAX_FRAME_BYTES];
	size_t bit_count, bit_index;
	uint32_t data_frames;
	bool active;
	bool cleardown;
	bool zero_received;
};

static bool v92_fast_peer_bits(void *context, uint8_t *bits, size_t count)
{
	struct v92_fast_peer_source *source = context;
	if (!source->active) {
		pcm_payload_bits(bits, count, source->data_frames++);
		return true;
	}
	if (source->bit_index == source->bit_count) {
		if (source->cleardown) {
			const struct v92_cpus_parameters zero = { .acknowledges_cpd = source->zero_received };
			if (v92_cpus_encode(&zero, count, source->record, sizeof(source->record), &source->bit_count) != V92_CPU_OK)
				return false;
		} else if (!v92_upstream_control_start(&source->control, count, source->record,
			sizeof(source->record), &source->bit_count))
			return false;
		source->bit_index = 0u;
	}
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = packed_msb_bit(source->record, source->bit_index++);
	return true;
}

static bool v92_fast_boundary_checks(const struct pcm_call *snapshot, uint8_t received)
{
	struct pcm_call call = *snapshot;
	struct pcm_call_data_event event;
	uint8_t generated;
	call.state.data_path.v92_digital.fast.deadline_remaining = 1u;
	CHECK(pcm_call_is_valid(&call) && pcm_call_data_octet(&call, received, decode_octet(call.law, received),
		true, NULL, &generated, &event) == PCM_CALL_OK && call.stage == PCM_CALL_RETRAIN_SILENCE &&
	      call.retrain_active && !call.failed && pcm_call_is_valid(&call) && event.kind == PCM_CALL_DATA_NONE &&
	      call.pcm_upstream_quality.automatic_retrains == snapshot->pcm_upstream_quality.automatic_retrains &&
	      memcmp(&call.local_info0, &snapshot->local_info0, sizeof(call.local_info0)) == 0 &&
	      memcmp(&call.peer_info0, &snapshot->peer_info0, sizeof(call.peer_info0)) == 0,
	      "fast timeout lost its bounded full-retrain recovery");
	call = *snapshot;
	call.state.data_path.v92_digital.fast.stage = PCM_CALL_V92_FAST_FINISH + 1;
	CHECK(!pcm_call_is_valid(&call), "invalid fast owner was accepted");
	call = *snapshot;
	CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_INVALID_STATE &&
	      memcmp(&call, snapshot, sizeof(call)) == 0, "nested rate exchange disturbed the fast owner");
	if (snapshot->state.data_path.v92_digital.fast.stage == PCM_CALL_V92_FAST_CONTROL) {
		CHECK(pcm_call_start_cleardown(&call) == PCM_CALL_OK, "silent-peer fast hangup did not start");
		for (unsigned clock = 0; clock < 8000u && call.stage != PCM_CALL_CLOSED; ++clock) {
			enum pcm_call_result result = pcm_call_data_octet(&call, silence_octet(call.law), 0, true,
				NULL, &generated, &event);
			CHECK((result == PCM_CALL_OK || result == PCM_CALL_CLEARDOWN_COMPLETE) && pcm_call_is_valid(&call) &&
			      call.stage != PCM_CALL_RETRAIN_SILENCE, "silent peer prevented bounded fast hangup: %s", pcm_call_result_name(result));
		}
		CHECK(call.stage == PCM_CALL_CLOSED, "fast hangup waited indefinitely for silent peer");
	}
	return true;
}

static bool run_v92_fast_response(struct v92_online_pair *pair, unsigned hangup)
{
	struct pcm_call *call = &pair->digital;
	struct v92_upstream_data_tx *upstream = &pair->analog.state.data_path.v92_analog.upstream;
	struct v92_pcm_mapping mapping = upstream->encoder.mapping;
	struct v92_cpu_parameters cpu = call->state.data_path.v92_digital.cpu;
	struct v90_pcm_mapping downstream_mapping = call->state.data_path.v92_digital.downstream.data.transmitter.encoder.mapping;
	struct v92_fast_peer_source peer = { 0 };
	struct pcm_payload_source source = { 0 };
	struct pcm_r_rx rf;
	struct pcm_downstream_rx control;
	struct v90_downstream_data_rx data;
	uint32_t up_frames = 0, down_frames = 0, held_frames = 0;
	bool held = false, new_upstream = false, ed = false, finishing = false;
	unsigned checked_stages = 0;
	bool backpressure_checked = false;
	bool hangup_requested = false;
	cpu.acknowledges_cpd = false;
	cpu.data_rate_number--;
	CHECK(v92_upstream_control_init(&peer.control, &pair->analog.v92_analog_phase4_configuration.suvu, &cpu) &&
	      pcm_r_rx_init(&rf, &downstream_mapping, V90_PCM_MAPPING_TRANSMIT, PCM_R_RF) == PCM_R_RX_OK &&
	      pcm_downstream_rx_init(&control, PCM_PROTOCOL_V92, &downstream_mapping, V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_upstream_data_tx_start_rm(upstream) == V92_UPSTREAM_DATA_TX_OK,
	      "fast response fixture did not initialize");
	unsigned old_budget = call->pcm_upstream_quality.automatic_retrains;
	for (unsigned clock = 0; clock < 16000u; ++clock) {
		uint8_t ar, dr, transmitted;
		int16_t sample;
		struct pcm_call_data_event event;
		const struct pcm_call_data_sources sources = { .downstream = pcm_payload, .downstream_context = &source };
		channel_receive(&pair->wire, &ar, &dr);
		if (!hangup && !finishing && peer.active && peer.control.record_complete && !upstream->queue_count &&
		    v92_upstream_control_can_finish(&peer.control)) {
			CHECK(v92_upstream_data_tx_start_e2u(upstream) == V92_UPSTREAM_DATA_TX_OK, "fast E2u failed");
			finishing = true;
		}
		enum v92_upstream_data_tx_result tx = v92_upstream_data_tx_next(upstream, v92_fast_peer_bits, &peer, &sample);
		CHECK(tx == V92_UPSTREAM_DATA_TX_OK || tx == V92_UPSTREAM_DATA_TX_RM_COMPLETE ||
		      tx == V92_UPSTREAM_DATA_TX_FB1U_COMPLETE, "fast peer TX failed");
		if (tx == V92_UPSTREAM_DATA_TX_RM_COMPLETE)
			peer.active = true;
		if (peer.active && !peer.control.record_complete && peer.bit_index == peer.bit_count && !upstream->queue_count)
			CHECK(v92_upstream_control_complete(&peer.control), "fast peer record did not finish");
		if (tx == V92_UPSTREAM_DATA_TX_FB1U_COMPLETE) {
			CHECK(v92_upstream_data_tx_init(upstream, &mapping, upstream->upstream_level) == V92_UPSTREAM_DATA_TX_OK,
			      "fast new B1u failed");
			peer.active = false;
			peer.data_frames = 0;
			new_upstream = true;
		}
		enum pcm_call_result result = pcm_call_data_octet(call, dr, decode_octet(call->law, dr), true,
			&sources, &transmitted, &event);
		if (hangup && result == PCM_CALL_CLEARDOWN_COMPLETE) {
			CHECK(peer.zero_received && call->stage == PCM_CALL_CLOSED && pcm_call_is_disconnecting(call) &&
			      pcm_call_is_valid(call) && !call->failed, "fast hangup omitted zero-rate CPd");
			return true;
		}
		CHECK(result == PCM_CALL_OK && pcm_call_is_valid(call) && call->stage == PCM_CALL_DATA_PATH &&
		      call->pcm_upstream_quality.automatic_retrains == old_budget,
		      "fast response failed at %u: %s stage=%u fast=%u", clock, pcm_call_result_name(result),
		      call->stage, call->state.data_path.v92_digital.fast.stage);
		struct pcm_call_v92_fast_exchange *fast = &call->state.data_path.v92_digital.fast;
		if (!hangup && fast->stage != PCM_CALL_V92_FAST_IDLE && !(checked_stages & (1u << fast->stage))) {
			CHECK(v92_fast_boundary_checks(call, dr), "fast boundary contract failed");
			checked_stages |= 1u << fast->stage;
		}
		if (hangup == 1u && !hangup_requested && fast->stage == PCM_CALL_V92_FAST_CONTROL) {
			CHECK(pcm_call_start_cleardown(call) == PCM_CALL_OK && pcm_call_is_disconnecting(call) &&
			      pcm_call_is_valid(call), "local fast hangup failed");
			hangup_requested = true;
		}
		if (!backpressure_checked && fast->stage == PCM_CALL_V92_FAST_FINISH &&
		    call->state.data_path.v92_digital.downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA) {
			struct pcm_call rejected = *call;
			struct pcm_payload_source rejecting = { .reject = true };
			struct pcm_call_data_event ignored_event;
			uint8_t ignored_octet;
			const struct pcm_call_data_sources blocked = { .downstream = pcm_payload, .downstream_context = &rejecting };
			enum pcm_call_result r = pcm_call_data_octet(&rejected, dr, decode_octet(call->law, dr), true,
				&blocked, &ignored_octet, &ignored_event);
			if (r == PCM_CALL_SOURCE_FAILURE) {
				CHECK(memcmp(&rejected, call, sizeof(rejected)) == 0 && !rejecting.reject,
				      "fast source failure consumed media or changed receive/deadline state");
				backpressure_checked = true;
			}
		}
		if (!held && fast->stage != PCM_CALL_V92_FAST_IDLE) {
			held = true;
			held_frames = source.frames;
		}
		if (held && fast->stage != PCM_CALL_V92_FAST_IDLE && fast->stage != PCM_CALL_V92_FAST_FINISH)
			CHECK(source.frames == held_frames && event.kind == PCM_CALL_DATA_NONE && !pcm_call_is_online(call),
			      "fast response consumed source or exposed control payload");
		if (new_upstream && event.kind == PCM_CALL_PCM_UPSTREAM_FRAME)
			CHECK(v92_rate_payload(event.frame.pcm_upstream.bits, event.frame.pcm_upstream.bit_count, &up_frames),
			      "fast upstream payload changed");
		if (!pcm_r_rx_is_complete(&rf)) {
			enum pcm_r_rx_result r = pcm_r_rx_receive(&rf, ar);
			CHECK(r == PCM_R_RX_BUFFERING || r == PCM_R_RX_SIGNAL_DETECTED ||
			      r == PCM_R_RX_BAR_STARTED || r == PCM_R_RX_BAR_COMPLETE, "fast Rf failed");
		} else if (!ed) {
			struct pcm_downstream_event received;
			enum pcm_downstream_rx_result r = pcm_downstream_rx_receive(&control, ar, &received);
			CHECK(r == PCM_DOWNSTREAM_RX_BUFFERING || r == PCM_DOWNSTREAM_RX_SUVD ||
			      r == PCM_DOWNSTREAM_RX_CPD || r == PCM_DOWNSTREAM_RX_ED, "fast downstream control failed: %u", r);
			if (hangup == 2u && r == PCM_DOWNSTREAM_RX_SUVD)
				peer.cleardown = true;
			if (hangup && r == PCM_DOWNSTREAM_RX_CPD && !received.control.record.cpd.parameters.data_rate_number) {
				peer.cleardown = true;
				peer.zero_received = true;
			}
			if (!peer.cleardown && !v92_upstream_control_can_finish(&peer.control)) {
				if (r == PCM_DOWNSTREAM_RX_SUVD)
					CHECK(v92_upstream_control_note_suvd(&peer.control, &received.control.record.suvd.parameters), "SUVd rejected");
				if (r == PCM_DOWNSTREAM_RX_CPD)
					CHECK(v92_pcm_mapping_init(&mapping, call->law, &received.control.record.cpd.parameters) == V92_PCM_MAPPING_OK &&
					      v92_upstream_control_note_cpd(&peer.control, received.control.record.cpd.parameters.acknowledges_cpu), "CPd rejected");
			}
			if (!control.ed_requested && v92_upstream_control_can_finish(&peer.control) &&
			    (r == PCM_DOWNSTREAM_RX_SUVD || r == PCM_DOWNSTREAM_RX_CPD))
				CHECK(pcm_downstream_rx_expect_ed(&control) == PCM_DOWNSTREAM_RX_OK, "Ed boundary lost");
			if (r == PCM_DOWNSTREAM_RX_ED) {
				CHECK(v90_pcm_mapping_init_from_v92_cpu(&downstream_mapping, &cpu) == V90_PCM_MAPPING_OK &&
				      v90_downstream_data_rx_start_after_ed(&data, &control, &downstream_mapping) == V90_DOWNSTREAM_DATA_RX_OK,
				      "new B1d failed");
				ed = true;
			}
		} else {
			struct v90_pcm_stream_frame frame;
			enum v90_downstream_data_rx_result r = v90_downstream_data_rx_receive_octet(&data, ar, &frame);
			CHECK(r == V90_DOWNSTREAM_DATA_RX_BUFFERING || r == V90_DOWNSTREAM_DATA_RX_B1D_FRAME ||
			      r == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE || r == V90_DOWNSTREAM_DATA_RX_DATA_FRAME, "new downstream data failed");
			if (r == V90_DOWNSTREAM_DATA_RX_DATA_FRAME) {
				uint32_t index = held_frames + down_frames;
				CHECK(v92_rate_payload(frame.bits, frame.bit_count, &index), "fast downstream payload changed");
				down_frames++;
			}
		}
		channel_transmit(&pair->wire, encode_octet(call->law, sample), transmitted);
		if (up_frames >= 20u && down_frames >= 20u) {
			CHECK(held && backpressure_checked && checked_stages == 62u && pcm_call_is_online(call) && !call->retrain_active &&
			      fast->stage == PCM_CALL_V92_FAST_IDLE && !mapping.cpd.extend_e2u &&
			      call->state.data_path.v92_digital.cpu.data_rate_number == cpu.data_rate_number,
			      "fast response did not resume the call at the accepted mapping");
			return true;
		}
	}
	CHECK(false, "fast response did not resume duplex data: %u/%u finish=%u new=%u stage=%u b1=%llu/%u frames=%llu bad=%u err=%g",
		up_frames, down_frames, finishing, new_upstream, call->state.data_path.v92_digital.fast.stage,
		(unsigned long long)call->state.data_path.v92_digital.fast.b1_frame, call->state.data_path.v92_digital.fast.b1_started,
		(unsigned long long)call->state.data_path.v92_digital.upstream.line.received_frames,
		call->state.data_path.v92_digital.b1u_decode_failed, call->state.data_path.v92_digital.b1u_squared_error);
}

static bool test_v92_fast_response(void)
{
	const unsigned rates[] = { 1u, 7u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			struct v92_online_pair pair;
			const struct v92_upstream_offer offer = {
				.peer_mask = 1u << (rates[rate] - 1u), .local_maximum = rates[rate], .expected = rates[rate],
			};
			CHECK(capture_v92_duplex_data(law, rate ? 13u : 1u, false, &offer, NULL, 0u, &pair), "fast call did not train");
			for (unsigned hangup = 0; hangup < 3; ++hangup) {
				struct v92_online_pair trial = pair;
				CHECK(run_v92_fast_response(&trial, hangup), "law %u rate %u hangup %u fast response failed", law, rates[rate], hangup);
			}
		}
	return true;
}

static bool v92_fast_analog_contracts(const struct pcm_call *snapshot, uint8_t received)
{
	struct pcm_call call = *snapshot;
	struct pcm_call_data_event event;
	uint8_t generated;
	call.state.data_path.v92_analog.fast.deadline_remaining = 1u;
	CHECK(pcm_call_data_octet(&call, received, decode_octet(call.law, received), true, NULL, &generated, &event) == PCM_CALL_OK &&
	      call.stage == PCM_CALL_RETRAIN_SILENCE && pcm_call_is_valid(&call) && event.kind == PCM_CALL_DATA_NONE &&
	      call.pcm_upstream_quality.automatic_retrains == snapshot->pcm_upstream_quality.automatic_retrains,
	      "analogue fast timeout did not retain bounded recovery");
	call = *snapshot;
	call.state.data_path.v92_analog.fast.record_index = sizeof(call.state.data_path.v92_analog.fast.record) * 8u + 1u;
	CHECK(!pcm_call_is_valid(&call), "oversized fast record accepted");
	call = *snapshot;
	call.state.data_path.v92_analog.fast.deadline_remaining = 0u;
	CHECK(!pcm_call_is_valid(&call), "expired fast owner reported idle");
	call = *snapshot;
	call.state.data_path.v92_analog.fast.control.suvu.request_silence = true;
	CHECK(!pcm_call_is_valid(&call), "fast owner accepted a silence request");
	call = *snapshot;
	CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_INVALID_STATE &&
	      memcmp(&call, snapshot, sizeof(call)) == 0, "nested rate request changed fast owner");
	if (pcm_r_rx_is_complete(&call.state.data_path.v92_analog.rf) &&
	    !call.state.data_path.v92_analog.fast.finish_started) {
		CHECK(pcm_call_start_cleardown(&call) == PCM_CALL_OK, "silent-peer fast close rejected");
		for (unsigned clock = 0; clock < 8000u && call.stage != PCM_CALL_CLOSED; ++clock) {
			enum pcm_call_result result = pcm_call_data_octet(&call, silence_octet(call.law), 0, true, NULL, &generated, &event);
			CHECK((result == PCM_CALL_OK || result == PCM_CALL_CLEARDOWN_COMPLETE) && pcm_call_is_valid(&call) &&
			      call.stage != PCM_CALL_RETRAIN_SILENCE, "silent peer interrupted fast close: %s", pcm_call_result_name(result));
		}
		CHECK(call.stage == PCM_CALL_CLOSED, "silent peer prevented bounded fast close");
	}
	return true;
}

static bool run_v92_fast_pair(struct v92_online_pair *pair, unsigned initiators, unsigned hangup,
	unsigned burst_symbols)
{
	struct pcm_call *calls[] = { &pair->analog, &pair->digital };
	struct pcm_payload_source source[2] = { 0 };
	uint32_t held[2] = { 0 }, frames[2] = { 0 };
	bool seen[2] = { false, false };
	bool closed[2] = { false, false }, zero_received[2] = { false, false };
	bool hangup_requested = false;
	unsigned checked = 0u;
	unsigned damaged = 0u;
	/* A saved setup may request silence, but fast exchange must not inherit it. */
	pair->analog.v92_analog_phase4_configuration.suvu.request_silence = true;
	for (unsigned role = 0; role < 2u; ++role) {
		if (!(initiators & (1u << role)))
			continue;
		CHECK(pcm_call_start_fast_exchange(calls[role]) == PCM_CALL_OK, "fast pair request failed role %u", role);
		struct pcm_call retained = *calls[role];
		CHECK(pcm_call_start_fast_exchange(calls[role]) == PCM_CALL_INVALID_STATE &&
		      memcmp(calls[role], &retained, sizeof(retained)) == 0, "duplicate fast request changed state");
	}
	for (unsigned clock = 0; clock < 24000u; ++clock) {
		uint8_t received[2], transmitted[2];
		channel_receive(&pair->wire, &received[0], &received[1]);
		for (unsigned role = 0; role < 2u; ++role) {
			struct pcm_call *call = calls[role];
			transmitted[role] = silence_octet(call->law);
			if (closed[role])
				continue;
			const struct pcm_call_data_sources sources = {
				.downstream = pcm_payload, .downstream_context = &source[role],
				.pcm_upstream = pcm_payload, .pcm_upstream_context = &source[role],
			};
			struct pcm_call_data_event event;
			enum pcm_call_result result = pcm_call_data_octet(call, received[role], decode_octet(call->law, received[role]),
				true, &sources, &transmitted[role], &event);
			if (hangup && result == PCM_CALL_CLEARDOWN_COMPLETE) {
				CHECK(call->stage == PCM_CALL_CLOSED && pcm_call_is_valid(call), "fast close left invalid call");
				closed[role] = true;
				continue;
			}
			CHECK(result == PCM_CALL_OK && call->stage == PCM_CALL_DATA_PATH && pcm_call_is_valid(call),
			      "fast pair mask %u role %u clock %u failed: %s stage=%u", initiators, role, clock,
			      pcm_call_result_name(result), call->stage);
			if (!seen[role] && call->retrain_active) {
				seen[role] = true;
				held[role] = source[role].frames;
				if (!role)
					CHECK(!call->state.data_path.v92_analog.fast.control.suvu.request_silence &&
					      call->v92_analog_phase4_configuration.suvu.request_silence,
					      "fast exchange inherited silence or changed saved setup");
			}
			if (!hangup && !role && call->state.data_path.v92_analog.fast.deadline_remaining) {
				const struct pcm_call_v92_analog_fast *fast = &call->state.data_path.v92_analog.fast;
				unsigned phase = !fast->rm_complete ? 0u : !pcm_r_rx_is_complete(&call->state.data_path.v92_analog.rf) ? 1u :
					!fast->finish_started ? 2u : !fast->b1_started ? 3u : 4u;
				if (!(checked & (1u << phase))) {
					CHECK(v92_fast_analog_contracts(call, received[role]), "fast analogue boundary failed");
					checked |= 1u << phase;
				}
			}
			zero_received[role] |= role ? call->state.data_path.v92_digital.fast.control.cleardown.peer_requested :
				call->state.data_path.v92_analog.fast.cleardown.peer_requested;
			if (!hangup && seen[0] && seen[1] && event.kind != PCM_CALL_DATA_NONE) {
				const uint8_t *bits = role ? event.frame.pcm_upstream.bits : event.frame.downstream.bits;
				size_t count = role ? event.frame.pcm_upstream.bit_count : event.frame.downstream.bit_count;
				uint32_t index = held[1u - role] + frames[role];
				CHECK(v92_rate_payload(bits, count, &index), "fast pair payload changed role %u clock %u", role, clock);
				frames[role]++;
			}
		}
		const struct pcm_call_v92_analog_fast *fast = &pair->analog.state.data_path.v92_analog.fast;
		/* Corrupt generated CPu octets, not receiver event flags. The peer
		 * must recover its continuous decoder and qualify a later repeat. */
		if (damaged < burst_symbols && (damaged || (fast->rm_complete && !fast->finish_started &&
		    fast->control.record == V92_UPSTREAM_CONTROL_CPU && fast->record_index > 0u))) {
			transmitted[0] = silence_octet(pair->analog.law);
			++damaged;
		}
		channel_transmit(&pair->wire, transmitted[0], transmitted[1]);
		if (hangup && !hangup_requested && pair->digital.state.data_path.v92_digital.fast.stage == PCM_CALL_V92_FAST_CONTROL &&
		    pair->analog.state.data_path.v92_analog.fast.rm_complete) {
			for (unsigned role = 0; role < 2u; ++role)
				if (hangup & (1u << role))
					CHECK(pcm_call_start_cleardown(calls[role]) == PCM_CALL_OK, "fast hangup request failed role %u", role);
			hangup_requested = true;
		}
		if (closed[0] && closed[1]) {
			CHECK(zero_received[0] && zero_received[1] && source[0].frames == held[0] && source[1].frames == held[1],
			      "fast hangup did not exchange zero CP or consumed payload: zero=%u/%u", zero_received[0], zero_received[1]);
			return true;
		}
		if (frames[0] >= 20u && frames[1] >= 20u) {
			CHECK(pcm_call_is_online(calls[0]) && pcm_call_is_online(calls[1]) &&
			      !calls[0]->retrain_active && !calls[1]->retrain_active && (checked & 29u) == 29u &&
			      (initiators != 1u || checked == 31u) && damaged == burst_symbols,
			      "fast pair did not resume online or check every boundary");
			return true;
		}
	}
	CHECK(false, "fast pair timed out frames=%u/%u af=%u/%u/%u rx=%u df=%u rm=%u b1=%u",
		frames[0], frames[1], pair->analog.state.data_path.v92_analog.fast.rm_complete,
		pair->analog.state.data_path.v92_analog.fast.finish_started, pair->analog.state.data_path.v92_analog.fast.b1_started,
		pair->analog.state.data_path.v92_analog.downstream.training.stage,
		pair->digital.state.data_path.v92_digital.fast.stage, pair->digital.state.data_path.v92_digital.fast.rm_complete,
		pair->digital.state.data_path.v92_digital.fast.b1_started);
}

static bool test_v92_fast_pair(void)
{
	const unsigned rates[] = { 1u, 7u, 15u, 17u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			struct v92_online_pair pair;
			const struct v92_upstream_offer offer = {
				.peer_mask = 1u << (rates[rate] - 1u), .local_maximum = rates[rate], .expected = rates[rate],
			};
			CHECK(capture_v92_duplex_data(law, rate ? 13u : 1u, false, &offer, NULL, 0u, &pair), "fast pair did not train");
			for (unsigned initiators = 1u; initiators <= 3u; ++initiators)
			for (unsigned hangup = 0; hangup <= 3u; ++hangup) {
				struct v92_online_pair trial = pair;
				CHECK(run_v92_fast_pair(&trial, initiators, hangup, 0u), "fast pair law %u rate %u hangup %u failed", law, rates[rate], hangup);
			}
			if (rates[rate] == 19u)
				for (unsigned initiators = 1u; initiators <= 3u; ++initiators) {
					struct v92_online_pair trial = pair;
					CHECK(run_v92_fast_pair(&trial, initiators, 0u, 80u),
					      "fast CPu burst recovery failed law %u initiators %u", law, initiators);
				}
		}
	return true;
}

static bool v92_analog_rate_prefix_contracts(const struct pcm_call *online)
{
	struct pcm_call shifted = *online;
	/* A future full retrain may use a different nominal preference. Ru
	 * must retain the live upstream level, including negotiated reduction. */
	shifted.phase3_configuration.v92_analog.upstream_lu_ucode -= 4u;
	for (unsigned offset = 0; offset < V92_PCM_MAPPING_FRAME_SYMBOLS; ++offset) {
		struct pcm_call call = shifted;
		struct v92_upstream_data_tx data = call.state.data_path.v92_analog.upstream;
		unsigned queued = data.queue_count;
		struct pcm_call_observation observation;
		uint8_t transmitted;
		CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_OK &&
		      !pcm_call_is_online(&call) && call.v92_rate_renegotiation_active &&
		      call.rate_renegotiation_deadline_armed && !call.b1_deadline_armed &&
		      call.rate_renegotiation_deadline_remaining_samples == 40000u + 2u * call.round_trip_delay_samples,
		      "analogue rate entry failed at offset %u", offset);
		struct pcm_call snapshot = call;
		CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_INVALID_STATE &&
		      memcmp(&call, &snapshot, sizeof(call)) == 0, "duplicate rate entry changed call");
		for (unsigned i = 0; i < queued + 408u; ++i) {
			int16_t expected;
			if (i < queued) {
				CHECK(v92_upstream_data_tx_next(&data, NULL, NULL, &expected) == V92_UPSTREAM_DATA_TX_OK,
				      "expected data drain failed");
			} else {
				unsigned symbol = i - queued;
				bool positive = (symbol % 6u < 3u) != (symbol >= 384u);
				expected = positive ? data.upstream_level : -data.upstream_level;
			}
			CHECK(pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
			      transmitted == encode_octet(call.law, expected),
			      "Ru moved off the queued frame boundary offset %u symbol %u", offset, i);
		}
		CHECK(call.state.phase4.v92.owner.analog.station.transmitted_symbols == 0u &&
		      pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
		      call.state.phase4.v92.owner.analog.station.transmitted_symbols == 1u,
		      "TRN2u did not start immediately after Ru-bar while awaiting Rd");
		call.rate_renegotiation_deadline_remaining_samples = 1u;
		CHECK(pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
		      call.stage == PCM_CALL_RETRAIN_SILENCE && !call.failed &&
		      !call.v92_rate_renegotiation_active, "missing Rd did not use bounded full retrain");
		struct pcm_call_data_event event;
		CHECK(pcm_call_data_octet(&shifted, silence_octet(shifted.law), 0, true, NULL,
			&transmitted, &event) == PCM_CALL_OK, "data offset failed");
	}
	return true;
}

static bool v92_analog_remote_rd(const struct pcm_call *online)
{
	struct pcm_call call = *online;
	call.phase3_configuration.v92_analog.upstream_lu_ucode -= 4u;
	struct pcm_r_tx rd;
	struct v92_upstream_data_tx data = call.state.data_path.v92_analog.upstream;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx training;
	struct pcm_downstream_rx expected_receive;
	struct pcm_call_data_event event;
	struct pcm_call_observation observation;
	struct v90_pcm_symbol symbol;
	uint8_t transmitted;
	CHECK(pcm_r_tx_init(&rd, &call.state.data_path.v92_analog.rd.mapping, PCM_R_RD) == PCM_R_TX_OK &&
	      v90_pcm_mapping_init_rate_renegotiation(&mapping,
		&call.state.data_path.v92_analog.training_mapping, &rd.mapping) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&training, PCM_PROTOCOL_V92, &mapping) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(&expected_receive, PCM_PROTOCOL_V92, &mapping,
		V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK, "remote Rd setup failed");
	for (unsigned i = 0; i <= PCM_R_SYMBOLS; ++i) {
		int16_t expected;
		CHECK(pcm_r_tx_next(&rd, &symbol) == PCM_R_TX_OK &&
		      v92_upstream_data_tx_next(&data, NULL, NULL, &expected) == V92_UPSTREAM_DATA_TX_OK &&
		      pcm_call_data_octet(&call, symbol.octet, symbol.sample, true, NULL, &transmitted, &event) == PCM_CALL_OK &&
		      transmitted == encode_octet(call.law, expected), "remote Rd changed data framing");
		CHECK(i < PCM_R_RX_MIN_SYMBOLS - 1u || event.kind == PCM_CALL_DATA_NONE,
		      "qualified Rd escaped as host payload");
		CHECK(call.stage == (i < PCM_R_SYMBOLS ? PCM_CALL_DATA_PATH : PCM_CALL_PHASE4),
		      "remote Rd response did not wait for its polarity reversal");
	}
	struct pcm_call broken = call;
	CHECK(pcm_call_octet(&broken, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
	      broken.stage == PCM_CALL_RETRAIN_SILENCE && !broken.failed,
	      "broken Rd-bar did not recover through full retrain");
	unsigned queued = data.queue_count;
	for (unsigned i = 0; i < queued + 408u; ++i) {
		int16_t expected;
		if (i < queued) {
			CHECK(v92_upstream_data_tx_next(&data, NULL, NULL, &expected) == V92_UPSTREAM_DATA_TX_OK,
			      "remote Rd expected drain failed");
		} else {
			unsigned n = i - queued;
			bool positive = (n % 6u < 3u) != (n >= 384u);
			expected = positive ? data.upstream_level : -data.upstream_level;
		}
		if (!pcm_r_tx_is_complete(&rd)) {
			CHECK(pcm_r_tx_next(&rd, &symbol) == PCM_R_TX_OK, "remote Rd tail failed");
		} else {
			struct pcm_downstream_event received;
			struct pcm_downstream_symbol generated;
			CHECK(pcm_downstream_tx_next(&training, &generated) == PCM_DOWNSTREAM_TX_OK,
			      "remote TRN2d generation failed");
			symbol = generated.pcm;
			enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(
				&expected_receive, symbol.octet, &received);
			CHECK(result == PCM_DOWNSTREAM_RX_BUFFERING || result == PCM_DOWNSTREAM_RX_TRN2D_FRAME,
			      "independent TRN2d decode failed");
		}
		CHECK(pcm_call_octet(&call, symbol.octet, symbol.sample, &transmitted, &observation) == PCM_CALL_OK &&
		      transmitted == encode_octet(call.law, expected), "remote Rd response waveform failed");
	}
	CHECK(memcmp(&call.state.phase4.v92.owner.analog.station.receive, &expected_receive,
		sizeof(expected_receive)) == 0, "remote Rd lost overlapping TRN2d samples");
	return true;
}

static bool v92_digital_rate_prefix_contracts(const struct pcm_call *online)
{
	struct pcm_call shifted = *online;
	for (unsigned offset = 0; offset < V90_CP_FRAME_INTERVALS; ++offset) {
		struct pcm_call call = shifted;
		struct v90_downstream_data_stream data = call.state.data_path.v92_digital.downstream.data;
		struct v90_pcm_mapping mapping;
		struct pcm_r_tx rd;
		struct pcm_downstream_tx training;
		const struct v92_suvd_parameters suvd = { 0 };
		struct pcm_call_observation observation;
		struct v90_pcm_symbol symbol;
		uint8_t transmitted;
		CHECK(pcm_r_tx_init(&rd, &data.transmitter.encoder.mapping, PCM_R_RD) == PCM_R_TX_OK &&
		      v90_pcm_mapping_init_rate_renegotiation(&mapping,
			&call.state.data_path.v92_digital.training_mapping,
			&data.transmitter.encoder.mapping) == V90_PCM_MAPPING_OK &&
		      pcm_downstream_tx_init(&training, PCM_PROTOCOL_V92, &mapping) == PCM_DOWNSTREAM_TX_OK &&
		      pcm_downstream_tx_start_suvd(&training, &suvd) == PCM_DOWNSTREAM_TX_OK,
		      "digital rate prefix setup failed");
		CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_OK &&
		      !pcm_call_is_online(&call) && call.v92_rate_renegotiation_active &&
		      call.rate_renegotiation_deadline_armed && !call.b1_deadline_armed,
		      "digital rate entry failed at offset %u", offset);
		struct pcm_call snapshot = call;
		CHECK(pcm_call_start_rate_renegotiation(&call) == PCM_CALL_INVALID_STATE &&
		      memcmp(&snapshot, &call, sizeof(call)) == 0, "duplicate digital rate entry changed call");
		unsigned drained = 0;
		for (;;) {
			enum v90_downstream_data_stream_result result = v90_downstream_data_stream_finish(&data, &symbol);
			if (result == V90_DOWNSTREAM_DATA_STREAM_COMPLETE)
				break;
			CHECK(result == V90_DOWNSTREAM_DATA_STREAM_OK && ++drained <= 64u &&
			      pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
			      transmitted == symbol.octet, "digital rate entry truncated a data frame");
		}
		for (unsigned i = 0; i < PCM_R_SYMBOLS + PCM_R_BAR_SYMBOLS; ++i)
			CHECK(pcm_r_tx_next(&rd, &symbol) == PCM_R_TX_OK &&
			      pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
			      transmitted == symbol.octet, "digital Rd waveform failed at %u/%u", offset, i);
		/* No Ru arrives: TRN2d must still occupy the very next interval,
		 * continue through its complete minimum duration, then repeat SUVd. */
		for (unsigned i = 0; i < (offset ? 1u : 3000u); ++i) {
			struct pcm_downstream_symbol expected;
			CHECK(pcm_downstream_tx_next(&training, &expected) == PCM_DOWNSTREAM_TX_OK,
			      "expected digital training failed");
			enum pcm_call_result result = pcm_call_octet(&call, silence_octet(call.law), 0,
				&transmitted, &observation);
			CHECK(result == PCM_CALL_OK && call.stage == PCM_CALL_PHASE4 &&
			      transmitted == expected.pcm.octet,
			      "digital rate training lost its output interval at %u/%u: %s stage %u octet %u/%u",
			      offset, i, pcm_call_result_name(result), call.stage, transmitted, expected.pcm.octet);
			CHECK(call.state.phase4.v92.owner.ru.transmit.transmitted_symbols == training.transmitted_symbols,
			      "digital training did not advance its continuous transmitter");
		}
		CHECK(offset || training.stage == PCM_DOWNSTREAM_TX_V92_CONTROL,
		      "missing Ru left digital TRN2d unbounded");
		call.rate_renegotiation_deadline_remaining_samples = 1u;
		CHECK(pcm_call_octet(&call, silence_octet(call.law), 0, &transmitted, &observation) == PCM_CALL_OK &&
		      call.stage == PCM_CALL_RETRAIN_SILENCE && !call.failed && !call.v92_rate_renegotiation_active,
		      "missing Ru did not use bounded full retrain");
		struct pcm_call_data_event event;
		CHECK(pcm_call_data_octet(&shifted, silence_octet(shifted.law), 0, true, NULL,
			&transmitted, &event) == PCM_CALL_OK, "digital data offset failed");
	}
	return true;
}

static bool v92_call_cleardown(const struct v92_online_pair *online, unsigned initiators)
{
	struct v92_online_pair pair = *online;
	struct pcm_call *calls[] = { &pair.analog, &pair.digital };
	struct pcm_downstream_rx downstream;
	struct v92_phase4_digital_rx upstream;
	bool observing[2] = { false, false }, zero[2] = { false, false };
	bool acknowledged[2] = { false, false }, closed[2] = { false, false };
	bool expect_ack[2] = { false, false };
	enum g711_law law = pair.analog.law;
	unsigned tail = 0u;

	for (unsigned role = 0; role < 2u; ++role) {
		if (!(initiators & (1u << role)))
			continue;
		if (initiators == 3u && calls[role]->stage == PCM_CALL_DATA_PATH)
			CHECK(pcm_call_start_rate_renegotiation(calls[role]) == PCM_CALL_OK,
			      "cleardown could not join an existing rate exchange");
		CHECK(pcm_call_start_cleardown(calls[role]) == PCM_CALL_OK &&
		      pcm_call_is_disconnecting(calls[role]) && !pcm_call_is_online(calls[role]),
		      "PCM data cleardown did not enter rate exchange role %u", role);
		struct pcm_call retained = *calls[role];
		CHECK(pcm_call_start_cleardown(calls[role]) == PCM_CALL_OK &&
		      pcm_call_start_retrain(calls[role]) == PCM_CALL_INVALID_STATE &&
		      memcmp(calls[role], &retained, sizeof(retained)) == 0,
		      "duplicate cleardown or ATO1 restarted entry");
		/* No response is a bounded close, not another full retrain. */
		retained.rate_renegotiation_deadline_remaining_samples = 1u;
		struct pcm_call_observation observation;
		uint8_t transmitted;
		CHECK(pcm_call_octet(&retained, silence_octet(law), 0, &transmitted, &observation) ==
			PCM_CALL_CLEARDOWN_COMPLETE && retained.stage == PCM_CALL_CLOSED &&
		      pcm_call_is_valid(&retained), "unanswered cleardown restarted training");
	}
	for (unsigned clock = 0; clock < 24000u && tail < pair.wire.delay +
	     V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY + V92_RU_RX_FRAME_SYMBOLS; ++clock) {
		uint8_t received[2], transmitted[2];
		channel_receive(&pair.wire, &received[0], &received[1]);
		for (unsigned role = 0; role < 2u; ++role) {
			struct pcm_call *call = calls[role];
			transmitted[role] = silence_octet(law);
			struct v92_cleardown final = { 0 };
			uint64_t sent = 0;
			if (call->stage == PCM_CALL_PHASE4 && call->state.phase4.v92.child != PCM_CALL_V92_PHASE4_CONTROL)
				observing[role] = false;
			if (call->stage == PCM_CALL_PHASE4 &&
			    call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL) {
				struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
				bool receiving = role != 0u || state->prefix_stage == PCM_CALL_V92_PREFIX_NONE ||
					pcm_r_rx_is_complete(&state->prefix.analog.receive);
				if (role == 0u) {
					if (!observing[role] && receiving)
						downstream = state->owner.analog.station.receive;
					final = state->owner.analog.station.cleardown;
					sent = state->owner.analog.station.transmitted_symbols;
				} else {
					if (!observing[role])
						upstream = state->owner.digital.receive;
					final = state->owner.digital.control.cleardown;
					sent = state->owner.digital.control.transmit.transmitted_symbols;
				}
				observing[role] |= receiving;
			}
			/* Independent decoders inspect the actual received wire. A host
			 * stop or a transmitter's internal zero-rate flag is insufficient. */
			if (observing[role] && role == 0u) {
				struct pcm_downstream_event event;
				enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(
					&downstream, received[role], &event);
				if (result == PCM_DOWNSTREAM_RX_CPD && event.control.record.cpd.frame_result == V92_CPD_OK) {
					zero[role] |= event.control.record.cpd.parameters.data_rate_number == 0u;
					acknowledged[role] |= event.control.record.cpd.parameters.acknowledges_cpu;
				}
			} else if (observing[role]) {
				struct v92_control_event event;
				enum v92_phase4_digital_rx_result result = v92_phase4_digital_rx_sample(
					&upstream, decode_octet(law, received[role]), &event);
				if (result == V92_PHASE4_DIGITAL_RX_EVENT && event.type == V92_CONTROL_EVENT_CPU) {
					zero[role] |= event.record.cpu.parameters.data_rate_number == 0u;
					acknowledged[role] |= event.record.cpu.parameters.acknowledges_cpd;
				}
			}
			/* A legal 100 ms initiator timeout may precede the peer's
			 * complete response. Keep recording that final wire tail. */
			if (closed[role])
				continue;
			CHECK(!pcm_call_phase4_is_ready(call), "cleardown reached B1");
			enum pcm_call_result result;
			if (call->stage == PCM_CALL_DATA_PATH) {
				struct pcm_call_data_event event;
				result = pcm_call_data_octet(call, received[role], decode_octet(law, received[role]), true,
					NULL, &transmitted[role], &event);
			} else {
				struct pcm_call_observation observation;
				result = pcm_call_octet(call, received[role], decode_octet(law, received[role]),
					&transmitted[role], &observation);
			}
			if (result == PCM_CALL_CLEARDOWN_COMPLETE) {
				CHECK(final.active && final.cp_end && sent >= final.cp_end &&
				      (!final.peer_requested || (final.acknowledgement_end && sent >= final.acknowledgement_end)) &&
				      ((final.initiator && final.acknowledged) ||
				       sent >= (final.initiator ? final.cp_end : final.acknowledgement_end) +
					800u + (final.initiator ? final.round_trip_delay : (final.round_trip_delay + 1u) / 2u)),
				      "cleardown skipped zero CP or line delay role %u", role);
				expect_ack[1u - role] = final.peer_requested;
				closed[role] = true;
			} else {
				CHECK(result == PCM_CALL_OK && call->stage != PCM_CALL_RETRAIN_SILENCE,
				      "PCM cleardown law %u mask %u role %u clock %u failed: %s stage %u",
				      law, initiators, role, clock, pcm_call_result_name(result), call->stage);
			}
			CHECK(pcm_call_is_valid(call), "cleardown state invalid");
		}
		channel_transmit(&pair.wire, transmitted[0], transmitted[1]);
		if (closed[0] && closed[1])
			tail++;
	}
	CHECK(closed[0] && closed[1] && zero[0] && zero[1] &&
	      (!expect_ack[0] || acknowledged[0]) && (!expect_ack[1] || acknowledged[1]),
	      "PCM cleardown incomplete law %u mask %u closed %u/%u zero %u/%u ack %u/%u",
	      law, initiators, closed[0], closed[1], zero[0], zero[1], acknowledged[0], acknowledged[1]);
	return true;
}

static bool test_v92_rate_request(void)
{
	const unsigned rates[] = { 1u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned i = 0; i < ARRAY_LENGTH(rates); ++i) {
			struct v92_online_pair pair;
			const struct v92_upstream_offer offer = {
				.peer_mask = UINT32_C(1) << (rates[i] - 1u),
				.local_maximum = rates[i], .expected = rates[i],
			};
			const struct v92_jp jp = {
				.training_constellation = i ? V92_CONSTELLATION_8_POINT : V92_CONSTELLATION_4_POINT,
				.rate_renegotiation_constellation = i ? V92_CONSTELLATION_4_POINT : V92_CONSTELLATION_8_POINT,
			};
			CHECK(capture_v92_duplex_data(law, i ? 13u : 1u, false, &offer, &jp, 0u, &pair),
			      "analogue rate fixture did not train");
			CHECK(pair.analog.state.data_path.v92_analog.rate_renegotiation_constellation ==
				jp.rate_renegotiation_constellation && pair.digital.state.data_path.v92_digital.ru.constellation ==
				jp.rate_renegotiation_constellation, "rate exchange lost the Jp bit 49 constellation");
			CHECK(v92_analog_rate_prefix_contracts(&pair.analog), "analogue rate prefix failed");
			CHECK(v92_analog_remote_rd(&pair.analog), "remote Rd response failed");
			CHECK(v92_digital_rate_prefix_contracts(&pair.digital), "digital rate prefix failed");
			for (unsigned initiators = 1u; initiators <= 3u; ++initiators)
				CHECK(v92_call_cleardown(&pair, initiators), "PCM data cleardown failed");
			CHECK(pcm_call_start_rate_renegotiation(NULL) == PCM_CALL_INVALID_ARGUMENT,
			      "null rate request accepted");
			for (unsigned epoch = 0; epoch < 12u; ++epoch) {
				struct pcm_call *calls[] = { &pair.analog, &pair.digital };
				struct pcm_payload_source source[2] = { { .reject = true }, { .reject = true } };
				uint32_t frames[2] = { 0 };
				bool entered[2] = { false, false };
				bool paused[2] = { false, false }, resumed[2] = { false, false };
				bool pause_checked = false;
				unsigned e2u_checked = 0u;
				unsigned requests = epoch / 3u;
				bool lose_ack = requests && epoch % 3u == 2u;
				unsigned damaged = 0u;
				bool e2u_confirmed = false;
				pair.analog.v92_analog_phase4_configuration.suvu.request_silence = (requests & 1u) != 0u;
				pair.digital.v92_digital_phase4_configuration.suvd.request_silence = (requests & 2u) != 0u;
				pair.digital.pcm_upstream_quality.automatic_retrains = V92_DATA_QUALITY_MAX_RETRAINS;
				unsigned initiator = epoch % 3u == 1u ? 1u : 0u;
				CHECK(pcm_call_start_rate_renegotiation(calls[initiator]) == PCM_CALL_OK &&
				      (epoch % 3u != 2u || pcm_call_start_rate_renegotiation(&pair.digital) == PCM_CALL_OK),
				      "rate exchange did not start at epoch %u", epoch);
				for (unsigned clock = 0; clock < 40000u && (frames[0] < 200u || frames[1] < 200u); ++clock) {
					uint8_t received[2], transmitted[2];
					channel_receive(&pair.wire, &received[0], &received[1]);
					for (unsigned role = 0; role < 2u; ++role) {
						struct pcm_call *call = calls[role];
						bool was_control = call->stage == PCM_CALL_PHASE4 &&
							call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL;
						if (pcm_call_phase4_is_ready(call)) {
							CHECK(!entered[role] && pcm_call_start_data_path(call) == PCM_CALL_OK,
							      "rate exchange data handoff failed");
							entered[role] = true;
						}
						if (call->stage == PCM_CALL_DATA_PATH && entered[role]) {
							CHECK(v92_payload_sample(call, received[role], decode_octet(law, received[role]),
								&source[role], &frames[role], &transmitted[role]), "post-rate payload failed");
						} else if (call->stage == PCM_CALL_DATA_PATH) {
							struct pcm_call_data_event event;
							CHECK(pcm_call_data_octet(call, received[role], decode_octet(law, received[role]), true,
								NULL, &transmitted[role], &event) == PCM_CALL_OK, "peer Ru detection failed");
						} else {
							struct pcm_call_observation observation;
							enum pcm_call_result result = pcm_call_octet(call, received[role],
								decode_octet(law, received[role]), &transmitted[role], &observation);
							CHECK(result == PCM_CALL_OK && call->stage == PCM_CALL_PHASE4,
							      "law %u rate %u epoch %u role %u clock %u rate control failed: %s stage %u",
							      law, rates[i], epoch, role, clock, pcm_call_result_name(result), call->stage);
						}
						CHECK(pcm_call_is_valid(call), "rate exchange state invalid");
						if (lose_ack && role == 0u && !resumed[role] && call->stage == PCM_CALL_PHASE4) {
							const struct v92_phase4_record_tx *record = NULL;
							struct pcm_call_v92_phase4_state *state = &call->state.phase4.v92;
							if (state->child == PCM_CALL_V92_PHASE4_CONTROL &&
							    state->owner.analog.station.transmit.stage == V92_PHASE4_ANALOG_TX_SUVU)
								record = &state->owner.analog.station.transmit.signal.record;
							else if (state->child == PCM_CALL_V92_PHASE4_E2U && state->owner.e2u.state.analog.finishing_control)
								record = &state->owner.e2u.state.analog.transmit;
							struct v92_suvu_parameters status;
							if (record && record->bit_index == 42u &&
							    v92_suvu_decode(record->frame, sizeof(record->frame), record->frame_bit_count,
								12u * v92_phase4_modulator_bits_per_symbol(record->modulator.constellation), &status) == V92_SUVU_OK &&
							    status.acknowledges_cpd) {
								/* One on-wire sign error invalidates each pause ACK's CRC;
								 * neither endpoint's coder or decoded event is altered. */
								transmitted[role] ^= 0x80u;
								damaged++;
							}
						}
						if (call->stage == PCM_CALL_PHASE4 && call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_PAUSE) {
							CHECK(requests && !pcm_call_phase4_is_ready(call) && !pcm_call_is_online(call),
							      "timed pause authorized data or appeared without a request");
							if (!paused[role]) {
								struct pcm_call invalid = *call;
								invalid.state.phase4.v92.peer_requested_silence ^= true;
								CHECK(!pcm_call_is_valid(&invalid), "pause changed the retained request owner");
								invalid = *call;
								if (role)
									invalid.state.phase4.v92.owner.digital_pause.mapping.law ^= 1u;
								else
									invalid.state.phase4.v92.owner.analog_pause.mapping.law ^= 1u;
								struct pcm_call before = invalid;
								struct pcm_call_observation observation = { 0 }, saved = observation;
								uint8_t octet = 0x5a;
								CHECK(!pcm_call_is_valid(&invalid) && pcm_call_octet(&invalid, 0, 0, &octet, &observation) ==
									PCM_CALL_INVALID_ARGUMENT && octet == 0x5a && memcmp(&observation, &saved, sizeof(saved)) == 0 &&
								      memcmp(&invalid, &before, sizeof(before)) == 0, "invalid pause consumed a line interval");
							}
							paused[role] = true;
							e2u_confirmed |= role == 1u && was_control;
						} else if (paused[role] && !resumed[role] && call->stage == PCM_CALL_PHASE4 &&
							   call->state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL) {
							CHECK(role ? !call->state.phase4.v92.owner.digital.control.request_silence &&
								!call->state.phase4.v92.owner.digital.control.cpu_received :
								!call->state.phase4.v92.owner.analog.station.transmit.control.suvu.request_silence &&
								!call->state.phase4.v92.owner.analog.station.cpd_received,
							      "pause reused old requests or parameter acknowledgements");
							resumed[role] = true;
						}
					}
					channel_transmit(&pair.wire, transmitted[0], transmitted[1]);
					if (requests && epoch % 3u == 0u) {
						for (unsigned role = 0; role < 2u; ++role) {
							if (!(e2u_checked & (1u << role)) && calls[role]->stage == PCM_CALL_PHASE4 &&
							    calls[role]->state.phase4.v92.child == PCM_CALL_V92_PHASE4_E2U) {
								CHECK(v92_call_cleardown(&pair, 1u << role), "E2u pause cleardown failed");
								e2u_checked |= 1u << role;
							}
						}
						if (!pause_checked && calls[0]->stage == PCM_CALL_PHASE4 && calls[1]->stage == PCM_CALL_PHASE4 &&
						    calls[0]->state.phase4.v92.child == PCM_CALL_V92_PHASE4_PAUSE &&
						    calls[1]->state.phase4.v92.child == PCM_CALL_V92_PHASE4_PAUSE) {
							for (unsigned mask = 1u; mask <= 3u; ++mask)
								CHECK(v92_call_cleardown(&pair, mask), "timed pause cleardown failed");
							for (unsigned role = 0; role < 2u; ++role) {
								struct pcm_call stalled = *calls[role];
								struct pcm_call_observation observation;
								uint8_t octet;
								stalled.rate_renegotiation_deadline_remaining_samples = 1u;
								CHECK(pcm_call_octet(&stalled, silence_octet(law), 0, &octet, &observation) == PCM_CALL_OK &&
								      stalled.stage == PCM_CALL_RETRAIN_SILENCE && !stalled.failed && pcm_call_is_valid(&stalled),
								      "unanswered timed pause did not reach bounded retrain");
							}
							pause_checked = true;
						}
					}
				}
				CHECK(paused[0] == (requests != 0u) && paused[1] == (requests != 0u) &&
				      (!lose_ack || (damaged && e2u_confirmed)) &&
				      (!requests || epoch % 3u || (pause_checked && e2u_checked == 3u)) &&
				      resumed[0] == paused[0] && resumed[1] == paused[1] &&
				      frames[0] >= 200u && frames[1] >= 200u &&
				      pcm_call_is_online(&pair.analog) && pcm_call_is_online(&pair.digital) &&
				      !pair.analog.v92_rate_renegotiation_active && !pair.digital.v92_rate_renegotiation_active &&
				      !pair.analog.rate_renegotiation_deadline_armed && !pair.digital.rate_renegotiation_deadline_armed &&
				      pair.digital.pcm_upstream_quality.automatic_retrains == V92_DATA_QUALITY_MAX_RETRAINS,
				      "rate exchange did not return to ordered duplex data");
			}
		}
	return true;
}

static bool v92_collision_wire(struct pcm_call_v92_fast_exchange *fast,
	uint8_t *octets, unsigned *count)
{
	*count = 0;
	if (fast->stage == PCM_CALL_V92_FAST_RF) {
		do {
			struct v90_pcm_symbol symbol;
			CHECK(pcm_r_tx_next(&fast->rf, &symbol) == PCM_R_TX_OK, "expected Rf tail failed");
			octets[(*count)++] = symbol.octet;
		} while (fast->rf.transmitted_symbols % V90_CP_FRAME_INTERVALS);
	} else {
		CHECK(v92_downstream_control_transmit(&fast->control, octets), "expected control interval failed");
		*count = 1;
		for (unsigned i = 0; i < fast->control.transmit.queue_count; ++i)
			octets[(*count)++] = fast->control.transmit.queue[i].pcm.octet;
	}
	struct pcm_r_tx rd;
	CHECK(pcm_r_tx_init(&rd, &fast->rf.mapping, PCM_R_RD) == PCM_R_TX_OK, "expected Rd setup failed");
	for (unsigned i = 0; i < PCM_R_SEQUENCE_SYMBOLS; ++i) {
		struct v90_pcm_symbol symbol;
		CHECK(pcm_r_tx_next(&rd, &symbol) == PCM_R_TX_OK, "expected Rd failed");
		octets[(*count)++] = symbol.octet;
	}
	return true;
}

static bool run_v92_fast_rate_pair(struct v92_online_pair *pair, unsigned fast_role, int offset, unsigned *coverage)
{
	struct pcm_call *calls[] = { &pair->analog, &pair->digital };
	struct pcm_payload_source source[2] = { { .reject = true }, { .reject = true } };
	uint32_t frames[2] = { 0 };
	bool entered[2] = { false, false }, rate_seen[2] = { false, false };
	uint8_t expected[V90_CP_FRAME_INTERVALS + PCM_R_SEQUENCE_SYMBOLS];
	unsigned expected_count = 0, checked = 0;
	pair->digital.pcm_upstream_quality.automatic_retrains = V92_DATA_QUALITY_MAX_RETRAINS;
	unsigned fast_start = offset < 0 ? (unsigned)-offset : 0u;
	unsigned rate_start = offset > 0 ? (unsigned)offset : 0u;
	for (unsigned clock = 0; clock < 40000u; ++clock) {
		if (clock == fast_start)
			CHECK(pcm_call_start_fast_exchange(calls[fast_role]) == PCM_CALL_OK, "collision fast start failed");
		if (clock == rate_start)
			CHECK(pcm_call_start_rate_renegotiation(calls[1u - fast_role]) == PCM_CALL_OK,
			      "collision rate start failed: fast role %u offset %d", fast_role, offset);
		uint8_t received[2], transmitted[2];
		channel_receive(&pair->wire, &received[0], &received[1]);
		for (unsigned role = 0; role < 2u; ++role) {
			struct pcm_call *call = calls[role];
			bool audit = role == 1u && call->stage == PCM_CALL_DATA_PATH && !entered[role] &&
				(call->state.data_path.v92_digital.fast.stage == PCM_CALL_V92_FAST_RF ||
				 call->state.data_path.v92_digital.fast.stage == PCM_CALL_V92_FAST_CONTROL) &&
				call->state.data_path.v92_digital.ru.signal.run == V92_RU_RX_MIN_SYMBOLS;
			struct pcm_call_v92_fast_exchange before;
			if (audit)
				before = call->state.data_path.v92_digital.fast;
			if (pcm_call_phase4_is_ready(call)) {
				CHECK(!entered[role] && pcm_call_start_data_path(call) == PCM_CALL_OK, "collision data entry failed");
				entered[role] = true;
			}
			if (call->stage == PCM_CALL_DATA_PATH && entered[role]) {
				CHECK(v92_payload_sample(call, received[role], decode_octet(call->law, received[role]),
					&source[role], &frames[role], &transmitted[role]), "collision payload failed");
			} else if (call->stage == PCM_CALL_DATA_PATH) {
				struct pcm_call_data_event event;
				CHECK(pcm_call_data_octet(call, received[role], decode_octet(call->law, received[role]),
					true, NULL, &transmitted[role], &event) == PCM_CALL_OK, "collision data clock failed");
				if (clock >= fast_start && clock >= rate_start)
					CHECK(event.kind == PCM_CALL_DATA_NONE, "collision exposed control as payload");
			} else {
				struct pcm_call_observation observation;
				CHECK(pcm_call_octet(call, received[role], decode_octet(call->law, received[role]),
					&transmitted[role], &observation) == PCM_CALL_OK, "collision control clock failed");
			}
			if (audit && call->stage == PCM_CALL_PHASE4) {
				CHECK(!expected_count && call->state.phase4.v92.owner.ru.receive.signal.bar_symbols == 1u,
				      "collision did not wait for the first reversed Ru symbol");
				*coverage |= 1u << (before.stage - PCM_CALL_V92_FAST_RF);
				CHECK(v92_collision_wire(&before, expected, &expected_count), "collision wire setup failed");
				CHECK(call->state.phase4.v92.prefix_queue_count == expected_count - PCM_R_SEQUENCE_SYMBOLS - 1u,
				      "collision lost the partial transmit frame");
				struct pcm_call invalid = *call;
				invalid.state.phase4.v92.prefix_queue_count = V90_CP_FRAME_INTERVALS;
				CHECK(!pcm_call_is_valid(&invalid), "oversized collision tail accepted");
				invalid = *call;
				invalid.state.phase4.v92.prefix_queue[V90_CP_FRAME_INTERVALS - 1u] = 1;
				CHECK(!pcm_call_is_valid(&invalid), "unowned collision tail accepted");
			}
			if (role == 1u && checked < expected_count) {
				CHECK(transmitted[role] == expected[checked], "collision tail/Rd wire changed at interval %u", checked);
				checked++;
			}
			rate_seen[role] |= call->v92_rate_renegotiation_active;
			CHECK(pcm_call_is_valid(call) && (call->stage == PCM_CALL_PHASE4 || call->stage == PCM_CALL_DATA_PATH),
			      "fast/rate collision retrained: fast role %u offset %d role %u clock %u stage %u",
			      fast_role, offset, role, clock, call->stage);
		}
		channel_transmit(&pair->wire, transmitted[0], transmitted[1]);
		if (frames[0] >= 20u && frames[1] >= 20u) {
			CHECK((!fast_role || (expected_count && checked == expected_count)) &&
			      rate_seen[0] && rate_seen[1] && pcm_call_is_online(calls[0]) && pcm_call_is_online(calls[1]) &&
			      !calls[0]->retrain_active && !calls[1]->retrain_active &&
			      !calls[0]->rate_renegotiation_deadline_armed && !calls[1]->rate_renegotiation_deadline_armed &&
			      pair->digital.pcm_upstream_quality.automatic_retrains == V92_DATA_QUALITY_MAX_RETRAINS,
			      "collision did not complete rate exchange");
			return true;
		}
	}
	CHECK(false, "fast/rate collision did not resume duplex data");
}

static bool test_v92_fast_rate_collision(void)
{
	const int offsets[] = { -200, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 100, 200 };
	const unsigned rates[] = { 1u, 19u };
	unsigned coverage = 0;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
	for (unsigned r = 0; r < ARRAY_LENGTH(rates); ++r) {
		struct v92_online_pair pair;
		const struct v92_upstream_offer offer = {
			.peer_mask = 1u << (rates[r] - 1u), .local_maximum = rates[r], .expected = rates[r],
		};
		CHECK(capture_v92_duplex_data(law, r ? 13u : 1u, false, &offer, NULL, 0u, &pair), "collision pair did not train");
		for (unsigned role = 0; role < 2u; ++role)
			for (unsigned i = 0; i < ARRAY_LENGTH(offsets); ++i) {
				struct v92_online_pair trial = pair;
				CHECK(run_v92_fast_rate_pair(&trial, role, offsets[i], &coverage), "collision law %u failed", law);
			}
	}
	CHECK(coverage == 3u, "collision did not cover both Rf and SUVd output tails");
	return true;
}

static bool test_v92_upstream_negotiation(void)
{
	static const struct v92_upstream_offer offers[] = {
		/* Recorded peer mask: 24k through 28k, against 48k and 36k caps. */
		{ UINT32_C(0xf), 19u, 4u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(0xf), 10u, 4u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		/* Holes below and rates above our cap are not usable alternatives. */
		{ UINT32_C(0x40009), 3u, 1u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		/* Selecting 40k must also rebuild the mapping for the 32-state trellis. */
		{ UINT32_C(1) << 12u, 19u, 13u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		/* Dense maps must use the trained LU, not just normalize B1u power. */
		{ UINT32_C(1) << 18u, 19u, 19u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		/* Gain alone must retain the quantizer-aware direct selection and
		 * learned receive FIR, including the dense maximum-rate mapping. */
		{ UINT32_C(1) << 18u, 19u, 19u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1) << 18u, 19u, 19u, 0.0, 0.5, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1), 1u, 1u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		/* Balanced 44k must now carry data at the ordinary trained level,
		 * without falling back to the peer's lower 41 1/3 kbit/s offer. */
		{ (UINT32_C(1) << 15u) | (UINT32_C(1) << 13u), 16u, 16u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1) << 15u, 16u, 16u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		/* The neighboring 42 2/3k and 45 1/3k direct offers must not be
		 * skipped because the initial template concentrates trellis points. */
		{ (UINT32_C(1) << 14u) | (UINT32_C(1) << 13u), 15u, 15u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1) << 14u, 15u, 15u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		{ (UINT32_C(1) << 16u) | (UINT32_C(1) << 15u), 17u, 17u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1) << 16u, 17u, 17u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		/* Exercise the 70-bit balanced mapping through real CPd exchange,
		 * training and duplex data, including post-codec receive gain. */
		{ (UINT32_C(1) << 17u) | (UINT32_C(1) << 15u), 18u, 18u, 0.0, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1) << 17u, 18u, 18u, 0.0, 0.75, 0u, 0u, 0u, 0u, 0.0 },
		/* A real channel-memory boundary must select and use the filters,
		 * not pass only because both coders share an identity mapping. */
		{ UINT32_C(1), 1u, 1u, 0.10, 0.0, 0u, 0u, 0u, 0u, 0.0 },
		{ UINT32_C(1), 1u, 1u, 0.10, 0.75, 0u, 0u, 0u, 0u, 0.0 },
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t index = 0u; index < ARRAY_LENGTH(offers); ++index)
			CHECK(run_v92_duplex_data(law, 3u, false, &offers[index]),
			      "V.92 upstream selection law %u case %zu failed", law, index);
	/* Both trellis families through the complete compensated CPd/B1u/data
	 * handoff, with amplitude-dependent noise from tandem quantization. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 10u; rate <= 13u; rate += 3u) {
			const struct v92_upstream_offer compensated = {
				UINT32_C(1) << (rate - 1u), 19u, rate, 0.10, 0.75, 0u, 0u, 0u, 0u, 0.0,
			};

			CHECK(run_v92_duplex_data(law, 3u, false, &compensated),
			      "V.92 compensated law %u rate %u call failed", law, rate);
		}
	}
	return true;
}

static bool test_v92_b1_rate_recovery(void)
{
	/* First prove that codec noise alone need not force the old 48k->40k
	 * downshift. Then replace held-out B1u with silence in selected epochs.
	 * Re-enter the complete on-wire negotiation, skip holes in Ja, rebuild
	 * CPd for the 40k trellis and verify 2000 exact frames in each direction. */
	const struct v92_upstream_offer clean = {
		(UINT32_C(1) << 18u) | (UINT32_C(1) << 12u), 19u, 19u, 0.10, 0.75, 0u, 0u, 0u, 0u, 0.0,
	};
	CHECK(run_v92_duplex_data(G711_LAW_PCMU, 3u, false, &clean),
	      "V.92 lowered the rate on a usable two-codec channel");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned retries = 1u; retries <= 2u; ++retries) {
			const struct v92_upstream_offer offer = {
				(UINT32_C(1) << 18u) | (UINT32_C(1) << 12u) |
					(retries == 2u ? UINT32_C(1) << 17u : 0u),
				19u, 13u, 0.10, 0.75, retries, 0u, 0u, 0u, 0.0,
			};

			CHECK(capture_v92_duplex_data(law, 3u, false, &offer, NULL, retries, NULL),
			      "V.92 B1 recovery law %u retries %u failed", law, retries);
		}
		const struct v92_upstream_offer exhausted = {
			UINT32_C(1) << 18u, 19u, 0u, 0.10, 0.75, 1u, 0u, 0u, 0u, 0.0,
		};

		CHECK(capture_v92_duplex_data(law, 3u, false, &exhausted, NULL, 1u, NULL),
		      "V.92 retried a failed rate or selected a disabled lower rate");
	}
	return true;
}

static bool run_v92_excess_lookahead(enum g711_law law, uint8_t maximum, bool cpt)
{
	struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2];
	struct channel wire;
	bool injected = false;

	dc.phase3.v92_digital.jd.maximum_spectral_shaping_lookahead = maximum;
	CHECK(pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
	      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK && channel_init(&wire, law, 3u),
	      "V.92 excess lookahead fixture failed");
	for (size_t clock = 0; clock < CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t rx[2], tx[2];

		/* Change only the peer's request, before normal encoding and CRC.
		 * Exercise CPt and CPu separately; a bad CPt must not hide CPu. */
		if (!injected && cpt && calls[0].stage == PCM_CALL_PHASE3 &&
		    calls[0].state.phase3.v92_analog.transmit_stage == V92_PHASE3_ANALOG_TRANSMIT_SU) {
			calls[0].state.phase3.v92_analog.cpt_parameters.spectral_lookahead = maximum + 1u;
			injected = true;
		}
		if (!injected && !cpt && calls[0].stage == PCM_CALL_PHASE4 &&
		    calls[0].state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL) {
			calls[0].state.phase4.v92.owner.analog.station.transmit.control.cpu
				.spectral_lookahead_frames = maximum + 1u;
			injected = true;
		}
		channel_receive(&wire, &rx[0], &rx[1]);
		for (unsigned side = 0; side < 2; ++side) {
			struct pcm_call_observation observation;
			enum pcm_call_result result = pcm_call_octet(&calls[side], rx[side],
				decode_octet(law, rx[side]), &tx[side], &observation);

			if (side == 1 && result == PCM_CALL_UNUSABLE_SELECTION) {
				CHECK(injected && calls[1].failed && pcm_call_is_valid(&calls[1]) &&
				      !pcm_call_is_online(&calls[1]) &&
				      calls[1].phase3_configuration.v92_digital.jd
					      .maximum_spectral_shaping_lookahead == maximum,
				      "excess lookahead did not preserve a failed, bounded call");
				if (cpt) {
					CHECK(calls[1].stage == PCM_CALL_PHASE3 &&
					      calls[1].state.phase3.v92_digital.receive_stage ==
						      V92_PHASE3_DIGITAL_RECEIVE_CPT &&
					      !calls[1].state.phase3.v92_digital.ri.finish_requested,
					      "excess CPt lookahead was acknowledged before rejection");
				} else {
					CHECK(calls[1].stage == PCM_CALL_PHASE4 &&
					      calls[1].state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL &&
					      calls[1].state.phase4.v92.owner.digital.control.cpu_received &&
					      calls[1].state.phase4.v92.owner.digital.control.cpu
						      .spectral_lookahead_frames == maximum + 1u &&
					      !calls[1].state.phase4.v92.owner.digital.control.transmit_acknowledges_cpu,
					      "excess CPu lookahead was acknowledged before rejection");
				}
				return true;
			}
			CHECK(result == PCM_CALL_OK, "law %u CP%c side %u failed at %zu: %s",
			      law, cpt ? 't' : 'u', side, clock, pcm_call_result_name(result));
		}
		channel_transmit(&wire, tx[0], tx[1]);
		CHECK(!cpt || calls[1].stage != PCM_CALL_PHASE3_MIXED,
		      "law %u digital accepted CPt lookahead %u above Jd %u", law, maximum + 1u, maximum);
		CHECK(cpt || calls[1].stage != PCM_CALL_PHASE4 ||
		      calls[1].state.phase4.v92.child != PCM_CALL_V92_PHASE4_CONTROL ||
		      !calls[1].state.phase4.v92.owner.digital.control.cpu_received,
		      "law %u digital accepted CPu lookahead %u above Jd %u", law, maximum + 1u, maximum);
	}
	CHECK(false, "law %u CP%c excess lookahead never arrived", law, cpt ? 't' : 'u');
}

static bool test_v92_received_lookahead(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t maximum = 1u; maximum <= 2u; ++maximum)
			for (unsigned kind = 0; kind < 2u; ++kind)
				CHECK(run_v92_excess_lookahead(law, maximum, kind == 0u),
				      "V.92 excess lookahead was not rejected");
	return true;
}

static bool run_v92_downstream_reselection(bool generated, uint8_t power_code)
{
	static const uint32_t peer_masks[] = {
		(UINT32_C(1) << 4u) | (UINT32_C(1) << 9u) | (UINT32_C(1) << 18u),
		UINT32_C(1) | (UINT32_C(1) << 4u) | (UINT32_C(1) << 18u),
		V92_JD_RATE_CAPABILITY_MASK,
	};
	static const uint8_t expected_rates[] = { 10u, 1u, 11u };
	static const uint8_t peer_lookaheads[] = { 1u, 3u, 2u };
	const uint32_t peer_rms = power_code == 23u ? 4024u : 2540u;

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
		struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
		struct pcm_call calls[2];
		struct channel line;

		if (generated) {
			CHECK(v92_profile_init(&ac, V90_ANALOG_MODEM, law, 28000, 56000,
					      14u, NULL) == V92_PROFILE_OK,
			      "generated downstream-reselection profile failed");
			ac.v92_analog_phase4.cpu.data_rate_number = 11u;
			/* Exercise reduction of an over-power generated training seed as
			 * well as preservation of the native sparse CPt at the default limit. */
			if (power_code == 31u)
				ac.phase3.v92_analog.cpt.transmit_constellations[0] =
					ac.analog_phase4.training_parameters.transmit_constellations[0];
			dc.local_info0.v92_digital.v90.maximum_transmit_power = power_code;
			dc.local_info0.v92_digital.v90.measure_power_at_codec_output = power_code == 23u;
		}
		/* Keep holes below the configured CPu ceiling and a common enabled
		 * rate above it. Neither may replace the highest usable selection. */
		ac.enabled_downstream_rate_mask = UINT32_C(1) | (UINT32_C(1) << 9u) |
			(UINT32_C(1) << 10u) | (UINT32_C(1) << 18u);
		ac.analog_phase4.data_parameters.selected_rate_index = 11u;
		ac.phase3.v92_analog.cpt.spectral_lookahead = 3u;
		dc.enabled_downstream_rate_mask = peer_masks[0];
		dc.phase3.digital.jd.data_rate_capability_mask = peer_masks[0];
		dc.phase3.v92_digital.jd.data_rate_capability_mask = peer_masks[0];
		dc.phase3.v92_digital.jd.maximum_spectral_shaping_lookahead = peer_lookaheads[0];
		CHECK(pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
		      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK &&
		      channel_init(&line, law, 3u), "downstream-reselection setup failed");
		for (size_t epoch = 0u; epoch < ARRAY_LENGTH(peer_masks); ++epoch) {
			struct pcm_payload_source sources[2] = { { .reject = true }, { .reject = true } };
			uint32_t frames[2] = { 0u, 0u };
			bool data_started[2] = { false, false };
			bool cpt_checked = false;
			struct v90_pcm_mapping accepted_training;
			uint32_t at, ar, dt, dr;
			struct v92_cpu_parameters expected = ac.v92_analog_phase4.cpu;
			const struct v92_cpu_parameters *received;
			uint8_t expected_wire[V92_CPU_MAX_FRAME_BYTES], received_wire[V92_CPU_MAX_FRAME_BYTES];
			size_t expected_bits, received_bits;

			for (size_t clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
				uint8_t rx[2], tx[2];

				channel_receive(&line, &rx[0], &rx[1]);
				for (size_t side = 0u; side < 2u; ++side) {
					struct pcm_call *call = &calls[side];
					struct pcm_call_observation observation;

					if (pcm_call_phase4_is_ready(call)) {
						CHECK(pcm_call_start_data_path(call) == PCM_CALL_OK,
						      "downstream-reselection data handoff failed");
						data_started[side] = true;
					}
					if (call->stage == PCM_CALL_DATA_PATH && data_started[side])
						CHECK(v92_payload_sample(call, rx[side], decode_octet(law, rx[side]), &sources[side],
									&frames[side], &tx[side]),
						      "law %u epoch %zu side %zu data failed", law, epoch, side);
					else if (call->stage == PCM_CALL_DATA_PATH) {
						struct pcm_call_data_event event;

						/* Drain the old call until it detects the peer's retrain;
						 * this epoch's payload starts at the new data handoff. */
						CHECK(pcm_call_data_octet(call, rx[side], decode_octet(law, rx[side]), true,
									 NULL, &tx[side], &event) == PCM_CALL_OK,
						      "downstream-reselection retrain detection failed");
					} else
						CHECK(pcm_call_octet(call, rx[side], decode_octet(law, rx[side]),
								     &tx[side], &observation) == PCM_CALL_OK,
						      "law %u epoch %zu side %zu training failed at %zu",
						      law, epoch, side, clock);
				}
				channel_transmit(&line, tx[0], tx[1]);
				if (!cpt_checked && calls[1].stage == PCM_CALL_PHASE3_MIXED) {
					struct v92_cpt_parameters cpt;

					if (v92_phase3_digital_station_downstream_handoff(
						    &calls[1].state.mixed.v92_digital.phase3, &cpt) ==
					    V92_PHASE3_DIGITAL_STATION_OK) {
						struct v92_cpt_parameters expected_cpt = ac.phase3.v92_analog.cpt;
						uint8_t sent[V92_CPT_MAX_FRAME_BYTES], got[V92_CPT_MAX_FRAME_BYTES];
						size_t sent_bits, got_bits;

						expected_cpt.spectral_lookahead = peer_lookaheads[epoch];
						if (generated) {
							uint32_t lower, upper;

							CHECK(v90_pcm_mapping_init_from_v92_cpt(&accepted_training, &cpt) ==
								      V90_PCM_MAPPING_OK &&
							      v90_pcm_mapping_power(&accepted_training, V90_PCM_MAPPING_CODEC, law,
										    &lower, &upper) == V90_PCM_MAPPING_OK &&
							      upper <= peer_rms * peer_rms &&
							      accepted_training.modulus[0] ==
								      pcm_constellation_count(&expected_cpt.transmit_constellations[0]),
							      "generated CPt exceeded peer power or lost capacity");
							if (power_code == 31u)
								expected_cpt.transmit_constellations[0] = cpt.transmit_constellations[0];
						}
						CHECK(v92_cpt_encode(&expected_cpt, sent, sizeof(sent), &sent_bits) == V92_CPT_OK &&
						      v92_cpt_encode(&cpt, got, sizeof(got), &got_bits) == V92_CPT_OK &&
						      sent_bits == got_bits && memcmp(sent, got, (sent_bits + 7u) / 8u) == 0,
						      "law %u epoch %zu CPt did not retain its map with peer lookahead %u",
						      law, epoch, peer_lookaheads[epoch]);
						cpt_checked = true;
					}
				}
				if (frames[0] >= 200u && frames[1] >= 200u)
					break;
			}
			CHECK(cpt_checked && frames[0] >= 200u && frames[1] >= 200u &&
			      sources[0].rejection_checked && sources[1].rejection_checked &&
			      pcm_call_is_online(&calls[0]) && pcm_call_is_online(&calls[1]) &&
			      !calls[0].retrain_active && !calls[1].retrain_active &&
			      pcm_call_get_data_rates(&calls[0], &at, &ar) &&
			      pcm_call_get_data_rates(&calls[1], &dt, &dr) && at == dr && ar == dt &&
			      ar == ((expected_rates[epoch] + 20u) * 8000u + 3u) / 6u,
			      "law %u epoch %zu did not exchange data at the common downstream rate", law, epoch);
			received = &calls[1].state.data_path.v92_digital.cpu;
			expected.data_rate_number = expected_rates[epoch];
			if (expected.spectral_lookahead_frames > peer_lookaheads[epoch])
				expected.spectral_lookahead_frames = peer_lookaheads[epoch];
			expected.acknowledges_cpd = received->acknowledges_cpd;
			if (generated) {
				struct v90_pcm_mapping data, original;
				uint32_t tf = 0, tc = 0, df = 0, dc_power = 0;
				enum v90_pcm_mapping_plane plane = power_code == 31u ?
					V90_PCM_MAPPING_TRANSMIT : V90_PCM_MAPPING_CODEC;

				CHECK(v90_pcm_mapping_init_from_v92_cpu(&data, received) == V90_PCM_MAPPING_OK &&
				      v90_pcm_mapping_init_from_v92_cpu(&original, &expected) == V90_PCM_MAPPING_OK &&
				      v90_pcm_mapping_power(&accepted_training, plane, law, &tf, &tc) == V90_PCM_MAPPING_OK &&
				      v90_pcm_mapping_power(&data, plane, law, &df, &dc_power) == V90_PCM_MAPPING_OK &&
				      tc <= peer_rms * peer_rms &&
				      dc_power <= peer_rms * peer_rms &&
				      dc_power <= tf * pow(10.0, 0.3) &&
				      data.modulus[0] <= original.modulus[0],
				      "law %u epoch %zu generated CPu power %u, CPt %u, limit %u, or capacity grew",
				      law, epoch, dc_power, tf, peer_rms * peer_rms);
				expected.transmit_constellations[0] = received->transmit_constellations[0];
			}
			CHECK(v92_cpu_encode(&expected, 24u, expected_wire, sizeof(expected_wire),
					     &expected_bits) == V92_CPU_OK &&
			      v92_cpu_encode(received, 24u, received_wire, sizeof(received_wire),
					     &received_bits) == V92_CPU_OK &&
			      expected_bits == received_bits &&
			      memcmp(expected_wire, received_wire, (expected_bits + 7u) / 8u) == 0 &&
			      memcmp(&calls[0].v92_analog_phase4_configuration, &ac.v92_analog_phase4,
				     sizeof(ac.v92_analog_phase4)) == 0 &&
			      calls[0].phase3_configuration.v92_analog.cpt.spectral_lookahead == 3u,
			      "downstream reselection changed constellation fields or configured preferences");
			struct pcm_call invalid = calls[0];

			invalid.v92_peer_jd.maximum_spectral_shaping_lookahead = 0u;
			CHECK(!pcm_call_is_valid(&invalid), "retained Jd accepted zero lookahead");
			invalid.v92_peer_jd.maximum_spectral_shaping_lookahead = 4u;
			CHECK(!pcm_call_is_valid(&invalid), "retained Jd accepted unsupported lookahead");
			invalid = calls[1];
			invalid.v92_peer_jd.maximum_spectral_shaping_lookahead = 1u;
			CHECK(!pcm_call_is_valid(&invalid), "unreceived Jd retained a lookahead value");
			if (epoch + 1u < ARRAY_LENGTH(peer_masks)) {
				calls[1].enabled_downstream_rate_mask = peer_masks[epoch + 1u];
				calls[1].phase3_configuration.digital.jd.data_rate_capability_mask = peer_masks[epoch + 1u];
				calls[1].phase3_configuration.v92_digital.jd.data_rate_capability_mask = peer_masks[epoch + 1u];
				calls[1].phase3_configuration.v92_digital.jd.maximum_spectral_shaping_lookahead =
					peer_lookaheads[epoch + 1u];
				CHECK(pcm_call_start_retrain(&calls[1u - epoch]) == PCM_CALL_OK,
				      "downstream-reselection retrain failed");
			}
		}
	}
	return true;
}

static bool test_v92_downstream_reselection(void)
{
	return run_v92_downstream_reselection(false, 23u) &&
	       run_v92_downstream_reselection(true, 23u) &&
	       run_v92_downstream_reselection(true, 31u);
}

static bool test_v92_upstream_reselection(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_call_configuration ac = v92_configuration(V90_ANALOG_MODEM, law);
		struct pcm_call_configuration dc = v92_configuration(V90_DIGITAL_MODEM, law);
		struct pcm_call calls[2];
		struct channel line;

		CHECK(pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
		      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK &&
		      channel_init(&line, law, 3u), "mode-reselection setup failed");
		/* Reuse the same live owners: PCM, conventional, then PCM again.
		 * Each retrain must discard the old child but retain V.92 INFO0. */
		for (unsigned int epoch = 0u; epoch < 3u; ++epoch) {
			bool online = false;

			for (size_t clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
				uint8_t received[2], transmitted[2];

				channel_receive(&line, &received[0], &received[1]);
				for (unsigned int side = 0u; side < 2u; ++side) {
					struct pcm_call *call = &calls[side];
					struct pcm_call_data_event data;
					struct pcm_call_observation training;

					if (pcm_call_phase4_is_ready(call))
						CHECK(pcm_call_start_data_path(call) == PCM_CALL_OK,
						      "mode-reselection data handoff failed");
					enum pcm_call_result result = call->stage == PCM_CALL_DATA_PATH ?
						pcm_call_data_octet(call, received[side],
							decode_octet(law, received[side]), true, NULL,
							&transmitted[side], &data) :
						pcm_call_octet(call, received[side],
							decode_octet(law, received[side]),
							&transmitted[side], &training);

					CHECK(result == PCM_CALL_OK && call->protocol == PCM_PROTOCOL_V92,
					      "law %u epoch %u side %u stopped at %zu: %s",
					      law, epoch, side, clock, pcm_call_result_name(result));
				}
				channel_transmit(&line, transmitted[0], transmitted[1]);
				if (pcm_call_is_online(&calls[0]) && pcm_call_is_online(&calls[1])) {
					online = true;
					break;
				}
			}
			CHECK(online && !calls[0].retrain_active && !calls[1].retrain_active &&
			      pcm_call_uses_conventional_upstream(&calls[0]) == (epoch == 1u) &&
			      pcm_call_uses_conventional_upstream(&calls[1]) == (epoch == 1u),
			      "law %u epoch %u failed to reselect upstream: online %u retrain %u/%u "
			      "stage %u/%u conventional %u/%u enabled %u/%u",
			      law, epoch, online, calls[0].retrain_active, calls[1].retrain_active,
			      calls[0].stage, calls[1].stage,
			      pcm_call_uses_conventional_upstream(&calls[0]),
			      pcm_call_uses_conventional_upstream(&calls[1]),
			      calls[0].local_info1.pcm_upstream_enabled,
			      calls[1].local_info1.v92_digital.pcm_upstream_supported);
			if (epoch < 2u) {
				calls[1].local_info1.v92_digital.pcm_upstream_supported = epoch != 0u;
				CHECK(pcm_call_start_retrain(&calls[1u - epoch]) == PCM_CALL_OK &&
				      !calls[1u - epoch].phase3_entry_stored,
				      "mode reselection retained the retired child");
			}
		}
	}
	return true;
}

enum v92_unavailable_selection {
	V92_UNAVAILABLE_DOWNSTREAM_RATE,
	V92_UNAVAILABLE_UPSTREAM_RATE,
	V92_UNAVAILABLE_DIRECT_UPSTREAM_RATE,
	V92_UNAVAILABLE_Z1,
	V92_UNAVAILABLE_P2,
	V92_UNAVAILABLE_SECTION_LENGTH,
	V92_UNAVAILABLE_TOTAL_LENGTH,
};

static bool run_v92_unavailable_selection(
	enum v92_unavailable_selection selection, bool peer_violation)
{
	const enum g711_law law = G711_LAW_PCMU;
	struct pcm_call_configuration analog_configuration =
		v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration digital_configuration =
		v92_configuration(V90_DIGITAL_MODEM, law);
	struct channel channel;
	struct pcm_call analog;
	struct pcm_call digital;
	struct v92_cpd_parameters unsupported = digital_configuration.v92_digital_phase4.cpd;
	struct v92_cpd_parameters *cpd = peer_violation ?
		&unsupported : &digital_configuration.v92_digital_phase4.cpd;
	bool downstream = selection == V92_UNAVAILABLE_DOWNSTREAM_RATE;
	bool reject_analog = downstream || peer_violation;
	bool injected = false;

	if (downstream) {
		uint32_t selected = UINT32_C(1) <<
			(analog_configuration.v92_analog_phase4.cpu
				 .data_rate_number - 1u);

		digital_configuration.enabled_downstream_rate_mask &= ~selected;
		analog_configuration.enabled_downstream_rate_mask = selected;
		analog_configuration.analog_phase4.data_parameters.selected_rate_index =
			analog_configuration.v92_analog_phase4.cpu.data_rate_number;
		digital_configuration.phase3.digital.jd
			.data_rate_capability_mask &= ~selected;
		digital_configuration.phase3.v92_digital.jd
			.data_rate_capability_mask &= ~selected;
	} else if (selection == V92_UNAVAILABLE_UPSTREAM_RATE) {
		uint32_t selected = UINT32_C(1) <<
			(digital_configuration.v92_digital_phase4.cpd
				 .data_rate_number - 1u);

		analog_configuration.phase3.v92_analog.upstream_rate_mask &=
			~selected;
	} else if (selection == V92_UNAVAILABLE_DIRECT_UPSTREAM_RATE) {
		digital_configuration.v92_digital_phase4.upstream_selection =
			PCM_CALL_V92_UPSTREAM_DIRECT;
		digital_configuration.v92_digital_phase4.direct_maximum_data_rate_number = 3u;
		analog_configuration.phase3.v92_analog.upstream_rate_mask = UINT32_C(8);
	} else {
		struct v92_cpd_filter *filter;

		switch (selection) {
		case V92_UNAVAILABLE_Z1:
			filter = &cpd->filter[V92_CPD_PRECODER_FEED_FORWARD];
			filter->count = 1u;
			filter->coefficient[0] = 32767;
			break;
		case V92_UNAVAILABLE_P2:
			filter = &cpd->filter[V92_CPD_PREFILTER_FEEDBACK];
			filter->count = 1u;
			filter->coefficient[0] = 1;
			break;
		case V92_UNAVAILABLE_SECTION_LENGTH:
			filter = &cpd->filter[V92_CPD_PRECODER_FEEDBACK];
			filter->count = 129u;
			filter->coefficient[128] = 1;
			break;
		case V92_UNAVAILABLE_TOTAL_LENGTH:
			cpd->filter[V92_CPD_PRECODER_FEEDBACK].count = 128u;
			cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 128u;
			break;
		default:
			CHECK(false, "invalid V.92 unavailable-selection fixture");
		}
	}
	CHECK(channel_init(&channel, law, 1u) &&
	      pcm_call_init(&analog, &analog_configuration) == PCM_CALL_OK &&
	      pcm_call_init(&digital, &digital_configuration) == PCM_CALL_OK,
	      "V.92 unavailable-selection %u calls did not initialize", selection);
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
		/* A nonconforming peer may send a valid CPd outside our advertised
		 * filter limits. Replace its not-yet-started wire record so rejection
		 * exercises the analogue receive boundary, not our sender policy. */
		if (peer_violation && !injected && digital_result == PCM_CALL_OK &&
		    digital.stage == PCM_CALL_PHASE4 &&
		    digital.state.phase4.v92.child == PCM_CALL_V92_PHASE4_CONTROL) {
			struct v92_phase4_digital_station *station =
				&digital.state.phase4.v92.owner.digital;

			CHECK(station->control.transmit_stage == V92_DOWNSTREAM_CONTROL_INITIAL_SUVD &&
			      v92_phase4_cpd_tx_init(&station->control.initial_cpd, &station->control.transmit,
						    &unsupported),
			      "could not inject the unsupported peer CPd");
			injected = true;
		}
		if (analog_result != PCM_CALL_OK ||
		    digital_result != PCM_CALL_OK) {
			enum pcm_call_result expected_analog = reject_analog ?
				PCM_CALL_UNUSABLE_SELECTION : PCM_CALL_OK;
			enum pcm_call_result expected_digital = reject_analog ?
				PCM_CALL_OK : PCM_CALL_UNUSABLE_SELECTION;

			CHECK(analog_result == expected_analog &&
			      digital_result == expected_digital &&
			      pcm_call_is_valid(&analog) &&
			      pcm_call_is_valid(&digital) &&
			      pcm_call_has_failed(
				      reject_analog ? &analog : &digital) &&
			      !pcm_call_has_failed(
				      reject_analog ? &digital : &analog),
			      "V.92 accepted unavailable selection %u peer=%u: %s/%s",
			      selection, peer_violation,
			      pcm_call_result_name(analog_result),
			      pcm_call_result_name(digital_result));
			return true;
		}
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
	}
	CHECK(false, "V.92 unavailable selection %u did not terminate", selection);
	return false;
}

static bool test_v92_phase4_control(void)
{
	for (size_t delay = 1; delay <= 3; ++delay)
		CHECK(run_v92_phase4_control(G711_LAW_PCMU, delay) &&
		      run_v92_phase4_control(G711_LAW_PCMA, delay),
		      "delay %zu V.92 role-local Phase 4 handoff failed",
		      delay);
	for (enum v92_unavailable_selection selection = V92_UNAVAILABLE_DOWNSTREAM_RATE;
	     selection <= V92_UNAVAILABLE_TOTAL_LENGTH; ++selection)
		CHECK(run_v92_unavailable_selection(selection, false) &&
		      (selection < V92_UNAVAILABLE_Z1 ||
		       run_v92_unavailable_selection(selection, true)),
		      "V.92 selection %u was not rejected", selection);
	return true;
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
	invalid.v92_upstream_retry_ceiling = 1u;
	CHECK(!pcm_call_is_valid(&invalid), "V.90 accepted a PCM-upstream retry ceiling");
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
	      pcm_call_start_rate_renegotiation(&call) == PCM_CALL_INVALID_STATE &&
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
		      &call, silence_octet(G711_LAW_PCMU), 0, true, NULL,
		      &output, &data_event) == PCM_CALL_INVALID_STATE &&
	      output == 0xa5 &&
	      memcmp(&observation, &saved_observation,
		     sizeof(observation)) == 0 &&
	      memcmp(&data_event, &saved_data_event,
		     sizeof(data_event)) == 0 &&
	      memcmp(&call, &saved, sizeof(call)) == 0 &&
	      !pcm_call_is_valid(NULL) && !pcm_call_has_failed(NULL) &&
	      !pcm_call_phase4_is_ready(NULL) &&
	      !pcm_call_is_online(NULL) && !pcm_call_is_disconnecting(NULL) &&
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
	{
		static const uint32_t bad_durations[] = {
			V90_PHASE3_TRN1D_MIN_SYMBOLS - V90_PHASE3_TRN1D_ALIGNMENT,
			V90_PHASE3_TRN1D_MIN_SYMBOLS + 1u,
			V90_PHASE3_TRN1D_MAX_SYMBOLS + V90_PHASE3_TRN1D_ALIGNMENT,
		};
		for (size_t i = 0; i < ARRAY_LENGTH(bad_durations); ++i) {
			selected.phase3.v92_digital.trn1d_symbols = bad_durations[i];
			CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT &&
			      memcmp(&untouched, &before, sizeof(before)) == 0,
			      "invalid TRN1d duration changed the call");
		}
		selected.phase3.v92_digital.trn1d_symbols = V90_PHASE3_TRN1D_SYMBOLS;
		CHECK(pcm_call_init(&digital, &selected) == PCM_CALL_OK,
		      "host echo-training duration was rejected");
		selected.phase3.v92_digital.trn1d_symbols = 0u;
	}
	{
		struct pcm_call invalid = analog;

		invalid.v92_upstream_retry_ceiling = 1u;
		CHECK(!pcm_call_is_valid(&invalid), "analogue V.92 accepted a digital retry ceiling");
		invalid = digital;
		invalid.v92_upstream_retry_ceiling = 1u;
		CHECK(!pcm_call_is_valid(&invalid), "fixed CPd accepted a retry ceiling");
		struct pcm_call_configuration direct;

		CHECK(v92_profile_init(&direct, V90_DIGITAL_MODEM, G711_LAW_PCMU,
			28000u, 56000u, 19u, NULL) == V92_PROFILE_OK &&
		      pcm_call_init(&invalid, &direct) == PCM_CALL_OK &&
		      invalid.v92_upstream_retry_ceiling == 0u, "new call inherited a retry ceiling");
		invalid.v92_upstream_retry_ceiling = 19u;
		CHECK(!pcm_call_is_valid(&invalid), "retry ceiling did not lower the configured maximum");
		invalid.v92_upstream_retry_ceiling = 18u;
		CHECK(pcm_call_is_valid(&invalid) && pcm_call_init(&invalid, &direct) == PCM_CALL_OK &&
		      invalid.v92_upstream_retry_ceiling == 0u, "retry ceiling survived a new call");
	}
	selected.phase3.digital.jd.data_rate_capability_mask = 0u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted an unusable conventional Jd");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.phase3.digital.jd.data_rate_capability_mask >>= 1u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted inconsistent conventional downstream rates");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.digital_phase4.initial_mp.type = (enum v90_mp_record_type)99;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted an invalid conventional MP");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.local_info1.analog.training_ucode = 0u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted an unusable conventional INFO1a");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.phase3.analog.nominal_peak_amplitude = 0;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted an unusable conventional transmitter");
	selected = v92_configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.analog_phase4.data_parameters.law = G711_LAW_PCMA;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "V.92 call accepted a mismatched conventional CP law");
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
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
	selected.v92_digital_phase4.upstream_selection =
		(enum pcm_call_v92_upstream_selection)99;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an invalid V.92 upstream selection policy");
	selected.v92_digital_phase4.upstream_selection = PCM_CALL_V92_UPSTREAM_DIRECT;
	selected.v92_digital_phase4.direct_maximum_data_rate_number = 0u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a zero V.92 upstream rate cap");
	selected.v92_digital_phase4.direct_maximum_data_rate_number = 20u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted an out-of-range V.92 upstream rate cap");
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
	selected = v92_configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.v92_digital_phase4.cpd.modulus[0] = 1u;
	CHECK(pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a V.92 CPd with insufficient mapping capacity");
	selected.v92_digital_phase4.cpd = (struct v92_cpd_parameters){
		.four_times_gain_q0_16 = 1234u,
		.data_rate_number = 12u,
		.trellis = V92_CPD_TRELLIS_16_STATE,
	};
	CHECK(v92_cpd_parameters_are_valid(&selected.v92_digital_phase4.cpd) &&
	      pcm_call_init(&untouched, &selected) == PCM_CALL_INVALID_ARGUMENT,
	      "call accepted a table-free V.92 CPd as startup");
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
	CHECK(pcm_call_init(&digital, &selected) == PCM_CALL_OK &&
	      !digital.local_info1.v92_digital.pcm_upstream_supported &&
	      !pcm_call_uses_conventional_upstream(&digital),
	      "V.92 conventional capability was rejected or selected before INFO1");
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

static bool check_v34_restart(
	enum pcm_protocol protocol, enum g711_law law, bool remote)
{
	const struct pcm_call_configuration selected = protocol == PCM_PROTOCOL_V92 ?
		v92_configuration(V90_DIGITAL_MODEM, law) :
		configuration(V90_DIGITAL_MODEM, law);
	struct pcm_fallback_restart restart = {
		.protocol = protocol,
		.local_info0 = selected.local_info0,
		.peer_info0.analog = analog_info0(),
		.local_info1 = selected.local_info1,
		.phase3 = selected.phase3.digital,
		.v92_phase3 = selected.phase3.v92_digital,
		.phase4 = selected.digital_phase4,
		.v92_phase4 = selected.v92_digital_phase4,
		.enabled_downstream_rate_mask = selected.enabled_downstream_rate_mask,
		.round_trip_delay_samples = 800u,
		.phase2_nominal_amplitude = selected.phase2_nominal_amplitude,
		.law = law,
	};
	struct v34_full_retrain response;
	struct v34_phase2_tone_transmitter peer;
	struct pcm_call call;
	struct pcm_call before;
	bool detected = false;

	if (protocol == PCM_PROTOCOL_V92) {
		restart.peer_info0.v92_analog = (struct v92_info0a){
			.v90 = analog_info0(), .v92_capable = true,
		};
		restart.v92_phase4 = (struct pcm_call_v92_digital_phase4_configuration){
			.upstream_selection = PCM_CALL_V92_UPSTREAM_DIRECT,
			.direct_maximum_data_rate_number = 19u,
		};
		restart.upstream_retry_ceiling = 17u;
	}
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
	      call.protocol == protocol &&
	      call.enabled_downstream_rate_mask == restart.enabled_downstream_rate_mask &&
	      call.v92_upstream_retry_ceiling == restart.upstream_retry_ceiling &&
	      memcmp(&call.local_info1, &restart.local_info1,
		     sizeof(restart.local_info1)) == 0 &&
	      memcmp(&call.local_info0, &restart.local_info0,
		     sizeof(restart.local_info0)) == 0 &&
	      memcmp(&call.peer_info0, &restart.peer_info0,
		     sizeof(restart.peer_info0)) == 0,
	      "fallback lost its PCM agreement or restarted the silence");
	before = call;
	restart.law = (enum g711_law)99;
	CHECK(pcm_call_init_from_v34(&call, &restart, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &before, sizeof(call)) == 0,
	      "invalid fallback media changed the call");
	restart.law = law;
	restart.protocol = (enum pcm_protocol)99;
	CHECK(pcm_call_init_from_v34(&call, &restart, &response) ==
		      PCM_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &before, sizeof(call)) == 0,
	      "invalid fallback protocol changed the call");
	restart.protocol = protocol;
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
	CHECK(call.protocol == protocol &&
	      call.state.phase2.info0_protocol == protocol &&
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
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(check_v34_restart(protocol, law, false) &&
			      check_v34_restart(protocol, law, true),
			      "fallback restart failed for protocol %u law %u", protocol, law);
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

static bool run_info1a_training_power(
	unsigned mode, enum g711_law law, uint8_t power_code, uint16_t rms, uint8_t proposal)
{
	struct pcm_call_configuration ac = mode == 0u ?
		configuration(V90_ANALOG_MODEM, law) : v92_configuration(V90_ANALOG_MODEM, law);
	struct pcm_call_configuration dc = mode == 0u ?
		configuration(V90_DIGITAL_MODEM, law) : v92_configuration(V90_DIGITAL_MODEM, law);
	struct pcm_call calls[2];
	struct channel wire;
	uint8_t expected = 0u;
	struct v90_info0d *info0d = mode == 0u ?
		&dc.local_info0.digital : &dc.local_info0.v92_digital.v90;

	info0d->maximum_transmit_power = power_code;
	info0d->measure_power_at_codec_output = (power_code & 1u) != 0u;
	ac.local_info1.analog.training_ucode = proposal;
	if (mode != 0u) {
		ac.local_info1.v92_analog.training_ucode = proposal;
		ac.phase3.v92_analog.local_info1a = ac.local_info1.v92_analog;
		ac.local_info1.pcm_upstream_enabled = mode == 2u;
	}
	for (unsigned code = 67u; code <= proposal; ++code) {
		int16_t point;

		CHECK(g711_ucode_sample(law, (uint8_t)code, true, &point), "invalid test Ucode");
		if (point <= rms)
			expected = (uint8_t)code;
	}
	CHECK(expected != 0u && channel_init(&wire, law, 3u) &&
	      pcm_call_init(&calls[0], &ac) == PCM_CALL_OK &&
	      pcm_call_init(&calls[1], &dc) == PCM_CALL_OK, "training-power setup failed");
	for (size_t clock = 0u; clock < CALL_SAMPLE_LIMIT; ++clock) {
		uint8_t rx[2], tx[2];

		channel_receive(&wire, &rx[0], &rx[1]);
		for (size_t side = 0u; side < 2u; ++side) {
			struct pcm_call_observation event;

			CHECK(pcm_call_octet(&calls[side], rx[side], decode_octet(law, rx[side]),
				&tx[side], &event) == PCM_CALL_OK,
			      "mode %u law %u power %u INFO1 failed", mode, law, power_code);
		}
		channel_transmit(&wire, tx[0], tx[1]);
		if (calls[0].phase3_entry_stored && calls[1].phase3_entry_stored)
			break;
	}
	for (size_t side = 0u; side < 2u; ++side) {
		const struct pcm_info1_selection *selection = &calls[side].phase3_entry.selection;
		uint8_t actual = mode == 2u ? selection->analog.pcm.training_ucode :
			selection->analog.conventional.training_ucode;

		CHECK(calls[side].phase3_entry_stored && actual == expected &&
		      (selection->upstream == PCM_UPSTREAM_PCM) == (mode == 2u),
		      "mode %u law %u power %u proposed %u got %u expected %u",
		      mode, law, power_code, proposal, actual, expected);
	}
	CHECK(memcmp(&calls[0].local_info1, &ac.local_info1, sizeof(ac.local_info1)) == 0 &&
	      memcmp(&calls[0].phase3_configuration, &ac.phase3, sizeof(ac.phase3)) == 0,
	      "power selection changed configured training preferences");
	if (mode == 2u)
		CHECK(calls[0].state.phase3.v92_analog.downstream_training_ucode == expected,
		      "V.92 receiver trained at the proposal rather than the transmitted Ucode");
	return true;
}

static bool test_info1a_training_power(void)
{
	static const struct { uint8_t code; uint16_t rms; } limits[] = {
		{ 0u, 15124u }, { 18u, 5368u }, { 23u, 4024u }, { 31u, 2540u },
	};
	static const uint8_t proposals[] = { 67u, 91u };

	/* Decode actual INFO1a through both call owners, including the V.92
	 * conventional-upstream branch. A quieter preference is not raised,
	 * and the configured proposal remains available for later retrains. */
	for (unsigned mode = 0u; mode < 3u; ++mode)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (size_t power = 0u; power < ARRAY_LENGTH(limits); ++power)
				for (size_t proposal = 0u; proposal < ARRAY_LENGTH(proposals); ++proposal)
					CHECK(run_info1a_training_power(mode, law, limits[power].code,
						limits[power].rms, proposals[proposal]), "training power selection failed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "V.90 peer CPs echo reconditioning", test_echo_reconditioning },
		{ "V.34 fallback retains PCM retrain agreement", test_v34_restart },
		{ "Phase 2 input ownership", test_phase2_input_ownership },
		{ "V.92 measured INFO1d wire offers", test_v92_measured_info1d },
		{ "PCM INFO1d respects peer capabilities", test_info1d_peer_capabilities },
		{ "PCM INFO1a respects peer training power", test_info1a_training_power },
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
		{ "standards-timed V.92 digital Phase 3 responses", test_v92_phase3_deadlines },
		{ "standards-timed V.92 analogue Phase 3 responses", test_v92_analog_phase3_deadlines },
		{ "standards-timed V.90 Sd response", test_phase3_sd_timeout },
		{ "downstream rate negotiation",
		  test_downstream_rate_negotiation },
		{ "V.92 role-local Phase 4 handoffs",
		  test_v92_phase4_control },
		{ "V.92 continuous duplex PCM data", test_v92_duplex_data },
		{ "V.92 damaged TRN2u recovers within budget", test_v92_trn2u_recovery },
		{ "V.92 two-postcursor PCM data", test_v92_two_postcursor_data },
		{ "V.92 codec-noise likelihood and recovery", test_v92_codec_noise_recovery },
		{ "V.92 direct B1 bit validation", test_v92_direct_b1_validation },
		{ "PCM downstream B1 bit validation", test_downstream_b1_validation },
		{ "V.92 post-acquisition B1 quality", test_v92_b1_quality_window },
		{ "V.92 delayed duplex PCM data", test_v92_delayed_duplex_data },
		{ "V.92 online quality rate recovery", test_v92_quality_rate_recovery },
		{ "V.92 lost decoder paths recover within budget", test_v92_no_path_recovery },
		{ "V.92 peer retrain outranks lost PCM paths", test_v92_peer_retrain_lost_paths },
		{ "V.92 lost B1 decoder paths retrain", test_v92_b1_no_path_recovery },
		{ "V.92 online Ru/Rd control exchange", test_v92_remote_rate_request },
		{ "V.92 digital fast-exchange response", test_v92_fast_response },
		{ "V.92 paired fast exchange", test_v92_fast_pair },
		{ "V.92 fast/rate exchange collision", test_v92_fast_rate_collision },
		{ "V.92 local and simultaneous rate exchange", test_v92_rate_request },
		{ "V.92 peer-limited upstream data", test_v92_upstream_negotiation },
		{ "V.92 failed B1 rate recovery", test_v92_b1_rate_recovery },
		{ "V.92 downstream reselection through retrain", test_v92_downstream_reselection },
		{ "V.92 received lookahead limits", test_v92_received_lookahead },
		{ "V.92 upstream reselection through retrain", test_v92_upstream_reselection },
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
