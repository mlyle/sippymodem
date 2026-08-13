// SPDX-License-Identifier: BSD-3-Clause

#include <v90_call.h>

#include <g711.h>

#include "test.h"

#include <string.h>

enum {
	CALL_SAMPLE_LIMIT = 120000,
	MAX_LINE_DELAY_CHARACTERS = 13,
	PHASE2_NOMINAL_AMPLITUDE = 12000,
	PHASE3_NOMINAL_AMPLITUDE = 6000,
};

struct channel {
	uint8_t analog_to_digital[MAX_LINE_DELAY_CHARACTERS];
	uint8_t digital_to_analog[MAX_LINE_DELAY_CHARACTERS];
	size_t delay;
	size_t cursor;
};

struct endpoint {
	struct v90_call call;
	union {
		struct v90_phase4_analog_handoff analog;
		struct v90_phase4_digital_handoff digital;
	} handoff;
	size_t mixed_octets;
	size_t phase4_octets;
	uint16_t phase4_entry_ri_symbols;
	bool entered_phase3;
	bool entered_mixed;
	bool entered_phase4;
	bool handoff_captured;
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

static struct v90_call_configuration configuration(
	enum v90_modem_role role, enum g711_law law)
{
	struct v90_call_configuration configuration = {
		.local_role = role,
		.law = law,
		.phase2_nominal_amplitude = PHASE2_NOMINAL_AMPLITUDE,
	};

	if (role == V90_ANALOG_MODEM) {
		configuration.local_info0.analog = analog_info0();
		configuration.local_info1.analog = analog_info1();
		configuration.phase3.analog.nominal_peak_amplitude =
			PHASE3_NOMINAL_AMPLITUDE;
		configuration.analog_phase4.training_parameters =
			training_parameters(law);
	} else {
		configuration.local_info0.digital = digital_info0(law);
		configuration.local_info1.digital = digital_info1();
		configuration.phase3.digital.receive = receive_configuration();
		configuration.phase3.digital.jd = jd_parameters();
		configuration.digital_phase4.initial_mp = initial_mp();
	}
	return configuration;
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
	enum g711_law law)
{
	struct v90_call_configuration selected = configuration(role, law);
	struct endpoint initialized = { 0 };

	if (endpoint == NULL ||
	    v90_call_init(&initialized.call, &selected) != V90_CALL_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_octet(
	struct endpoint *endpoint, uint8_t received,
	enum g711_law law, uint8_t *transmitted)
{
	struct v90_call_observation observation;

	if (v90_call_phase4_is_ready(&endpoint->call)) {
		if (endpoint->call.local_role == V90_ANALOG_MODEM) {
			struct v90_phase4_analog_handoff *handoff =
				&endpoint->handoff.analog;
			struct v90_downstream_event event;
			enum v90_downstream_rx_result receive_result;
			int16_t sample;

			if (!endpoint->handoff_captured &&
			    v90_phase4_analog_startup_handoff(
				    &endpoint->call.state.phase4.analog,
				    handoff) != V90_PHASE4_STARTUP_OK)
				return false;
			endpoint->handoff_captured = true;
			if (v90_phase4_cp_line_tx_record_is_complete(
				    &handoff->transmit) &&
			    v90_phase4_cp_line_tx_start_next_record(
				    &handoff->transmit,
				    &endpoint->call.analog_phase4_configuration
					     .training_parameters) !=
				    V90_PHASE4_CP_LINE_TX_OK)
				return false;
			if (v90_phase4_cp_line_tx_sample(
				    &handoff->transmit, &sample) !=
			    V90_PHASE4_CP_LINE_TX_OK)
				return false;
			receive_result = v90_downstream_rx_receive(
				&handoff->receive, received, &event);
			if (receive_result != V90_DOWNSTREAM_RX_BUFFERING &&
			    receive_result != V90_DOWNSTREAM_RX_TRN2D_FRAME)
				return false;
			*transmitted = law == G711_LAW_PCMA ?
				g711_encode_pcma(sample) :
				g711_encode_pcmu(sample);
		} else {
			struct v90_phase4_digital_handoff *handoff =
				&endpoint->handoff.digital;
			struct v90_downstream_symbol symbol;
			struct v90_cp_receive_event event = { 0 };
			enum v90_phase4_cp_line_rx_result receive_result;

			if (!endpoint->handoff_captured &&
			    v90_phase4_digital_startup_handoff(
				    &endpoint->call.state.phase4.digital,
				    handoff) != V90_PHASE4_STARTUP_OK)
				return false;
			endpoint->handoff_captured = true;
			if (v90_downstream_tx_next(
				    &handoff->transmit.transmitter, &symbol) !=
			    V90_DOWNSTREAM_TX_OK)
				return false;
			*transmitted = symbol.pcm.octet;
			receive_result = v90_phase4_cp_line_rx_sample(
				&handoff->receive,
				law == G711_LAW_PCMA ?
					g711_decode_pcma(received) :
					g711_decode_pcmu(received),
				&event);
			if (receive_result == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
				if (!v90_phase4_cp_line_rx_pending_record_is_valid(
					    &handoff->receive) ||
				    v90_phase4_cp_line_rx_continue(
					    &handoff->receive) !=
					    V90_PHASE4_CP_LINE_RX_OK)
					return false;
			} else if (receive_result != V90_PHASE4_CP_LINE_RX_OK &&
				   receive_result != V90_PHASE4_CP_LINE_RX_SYMBOL) {
				return false;
			}
		}
		return true;
	}
	if (v90_call_octet(
		    &endpoint->call, received, transmitted,
		    &observation) != V90_CALL_OK)
		return false;
	endpoint->mixed_octets +=
		observation.processed == V90_CALL_PHASE3_MIXED;
	endpoint->phase4_octets +=
		observation.processed == V90_CALL_PHASE4;
	endpoint->entered_phase3 |= observation.next == V90_CALL_PHASE3;
	endpoint->entered_mixed |=
		observation.next == V90_CALL_PHASE3_MIXED;
	endpoint->entered_phase4 |= observation.next == V90_CALL_PHASE4;
	if (observation.processed != V90_CALL_PHASE4 &&
	    observation.next == V90_CALL_PHASE4) {
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
	struct v90_call before = endpoint->call;
	struct v90_call_observation observation;
	struct v90_call_observation observation_before;
	uint8_t output = 0xa5;

	memset(&observation, 0x5a, sizeof(observation));
	observation_before = observation;
	return v90_call_octet(
		       &endpoint->call, silence_octet(law),
		       &output, &observation) == V90_CALL_PHASE4_READY &&
	       output == 0xa5 &&
	       memcmp(&observation, &observation_before,
		      sizeof(observation)) == 0 &&
	       memcmp(&endpoint->call, &before, sizeof(before)) == 0;
}

static bool run_call(
	enum g711_law law, size_t delay, bool require_complete_startup)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool milestone_reached = false;

	CHECK(channel_init(&channel, law, delay) &&
	      endpoint_init(&analog, V90_ANALOG_MODEM, law) &&
	      endpoint_init(&digital, V90_DIGITAL_MODEM, law),
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
		if (require_complete_startup ?
			    v90_call_phase4_is_ready(&analog.call) &&
				    v90_call_phase4_is_ready(&digital.call) :
			    analog.entered_phase4 && digital.entered_phase4) {
			milestone_reached = true;
			break;
		}
	}
	CHECK(milestone_reached &&
	      analog.entered_phase3 && digital.entered_phase3 &&
	      analog.entered_mixed && digital.entered_mixed &&
	      analog.entered_phase4 && digital.entered_phase4 &&
	      v90_call_current_stage(&analog.call) == V90_CALL_PHASE4 &&
	      v90_call_current_stage(&digital.call) == V90_CALL_PHASE4 &&
	      v90_phase4_analog_startup_is_valid(
		      &analog.call.state.phase4.analog) &&
	      v90_phase4_digital_startup_is_valid(
		      &digital.call.state.phase4.digital) &&
	      ri_progress_is_exact(&analog) &&
	      ri_progress_is_exact(&digital) &&
	      !v90_call_has_failed(&analog.call) &&
	      !v90_call_has_failed(&digital.call),
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
	if (require_complete_startup)
		CHECK(analog.phase4_octets > 0 && digital.phase4_octets > 0 &&
		      v90_phase4_analog_startup_is_complete(
			      &analog.call.state.phase4.analog) &&
		      v90_phase4_digital_startup_is_complete(
			      &digital.call.state.phase4.digital) &&
		      phase4_boundary_is_transactional(&analog, law) &&
		      phase4_boundary_is_transactional(&digital, law),
		      "law %u delay %zu did not preserve Phase 4 handoffs",
		      (unsigned int)law, delay);
	return true;
}

static bool test_generated_calls(void)
{
	static const size_t complete_delays[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(complete_delays); ++index)
		CHECK(run_call(
			      G711_LAW_PCMU, complete_delays[index], true) &&
		      run_call(
			      G711_LAW_PCMA, complete_delays[index], true),
		      "delay %zu generated V.90 handoffs failed",
		      complete_delays[index]);
	/* Retain the longer-delay pre-Phase-4 handoff regression separately
	 * from the carrier-phase combinations covered through CPt/Ri above. */
	CHECK(run_call(G711_LAW_PCMU, 13, false) &&
	      run_call(G711_LAW_PCMA, 13, false),
	      "generated V.90 calls failed");
	return true;
}

static bool test_validation(void)
{
	struct v90_call_configuration selected =
		configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	struct v90_call call;
	struct v90_call saved;
	struct v90_call_observation observation;
	struct v90_call_observation saved_observation;
	uint8_t output = 0xa5;

	memset(&call, 0xa5, sizeof(call));
	saved = call;
	CHECK(v90_call_init(NULL, &selected) == V90_CALL_INVALID_ARGUMENT &&
	      v90_call_init(&call, NULL) == V90_CALL_INVALID_ARGUMENT,
	      "call accepted a null initializer argument");
	selected.local_role = (enum v90_modem_role)99;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT,
	      "call accepted an invalid role");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.phase3.analog.nominal_peak_amplitude = 0;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT,
	      "call accepted a zero Phase 3 amplitude");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	selected.analog_phase4.training_parameters.request_silence = true;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT,
	      "call accepted a non-initial Phase 4 training record");
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.digital_phase4.initial_mp.parameters.type0.acknowledges_cp =
		true;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT,
	      "call accepted an acknowledging initial MP");
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.law = G711_LAW_PCMA;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid configurations changed the call");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	CHECK(v90_call_init(&call, &selected) == V90_CALL_OK &&
	      v90_call_is_valid(&call) &&
	      !v90_call_has_failed(&call) &&
	      !v90_call_phase4_is_ready(&call),
	      "valid call did not initialize");
	saved = call;
	memset(&observation, 0x5a, sizeof(observation));
	saved_observation = observation;
	CHECK(v90_call_octet(
		      NULL, silence_octet(G711_LAW_PCMU),
		      &output, &observation) == V90_CALL_INVALID_ARGUMENT &&
	      v90_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      NULL, &observation) == V90_CALL_INVALID_ARGUMENT &&
	      v90_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      &output, NULL) == V90_CALL_INVALID_ARGUMENT &&
	      v90_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      (uint8_t *)&call, &observation) ==
		      V90_CALL_INVALID_ARGUMENT &&
	      v90_call_octet(
		      &call, silence_octet(G711_LAW_PCMU),
		      &output, (struct v90_call_observation *)&call) ==
		      V90_CALL_INVALID_ARGUMENT &&
	      output == 0xa5 &&
	      memcmp(&observation, &saved_observation,
		     sizeof(observation)) == 0 &&
	      memcmp(&call, &saved, sizeof(call)) == 0 &&
	      !v90_call_is_valid(NULL) && !v90_call_has_failed(NULL) &&
	      !v90_call_phase4_is_ready(NULL) &&
	      v90_call_current_stage(NULL) == (enum v90_call_stage)-1,
	      "invalid call operation changed state or output");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"phase 4 ready",
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
			      v90_call_result_name(
				      (enum v90_call_result)index),
			      expected[index]) == 0,
		      "call result name %zu changed", index);
	CHECK(strcmp(
		      v90_call_result_name((enum v90_call_result)99),
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
		{ "generated PCMU and PCMA Phase 4 handoffs",
		  test_generated_calls },
		{ "validation and transactionality", test_validation },
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
