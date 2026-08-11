// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_info1_exchange.h>

#include "v34_phase2_test_pair.h"
#include "test.h"

#include <g711.h>

#include <limits.h>
#include <string.h>

enum {
	TEST_SAMPLE_LIMIT = 40000,
};

struct info1_exchange_pair {
	struct test_pair channel;
	struct sippymodem_v34_phase2_info1_exchange caller;
	struct sippymodem_v34_phase2_info1_exchange answerer;
};

struct info1_exchange_pair_event {
	struct sippymodem_v34_phase2_info1_exchange_event caller;
	struct sippymodem_v34_phase2_info1_exchange_event answerer;
	int16_t caller_transmit;
	int16_t answerer_transmit;
};

static struct sippymodem_v34_info1c info1c_fixture(void)
{
	struct sippymodem_v34_info1c info1c = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = -23,
		},
	};

	for (size_t index = 0; index < SIPPYMODEM_V34_SYMBOL_RATE_COUNT;
	     ++index) {
		info1c.answer_transmit[index] =
			(struct sippymodem_v34_info1_probe_result){
				.high_carrier = (index & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(2u * index),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(6u + index),
			};
	}
	return info1c;
}

static struct sippymodem_v34_info1a info1a_fixture(void)
{
	return (struct sippymodem_v34_info1a){
		.common = {
			.minimum_peer_power_reduction_db = 3,
			.additional_peer_power_reduction_db = 2,
			.local_md_length_35_ms_units = 5,
			.frequency_offset_1_50_hz_units = 31,
		},
		.call_transmit = { true, 4, 10 },
		.answer_to_call_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3200,
		.call_to_answer_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3000,
	};
}

static bool info1c_equal(
	const struct sippymodem_v34_info1c *left,
	const struct sippymodem_v34_info1c *right)
{
	uint8_t left_payload[SIPPYMODEM_V34_INFO1C_PAYLOAD_BYTES];
	uint8_t right_payload[SIPPYMODEM_V34_INFO1C_PAYLOAD_BYTES];

	return sippymodem_v34_info1c_encode(
		       left, left_payload, sizeof(left_payload)) ==
		       SIPPYMODEM_V34_INFO1_OK &&
	       sippymodem_v34_info1c_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       SIPPYMODEM_V34_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool info1a_equal(
	const struct sippymodem_v34_info1a *left,
	const struct sippymodem_v34_info1a *right)
{
	uint8_t left_payload[SIPPYMODEM_V34_INFO1A_PAYLOAD_BYTES];
	uint8_t right_payload[SIPPYMODEM_V34_INFO1A_PAYLOAD_BYTES];

	return sippymodem_v34_info1a_encode(
		       left, left_payload, sizeof(left_payload)) ==
		       SIPPYMODEM_V34_INFO1_OK &&
	       sippymodem_v34_info1a_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       SIPPYMODEM_V34_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	}
	return 0;
}

static int16_t delay_line_receive(const struct test_delay_line *line)
{
	return line->samples[line->cursor];
}

static void delay_line_transmit(
	struct test_delay_line *line, int16_t sample)
{
	line->samples[line->cursor] = sample;
	line->cursor = (line->cursor + 1u) % line->delay_samples;
}

static bool probes_are_complete(const struct test_pair *pair)
{
	return pair->caller.stage == TEST_ENDPOINT_RECIPROCAL_PROBE &&
	       pair->answerer.stage == TEST_ENDPOINT_RECIPROCAL_PROBE &&
	       pair->caller.reciprocal_probe.phase ==
		       SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE &&
	       pair->answerer.reciprocal_probe.phase ==
		       SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A;
}

static bool completed_probe_pair(
	struct test_pair *pair, enum test_codec codec, size_t delay_samples)
{
	if (!test_v34_phase2_pair_init(pair, codec, delay_samples))
		return false;
	pair->caller.continue_to_turnaround = true;
	pair->answerer.continue_to_turnaround = true;
	pair->caller.continue_to_reciprocal_probe = true;
	pair->answerer.continue_to_reciprocal_probe = true;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;

		if (!test_v34_phase2_pair_sample(pair, &event))
			return false;
		if (probes_are_complete(pair))
			return true;
	}
	return false;
}

static bool exchange_pair_init(
	struct info1_exchange_pair *pair, enum test_codec codec,
	size_t delay_samples, const struct sippymodem_v34_info1c *info1c)
{
	struct info1_exchange_pair initialized;

	memset(&initialized, 0, sizeof(initialized));
	if (!completed_probe_pair(
		    &initialized.channel, codec, delay_samples) ||
	    sippymodem_v34_phase2_info1_exchange_caller_init(
		    &initialized.caller,
		    &initialized.channel.caller.reciprocal_probe, info1c) !=
		    SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK ||
	    sippymodem_v34_phase2_info1_exchange_answerer_init(
		    &initialized.answerer,
		    &initialized.channel.answerer.reciprocal_probe) !=
		    SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK)
		return false;
	*pair = initialized;
	return true;
}

static bool exchange_pair_sample(
	struct info1_exchange_pair *pair,
	struct info1_exchange_pair_event *event)
{
	struct info1_exchange_pair_event emitted = { 0 };
	int16_t caller_receive =
		delay_line_receive(&pair->channel.answerer_to_caller);
	int16_t answerer_receive =
		delay_line_receive(&pair->channel.caller_to_answerer);

	if (sippymodem_v34_phase2_info1_exchange_sample(
		    &pair->caller, caller_receive, &emitted.caller_transmit,
		    &emitted.caller) != SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK ||
	    sippymodem_v34_phase2_info1_exchange_sample(
		    &pair->answerer, answerer_receive,
		    &emitted.answerer_transmit, &emitted.answerer) !=
		    SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK)
		return false;
	delay_line_transmit(
		&pair->channel.caller_to_answerer,
		codec_round_trip(emitted.caller_transmit, pair->channel.codec));
	delay_line_transmit(
		&pair->channel.answerer_to_caller,
		codec_round_trip(emitted.answerer_transmit, pair->channel.codec));
	*event = emitted;
	return true;
}

static bool check_bidirectional_exchange(
	enum test_codec codec, size_t delay_samples)
{
	struct sippymodem_v34_info1c info1c = info1c_fixture();
	struct sippymodem_v34_info1a info1a = info1a_fixture();
	struct info1_exchange_pair pair;
	size_t caller_sent = SIZE_MAX;
	size_t answerer_received = SIZE_MAX;
	size_t answerer_sent = SIZE_MAX;
	size_t caller_received = SIZE_MAX;

	CHECK(exchange_pair_init(&pair, codec, delay_samples, &info1c),
	      "INFO1 endpoints did not initialize for codec %d delay %zu",
	      codec, delay_samples);
	CHECK(pair.caller.nominal_amplitude == TEST_AMPLITUDE &&
	      pair.answerer.nominal_amplitude == TEST_AMPLITUDE,
	      "INFO1 handoff did not preserve nominal power");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct info1_exchange_pair_event event;

		CHECK(exchange_pair_sample(&pair, &event),
		      "INFO1 exchange failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);
		if (event.caller.kind ==
		    SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1C_SENT)
			caller_sent = index;
		if (event.answerer.kind ==
		    SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1C_RECEIVED) {
			answerer_received = index;
			CHECK(info1c_equal(
				      &event.answerer.peer_parameters.info1c,
				      &info1c) &&
			      sippymodem_v34_phase2_info1_exchange_answerer_reply(
				      &pair.answerer, &info1a) ==
				      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
			      "answerer did not accept decoded INFO1c");
			CHECK(pair.answerer.transmitter.modulation.carrier
				      .carrier_phase +
				      UINT32_C(0x40000000) ==
				      pair.answerer.answerer_tone_a.carrier_phase &&
			      pair.answerer.transmitter.modulation.guard_phase ==
				      pair.answerer.answerer_tone_a.guard_phase,
			      "Tone A to INFO1a phase continuity changed");
		}
		if (event.answerer.kind ==
		    SIPPYMODEM_V34_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT)
			answerer_sent = index;
		if (event.caller.kind ==
		    SIPPYMODEM_V34_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			caller_received = index;
			CHECK(info1a_equal(
				      &event.caller.peer_parameters.info1a, &info1a),
			      "caller decoded different INFO1a parameters");
		}
		if (pair.caller.phase == SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE &&
		    pair.answerer.phase == SIPPYMODEM_V34_PHASE2_INFO1_COMPLETE)
			break;
	}
	CHECK(caller_sent < answerer_received &&
	      answerer_received < answerer_sent &&
	      answerer_sent < caller_received,
	      "INFO1 event order changed: %zu/%zu/%zu/%zu",
	      caller_sent, answerer_received, answerer_sent, caller_received);
	CHECK(pair.caller.carrier_tail_symbols ==
		      SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
	      pair.answerer.carrier_tail_symbols ==
		      SIPPYMODEM_V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS &&
	      pair.caller.peer_parameters_received &&
	      pair.answerer.peer_parameters_received &&
	      info1a_equal(&pair.caller.peer_parameters.info1a, &info1a) &&
	      info1c_equal(&pair.answerer.peer_parameters.info1c, &info1c) &&
	      sippymodem_v34_phase2_info1_exchange_is_valid(&pair.caller) &&
	      sippymodem_v34_phase2_info1_exchange_is_valid(&pair.answerer),
	      "completed INFO1 state lost its parameters or causal tail");
	{
		struct sippymodem_v34_phase2_info1_exchange caller = pair.caller;
		struct sippymodem_v34_phase2_info1_exchange answerer = pair.answerer;
		struct info1_exchange_pair_event event;

		CHECK(exchange_pair_sample(&pair, &event) &&
		      event.caller_transmit == 0 && event.answerer_transmit == 0 &&
		      event.caller.kind == SIPPYMODEM_V34_PHASE2_INFO1_EVENT_NONE &&
		      event.answerer.kind == SIPPYMODEM_V34_PHASE2_INFO1_EVENT_NONE &&
		      memcmp(&pair.caller, &caller, sizeof(caller)) == 0 &&
		      memcmp(&pair.answerer, &answerer, sizeof(answerer)) == 0,
		      "completed INFO1 endpoints were not silent and stable");
	}
	return true;
}

static bool test_bidirectional_exchange(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
	} cases[] = {
		{ TEST_CODEC_LINEAR, 1 },
		{ TEST_CODEC_PCMU, 13 },
		{ TEST_CODEC_PCMA, 47 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_bidirectional_exchange(
			      cases[index].codec, cases[index].delay_samples),
		      "INFO1 exchange case %zu failed", index);
	return true;
}

static bool test_exact_deadlines(void)
{
	struct sippymodem_v34_info1c info1c = info1c_fixture();
	struct test_pair probes;
	struct sippymodem_v34_phase2_info1_exchange caller;
	struct sippymodem_v34_phase2_info1_exchange answerer;
	struct sippymodem_v34_phase2_info1_exchange terminal;
	struct sippymodem_v34_phase2_info1_exchange_event event;
	uint32_t caller_deadline;
	uint32_t answerer_deadline;
	uint32_t answerer_initial_elapsed;
	size_t caller_wait_samples = 0;
	size_t answerer_wait_samples = 0;
	int16_t transmit;

	CHECK(completed_probe_pair(&probes, TEST_CODEC_LINEAR, 13) &&
	      sippymodem_v34_phase2_info1_exchange_caller_init(
		      &caller, &probes.caller.reciprocal_probe, &info1c) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK &&
	      sippymodem_v34_phase2_info1_exchange_answerer_init(
		      &answerer, &probes.answerer.reciprocal_probe) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
	      "INFO1 timeout fixtures did not initialize");
	caller_deadline =
		(uint32_t)SIPPYMODEM_V34_PHASE2_CALLER_INFO1A_WAIT_BASE_SAMPLES +
		caller.round_trip_delay_samples;
	answerer_deadline =
		(uint32_t)SIPPYMODEM_V34_PHASE2_ANSWERER_INFO1C_WAIT_BASE_SAMPLES +
		2u * answerer.round_trip_delay_samples;
	answerer_initial_elapsed = answerer.peer_deadline_elapsed_samples;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		enum sippymodem_v34_phase2_info1_exchange_phase before =
			caller.phase;

		CHECK(sippymodem_v34_phase2_info1_exchange_sample(
			      &caller, 0, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
		      "caller timeout failed at sample %zu", index);
		if (before == SIPPYMODEM_V34_PHASE2_INFO1_WAITING_FOR_PEER)
			caller_wait_samples++;
		if (caller.phase == SIPPYMODEM_V34_PHASE2_INFO1_FAILED)
			break;
	}
	CHECK(caller_wait_samples == caller_deadline &&
	      caller.peer_deadline_elapsed_samples == caller_deadline &&
	      caller.failure == SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INFO1_EVENT_FAILED,
	      "caller did not fail exactly 700 ms plus RTD after INFO1c");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		CHECK(sippymodem_v34_phase2_info1_exchange_sample(
			      &answerer, 0, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
		      "answerer timeout failed at sample %zu", index);
		answerer_wait_samples++;
		if (answerer.phase == SIPPYMODEM_V34_PHASE2_INFO1_FAILED)
			break;
	}
	CHECK(answerer_wait_samples ==
		      answerer_deadline - answerer_initial_elapsed &&
	      answerer.peer_deadline_elapsed_samples == answerer_deadline &&
	      answerer.failure == SIPPYMODEM_V34_PHASE2_INFO1_PEER_TIMEOUT &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INFO1_EVENT_FAILED,
	      "answerer did not preserve the 2000 ms plus two-RTD epoch");

	terminal = caller;
	transmit = 1234;
	CHECK(sippymodem_v34_phase2_info1_exchange_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK &&
	      transmit == 0 &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INFO1_EVENT_NONE &&
	      memcmp(&caller, &terminal, sizeof(caller)) == 0,
	      "failed INFO1 controller was not silent and stable");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_info1c info1c = info1c_fixture();
	struct sippymodem_v34_info1c invalid_info1c = info1c;
	struct sippymodem_v34_info1a info1a = info1a_fixture();
	struct test_pair probes;
	struct sippymodem_v34_phase2_info1_exchange exchange;
	struct sippymodem_v34_phase2_info1_exchange before;
	struct sippymodem_v34_phase2_info1_exchange invalid;
	struct sippymodem_v34_phase2_info1_exchange_event event;
	struct sippymodem_v34_phase2_info1_exchange_event event_before;
	int16_t transmit = 1234;

	CHECK(completed_probe_pair(&probes, TEST_CODEC_LINEAR, 1),
	      "INFO1 validation probes did not complete");
	invalid_info1c.common.frequency_offset_1_50_hz_units = -513;
	memset(&exchange, 0xa5, sizeof(exchange));
	before = exchange;
	CHECK(sippymodem_v34_phase2_info1_exchange_caller_init(
		      &exchange, &probes.answerer.reciprocal_probe, &info1c) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info1_exchange_caller_init(
		      &exchange, &probes.caller.reciprocal_probe,
		      &invalid_info1c) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info1_exchange_caller_init(
		      NULL, &probes.caller.reciprocal_probe, &info1c) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0,
	      "invalid INFO1 initialization mutated output state");
	CHECK(sippymodem_v34_phase2_info1_exchange_caller_init(
		      &exchange, &probes.caller.reciprocal_probe, &info1c) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
	      "valid INFO1 caller did not initialize");
	before = exchange;
	memset(&event, 0x5a, sizeof(event));
	event_before = event;
	CHECK(sippymodem_v34_phase2_info1_exchange_sample(
		      &exchange, 0, NULL, &event) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info1_exchange_sample(
		      &exchange, 0, &transmit, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info1_exchange_answerer_reply(
		      &exchange, &info1a) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      transmit == 1234,
	      "invalid INFO1 operation was not transactional");
	invalid = exchange;
	invalid.peer_parameters_received = true;
	CHECK(!sippymodem_v34_phase2_info1_exchange_is_valid(&invalid),
	      "INFO1 validation accepted parameters without a peer event");
	CHECK(sippymodem_v34_phase2_info1_exchange_answerer_init(
		      &exchange, &probes.answerer.reciprocal_probe) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_OK,
	      "valid INFO1 answerer did not initialize");
	before = exchange;
	CHECK(sippymodem_v34_phase2_info1_exchange_answerer_reply(
		      &exchange, &info1a) ==
		      SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
	      strcmp(sippymodem_v34_phase2_info1_exchange_result_name(
			     SIPPYMODEM_V34_PHASE2_INFO1_EXCHANGE_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_phase2_info1_exchange_result_name(
			     (enum sippymodem_v34_phase2_info1_exchange_result)99),
		     "unknown") == 0,
	      "INFO1 reply validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional exchange", test_bidirectional_exchange },
		{ "exact peer deadlines", test_exact_deadlines },
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
