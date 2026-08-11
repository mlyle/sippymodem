// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_info0_exchange.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLE_LIMIT = 3200,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct exchange_observation {
	size_t info_received_count;
	size_t info_rejected_count;
	size_t tone_started_count;
	size_t tone_reversed_count;
	size_t tone_ended_count;
	size_t info_sample;
	struct v34_info0 peer_info0;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	}
	return 0;
}

static struct v34_info0 calling_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.low_carrier_3000_supported = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.transmit_clock_source =
			V34_INFO0_CLOCK_INTERNAL,
		.acknowledges_peer_info0 = true,
	};
}

static struct v34_info0 answering_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.cme_modem = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			V34_INFO0_CLOCK_RECEIVE_TIMING,
		.acknowledges_peer_info0 = true,
	};
}

static bool info0_matches(
	const struct v34_info0 *left,
	const struct v34_info0 *right)
{
	uint8_t left_payload[V34_INFO0_PAYLOAD_BYTES];
	uint8_t right_payload[V34_INFO0_PAYLOAD_BYTES];

	return v34_info0_encode(
		       left, left_payload, sizeof(left_payload)) ==
		       V34_INFO0_OK &&
	       v34_info0_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       V34_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool observe_event(
	const struct v34_phase2_info0_exchange_event *event,
	size_t sample_index, struct exchange_observation *observation)
{
	switch (event->peer_info0_event) {
	case V34_PHASE2_PEER_INFO0_NO_EVENT:
		break;
	case V34_PHASE2_PEER_INFO0_RECEIVED:
		observation->info_received_count++;
		observation->info_sample = sample_index;
		observation->peer_info0 = event->peer_info0;
		break;
	case V34_PHASE2_PEER_INFO0_REJECTED:
		observation->info_rejected_count++;
		break;
	}
	switch (event->tone_event.kind) {
	case V34_PHASE2_TONE_NO_EVENT:
		break;
	case V34_PHASE2_TONE_STARTED:
		observation->tone_started_count++;
		break;
	case V34_PHASE2_TONE_PHASE_REVERSED:
		observation->tone_reversed_count++;
		break;
	case V34_PHASE2_TONE_ENDED:
		observation->tone_ended_count++;
		break;
	}
	return true;
}

static bool check_exchange(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_info0 expected_calling = calling;
	struct v34_info0 expected_answering = answering;
	struct v34_phase2_info0_exchange caller;
	struct v34_phase2_info0_exchange answerer;
	struct exchange_observation caller_observation = { 0 };
	struct exchange_observation answer_observation = { 0 };
	int16_t caller_line_sample = 0;
	int16_t answer_line_sample = 0;
	size_t caller_info_start = SIZE_MAX;
	size_t answer_info_start = SIZE_MAX;
	size_t caller_tone_start = SIZE_MAX;
	size_t answer_tone_start = SIZE_MAX;

	expected_calling.acknowledges_peer_info0 = false;
	expected_answering.acknowledges_peer_info0 = false;
	CHECK(v34_phase2_info0_exchange_init(
		      &caller, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &calling) ==
		      V34_PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_info0_exchange_init(
			      &answerer,
			      V34_ANSWERING_STATION,
			      TEST_AMPLITUDE, &answering) ==
			      V34_PHASE2_INFO0_EXCHANGE_OK,
	      "INFO0 exchange endpoints did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_info0_exchange_event caller_event;
		struct v34_phase2_info0_exchange_event answer_event;
		enum v34_phase2_info0_transmit_phase caller_before =
			caller.transmit_phase;
		enum v34_phase2_info0_transmit_phase answer_before =
			answerer.transmit_phase;
		int16_t next_caller_sample;
		int16_t next_answer_sample;

		CHECK(v34_phase2_info0_exchange_sample(
			      &caller,
			      codec_round_trip(answer_line_sample, codec),
			      &next_caller_sample, &caller_event) ==
			      V34_PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_info0_exchange_sample(
			      &answerer,
			      codec_round_trip(caller_line_sample, codec),
			      &next_answer_sample, &answer_event) ==
			      V34_PHASE2_INFO0_EXCHANGE_OK &&
		      observe_event(
			      &caller_event, index, &caller_observation) &&
		      observe_event(
			      &answer_event, index, &answer_observation),
		      "INFO0 exchange failed at sample %zu", index);
		if (index < V34_PHASE2_POST_V8_SILENCE_SAMPLES)
			CHECK(next_caller_sample == 0 && next_answer_sample == 0,
			      "post-V.8 silence ended at sample %zu", index);
		if (caller_before != caller.transmit_phase) {
			if (caller.transmit_phase ==
			    V34_PHASE2_INFO0_SEQUENCE)
				caller_info_start = index + 1u;
			if (caller.transmit_phase ==
			    V34_PHASE2_INFO0_TONE)
				caller_tone_start = index + 1u;
		}
		if (answer_before != answerer.transmit_phase) {
			if (answerer.transmit_phase ==
			    V34_PHASE2_INFO0_SEQUENCE)
				answer_info_start = index + 1u;
			if (answerer.transmit_phase ==
			    V34_PHASE2_INFO0_TONE)
				answer_tone_start = index + 1u;
		}
		caller_line_sample = next_caller_sample;
		answer_line_sample = next_answer_sample;
		if (caller_observation.info_received_count == 1 &&
		    answer_observation.info_received_count == 1 &&
		    caller_observation.tone_started_count == 1 &&
		    answer_observation.tone_started_count == 1 &&
		    index > caller_observation.info_sample + 200u &&
		    index > answer_observation.info_sample + 200u)
			break;
	}
	CHECK(caller_info_start ==
		      V34_PHASE2_POST_V8_SILENCE_SAMPLES &&
		      answer_info_start == caller_info_start &&
		      caller_tone_start > caller_info_start + 600u &&
		      caller_tone_start < caller_info_start + 750u &&
		      answer_tone_start == caller_tone_start,
	      "INFO0 transmit timing changed");
	CHECK(caller.transmit_phase == V34_PHASE2_INFO0_TONE &&
		      answerer.transmit_phase ==
			      V34_PHASE2_INFO0_TONE &&
		      caller.peer_info0_received && answerer.peer_info0_received,
	      "INFO0 exchange stopped at phases %d/%d peer %d/%d events %zu/%zu rejected %zu/%zu",
	      caller.transmit_phase, answerer.transmit_phase,
	      caller.peer_info0_received, answerer.peer_info0_received,
	      caller_observation.info_received_count,
	      answer_observation.info_received_count,
	      caller_observation.info_rejected_count,
	      answer_observation.info_rejected_count);
	CHECK(caller_observation.info_received_count == 1 &&
		      caller_observation.info_rejected_count == 0 &&
		      answer_observation.info_received_count == 1 &&
		      answer_observation.info_rejected_count == 0 &&
		      info0_matches(
			      &caller_observation.peer_info0,
			      &expected_answering) &&
		      info0_matches(
			      &answer_observation.peer_info0,
			      &expected_calling),
	      "INFO0 capabilities changed across codec %d", codec);
	CHECK(caller_observation.tone_started_count == 1 &&
		      answer_observation.tone_started_count == 1 &&
		      caller_observation.tone_reversed_count == 0 &&
		      answer_observation.tone_reversed_count == 0 &&
		      caller_observation.tone_ended_count == 0 &&
		      answer_observation.tone_ended_count == 0,
	      "initial INFO modulation produced a false tone edge");
	return true;
}

static bool test_bidirectional_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_exchange(codec),
		      "bidirectional INFO0 exchange failed for codec %d", codec);
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 capabilities = calling_capabilities();
	struct v34_phase2_info0_exchange exchange;
	struct v34_phase2_info0_exchange before;
	struct v34_phase2_info0_exchange_event event = {
		.peer_info0_event = V34_PHASE2_PEER_INFO0_REJECTED,
	};
	int16_t sample = 1234;

	capabilities.maximum_symbol_rate_difference_steps = 6;
	CHECK(v34_phase2_info0_exchange_init(
		      &exchange, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &capabilities) ==
		      V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "INFO0 exchange accepted invalid capabilities");
	capabilities = calling_capabilities();
	CHECK(v34_phase2_info0_exchange_init(
		      &exchange, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &capabilities) ==
		      V34_PHASE2_INFO0_EXCHANGE_OK,
	      "INFO0 validation fixture did not initialize");
	exchange.info_receiver.modulation.detector_output = NAN;
	before = exchange;
	CHECK(v34_phase2_info0_exchange_sample(
		      &exchange, 0, &sample, &event) ==
		      V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT &&
		      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
		      sample == 1234 &&
		      event.peer_info0_event ==
			      V34_PHASE2_PEER_INFO0_REJECTED,
	      "invalid INFO0 exchange mutated state or outputs");
	CHECK(strcmp(
		      v34_phase2_info0_exchange_result_name(
			      V34_PHASE2_INFO0_EXCHANGE_OK),
		      "ok") == 0 &&
		      strcmp(
			      v34_phase2_info0_exchange_result_name(
				      (enum v34_phase2_info0_exchange_result)99),
			      "unknown") == 0,
	      "INFO0 exchange result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional exchange", test_bidirectional_exchange },
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
