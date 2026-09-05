// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_info0_exchange.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLE_LIMIT = 5000,
	TEST_ANSWER_START_DELAY =
		V34_PHASE2_SAMPLE_RATE * 15 / 1000,
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

static bool check_exchange(
	enum test_codec codec, size_t answer_start_delay)
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
		struct v34_phase2_info0_exchange_event answer_event = { 0 };
		enum phase2_info0_transmit_phase caller_before =
			caller.transport.transmit_phase;
		enum phase2_info0_transmit_phase answer_before =
			answerer.transport.transmit_phase;
		int16_t next_caller_sample;
		int16_t next_answer_sample = 0;

		CHECK(v34_phase2_info0_exchange_sample(
			      &caller,
			      codec_round_trip(answer_line_sample, codec),
			      &next_caller_sample, &caller_event) ==
			      V34_PHASE2_INFO0_EXCHANGE_OK &&
		      observe_event(
			      &caller_event, index, &caller_observation),
		      "INFO0 exchange failed at sample %zu", index);
		if (index >= answer_start_delay)
			CHECK(v34_phase2_info0_exchange_sample(
				      &answerer,
				      codec_round_trip(caller_line_sample, codec),
				      &next_answer_sample, &answer_event) ==
				      V34_PHASE2_INFO0_EXCHANGE_OK &&
			      observe_event(
				      &answer_event, index,
				      &answer_observation),
			      "delayed INFO0 answerer failed at sample %zu",
			      index);
		if (index < V34_PHASE2_POST_V8_SILENCE_SAMPLES)
			CHECK(next_caller_sample == 0,
			      "post-V.8 silence ended at sample %zu", index);
		if (index < answer_start_delay +
				    V34_PHASE2_POST_V8_SILENCE_SAMPLES)
			CHECK(next_answer_sample == 0,
			      "delayed post-V.8 silence ended at sample %zu",
			      index);
		if (caller_before != caller.transport.transmit_phase) {
			if (caller.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE)
				caller_info_start = index + 1u;
			if (caller.transport.transmit_phase ==
			    PHASE2_INFO0_TONE)
				caller_tone_start = index + 1u;
		}
		if (index >= answer_start_delay &&
		    answer_before != answerer.transport.transmit_phase) {
			if (answerer.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE)
				answer_info_start = index + 1u;
			if (answerer.transport.transmit_phase ==
			    PHASE2_INFO0_TONE)
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
		      answer_info_start == answer_start_delay +
			      V34_PHASE2_POST_V8_SILENCE_SAMPLES,
	      "INFO0 transmit guard changed for delay %zu",
	      answer_start_delay);
	CHECK(caller_tone_start > caller_info_start + 600u &&
		      answer_tone_start > answer_info_start + 600u,
	      "INFO0 transmit timing changed");
	if (answer_start_delay == 0)
		CHECK(caller_tone_start < caller_info_start + 750u &&
			      answer_tone_start == caller_tone_start,
		      "synchronous INFO0 timing changed");
	CHECK(caller.transport.transmit_phase == PHASE2_INFO0_TONE &&
		      answerer.transport.transmit_phase ==
			      PHASE2_INFO0_TONE &&
		      caller.peer_info0_received && answerer.peer_info0_received,
	      "INFO0 exchange stopped at phases %d/%d peer %d/%d events %zu/%zu rejected %zu/%zu",
	      caller.transport.transmit_phase,
	      answerer.transport.transmit_phase,
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
		CHECK(check_exchange(codec, 0) &&
			      check_exchange(
				      codec, TEST_ANSWER_START_DELAY),
		      "bidirectional INFO0 exchange failed for codec %d", codec);
	return true;
}

static bool test_reversal_during_recovery_info(void)
{
	struct v34_info0 capabilities = calling_capabilities();
	struct phase2_info0_exchange baseline;
	struct phase2_info0_exchange reversed;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	bool reached_tone = false;

	CHECK(v34_info0_encode(
		      &capabilities, payload, sizeof(payload)) == V34_INFO0_OK &&
	      phase2_info0_exchange_init(
		      &baseline, PHASE2_SIGNAL_B, TEST_AMPLITUDE,
		      payload, V34_INFO0_PAYLOAD_BITS,
		      V34_INFO0_PAYLOAD_BITS) == PHASE2_INFO0_EXCHANGE_OK &&
	      phase2_info0_exchange_start_tone(&baseline) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "recovery reversal fixture did not initialize");
	reversed = baseline;
	CHECK(phase2_info0_exchange_restart_info(
		      &baseline, payload, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      phase2_info0_exchange_restart_info(
		      &reversed, payload, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      v34_phase2_tone_transmitter_reverse(
		      &reversed.tone_transmitter) == V34_PHASE2_TONE_OK,
	      "recovery reversal did not start");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event baseline_event;
		struct phase2_info0_exchange_event reversed_event;
		enum phase2_info0_transmit_phase transmit_before =
			baseline.transmit_phase;
		int16_t baseline_sample;
		int16_t reversed_sample;

		CHECK(phase2_info0_exchange_sample(
			      &baseline, 0, &baseline_sample,
			      &baseline_event) == PHASE2_INFO0_EXCHANGE_OK &&
		      phase2_info0_exchange_sample(
			      &reversed, 0, &reversed_sample,
			      &reversed_event) == PHASE2_INFO0_EXCHANGE_OK,
		      "recovery INFO failed at sample %zu", index);
		if (transmit_before != PHASE2_INFO0_TONE) {
			CHECK(reversed.transmit_phase == baseline.transmit_phase &&
			      reversed_sample == baseline_sample,
			      "hidden reversal changed INFO at sample %zu", index);
			continue;
		}
		CHECK(reversed.transmit_phase == PHASE2_INFO0_TONE &&
		      reversed.tone_transmitter.carrier_phase ==
			      baseline.tone_transmitter.carrier_phase +
				      UINT32_C(0x80000000) &&
		      reversed_sample == -baseline_sample,
		      "recovery handoff lost the retained reversal: phase %08x/%08x sample %d/%d",
		      reversed.tone_transmitter.carrier_phase,
		      baseline.tone_transmitter.carrier_phase,
		      reversed_sample, baseline_sample);
		reached_tone = true;
		break;
	}
	CHECK(reached_tone, "recovery INFO did not return to tone");
	return true;
}

static bool test_retained_receive_level(void)
{
	enum { REPEAT, MISMATCH, INVALID_INITIAL, BAD_CRC_INITIAL };

	for (unsigned int test_case = REPEAT;
	     test_case <= BAD_CRC_INITIAL; ++test_case) {
		struct v34_info0 local = calling_capabilities();
		struct v34_info0 retained = answering_capabilities();
		struct v34_info0 peer_record = retained;
		struct v34_phase2_info0_exchange exchange;
		struct phase2_info0_exchange peer;
		uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
		bool received = false;
		bool corrupted = false;
		bool initial = test_case >= INVALID_INITIAL;
		enum v34_phase2_info0_exchange_result initialized = initial ?
			v34_phase2_info0_exchange_init(&exchange,
				V34_CALLING_STATION, TEST_AMPLITUDE, &local) :
			v34_phase2_info0_exchange_init_retrain(&exchange,
				V34_CALLING_STATION, TEST_AMPLITUDE,
				&local, &retained);

		if (test_case == MISMATCH)
			peer_record.symbol_rate_2743_supported =
				!peer_record.symbol_rate_2743_supported;
		CHECK(initialized == V34_PHASE2_INFO0_EXCHANGE_OK &&
		      v34_info0_encode(&peer_record, payload, sizeof(payload)) ==
			V34_INFO0_OK, "receive level fixture did not encode");
		if (test_case == INVALID_INITIAL)
			payload[1] |= 0x70; /* Invalid symbol-rate difference 7. */
		CHECK(phase2_info0_exchange_init(
			&peer, PHASE2_SIGNAL_A, TEST_AMPLITUDE, payload,
			V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
			PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_tone_detector_set_receive_gain(
			&exchange.transport.tone_detector, 8192) ==
			V34_PHASE2_TONE_OK,
		      "retained receive level fixture did not initialize");
		for (size_t i = 0; i < TEST_SAMPLE_LIMIT && !received; ++i) {
			struct phase2_info0_exchange_event peer_event;
			struct v34_phase2_info0_exchange_event event;
			struct v34_phase2_info_receiver *frame =
				&exchange.transport.info_receiver.frame;
			int16_t sample, ignored;

			if (test_case == BAD_CRC_INITIAL && !corrupted &&
			    frame->stage == V34_PHASE2_INFO_RECEIVE_CRC &&
			    frame->crc_bit_count == 1) {
				frame->received_crc ^= 1;
				corrupted = true;
			}
			CHECK(phase2_info0_exchange_sample(
				&peer, 0, &sample, &peer_event) ==
				PHASE2_INFO0_EXCHANGE_OK &&
			      v34_phase2_info0_exchange_sample(
				&exchange,
				codec_round_trip(sample, TEST_CODEC_PCMU),
				&ignored, &event) ==
				V34_PHASE2_INFO0_EXCHANGE_OK,
			      "retained receive level input failed");
			CHECK(exchange.transport.tone_detector.carrier.profile
				.minimum_line_power == 1024.0 &&
			      (initial ? !exchange.peer_info0_received :
				info0_matches(&exchange.peer_info0, &retained)),
			      "unqualified INFO0 changed receive level or record");
			received = event.peer_info0_event == (test_case == REPEAT ?
				V34_PHASE2_PEER_INFO0_RECEIVED :
				V34_PHASE2_PEER_INFO0_REJECTED);
		}
		CHECK(received && (test_case != BAD_CRC_INITIAL || corrupted),
		      "receive level case %u missed expected event", test_case);
	}
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

	CHECK(phase2_signal_role_is_valid(PHASE2_SIGNAL_A) &&
	      phase2_signal_role_is_valid(PHASE2_SIGNAL_B) &&
	      !phase2_signal_role_is_valid((enum phase2_signal_role)-1) &&
	      v34_phase2_signal_for_station(V34_CALLING_STATION) ==
		      PHASE2_SIGNAL_B &&
	      v34_phase2_signal_for_station(V34_ANSWERING_STATION) ==
		      PHASE2_SIGNAL_A &&
	      v34_phase2_station_for_signal(PHASE2_SIGNAL_B) ==
		      V34_CALLING_STATION &&
	      v34_phase2_station_for_signal(PHASE2_SIGNAL_A) ==
		      V34_ANSWERING_STATION &&
	      !phase2_signal_role_is_valid(v34_phase2_signal_for_station(
		      (enum v34_station_role)-1)) &&
	      !v34_station_role_is_valid(v34_phase2_station_for_signal(
		      (enum phase2_signal_role)-1)),
	      "Phase 2 role mappings changed");
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
	exchange.transport.info_receiver.modulation.detector_output = NAN;
	before = exchange;
	CHECK(v34_phase2_info0_exchange_sample(
		      &exchange, 0, &sample, &event) ==
		      V34_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT &&
		      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
		      sample == 1234 &&
		      event.peer_info0_event ==
			      V34_PHASE2_PEER_INFO0_REJECTED,
	      "invalid INFO0 exchange mutated state or outputs");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional exchange", test_bidirectional_exchange },
		{ "reversal during recovery INFO",
		  test_reversal_during_recovery_info },
		{ "retained receive level", test_retained_receive_level },
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
