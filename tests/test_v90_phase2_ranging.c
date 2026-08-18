// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase2_ranging.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_DELAY_SAMPLES = 13,
	TEST_SAMPLE_LIMIT = 6000,
	TEST_EDGE_TOLERANCE_SAMPLES = 4,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_delay_line {
	int16_t samples[TEST_LINE_DELAY_SAMPLES];
	size_t cursor;
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

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
}

static bool check_ranging(enum test_codec codec)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(codec);
	struct v90_phase2_ranging analog_end;
	struct v90_phase2_ranging digital_end;
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	uint16_t analog_delay = 0;
	uint16_t digital_delay = 0;
	bool analog_ready = false;
	bool digital_ready = false;
	bool analog_received_info = false;
	bool digital_received_info = false;
	size_t analog_tone_handoffs = 0;
	size_t analog_ack_tone_start = SIZE_MAX;
	size_t analog_first_reversal = SIZE_MAX;

	CHECK(v90_phase2_ranging_init_analog(
		      &analog_end, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_RANGING_OK &&
	      v90_phase2_ranging_init_digital(
		      &digital_end, TEST_AMPLITUDE, &digital) ==
		      V90_PHASE2_RANGING_OK &&
	      analog_end.info0_exchange.transport.signal_role ==
		      PHASE2_SIGNAL_A &&
	      digital_end.info0_exchange.transport.signal_role ==
		      PHASE2_SIGNAL_B,
	      "V.90 ranging endpoints did not initialize");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v90_phase2_ranging_event analog_event;
		struct v90_phase2_ranging_event digital_event;
		enum v90_phase2_ranging_result analog_result;
		enum v90_phase2_ranging_result digital_result;
		enum phase2_info0_transmit_phase analog_transmit_before =
			analog_end.info0_exchange.transport.transmit_phase;
		enum phase2_ranging_phase analog_phase_before =
			analog_end.control.phase;
		int16_t analog_transmit;
		int16_t digital_transmit;

		analog_result = v90_phase2_ranging_sample(
			&analog_end, delay_line_receive(&digital_to_analog),
			&analog_transmit, &analog_event);
		digital_result = v90_phase2_ranging_sample(
			&digital_end, delay_line_receive(&analog_to_digital),
			&digital_transmit, &digital_event);
		CHECK(analog_result == V90_PHASE2_RANGING_OK &&
		      digital_result == V90_PHASE2_RANGING_OK,
		      "V.90 ranging failed for codec %d at sample %zu (analog %d digital %d)",
		      codec, index, analog_result, digital_result);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (analog_transmit_before == PHASE2_INFO0_CARRIER_TAIL &&
		    analog_end.info0_exchange.transport.transmit_phase ==
			    PHASE2_INFO0_TONE &&
		    ++analog_tone_handoffs == 2u)
			analog_ack_tone_start = index + 1u;
		if (analog_phase_before == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    analog_end.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
			analog_first_reversal = index;

		if (analog_event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_RECEIVED) {
			analog_received_info =
				analog_event.exchange_event.peer_info0.digital
					.nominal_transmit_power ==
				digital.nominal_transmit_power;
		}
		if (digital_event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_RECEIVED) {
			digital_received_info =
				digital_event.exchange_event.peer_info0.analog
					.transmit_power_reduction_supported ==
				analog.transmit_power_reduction_supported;
		}
		if (analog_event.kind == PHASE2_PROBE_TRANSMIT_READY) {
			analog_ready = true;
			analog_delay = analog_event.round_trip_delay_samples;
		}
		if (digital_event.kind == PHASE2_PROBE_RECEIVE_READY) {
			digital_ready = true;
			digital_delay = digital_event.round_trip_delay_samples;
		}
		if (analog_end.control.phase == PHASE2_RANGING_PROBE_READY &&
		    digital_end.control.phase == PHASE2_RANGING_PROBE_READY)
			break;
	}

	CHECK(analog_end.control.phase == PHASE2_RANGING_PROBE_READY &&
	      digital_end.control.phase == PHASE2_RANGING_PROBE_READY &&
	      analog_received_info && digital_received_info &&
	      analog_ready && digital_ready,
	      "V.90 ranging did not reach both typed probe boundaries");
	CHECK(absolute_difference(
		      analog_delay, 2u * TEST_LINE_DELAY_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES &&
	      absolute_difference(
		      digital_delay, 2u * TEST_LINE_DELAY_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "V.90 round-trip estimates changed for codec %d: analog %u digital %u",
	      codec, analog_delay, digital_delay);
	CHECK(analog_ack_tone_start != SIZE_MAX &&
	      analog_first_reversal != SIZE_MAX &&
	      analog_first_reversal - analog_ack_tone_start ==
		      PHASE2_RANGING_INITIAL_TONE_SAMPLES,
	      "repeated INFO0 did not restart the 50 ms Tone A epoch for codec %d: %zu/%zu",
	      codec, analog_ack_tone_start, analog_first_reversal);
	return true;
}

static bool test_pcm_ranging(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_ranging(codec),
		      "V.90 ranging failed for codec %d", codec);
	return true;
}

static bool test_validation(void)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_phase2_ranging ranging;
	struct v90_phase2_ranging before;
	struct v90_phase2_ranging_event event = {
		.kind = PHASE2_RANGING_TIMEOUT,
	};
	int16_t sample = 1234;

	CHECK(v90_phase2_ranging_init_analog(
		      &ranging, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_RANGING_OK,
	      "validation fixture did not initialize");
	ranging.info0_exchange.transport.info_receiver.modulation.detector_output =
		NAN;
	before = ranging;
	CHECK(v90_phase2_ranging_sample(
		      &ranging, 0, &sample, &event) ==
		      V90_PHASE2_RANGING_INVALID_ARGUMENT &&
	      memcmp(&ranging, &before, sizeof(ranging)) == 0 &&
	      sample == 1234 && event.kind == PHASE2_RANGING_TIMEOUT,
	      "invalid V.90 ranging state mutated state or outputs");
	CHECK(strcmp(
		      v90_phase2_ranging_result_name(V90_PHASE2_RANGING_OK),
		      "ok") == 0 &&
	      strcmp(v90_phase2_ranging_result_name(
			     (enum v90_phase2_ranging_result)99),
		     "unknown") == 0,
	      "V.90 ranging result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "PCMU and PCMA ranging", test_pcm_ranging },
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
