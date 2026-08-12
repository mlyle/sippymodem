// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase2_initial_probe.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_DELAY_SAMPLES = 13,
	TEST_SAMPLE_LIMIT = 12000,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_delay_line {
	int16_t samples[TEST_LINE_DELAY_SAMPLES];
	size_t cursor;
};

struct test_endpoint {
	struct v90_phase2_ranging ranging;
	struct v90_phase2_initial_probe probe;
	bool probing;
	bool analog;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	return codec == TEST_CODEC_PCMU ?
		       g711_decode_pcmu(g711_encode_pcmu(sample)) :
		       g711_decode_pcma(g711_encode_pcma(sample));
}

static int16_t delay_line_receive(const struct test_delay_line *line)
{
	return line->samples[line->cursor];
}

static void delay_line_transmit(
	struct test_delay_line *line, int16_t sample)
{
	line->samples[line->cursor] = sample;
	line->cursor = (line->cursor + 1u) % TEST_LINE_DELAY_SAMPLES;
}

static struct v90_info0 analog_capabilities(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
	};
}

static struct v90_info0d digital_capabilities(enum test_codec codec)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.high_carrier_3000_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = codec == TEST_CODEC_PCMU ?
				   V90_PCM_MU_LAW : V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static bool endpoint_sample(
	struct test_endpoint *endpoint, int16_t received_sample,
	int16_t *transmit_sample,
	struct phase2_initial_probe_event *probe_event)
{
	if (endpoint->probing)
		return v90_phase2_initial_probe_sample(
			       &endpoint->probe, received_sample,
			       transmit_sample, probe_event) ==
		       PHASE2_INITIAL_PROBE_OK;
	{
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_event_kind ready =
			endpoint->analog ? PHASE2_PROBE_TRANSMIT_READY :
					   PHASE2_PROBE_RECEIVE_READY;

		if (v90_phase2_ranging_sample(
			    &endpoint->ranging, received_sample,
			    transmit_sample, &event) != V90_PHASE2_RANGING_OK)
			return false;
		*probe_event = (struct phase2_initial_probe_event){ 0 };
		if (event.kind == ready) {
			if (v90_phase2_initial_probe_init(
				    &endpoint->probe, &endpoint->ranging) !=
			    PHASE2_INITIAL_PROBE_OK)
				return false;
			endpoint->probing = true;
		}
	}
	return true;
}

static bool check_initial_probe(enum test_codec codec)
{
	struct v90_info0 analog_info = analog_capabilities();
	struct v90_info0d digital_info = digital_capabilities(codec);
	struct test_endpoint analog = { .analog = true };
	struct test_endpoint digital = { 0 };
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	struct v34_phase2_probe_receiver_measurement measurement;
	uint32_t transmitted_l2_samples = 0;
	bool analog_l2_started = false;
	bool analog_probe_complete = false;
	bool digital_probe_complete = false;
	bool tone_pair_active = false;

	CHECK(v90_phase2_ranging_init_analog(
		      &analog.ranging, TEST_AMPLITUDE, &analog_info) ==
		      V90_PHASE2_RANGING_OK &&
	      v90_phase2_ranging_init_digital(
		      &digital.ranging, TEST_AMPLITUDE, &digital_info) ==
		      V90_PHASE2_RANGING_OK,
	      "V.90 initial-probe endpoints did not initialize");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_initial_probe_event analog_event;
		struct phase2_initial_probe_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(endpoint_sample(
			      &analog, delay_line_receive(&digital_to_analog),
			      &analog_transmit, &analog_event) &&
		      endpoint_sample(
			      &digital, delay_line_receive(&analog_to_digital),
			      &digital_transmit, &digital_event),
		      "V.90 initial probe failed for codec %d at sample %zu",
		      codec, index);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));

		if (analog_event.transmit_event.kind ==
		    V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED)
			analog_l2_started = true;
		if (analog_event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
			analog_probe_complete = true;
			transmitted_l2_samples =
				analog_event.transmit_event.l2_samples;
		}
		if (digital_event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE)
			digital_probe_complete = true;
		if (digital_event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE)
			tone_pair_active = true;
		if (analog.probing && digital.probing &&
		    analog.probe.control.phase ==
			    PHASE2_INITIAL_PROBE_SIGNAL_A_TONE &&
		    digital.probe.control.phase ==
			    PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE)
			break;
	}

	CHECK(analog.probing && digital.probing &&
	      v90_phase2_initial_probe_is_valid(&analog.probe) &&
	      v90_phase2_initial_probe_is_valid(&digital.probe) &&
	      analog_l2_started && analog_probe_complete &&
	      digital_probe_complete && tone_pair_active,
	      "V.90 first probe did not complete both role handoffs");
	CHECK(transmitted_l2_samples >= V34_PHASE2_PROBE_L1_SAMPLES &&
	      transmitted_l2_samples <
		      (uint32_t)V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		      analog.probe.ranging.control.round_trip_delay_samples,
	      "V.90 analog modem ended L2 outside its bounded window");
	CHECK(v90_phase2_initial_probe_measure(
		      &digital.probe, &measurement) ==
		      PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples >= V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l1.harmonics[0].mean_square > 0.0 &&
	      measurement.l2.harmonics[0].mean_square > 0.0 &&
	      v90_phase2_initial_probe_measure(
		      &analog.probe, &measurement) ==
		      PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "V.90 digital probe measurement was unavailable or malformed");

	{
		struct v90_phase2_initial_probe before;
		struct phase2_initial_probe_event event = {
			.kind = PHASE2_INITIAL_PROBE_EVENT_FAILED,
		};
		int16_t output = 1234;

		analog.probe.ranging.info0_exchange.transport.info_receiver
			.modulation.detector_output = NAN;
		before = analog.probe;
		CHECK(v90_phase2_initial_probe_sample(
			      &analog.probe, 0, &output, &event) ==
			      PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
		      memcmp(&analog.probe, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED,
		      "invalid V.90 probe state mutated state or outputs");
	}
	return true;
}

static bool test_pcm_initial_probe(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_initial_probe(codec),
		      "V.90 initial probe failed for codec %d", codec);
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] PCMU and PCMA first line probe\n");
	if (!test_pcm_initial_probe()) {
		fprintf(stderr, "[  FAILED  ] PCMU and PCMA first line probe\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] PCMU and PCMA first line probe\n");
	return 0;
}
