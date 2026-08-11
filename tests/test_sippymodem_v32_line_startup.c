// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_g711.h>
#include <sippymodem_v32_line_startup.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_TRANSITION_LIMIT = 800,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
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

static bool run_exchange(enum test_codec codec)
{
	struct sippymodem_v32_line_startup startup;
	struct sippymodem_single_tone_transmitter answerer;
	uint64_t calling_power = 0;
	size_t guard_samples = 0;
	bool complete = false;

	CHECK(sippymodem_v32_line_startup_init(
		      &startup, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      sippymodem_single_tone_transmitter_init(
			      &answerer, SIPPYMODEM_V32_ANSWER_TONE_HZ,
			      TEST_AMPLITUDE) == SIPPYMODEM_SINGLE_TONE_OK,
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
	for (size_t count = 0; count < 320; ++count) {
		int16_t transmit;

		CHECK(process_tone(
			      &startup, &answerer, codec, &transmit, &complete),
		      "calling carrier generation failed");
		calling_power += (uint64_t)((int32_t)transmit * transmit);
	}
	CHECK(calling_power > UINT64_C(1000000000),
	      "caller omitted its 1800 Hz response carrier");
	CHECK(sippymodem_single_tone_transmitter_reverse(&answerer) ==
		      SIPPYMODEM_SINGLE_TONE_OK &&
		      advance_to_state(
			      &startup, &answerer, codec,
			      SIPPYMODEM_V32_LINE_SEND_REVERSED_CALLING_CARRIER),
	      "caller did not answer the first phase reversal");
	for (size_t count = 0; count < 400; ++count) {
		int16_t transmit;

		CHECK(process_tone(
			      &startup, &answerer, codec, &transmit, &complete),
		      "reversed calling carrier failed");
	}
	CHECK(sippymodem_single_tone_transmitter_reverse(&answerer) ==
		      SIPPYMODEM_SINGLE_TONE_OK &&
		      advance_to_state(
			      &startup, &answerer, codec,
			      SIPPYMODEM_V32_LINE_WAIT_ANSWER_TONE_END),
	      "caller did not stop after the second phase reversal");
	for (size_t count = 0; count < TEST_TRANSITION_LIMIT && !complete;
	     ++count) {
		int16_t transmit = 1;

		CHECK(sippymodem_v32_line_startup_process(
			      &startup, 0, &transmit, &complete) ==
			      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      transmit == 0,
		      "caller did not remain silent during peer turnaround");
	}
	CHECK(complete && sippymodem_v32_line_startup_is_complete(&startup),
	      "caller did not finish the line startup after peer turnaround");
	return true;
}

static bool test_generated_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(run_exchange(codec), "V.32 line startup failed codec %d", codec);
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
	struct sippymodem_v32_line_startup before;
	int16_t transmit = 123;
	bool complete = true;

	CHECK(sippymodem_v32_line_startup_init(&startup, TEST_AMPLITUDE) ==
		      SIPPYMODEM_V32_LINE_STARTUP_OK &&
		      sippymodem_v32_line_startup_init(&startup, 0) ==
			      SIPPYMODEM_V32_LINE_STARTUP_INVALID_ARGUMENT,
	      "V.32 line-startup validation fixture changed");
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
