// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <line_startup.h>

#include "channel_bench.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_TURNAROUND_PRIMARY_AMPLITUDE = 9000,
	TEST_TURNAROUND_SECONDARY_AMPLITUDE = 8000,
	TEST_TRANSITION_LIMIT = 800,
	TEST_ROUND_TRIP_SAMPLES = 320,
	TEST_LINE_EXCHANGE_LIMIT = 6 * SINGLE_TONE_SAMPLE_RATE,
	TEST_RECORDING_SAMPLE_LIMIT = 120 * SINGLE_TONE_SAMPLE_RATE,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_turnaround_signal {
	struct single_tone_transmitter primary;
	struct single_tone_transmitter secondary;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_CODEC_PCMU)
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	if (codec == TEST_CODEC_PCMA)
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	return sample;
}

static bool process_tone(
	struct v32_line_startup *startup,
	struct single_tone_transmitter *answerer,
	enum test_codec codec, int16_t *transmit, bool *complete)
{
	int16_t answer_sample;

	return single_tone_transmitter_sample(
		       answerer, &answer_sample) == SINGLE_TONE_OK &&
	       v32_line_startup_process(
		       startup, codec_round_trip(answer_sample, codec), transmit,
		       complete) == V32_LINE_STARTUP_OK;
}

static bool turnaround_signal_init(
	struct test_turnaround_signal *signal, unsigned int primary_hz)
{
	unsigned int secondary_hz =
		primary_hz == V32_LOW_TURNAROUND_TONE_HZ ?
			V32_HIGH_TURNAROUND_TONE_HZ :
			V32_LOW_TURNAROUND_TONE_HZ;

	return single_tone_transmitter_init(
		       &signal->primary, primary_hz,
		       TEST_TURNAROUND_PRIMARY_AMPLITUDE) ==
		       SINGLE_TONE_OK &&
	       single_tone_transmitter_init(
		       &signal->secondary, secondary_hz,
		       TEST_TURNAROUND_SECONDARY_AMPLITUDE) ==
		       SINGLE_TONE_OK;
}

static bool process_turnaround(
	struct v32_line_startup *startup,
	struct test_turnaround_signal *signal, enum test_codec codec,
	int16_t *transmit, bool *complete)
{
	int16_t primary;
	int16_t secondary;

	return single_tone_transmitter_sample(
		       &signal->primary, &primary) == SINGLE_TONE_OK &&
	       single_tone_transmitter_sample(
		       &signal->secondary, &secondary) ==
		       SINGLE_TONE_OK &&
	       v32_line_startup_process(
		       startup,
		       codec_round_trip(
			       (int16_t)((int32_t)primary + secondary), codec),
		       transmit, complete) == V32_LINE_STARTUP_OK;
}

static bool reverse_turnaround(struct test_turnaround_signal *signal)
{
	return single_tone_transmitter_reverse(&signal->primary) ==
		       SINGLE_TONE_OK &&
	       single_tone_transmitter_reverse(&signal->secondary) ==
		       SINGLE_TONE_OK;
}

static bool advance_to_state(
	struct v32_line_startup *startup,
	struct single_tone_transmitter *answerer,
	enum test_codec codec,
	enum v32_line_startup_state target)
{
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT; ++count) {
		int16_t transmit;
		bool complete;

		if (!process_tone(
			    startup, answerer, codec, &transmit, &complete))
			return false;
		if (startup->state == target)
			return true;
	}
	return false;
}

static bool run_exchange(enum test_codec codec, unsigned int turnaround_hz)
{
	struct v32_line_startup startup;
	struct single_tone_transmitter answerer;
	struct test_turnaround_signal turnaround;
	uint64_t calling_power = 0;
	size_t guard_samples = 0;
	size_t reversal_response_samples = 0;
	bool complete = false;

	CHECK(v32_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK &&
		      v32_line_startup_round_trip_samples(&startup) ==
			      0 &&
		      single_tone_transmitter_init(
			      &answerer, V32_ANSWER_TONE_HZ,
			      TEST_AMPLITUDE) == SINGLE_TONE_OK &&
		      turnaround_signal_init(&turnaround, turnaround_hz),
	      "V.32 line-startup fixture did not initialize");
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit = 1;

		CHECK(v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_OK &&
		      transmit == 0 && !complete &&
		      startup.state == V32_LINE_WAIT_ANSWER_TONE,
		      "caller left silence before the answer tone");
	}
	CHECK(advance_to_state(
		      &startup, &answerer, codec,
		      V32_LINE_ANSWER_GUARD),
	      "caller did not acquire the answer tone");
	while (startup.state == V32_LINE_ANSWER_GUARD) {
		int16_t transmit;

		CHECK(process_tone(
			      &startup, &answerer, codec, &transmit, &complete) &&
		      transmit == 0 && !complete,
		      "caller emitted audio during the answer guard");
		guard_samples++;
	}
	CHECK(guard_samples == V32_ANSWER_GUARD_SAMPLES &&
		      startup.state ==
			      V32_LINE_SEND_CALLING_CARRIER,
	      "caller did not observe the exact one-second answer guard");
	for (size_t count = 0; count < 240; ++count) {
		int16_t transmit;

		CHECK(v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_OK &&
		      startup.state ==
			      V32_LINE_SEND_CALLING_CARRIER,
		      "caller rejected the answer-to-turnaround gap");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	while (startup.state ==
	       V32_LINE_SEND_CALLING_CARRIER) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      !complete,
		      "caller did not acquire the turnaround signal");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(calling_power > UINT64_C(1000000000),
	      "caller omitted its 1800 Hz response carrier");
	while (startup.state !=
		       V32_LINE_SEND_REVERSED_CALLING_CARRIER &&
	       reversal_response_samples < TEST_TRANSITION_LIMIT) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      !complete,
		      "caller did not schedule its carrier reversal");
		reversal_response_samples++;
	}
	CHECK(startup.state ==
		      V32_LINE_SEND_REVERSED_CALLING_CARRIER &&
		      startup.selected_turnaround_tone_hz == turnaround_hz &&
		      reversal_response_samples >=
			      V32_REVERSAL_RESPONSE_SAMPLES -
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      reversal_response_samples <=
			      V32_REVERSAL_RESPONSE_SAMPLES +
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES,
	      "caller did not answer the turnaround marker after 64 symbols (%zu)",
	      reversal_response_samples);
	{
		struct v32_line_startup lost = startup;
		enum v32_line_startup_result result =
			V32_LINE_STARTUP_OK;
		int16_t transmit = 1;
		bool lost_complete = false;

		for (size_t count = 0;
		     count < TEST_TRANSITION_LIMIT &&
		     result == V32_LINE_STARTUP_OK;
		     ++count)
			result = v32_line_startup_process(
				&lost, 0, &transmit, &lost_complete);
		CHECK(result == V32_LINE_STARTUP_PROTOCOL_ERROR &&
			      lost.state == V32_LINE_STARTUP_FAILED &&
			      transmit == 0 && !lost_complete,
		      "caller did not fail closed on turnaround-carrier loss");
	}
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      !complete &&
		      startup.state ==
			      V32_LINE_SEND_REVERSED_CALLING_CARRIER,
		      "reversed calling carrier failed");
	}
	CHECK(reverse_turnaround(&turnaround),
	      "turnaround tone did not reverse a second time");
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT && !complete;
	     ++count) {
		int16_t transmit = 1;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete),
		      "caller rejected the second phase reversal");
		if (complete)
			CHECK(transmit == 0,
			      "caller continued after the second reversal");
	}
	{
		uint32_t expected =
			V32_REVERSAL_RESPONSE_SAMPLES + 400;
		uint32_t measured =
			v32_line_startup_round_trip_samples(&startup);

		CHECK(complete &&
			      v32_line_startup_is_complete(&startup) &&
			      measured >=
				      expected -
					      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
			      measured <=
				      expected +
					      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES,
		      "caller measured %u samples for a %u-sample turnaround",
		      measured, expected);
	}
	return true;
}

static bool test_generated_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		CHECK(run_exchange(
			      codec, V32_LOW_TURNAROUND_TONE_HZ),
		      "V.32 low-tone startup failed codec %d", codec);
		CHECK(run_exchange(
			      codec, V32_HIGH_TURNAROUND_TONE_HZ),
		      "V.32 high-tone startup failed codec %d", codec);
	}
	return true;
}

static bool test_early_answer_loss_fails(void)
{
	struct v32_line_startup startup;
	struct single_tone_transmitter answerer;
	int16_t transmit = 1;
	bool complete = false;

	CHECK(v32_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK &&
		      single_tone_transmitter_init(
			      &answerer, V32_ANSWER_TONE_HZ,
			      TEST_AMPLITUDE) == SINGLE_TONE_OK &&
		      advance_to_state(
			      &startup, &answerer, TEST_CODEC_LINEAR,
			      V32_LINE_ANSWER_GUARD),
	      "early-loss fixture did not acquire the answer tone");
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT; ++count) {
		enum v32_line_startup_result result =
			v32_line_startup_process(
				&startup, 0, &transmit, &complete);

		if (result == V32_LINE_STARTUP_PROTOCOL_ERROR)
			break;
	}
	CHECK(startup.state == V32_LINE_STARTUP_FAILED &&
		      transmit == 0 && !complete &&
		      v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_PROTOCOL_ERROR,
	      "premature answer-tone loss did not fail closed");
	return true;
}

static bool answerer_process_tone(
	struct v32_answerer_line_startup *startup,
	struct single_tone_transmitter *caller,
	enum test_codec codec, int16_t *transmit, bool *complete)
{
	int16_t caller_sample;

	return single_tone_transmitter_sample(
		       caller, &caller_sample) == SINGLE_TONE_OK &&
	       v32_answerer_line_startup_process(
		       startup, codec_round_trip(caller_sample, codec),
		       transmit, complete) == V32_LINE_STARTUP_OK;
}

static bool run_answerer_standard_exchange(enum test_codec codec)
{
	struct v32_answerer_line_startup startup;
	struct single_tone_transmitter caller;
	uint64_t answer_power = 0;
	uint64_t turnaround_power = 0;
	size_t response_samples = 0;
	size_t quiet_samples = 0;
	bool complete = false;

	CHECK(v32_answerer_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK &&
		      single_tone_transmitter_init(
			      &caller, V32_CALLING_CARRIER_HZ,
			      TEST_AMPLITUDE) == SINGLE_TONE_OK,
	      "answerer line-startup fixture did not initialize");
	for (size_t count = 0; count < V32_ANSWERER_TONE_SAMPLES; ++count) {
		int16_t transmit;

		CHECK(v32_answerer_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_OK && !complete,
		      "answerer rejected its answer-tone interval");
		answer_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(startup.state == V32_ANSWERER_LINE_SILENCE &&
		      answer_power > UINT64_C(1000000000000),
	      "answerer omitted or mistimed its answer tone");
	for (size_t count = 0; count < V32_ANSWERER_SILENCE_SAMPLES;
	     ++count) {
		int16_t transmit = 1;

		CHECK(v32_answerer_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_OK && transmit == 0 && !complete,
		      "answerer did not preserve the answer-to-AC gap");
	}
	CHECK(startup.state == V32_ANSWERER_LINE_SEND_AC,
	      "answerer did not enter the AC interval");
	for (size_t count = 0; count < V32_ANSWERER_AC_SAMPLES; ++count) {
		int16_t transmit;

		CHECK(answerer_process_tone(
			      &startup, &caller, codec, &transmit, &complete) &&
			      !complete,
		      "answerer rejected the calling carrier");
		turnaround_power +=
			(uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(startup.state == V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL &&
		      turnaround_power > UINT64_C(10000000000),
	      "answerer did not send the minimum AC interval");
	for (size_t count = 0; count < TEST_ROUND_TRIP_SAMPLES; ++count) {
		int16_t transmit;

		CHECK(answerer_process_tone(
			      &startup, &caller, codec, &transmit, &complete) &&
			      startup.state ==
				      V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL,
		      "answerer left CA before the caller reversal");
	}
	CHECK(single_tone_transmitter_reverse(&caller) == SINGLE_TONE_OK,
	      "calling carrier did not reverse");
	while (startup.state != V32_ANSWERER_LINE_HOLD_RESPONSE &&
	       response_samples < TEST_TRANSITION_LIMIT) {
		int16_t transmit;

		CHECK(answerer_process_tone(
			      &startup, &caller, codec, &transmit, &complete),
		      "answerer rejected the caller reversal");
		response_samples++;
	}
	CHECK(startup.state == V32_ANSWERER_LINE_HOLD_RESPONSE &&
		      response_samples >=
			      V32_REVERSAL_RESPONSE_SAMPLES -
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      response_samples <=
			      V32_REVERSAL_RESPONSE_SAMPLES +
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      startup.round_trip_samples >=
			      TEST_ROUND_TRIP_SAMPLES -
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      startup.round_trip_samples <=
			      TEST_ROUND_TRIP_SAMPLES +
				      V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      !startup.caller_reversed_early,
	      "answerer response timing changed (%zu samples, MT=%u)",
	      response_samples, startup.round_trip_samples);
	for (size_t count = 0; count < SINGLE_TONE_WINDOW_SAMPLES; ++count) {
		int16_t transmit;

		CHECK(answerer_process_tone(
			      &startup, &caller, codec, &transmit, &complete),
		      "answerer did not hold its response");
	}
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT && !complete;
	     ++count) {
		enum v32_answerer_line_state before = startup.state;
		int16_t transmit = 1;

		CHECK(v32_answerer_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      V32_LINE_STARTUP_OK,
		      "answerer rejected the calling-carrier drop");
		if (before == V32_ANSWERER_LINE_POST_DROP_SILENCE) {
			CHECK(transmit == 0,
			      "answerer transmitted during its post-drop gap");
			quiet_samples++;
		}
	}
	CHECK(complete && v32_answerer_line_startup_is_complete(&startup) &&
		      quiet_samples == V32_ANSWERER_DROP_SILENCE_SAMPLES &&
		      v32_answerer_line_startup_round_trip_samples(&startup) ==
			      startup.round_trip_samples,
	      "answerer did not finish after the exact 16-symbol gap");
	return true;
}

static bool run_paired_line_exchange(enum test_codec codec)
{
	struct v32_line_startup caller;
	struct v32_answerer_line_startup answerer;
	int16_t caller_transmit = 0;
	int16_t answerer_transmit = 0;
	bool caller_complete = false;
	bool answerer_complete = false;

	CHECK(v32_line_startup_init(&caller, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK &&
		      v32_answerer_line_startup_init(
			      &answerer, TEST_AMPLITUDE) ==
			      V32_LINE_STARTUP_OK,
	      "paired line-startup fixture did not initialize");
	for (size_t count = 0;
	     count < TEST_LINE_EXCHANGE_LIMIT && !answerer_complete;
	     ++count) {
		int16_t next_caller;
		int16_t next_answerer;

		CHECK(v32_line_startup_process(
			      &caller,
			      codec_round_trip(answerer_transmit, codec),
			      &next_caller, &caller_complete) ==
			      V32_LINE_STARTUP_OK &&
		      v32_answerer_line_startup_process(
			      &answerer,
			      codec_round_trip(caller_transmit, codec),
			      &next_answerer, &answerer_complete) ==
			      V32_LINE_STARTUP_OK,
		      "paired caller and answerer did not interoperate");
		caller_transmit = next_caller;
		answerer_transmit = next_answerer;
	}
	CHECK(caller_complete && answerer_complete &&
		      answerer.caller_reversed_early &&
		      v32_line_startup_round_trip_samples(&caller) != 0 &&
		      v32_answerer_line_startup_round_trip_samples(&answerer) == 0,
	      "paired line startup did not exercise the early marker path");
	return true;
}

static bool test_generated_answerer_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		CHECK(run_answerer_standard_exchange(codec),
		      "standard answerer exchange failed codec %d", codec);
		CHECK(run_paired_line_exchange(codec),
		      "paired answerer exchange failed codec %d", codec);
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct v32_line_startup startup;
	struct v32_line_startup initialized;
	struct v32_line_startup before;
	struct v32_answerer_line_startup answerer;
	struct v32_answerer_line_startup answerer_before;
	int16_t transmit = 123;
	bool complete = true;

	CHECK(v32_line_startup_init(&startup, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK &&
		      v32_line_startup_init(&startup, 0) ==
		      V32_LINE_STARTUP_INVALID_ARGUMENT,
	      "V.32 line-startup validation fixture changed");
	initialized = startup;
	startup.answer_tone.phase_step++;
	before = startup;
	CHECK(v32_line_startup_process(
		      &startup, 0, &transmit, &complete) ==
		      V32_LINE_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &before, sizeof(startup)) == 0 &&
		      transmit == 123 && complete &&
		      strcmp(v32_line_startup_state_name(
			     V32_LINE_SEND_CALLING_CARRIER),
			     "calling carrier") == 0 &&
		      strcmp(v32_line_startup_result_name(
			     V32_LINE_STARTUP_PROTOCOL_ERROR),
			     "protocol error") == 0,
	      "invalid V.32 line-startup mutation was not transactional");
	startup = initialized;
	startup.round_trip_samples = 1;
	CHECK(!v32_line_startup_is_valid(&startup) &&
		      v32_line_startup_round_trip_samples(&startup) ==
			      0,
	      "V.32 line startup exposed an incomplete turnaround interval");
	CHECK(v32_answerer_line_startup_init(
		      &answerer, TEST_AMPLITUDE) ==
		      V32_LINE_STARTUP_OK,
	      "V.32 answerer validation fixture did not initialize");
	answerer.calling_carrier.phase_step++;
	answerer_before = answerer;
	CHECK(v32_answerer_line_startup_process(
		      &answerer, 0, &transmit, &complete) ==
		      V32_LINE_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&answerer, &answerer_before, sizeof(answerer)) == 0 &&
		      transmit == 123 && complete,
	      "invalid V.32 answerer mutation was not transactional");
	return true;
}

static bool test_optional_recording_acquisition(void)
{
	const char *path = getenv("MODEM_TEST_V32_ANSWERER_PCM");
	struct v32_line_startup startup;
	struct channel_bench_recording recording = { 0 };
	enum channel_bench_result load_result;
	bool acquired = false;
	bool processed = true;

	CHECK(path != NULL &&
		      v32_line_startup_init(&startup, TEST_AMPLITUDE) ==
			      V32_LINE_STARTUP_OK,
	      "optional V.32 recording fixture did not initialize");
	load_result = channel_bench_recording_load(
		path, TEST_RECORDING_SAMPLE_LIMIT, &recording);
	CHECK(load_result == CHANNEL_BENCH_OK,
	      "optional V.32 recording could not be loaded: %s",
	      channel_bench_result_name(load_result));
	for (size_t index = 0; index < recording.count; ++index) {
		int16_t transmit;
		bool complete;

		if (v32_line_startup_process(
			    &startup, recording.samples[index], &transmit,
			    &complete) != V32_LINE_STARTUP_OK) {
			processed = false;
			break;
		}
		if (startup.state ==
			    V32_LINE_DELAY_CALLING_CARRIER_REVERSAL ||
		    startup.state ==
			    V32_LINE_SEND_REVERSED_CALLING_CARRIER ||
		    startup.state == V32_LINE_STARTUP_COMPLETE) {
			acquired = true;
			break;
		}
	}
	channel_bench_recording_release(&recording);
	CHECK(processed,
	      "optional V.32 recording caused a startup error");
	CHECK(acquired,
	      "caller did not acquire the recorded answerer turnaround marker");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated exchange", test_generated_exchange },
		{ "generated answerer exchange", test_generated_answerer_exchange },
		{ "early answer loss", test_early_answer_loss_fails },
		{ "transactional validation", test_validation_is_transactional },
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
	if (getenv("MODEM_TEST_V32_ANSWERER_PCM") == NULL) {
		fprintf(stderr,
			"[  SKIPPED ] optional answerer recording acquisition "
			"(MODEM_TEST_V32_ANSWERER_PCM unset)\n");
	} else {
		fprintf(stderr,
			"[ RUN      ] optional answerer recording acquisition\n");
		if (!test_optional_recording_acquisition()) {
			fprintf(stderr,
				"[  FAILED  ] optional answerer recording acquisition\n");
			failures++;
		} else {
			fprintf(stderr,
				"[       OK ] optional answerer recording acquisition\n");
		}
	}
	return failures == 0 ? 0 : 1;
}
