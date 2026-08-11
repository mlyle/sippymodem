// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <line_startup.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_TURNAROUND_PRIMARY_AMPLITUDE = 9000,
	TEST_TURNAROUND_SECONDARY_AMPLITUDE = 6000,
	TEST_TRANSITION_LIMIT = 800,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_turnaround_signal {
	struct sippymodem_single_tone_transmitter primary;
	struct sippymodem_single_tone_transmitter secondary;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_CODEC_PCMU)
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	if (codec == TEST_CODEC_PCMA)
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	return sample;
}

static bool process_tone(
	struct sippymodem_v32_line_startup *startup,
	struct sippymodem_single_tone_transmitter *answerer,
	enum test_codec codec, int16_t *transmit, bool *complete)
{
	int16_t answer_sample;

	return sippymodem_single_tone_transmitter_sample(
		       answerer, &answer_sample) == SIPPYMODEM_SINGLE_TONE_OK &&
	       sippymodem_v32_line_startup_process(
		       startup, codec_round_trip(answer_sample, codec), transmit,
		       complete) == SIPPYMODEM_V32_LINE_STARTUP_OK;
}

static bool turnaround_signal_init(
	struct test_turnaround_signal *signal, unsigned int primary_hz)
{
	unsigned int secondary_hz =
		primary_hz == SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ ?
			SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ :
			SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ;

	return sippymodem_single_tone_transmitter_init(
		       &signal->primary, primary_hz,
		       TEST_TURNAROUND_PRIMARY_AMPLITUDE) ==
		       SIPPYMODEM_SINGLE_TONE_OK &&
	       sippymodem_single_tone_transmitter_init(
		       &signal->secondary, secondary_hz,
		       TEST_TURNAROUND_SECONDARY_AMPLITUDE) ==
		       SIPPYMODEM_SINGLE_TONE_OK;
}

static bool process_turnaround(
	struct sippymodem_v32_line_startup *startup,
	struct test_turnaround_signal *signal, enum test_codec codec,
	int16_t *transmit, bool *complete)
{
	int16_t primary;
	int16_t secondary;

	return sippymodem_single_tone_transmitter_sample(
		       &signal->primary, &primary) == SIPPYMODEM_SINGLE_TONE_OK &&
	       sippymodem_single_tone_transmitter_sample(
		       &signal->secondary, &secondary) ==
		       SIPPYMODEM_SINGLE_TONE_OK &&
	       sippymodem_v32_line_startup_process(
		       startup,
		       codec_round_trip(
			       (int16_t)((int32_t)primary + secondary), codec),
		       transmit, complete) == SIPPYMODEM_V32_LINE_STARTUP_OK;
}

static bool reverse_turnaround(struct test_turnaround_signal *signal)
{
	return sippymodem_single_tone_transmitter_reverse(&signal->primary) ==
		       SIPPYMODEM_SINGLE_TONE_OK &&
	       sippymodem_single_tone_transmitter_reverse(&signal->secondary) ==
		       SIPPYMODEM_SINGLE_TONE_OK;
}

static bool advance_to_state(
	struct sippymodem_v32_line_startup *startup,
	struct sippymodem_single_tone_transmitter *answerer,
	enum test_codec codec,
	enum sippymodem_v32_line_startup_state target)
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
	struct sippymodem_v32_line_startup startup;
	struct sippymodem_single_tone_transmitter answerer;
	struct test_turnaround_signal turnaround;
	uint64_t calling_power = 0;
	size_t guard_samples = 0;
	size_t reversal_response_samples = 0;
	bool complete = false;

	CHECK(sippymodem_v32_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      sippymodem_v32_line_startup_round_trip_samples(&startup) ==
			      0 &&
		      sippymodem_single_tone_transmitter_init(
			      &answerer, SIPPYMODEM_V32_ANSWER_TONE_HZ,
			      TEST_AMPLITUDE) == SIPPYMODEM_SINGLE_TONE_OK &&
		      turnaround_signal_init(&turnaround, turnaround_hz),
	      "V.32 line-startup fixture did not initialize");
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit = 1;

		CHECK(sippymodem_v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      transmit == 0 && !complete &&
		      startup.state == SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE,
		      "caller left silence before the answer tone");
	}
	CHECK(advance_to_state(
		      &startup, &answerer, codec,
		      SIPPYMODEM_V32_LINE_ANSWER_GUARD),
	      "caller did not acquire the answer tone");
	while (startup.state == SIPPYMODEM_V32_LINE_ANSWER_GUARD) {
		int16_t transmit;

		CHECK(process_tone(
			      &startup, &answerer, codec, &transmit, &complete) &&
		      transmit == 0 && !complete,
		      "caller emitted audio during the answer guard");
		guard_samples++;
	}
	CHECK(guard_samples == SIPPYMODEM_V32_ANSWER_GUARD_SAMPLES &&
		      startup.state ==
			      SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER,
	      "caller did not observe the exact one-second answer guard");
	for (size_t count = 0; count < 240; ++count) {
		int16_t transmit;

		CHECK(sippymodem_v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      startup.state ==
			      SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER,
		      "caller rejected the answer-to-turnaround gap");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      startup.state ==
			      SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER,
		      "calling carrier generation failed");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(calling_power > UINT64_C(1000000000),
	      "caller omitted its 1800 Hz response carrier");
	CHECK(reverse_turnaround(&turnaround),
	      "turnaround tone did not reverse");
	while (startup.state !=
		       SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER &&
	       reversal_response_samples < TEST_TRANSITION_LIMIT) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      !complete,
		      "caller did not schedule its carrier reversal");
		reversal_response_samples++;
	}
	CHECK(startup.state ==
		      SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER &&
		      startup.selected_turnaround_tone_hz == turnaround_hz &&
		      reversal_response_samples >=
			      SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES -
				      SIPPYMODEM_V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
		      reversal_response_samples <=
			      SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES +
				      SIPPYMODEM_V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES,
	      "caller did not answer the first reversal after 64 symbols (%zu)",
	      reversal_response_samples);
	{
		struct sippymodem_v32_line_startup lost = startup;
		enum sippymodem_v32_line_startup_result result =
			SIPPYMODEM_V32_LINE_STARTUP_OK;
		int16_t transmit = 1;
		bool lost_complete = false;

		for (size_t count = 0;
		     count < TEST_TRANSITION_LIMIT &&
		     result == SIPPYMODEM_V32_LINE_STARTUP_OK;
		     ++count)
			result = sippymodem_v32_line_startup_process(
				&lost, 0, &transmit, &lost_complete);
		CHECK(result == SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR &&
			      lost.state == SIPPYMODEM_V32_LINE_STARTUP_FAILED &&
			      transmit == 0 && !lost_complete,
		      "caller did not fail closed on turnaround-carrier loss");
	}
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit;

		CHECK(process_turnaround(
			      &startup, &turnaround, codec, &transmit, &complete) &&
		      !complete &&
		      startup.state ==
			      SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER,
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
			SIPPYMODEM_V32_REVERSAL_RESPONSE_SAMPLES + 400;
		uint32_t measured =
			sippymodem_v32_line_startup_round_trip_samples(&startup);

		CHECK(complete &&
			      sippymodem_v32_line_startup_is_complete(&startup) &&
			      measured >=
				      expected -
					      SIPPYMODEM_V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES &&
			      measured <=
				      expected +
					      SIPPYMODEM_V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES,
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
			      codec, SIPPYMODEM_V32_LOW_TURNAROUND_TONE_HZ),
		      "V.32 low-tone startup failed codec %d", codec);
		CHECK(run_exchange(
			      codec, SIPPYMODEM_V32_HIGH_TURNAROUND_TONE_HZ),
		      "V.32 high-tone startup failed codec %d", codec);
	}
	return true;
}

static bool test_early_answer_loss_fails(void)
{
	struct sippymodem_v32_line_startup startup;
	struct sippymodem_single_tone_transmitter answerer;
	int16_t transmit = 1;
	bool complete = false;

	CHECK(sippymodem_v32_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      sippymodem_single_tone_transmitter_init(
			      &answerer, SIPPYMODEM_V32_ANSWER_TONE_HZ,
			      TEST_AMPLITUDE) == SIPPYMODEM_SINGLE_TONE_OK &&
		      advance_to_state(
			      &startup, &answerer, TEST_CODEC_LINEAR,
			      SIPPYMODEM_V32_LINE_ANSWER_GUARD),
	      "early-loss fixture did not acquire the answer tone");
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT; ++count) {
		enum sippymodem_v32_line_startup_result result =
			sippymodem_v32_line_startup_process(
				&startup, 0, &transmit, &complete);

		if (result == SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR)
			break;
	}
	CHECK(startup.state == SIPPYMODEM_V32_LINE_STARTUP_FAILED &&
		      transmit == 0 && !complete &&
		      sippymodem_v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR,
	      "premature answer-tone loss did not fail closed");
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct sippymodem_v32_line_startup startup;
	struct sippymodem_v32_line_startup initialized;
	struct sippymodem_v32_line_startup before;
	int16_t transmit = 123;
	bool complete = true;

	CHECK(sippymodem_v32_line_startup_init(&startup, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      sippymodem_v32_line_startup_init(&startup, 0) ==
		      SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT,
	      "V.32 line-startup validation fixture changed");
	initialized = startup;
	startup.answer_tone.phase_step++;
	before = startup;
	CHECK(sippymodem_v32_line_startup_process(
		      &startup, 0, &transmit, &complete) ==
		      SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &before, sizeof(startup)) == 0 &&
		      transmit == 123 && complete &&
		      strcmp(sippymodem_v32_line_startup_state_name(
			     SIPPYMODEM_V32_LINE_SEND_CALLING_CARRIER),
			     "calling carrier") == 0 &&
		      strcmp(sippymodem_v32_line_startup_result_name(
			     SIPPYMODEM_V32_LINE_STARTUP_PROTOCOL_ERROR),
			     "protocol error") == 0,
	      "invalid V.32 line-startup mutation was not transactional");
	startup = initialized;
	startup.round_trip_samples = 1;
	CHECK(!sippymodem_v32_line_startup_is_valid(&startup) &&
		      sippymodem_v32_line_startup_round_trip_samples(&startup) ==
			      0,
	      "V.32 line startup exposed an incomplete turnaround interval");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated exchange", test_generated_exchange },
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
	return failures == 0 ? 0 : 1;
}
