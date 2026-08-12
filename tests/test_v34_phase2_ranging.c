// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_ranging.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_MAXIMUM_LINE_DELAY = 48,
	TEST_SAMPLE_LIMIT = 6000,
	TEST_EDGE_TOLERANCE_SAMPLES = 4,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_delay_line {
	int16_t samples[TEST_MAXIMUM_LINE_DELAY];
	size_t delay_samples;
	size_t cursor;
};

struct ranging_observation {
	size_t answer_tone_start_sample;
	size_t first_answer_reversal_sample;
	size_t caller_reversal_sample;
	size_t second_answer_reversal_sample;
	size_t answer_probe_ready_sample;
	size_t caller_probe_ready_sample;
	uint16_t answer_round_trip_delay_samples;
	uint16_t caller_round_trip_delay_samples;
	bool answer_delay_measured;
	bool caller_probe_ready;
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

static struct v34_info0 calling_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_2800_supported = true,
		.low_carrier_3000_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

static struct v34_info0 answering_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static void initialize_observation(struct ranging_observation *observation)
{
	*observation = (struct ranging_observation){
		.answer_tone_start_sample = SIZE_MAX,
		.first_answer_reversal_sample = SIZE_MAX,
		.caller_reversal_sample = SIZE_MAX,
		.second_answer_reversal_sample = SIZE_MAX,
		.answer_probe_ready_sample = SIZE_MAX,
		.caller_probe_ready_sample = SIZE_MAX,
	};
}

static void observe_ranging_events(
	const struct v34_phase2_ranging_event *caller_event,
	const struct v34_phase2_ranging_event *answer_event,
	size_t sample_index,
	struct ranging_observation *observation)
{
	if (answer_event->kind ==
	    PHASE2_ROUND_TRIP_DELAY_MEASURED) {
		observation->answer_delay_measured = true;
		observation->answer_round_trip_delay_samples =
			answer_event->round_trip_delay_samples;
	}
	if (caller_event->kind == PHASE2_PROBE_RECEIVE_READY) {
		observation->caller_probe_ready = true;
		observation->caller_probe_ready_sample = sample_index;
		observation->caller_round_trip_delay_samples =
			caller_event->round_trip_delay_samples;
	}
	if (answer_event->kind == PHASE2_PROBE_TRANSMIT_READY)
		observation->answer_probe_ready_sample = sample_index;
}

static void observe_phase_transition(
	enum phase2_ranging_phase before,
	enum phase2_info0_transmit_phase transmit_before,
	const struct v34_phase2_ranging *after,
	bool caller, size_t sample_index,
	struct ranging_observation *observation)
{
	if (!caller &&
	    transmit_before != PHASE2_INFO0_TONE &&
	    after->info0_exchange.transport.transmit_phase ==
		    PHASE2_INFO0_TONE &&
	    observation->answer_tone_start_sample == SIZE_MAX)
		observation->answer_tone_start_sample = sample_index + 1u;
	if (before == after->control.phase)
		return;
	if (caller &&
	    after->control.phase ==
		    PHASE2_RESPONSE_TONE_TAIL)
		observation->caller_reversal_sample = sample_index;
	if (!caller &&
	    after->control.phase ==
		    PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
		observation->first_answer_reversal_sample = sample_index;
	if (!caller &&
	    after->control.phase ==
		    PHASE2_RESPONSE_TONE_TAIL)
		observation->second_answer_reversal_sample = sample_index;
}

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
}

static bool check_ranging(enum test_codec codec, size_t line_delay_samples)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct v34_phase2_ranging answerer;
	struct test_delay_line caller_to_answer = {
		.delay_samples = line_delay_samples,
	};
	struct test_delay_line answer_to_caller = {
		.delay_samples = line_delay_samples,
	};
	struct ranging_observation observation;
	size_t expected_round_trip = line_delay_samples * 2u;

	initialize_observation(&observation);
	CHECK(line_delay_samples != 0 &&
		      line_delay_samples <= TEST_MAXIMUM_LINE_DELAY,
	      "invalid ranging-test line delay");
	CHECK(v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &calling) ==
		      V34_PHASE2_RANGING_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE, &answering) ==
		      V34_PHASE2_RANGING_OK,
	      "ranging endpoints did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_ranging_event caller_event;
		struct v34_phase2_ranging_event answer_event;
		enum phase2_ranging_phase caller_before =
			caller.control.phase;
		enum phase2_ranging_phase answer_before =
			answerer.control.phase;
		enum phase2_info0_transmit_phase
			caller_transmit_before =
				caller.info0_exchange.transport.transmit_phase;
		enum phase2_info0_transmit_phase
			answer_transmit_before =
				answerer.info0_exchange.transport.transmit_phase;
		int16_t caller_transmit;
		int16_t answer_transmit;

		CHECK(v34_phase2_ranging_sample(
			      &caller,
			      delay_line_receive(&answer_to_caller),
			      &caller_transmit, &caller_event) ==
			      V34_PHASE2_RANGING_OK &&
		      v34_phase2_ranging_sample(
			      &answerer,
			      delay_line_receive(&caller_to_answer),
			      &answer_transmit, &answer_event) ==
			      V34_PHASE2_RANGING_OK,
		      "ranging failed for codec %d delay %zu at sample %zu",
		      codec, line_delay_samples, index);
		if (caller_before ==
		    PHASE2_RANGING_PROBE_READY)
			CHECK(caller_transmit == 0,
			      "caller emitted audio after its probe boundary");
		if (answer_before ==
		    PHASE2_RANGING_PROBE_READY)
			CHECK(answer_transmit == 0,
			      "answerer emitted placeholder audio at its probe boundary");
		delay_line_transmit(
			&caller_to_answer,
			codec_round_trip(caller_transmit, codec));
		delay_line_transmit(
			&answer_to_caller,
			codec_round_trip(answer_transmit, codec));
		observe_phase_transition(
			caller_before, caller_transmit_before, &caller, true, index,
			&observation);
		observe_phase_transition(
			answer_before, answer_transmit_before, &answerer, false,
			index, &observation);
		observe_ranging_events(
			&caller_event, &answer_event, index, &observation);
		if (caller.control.phase ==
			    PHASE2_RANGING_PROBE_READY &&
		    answerer.control.phase ==
			    PHASE2_RANGING_PROBE_READY &&
		    index > observation.answer_probe_ready_sample + 1u)
			break;
	}
	CHECK(caller.control.phase ==
		      PHASE2_RANGING_PROBE_READY &&
	      answerer.control.phase ==
		      PHASE2_RANGING_PROBE_READY,
	      "ranging did not reach both probe boundaries for codec %d delay %zu",
	      codec, line_delay_samples);
	CHECK(observation.answer_tone_start_sample != SIZE_MAX &&
	      observation.first_answer_reversal_sample ==
		      observation.answer_tone_start_sample +
			      PHASE2_RANGING_INITIAL_TONE_SAMPLES,
	      "answerer did not preserve the initial 50 ms Tone A interval");
	CHECK(absolute_difference(
		      observation.caller_reversal_sample,
		      observation.first_answer_reversal_sample +
			      line_delay_samples +
			      PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "caller response was not 40 ms from the received Tone A edge");
	CHECK(absolute_difference(
		      observation.second_answer_reversal_sample,
		      observation.caller_reversal_sample +
			      line_delay_samples +
			      PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "answerer response was not 40 ms from the received Tone B edge");
	CHECK(observation.answer_probe_ready_sample ==
		      observation.second_answer_reversal_sample +
			      PHASE2_RANGING_TONE_TAIL_SAMPLES - 1u,
	      "answerer did not preserve the 10 ms post-reversal tone");
	CHECK(absolute_difference(
		      observation.caller_probe_ready_sample,
		      observation.answer_probe_ready_sample +
			      line_delay_samples) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "caller probe boundary did not follow the remote tone tail for codec %d delay %zu",
	      codec, line_delay_samples);
	CHECK(absolute_difference(
		      observation.answer_round_trip_delay_samples,
		      expected_round_trip) <= TEST_EDGE_TOLERANCE_SAMPLES &&
	      absolute_difference(
		      observation.caller_round_trip_delay_samples,
		      expected_round_trip) <= TEST_EDGE_TOLERANCE_SAMPLES,
	      "round-trip estimate changed for codec %d delay %zu: caller %u answer %u expected %zu",
	      codec, line_delay_samples,
	      observation.caller_round_trip_delay_samples,
	      observation.answer_round_trip_delay_samples,
	      expected_round_trip);
	CHECK(observation.answer_delay_measured &&
	      observation.caller_probe_ready,
	      "ranging event sequence changed for codec %d delay %zu",
	      codec, line_delay_samples);
	return true;
}

static bool test_bidirectional_ranging(void)
{
	static const size_t delays[] = { 1, 13, 47 };

	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index)
			CHECK(check_ranging(codec, delays[index]),
			      "ranging failed for codec %d", codec);
	return true;
}

static bool test_missing_second_reversal_times_out(void)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct v34_phase2_ranging answerer;
	int16_t caller_line_sample = 0;
	int16_t answer_line_sample = 0;
	bool caller_reversed = false;
	size_t timeout_count = 0;

	CHECK(v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &calling) ==
		      V34_PHASE2_RANGING_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE, &answering) ==
		      V34_PHASE2_RANGING_OK,
	      "timeout fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT && !caller_reversed;
	     ++index) {
		struct v34_phase2_ranging_event caller_event;
		struct v34_phase2_ranging_event answer_event;
		int16_t next_caller;
		int16_t next_answer;

		CHECK(v34_phase2_ranging_sample(
			      &caller, answer_line_sample, &next_caller,
			      &caller_event) == V34_PHASE2_RANGING_OK &&
		      v34_phase2_ranging_sample(
			      &answerer, caller_line_sample, &next_answer,
			      &answer_event) == V34_PHASE2_RANGING_OK,
		      "timeout setup failed at sample %zu", index);
		caller_line_sample = next_caller;
		answer_line_sample = next_answer;
		caller_reversed = caller.control.phase ==
			PHASE2_RESPONSE_TONE_TAIL;
	}
	CHECK(caller_reversed, "timeout fixture never reached caller reversal");
	for (size_t index = 0;
	     index < PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES + 4u;
	     ++index) {
		struct v34_phase2_ranging_event event;
		int16_t transmit;

		CHECK(v34_phase2_ranging_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_RANGING_OK,
		      "missing-reversal wait failed at sample %zu", index);
		if (event.kind == PHASE2_RANGING_TIMEOUT)
			timeout_count++;
	}
	CHECK(caller.control.phase == PHASE2_RANGING_TIMED_OUT &&
	      timeout_count == 1 &&
	      v34_phase2_ranging_is_valid(&caller),
	      "missing reversal did not produce one bounded timeout");
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 capabilities = calling_capabilities();
	struct v34_phase2_ranging ranging;
	struct v34_phase2_ranging before;
	struct v34_phase2_ranging_event event = {
		.kind = PHASE2_RANGING_TIMEOUT,
	};
	int16_t sample = 1234;

	CHECK(v34_phase2_ranging_init(
		      &ranging, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &capabilities) ==
		      V34_PHASE2_RANGING_OK,
	      "validation fixture did not initialize");
	ranging.info0_exchange.transport.info_receiver.modulation.detector_output =
		NAN;
	before = ranging;
	CHECK(v34_phase2_ranging_sample(
		      &ranging, 0, &sample, &event) ==
		      V34_PHASE2_RANGING_INVALID_ARGUMENT &&
	      memcmp(&ranging, &before, sizeof(ranging)) == 0 &&
	      sample == 1234 &&
	      event.kind == PHASE2_RANGING_TIMEOUT,
	      "invalid ranging state mutated state or outputs");
	CHECK(strcmp(
		      v34_phase2_ranging_result_name(
			      V34_PHASE2_RANGING_OK),
		      "ok") == 0 &&
	      strcmp(
		      v34_phase2_ranging_result_name(
			      (enum v34_phase2_ranging_result)99),
		      "unknown") == 0,
	      "ranging result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional ranging", test_bidirectional_ranging },
		{ "missing second reversal", test_missing_second_reversal_times_out },
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
