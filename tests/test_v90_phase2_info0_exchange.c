// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase2_info0_exchange.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLE_LIMIT = 4000,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct exchange_observation {
	size_t info_received;
	size_t info_rejected;
	size_t tone_started;
	union v90_info0_record peer_info0;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	return codec == TEST_CODEC_PCMU ?
		       g711_decode_pcmu(g711_encode_pcmu(sample)) :
		       g711_decode_pcma(g711_encode_pcma(sample));
}

static struct v90_info0 analog_capabilities(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
		.acknowledges_peer_info0 = true,
	};
}

static struct v90_info0d digital_capabilities(enum test_codec codec)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.high_carrier_3000_supported = true,
			.low_carrier_3200_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
			.acknowledges_peer_info0 = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = codec == TEST_CODEC_PCMU ?
				   V90_PCM_MU_LAW : V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static bool analog_equal(
	const struct v90_info0 *left, const struct v90_info0 *right)
{
	uint8_t left_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0A_PAYLOAD_BYTES];

	return v90_info0a_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO0_OK &&
	       v90_info0a_encode(right, right_payload, sizeof(right_payload)) ==
		       V90_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool digital_equal(
	const struct v90_info0d *left, const struct v90_info0d *right)
{
	uint8_t left_payload[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0D_PAYLOAD_BYTES];

	return v90_info0d_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO0_OK &&
	       v90_info0d_encode(right, right_payload, sizeof(right_payload)) ==
		       V90_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static void observe(
	const struct v90_phase2_info0_exchange_event *event,
	struct exchange_observation *observation)
{
	if (event->peer_info0_event == V90_PEER_INFO0_RECEIVED) {
		observation->info_received++;
		observation->peer_info0 = event->peer_info0;
	} else if (event->peer_info0_event == V90_PEER_INFO0_REJECTED) {
		observation->info_rejected++;
	}
	if (event->tone_event.kind == V34_PHASE2_TONE_STARTED)
		observation->tone_started++;
}

static bool check_exchange(enum test_codec codec)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0 expected_analog = analog;
	struct v90_info0d digital = digital_capabilities(codec);
	struct v90_info0d expected_digital = digital;
	struct v90_phase2_info0_exchange analog_end;
	struct v90_phase2_info0_exchange digital_end;
	struct exchange_observation analog_observation = { 0 };
	struct exchange_observation digital_observation = { 0 };
	int16_t analog_line = 0;
	int16_t digital_line = 0;
	size_t analog_tone_start = SIZE_MAX;
	size_t digital_tone_start = SIZE_MAX;

	expected_analog.acknowledges_peer_info0 = false;
	expected_digital.common.acknowledges_peer_info0 = false;
	CHECK(v90_phase2_info0_exchange_init_analog(
		      &analog_end, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_INFO0_EXCHANGE_OK &&
	      v90_phase2_info0_exchange_init_digital(
		      &digital_end, TEST_AMPLITUDE, &digital) ==
		      V90_PHASE2_INFO0_EXCHANGE_OK &&
	      analog_end.transport.signal_role == PHASE2_SIGNAL_A &&
	      digital_end.transport.signal_role == PHASE2_SIGNAL_B &&
	      analog_end.transport.info_transmitter.frame.payload_bit_count ==
		      V90_INFO0A_PAYLOAD_BITS &&
	      analog_end.transport.info_receiver.frame
		      .expected_payload_bit_count == V90_INFO0D_PAYLOAD_BITS &&
	      digital_end.transport.info_transmitter.frame.payload_bit_count ==
		      V90_INFO0D_PAYLOAD_BITS &&
	      digital_end.transport.info_receiver.frame
		      .expected_payload_bit_count == V90_INFO0A_PAYLOAD_BITS,
	      "V.90 INFO0 endpoints did not initialize with complementary roles");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v90_phase2_info0_exchange_event analog_event;
		struct v90_phase2_info0_exchange_event digital_event;
		enum phase2_info0_transmit_phase analog_before =
			analog_end.transport.transmit_phase;
		enum phase2_info0_transmit_phase digital_before =
			digital_end.transport.transmit_phase;
		int16_t next_analog;
		int16_t next_digital;

		CHECK(v90_phase2_info0_exchange_sample(
			      &analog_end,
			      codec_round_trip(digital_line, codec),
			      &next_analog, &analog_event) ==
			      V90_PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_info0_exchange_sample(
			      &digital_end,
			      codec_round_trip(analog_line, codec),
			      &next_digital, &digital_event) ==
			      V90_PHASE2_INFO0_EXCHANGE_OK,
		      "V.90 INFO0 exchange failed at sample %zu", index);
		observe(&analog_event, &analog_observation);
		observe(&digital_event, &digital_observation);
		if (index < PHASE2_INFO0_POST_V8_SILENCE_SAMPLES)
			CHECK(next_analog == 0 && next_digital == 0,
			      "post-V.8 silence ended at sample %zu", index);
		if (analog_before != analog_end.transport.transmit_phase &&
		    analog_end.transport.transmit_phase == PHASE2_INFO0_TONE)
			analog_tone_start = index + 1u;
		if (digital_before != digital_end.transport.transmit_phase &&
		    digital_end.transport.transmit_phase == PHASE2_INFO0_TONE)
			digital_tone_start = index + 1u;
		analog_line = next_analog;
		digital_line = next_digital;
		if (analog_observation.info_received == 1 &&
		    digital_observation.info_received == 1 &&
		    analog_observation.tone_started == 1 &&
		    digital_observation.tone_started == 1)
			break;
	}
	CHECK(analog_tone_start != SIZE_MAX &&
	      digital_tone_start != SIZE_MAX &&
	      analog_tone_start < digital_tone_start &&
	      analog_end.transport.transmit_phase == PHASE2_INFO0_TONE &&
	      digital_end.transport.transmit_phase == PHASE2_INFO0_TONE,
	      "unequal INFO0 frame timing or tone handoff changed");
	CHECK(analog_observation.info_received == 1 &&
	      analog_observation.info_rejected == 0 &&
	      digital_observation.info_received == 1 &&
	      digital_observation.info_rejected == 0 &&
	      analog_observation.tone_started == 1 &&
	      digital_observation.tone_started == 1 &&
	      digital_equal(
		      &analog_observation.peer_info0.digital,
		      &expected_digital) &&
	      analog_equal(
		      &digital_observation.peer_info0.analog,
		      &expected_analog),
	      "V.90 INFO0 records changed across codec %d", codec);
	return true;
}

static bool test_pcm_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_exchange(codec),
		      "V.90 INFO0 exchange failed for codec %d", codec);
	return true;
}

static bool test_validation(void)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(TEST_CODEC_PCMU);
	struct v90_phase2_info0_exchange exchange;
	struct v90_phase2_info0_exchange before;
	struct v90_phase2_info0_exchange_event event = {
		.peer_info0_event = V90_PEER_INFO0_REJECTED,
	};
	int16_t sample = 1234;

	analog.maximum_symbol_rate_difference_steps = 6;
	CHECK(v90_phase2_info0_exchange_init_analog(
		      &exchange, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "V.90 INFO0 exchange accepted invalid analog capabilities");
	digital.pcm_law = (enum v90_pcm_law)2;
	CHECK(v90_phase2_info0_exchange_init_digital(
		      &exchange, TEST_AMPLITUDE, &digital) ==
		      V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "V.90 INFO0 exchange accepted an invalid PCM law");
	analog = analog_capabilities();
	CHECK(v90_phase2_info0_exchange_init_analog(
		      &exchange, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_INFO0_EXCHANGE_OK,
	      "V.90 validation fixture did not initialize");
	exchange.transport.info_receiver.modulation.detector_output = NAN;
	before = exchange;
	CHECK(v90_phase2_info0_exchange_sample(
		      &exchange, 0, &sample, &event) ==
		      V90_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
	      sample == 1234 &&
	      event.peer_info0_event == V90_PEER_INFO0_REJECTED &&
	      strcmp(v90_phase2_info0_exchange_result_name(
		     V90_PHASE2_INFO0_EXCHANGE_OK), "ok") == 0 &&
	      strcmp(v90_phase2_info0_exchange_result_name(
		     (enum v90_phase2_info0_exchange_result)9), "unknown") == 0,
	      "invalid V.90 INFO0 state mutated outputs or result names");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "PCMU and PCMA exchange", test_pcm_exchange },
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
