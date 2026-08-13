// SPDX-License-Identifier: BSD-3-Clause

#include <v90_call.h>

#include <g711.h>

#include "test.h"

#include <string.h>

enum {
	CALL_SAMPLE_LIMIT = 60000,
	LINE_DELAY_CHARACTERS = 13,
	PHASE2_NOMINAL_AMPLITUDE = 12000,
	PHASE3_NOMINAL_AMPLITUDE = 6000,
};

struct channel {
	uint8_t analog_to_digital[LINE_DELAY_CHARACTERS];
	uint8_t digital_to_analog[LINE_DELAY_CHARACTERS];
	size_t cursor;
};

struct endpoint {
	struct v90_call call;
	bool entered_phase3;
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
	} else {
		configuration.local_info0.digital = digital_info0(law);
		configuration.local_info1.digital = digital_info1();
		configuration.phase3.digital.receive = receive_configuration();
		configuration.phase3.digital.jd = jd_parameters();
	}
	return configuration;
}

static void channel_init(struct channel *channel, enum g711_law law)
{
	uint8_t silence = silence_octet(law);

	for (size_t index = 0; index < LINE_DELAY_CHARACTERS; ++index) {
		channel->analog_to_digital[index] = silence;
		channel->digital_to_analog[index] = silence;
	}
	channel->cursor = 0;
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
	channel->cursor = (channel->cursor + 1u) % LINE_DELAY_CHARACTERS;
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

	if (v90_call_phase3_is_ready(&endpoint->call)) {
		*transmitted = silence_octet(law);
		return true;
	}
	if (v90_call_octet(
		    &endpoint->call, received, transmitted,
		    &observation) != V90_CALL_OK)
		return false;
	endpoint->entered_phase3 |= observation.next == V90_CALL_PHASE3;
	return true;
}

static bool phase3_boundary_is_transactional(
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
		       &output, &observation) == V90_CALL_PHASE3_READY &&
	       output == 0xa5 &&
	       memcmp(&observation, &observation_before,
		      sizeof(observation)) == 0 &&
	       memcmp(&endpoint->call, &before, sizeof(before)) == 0;
}

static bool run_call(enum g711_law law)
{
	struct channel channel;
	struct endpoint analog;
	struct endpoint digital;
	bool ready = false;

	channel_init(&channel, law);
	CHECK(endpoint_init(&analog, V90_ANALOG_MODEM, law) &&
	      endpoint_init(&digital, V90_DIGITAL_MODEM, law),
	      "law %u V.90 call endpoints did not initialize",
	      (unsigned int)law);
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
		      "law %u V.90 call stopped at character %zu",
		      (unsigned int)law, clock);
		channel_transmit(
			&channel, analog_transmitted, digital_transmitted);
		if (v90_call_phase3_is_ready(&analog.call) &&
		    v90_call_phase3_is_ready(&digital.call)) {
			ready = true;
			break;
		}
	}
	CHECK(ready && analog.entered_phase3 && digital.entered_phase3 &&
	      v90_call_current_stage(&analog.call) == V90_CALL_PHASE3 &&
	      v90_call_current_stage(&digital.call) == V90_CALL_PHASE3 &&
	      v90_phase3_analog_station_is_valid(
		      &analog.call.state.phase3.analog) &&
	      v90_phase3_digital_station_is_valid(
		      &digital.call.state.phase3.digital) &&
	      !v90_call_has_failed(&analog.call) &&
	      !v90_call_has_failed(&digital.call) &&
	      phase3_boundary_is_transactional(&analog, law) &&
	      phase3_boundary_is_transactional(&digital, law),
	      "law %u V.90 call did not initialize Phase 3: "
	      "ready=%d entered=%d/%d stages=%d/%d "
	      "analog=%d/%d digital=%d/%d/%d",
	      (unsigned int)law, ready,
	      analog.entered_phase3, digital.entered_phase3,
	      analog.call.stage, digital.call.stage,
	      analog.call.state.phase3.analog.transmit_stage,
	      analog.call.state.phase3.analog.receive_stage,
	      digital.call.state.phase3.digital.receive_stage,
	      digital.call.state.phase3.digital.transmit_started,
	      digital.call.state.phase3.digital.transmit.stage);
	return true;
}

static bool test_generated_calls(void)
{
	CHECK(run_call(G711_LAW_PCMU) && run_call(G711_LAW_PCMA),
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
	selected = configuration(V90_DIGITAL_MODEM, G711_LAW_PCMU);
	selected.law = G711_LAW_PCMA;
	CHECK(v90_call_init(&call, &selected) == V90_CALL_INVALID_ARGUMENT &&
	      memcmp(&call, &saved, sizeof(call)) == 0,
	      "invalid configurations changed the call");
	selected = configuration(V90_ANALOG_MODEM, G711_LAW_PCMU);
	CHECK(v90_call_init(&call, &selected) == V90_CALL_OK &&
	      v90_call_is_valid(&call) &&
	      !v90_call_has_failed(&call) &&
	      !v90_call_phase3_is_ready(&call),
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
	      !v90_call_phase3_is_ready(NULL) &&
	      v90_call_current_stage(NULL) == (enum v90_call_stage)-1,
	      "invalid call operation changed state or output");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"phase 3 ready",
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
		{ "generated PCMU and PCMA Phase 2 handoffs",
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
