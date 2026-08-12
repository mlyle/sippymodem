// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase2_initial_probe.h>
#include <v90_phase2_info1_exchange.h>
#include <v90_phase2_probe_turnaround.h>
#include <v90_phase2_reciprocal_probe.h>

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

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1d = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = -23,
		},
	};

	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT; ++index)
		info1d.answer_transmit[index] =
			(struct v34_info1_probe_result){
				.high_carrier = (index & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(2u * index),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(6u + index),
			};
	return info1d;
}

static struct v90_info1a analog_info1(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 5,
		.training_ucode = 91,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
		.frequency_offset_1_50_hz_units = 31,
	};
}

static bool info1d_equal(
	const struct v34_info1c *left, const struct v34_info1c *right)
{
	uint8_t left_payload[V34_INFO1C_PAYLOAD_BYTES];
	uint8_t right_payload[V34_INFO1C_PAYLOAD_BYTES];

	return v34_info1c_encode(left, left_payload, sizeof(left_payload)) ==
		       V34_INFO1_OK &&
	       v34_info1c_encode(right, right_payload, sizeof(right_payload)) ==
		       V34_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool info1a_equal(
	const struct v90_info1a *left, const struct v90_info1a *right)
{
	uint8_t left_payload[V90_INFO1A_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO1A_PAYLOAD_BYTES];

	return v90_info1a_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO1_OK &&
	       v90_info1a_encode(right, right_payload, sizeof(right_payload)) ==
		       V90_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool check_info1_exchange(
	enum test_codec codec, struct test_delay_line *analog_to_digital,
	struct test_delay_line *digital_to_analog,
	const struct v90_phase2_reciprocal_probe *analog_probe,
	const struct v90_phase2_reciprocal_probe *digital_probe)
{
	struct v34_info1c info1d = digital_info1();
	struct v90_info1a info1a = analog_info1();
	struct v90_phase2_info1_exchange analog;
	struct v90_phase2_info1_exchange digital;
	struct v90_phase2_info1_exchange untouched;
	struct v90_phase2_info1_exchange untouched_before;
	size_t digital_sent = SIZE_MAX;
	size_t analog_received = SIZE_MAX;
	size_t analog_sent = SIZE_MAX;
	size_t digital_received = SIZE_MAX;

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_before = untouched;
	CHECK(v90_phase2_info1_init_analog(&untouched, digital_probe) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      v90_phase2_info1_init_digital(
		      &untouched, analog_probe, &info1d) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_before, sizeof(untouched)) == 0,
	      "wrong-role V.90 INFO1 handoff mutated output state");
	CHECK(v90_phase2_info1_init_analog(&analog, analog_probe) ==
		      PHASE2_INFO1_OK &&
	      v90_phase2_info1_init_digital(
		      &digital, digital_probe, &info1d) == PHASE2_INFO1_OK &&
	      analog.control.nominal_amplitude == TEST_AMPLITUDE &&
	      digital.control.nominal_amplitude == TEST_AMPLITUDE,
	      "V.90 INFO1 endpoints did not preserve their handoff");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v90_phase2_info1_event analog_event;
		struct v90_phase2_info1_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(v90_phase2_info1_sample(
			      &analog, delay_line_receive(digital_to_analog),
			      &analog_transmit, &analog_event) == PHASE2_INFO1_OK &&
		      v90_phase2_info1_sample(
			      &digital, delay_line_receive(analog_to_digital),
			      &digital_transmit, &digital_event) == PHASE2_INFO1_OK,
		      "V.90 INFO1 exchange failed for codec %d at sample %zu",
		      codec, index);
		delay_line_transmit(
			analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (digital_event.kind ==
		    V90_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT)
			digital_sent = index;
		if (analog_event.kind ==
		    V90_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED) {
			struct v90_phase2_info1_exchange before = analog;
			struct v90_info1a invalid = info1a;

			analog_received = index;
			invalid.training_ucode = V90_INFO1A_MIN_TRAINING_UCODE - 1;
			CHECK(info1d_equal(
				      &analog_event.peer_record.info1d, &info1d) &&
			      v90_phase2_info1_reply_analog(&analog, &invalid) ==
				      PHASE2_INFO1_INVALID_ARGUMENT &&
			      memcmp(&analog, &before, sizeof(analog)) == 0 &&
			      v90_phase2_info1_reply_analog(&analog, &info1a) ==
				      PHASE2_INFO1_OK &&
			      analog.control.transmitter.modulation.carrier
					      .carrier_phase +
					      UINT32_C(0x40000000) ==
				      analog.control.signal_a_tone.carrier_phase &&
			      analog.control.transmitter.modulation.guard_phase ==
				      analog.control.signal_a_tone.guard_phase,
			      "analog modem did not accept INFO1d phase-continuously");
		}
		if (analog_event.kind ==
		    V90_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT)
			analog_sent = index;
		if (digital_event.kind ==
		    V90_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			digital_received = index;
			CHECK(info1a_equal(
				      &digital_event.peer_record.info1a, &info1a),
			      "digital modem decoded a different V.90 selection");
		}
		if (analog.control.phase == PHASE2_INFO1_COMPLETE &&
		    digital.control.phase == PHASE2_INFO1_COMPLETE)
			break;
	}
	CHECK(digital_sent < analog_received && analog_received < analog_sent &&
	      analog_sent < digital_received &&
	      analog.peer_record_received && digital.peer_record_received &&
	      info1d_equal(&analog.peer_record.info1d, &info1d) &&
	      info1a_equal(&digital.peer_record.info1a, &info1a) &&
	      v90_phase2_info1_is_valid(&analog) &&
	      v90_phase2_info1_is_valid(&digital),
	      "V.90 INFO1 order or retained selections changed");
	return true;
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
	struct v90_phase2_probe_turnaround analog_turnaround;
	struct v90_phase2_probe_turnaround digital_turnaround;
	struct v90_phase2_reciprocal_probe analog_reciprocal;
	struct v90_phase2_reciprocal_probe digital_reciprocal;
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
		struct v90_phase2_initial_probe invalid = analog.probe;
		struct v90_phase2_initial_probe before;
		struct phase2_initial_probe_event event = {
			.kind = PHASE2_INITIAL_PROBE_EVENT_FAILED,
		};
		int16_t output = 1234;

		invalid.ranging.info0_exchange.transport.info_receiver.modulation
			.detector_output = NAN;
		before = invalid;
		CHECK(v90_phase2_initial_probe_sample(
			      &invalid, 0, &output, &event) ==
			      PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
		      memcmp(&invalid, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED,
		      "invalid V.90 probe state mutated state or outputs");
	}
	CHECK(v90_phase2_probe_turnaround_init(
		      &analog_turnaround, &analog.probe) ==
		      PHASE2_PROBE_TURNAROUND_OK &&
	      v90_phase2_probe_turnaround_init(
		      &digital_turnaround, &digital.probe) ==
		      PHASE2_PROBE_TURNAROUND_OK,
	      "V.90 probe-turnaround endpoints did not initialize");
	{
		bool analog_ready = false;
		bool digital_ready = false;

		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			struct phase2_probe_turnaround_event analog_event;
			struct phase2_probe_turnaround_event digital_event;
			int16_t analog_transmit;
			int16_t digital_transmit;

			CHECK(v90_phase2_probe_turnaround_sample(
				      &analog_turnaround,
				      delay_line_receive(&digital_to_analog),
				      &analog_transmit, &analog_event) ==
				      PHASE2_PROBE_TURNAROUND_OK &&
			      v90_phase2_probe_turnaround_sample(
				      &digital_turnaround,
				      delay_line_receive(&analog_to_digital),
				      &digital_transmit, &digital_event) ==
				      PHASE2_PROBE_TURNAROUND_OK,
			      "V.90 probe turnaround failed for codec %d at sample %zu",
			      codec, index);
			delay_line_transmit(
				&analog_to_digital,
				codec_round_trip(analog_transmit, codec));
			delay_line_transmit(
				&digital_to_analog,
				codec_round_trip(digital_transmit, codec));
			analog_ready |= analog_event.kind ==
				PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;
			digital_ready |= digital_event.kind ==
				PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;
			if (analog_ready && digital_ready)
				break;
		}
		CHECK(analog_ready && digital_ready &&
		      analog_turnaround.control.phase ==
			      PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY &&
		      digital_turnaround.control.phase ==
			      PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY &&
		      analog_turnaround.control
				      .signal_a_reversal_elapsed_samples != 0 &&
		      v90_phase2_probe_turnaround_is_valid(
			      &analog_turnaround) &&
		      v90_phase2_probe_turnaround_is_valid(
			      &digital_turnaround),
		      "V.90 probe turnaround did not complete both role handoffs");
	}
	CHECK(v90_phase2_reciprocal_probe_init(
		      &analog_reciprocal, &analog_turnaround) ==
		      PHASE2_RECIPROCAL_PROBE_OK &&
	      v90_phase2_reciprocal_probe_init(
		      &digital_reciprocal, &digital_turnaround) ==
		      PHASE2_RECIPROCAL_PROBE_OK,
	      "V.90 reciprocal-probe endpoints did not initialize");
	{
		bool analog_measured = false;
		bool digital_acknowledged = false;

		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			struct phase2_reciprocal_probe_event analog_event;
			struct phase2_reciprocal_probe_event digital_event;
			int16_t analog_transmit;
			int16_t digital_transmit;

			CHECK(v90_phase2_reciprocal_probe_sample(
				      &analog_reciprocal,
				      delay_line_receive(&digital_to_analog),
				      &analog_transmit, &analog_event) ==
				      PHASE2_RECIPROCAL_PROBE_OK &&
			      v90_phase2_reciprocal_probe_sample(
				      &digital_reciprocal,
				      delay_line_receive(&analog_to_digital),
				      &digital_transmit, &digital_event) ==
				      PHASE2_RECIPROCAL_PROBE_OK,
			      "V.90 reciprocal probe failed for codec %d at sample %zu",
			      codec, index);
			delay_line_transmit(
				&analog_to_digital,
				codec_round_trip(analog_transmit, codec));
			delay_line_transmit(
				&digital_to_analog,
				codec_round_trip(digital_transmit, codec));
			analog_measured |= analog_event.kind ==
				PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED;
			digital_acknowledged |= digital_event.kind ==
				PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED;
			if (analog_measured && digital_acknowledged)
				break;
		}
		CHECK(analog_measured && digital_acknowledged &&
		      analog_reciprocal.control.phase ==
			      PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE &&
		      digital_reciprocal.control.phase ==
			      PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE &&
		      analog_reciprocal.control
				      .signal_a_reversal_elapsed_samples >
			      analog_turnaround.control
				      .signal_a_reversal_elapsed_samples &&
		      v90_phase2_reciprocal_probe_is_valid(
			      &analog_reciprocal) &&
		      v90_phase2_reciprocal_probe_is_valid(
			      &digital_reciprocal),
		      "V.90 reciprocal probe did not complete both role handoffs");
	}
	CHECK(v90_phase2_reciprocal_probe_measure(
		      &analog_reciprocal, &measurement) ==
		      PHASE2_RECIPROCAL_PROBE_OK &&
	      measurement.l1.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l1.harmonics[0].mean_square > 0.0 &&
	      measurement.l2.harmonics[0].mean_square > 0.0 &&
	      v90_phase2_reciprocal_probe_measure(
		      &digital_reciprocal, &measurement) ==
		      PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
		      "V.90 analog reciprocal-probe measurement was unavailable");
	CHECK(check_info1_exchange(
		      codec, &analog_to_digital, &digital_to_analog,
		      &analog_reciprocal, &digital_reciprocal),
	      "V.90 INFO1 exchange failed for codec %d", codec);

	{
		struct v90_phase2_probe_turnaround before;
		struct phase2_probe_turnaround_event event = {
			.kind = PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		};
		int16_t output = 1234;

		analog_turnaround.initial_probe.ranging.info0_exchange.transport
			.info_receiver
			.modulation.detector_output = NAN;
		before = analog_turnaround;
		CHECK(v90_phase2_probe_turnaround_sample(
			      &analog_turnaround, 0, &output, &event) ==
			      PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT &&
		      memcmp(&analog_turnaround, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		      "invalid V.90 turnaround mutated state or outputs");
	}
	{
		struct v90_phase2_reciprocal_probe before;
		struct phase2_reciprocal_probe_event event = {
			.kind = PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
		};
		int16_t output = 1234;

		analog_reciprocal.turnaround.initial_probe.ranging.info0_exchange
			.transport.info_receiver.modulation.detector_output = NAN;
		before = analog_reciprocal;
		CHECK(v90_phase2_reciprocal_probe_sample(
			      &analog_reciprocal, 0, &output, &event) ==
			      PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
		      memcmp(&analog_reciprocal, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
		      "invalid V.90 reciprocal probe mutated state or outputs");
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
	fprintf(stderr, "[ RUN      ] PCMU and PCMA Phase 2 through INFO1\n");
	if (!test_pcm_initial_probe()) {
		fprintf(stderr, "[  FAILED  ] PCMU and PCMA Phase 2 through INFO1\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] PCMU and PCMA Phase 2 through INFO1\n");
	return 0;
}
