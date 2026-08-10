// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v8_calling.h>

#include "test.h"

#include <string.h>

static bool test_answer_signal_dwell(void)
{
	struct sippymodem_v8_ansam_generator answer_signal;
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu menu;
	size_t dwell_started = 0;
	size_t ready = 0;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_ansam_generator_init(
		      &answer_signal, 10000, true) == SIPPYMODEM_V8_ANSAM_OK &&
	      sippymodem_v8_calling_init(&calling, &menu) ==
		      SIPPYMODEM_V8_CALLING_OK,
	      "V.8 answer-signal endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 3u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t receive;
		int16_t transmit = 1;
		enum sippymodem_v8_calling_state before = calling.state;

		CHECK(sippymodem_v8_ansam_generate(
			      &answer_signal, &receive, 1) ==
			      SIPPYMODEM_V8_ANSAM_OK &&
		      sippymodem_v8_calling_process(
			      &calling, &receive, &transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_OK &&
		      transmit == 0,
		      "V.8 answer acquisition failed at sample %zu", sample_index);
		if (before != calling.state) {
			if (calling.state ==
			    SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL)
				dwell_started = sample_index + 1u;
			else if (calling.state ==
				 SIPPYMODEM_V8_CALLING_READY_TO_EXCHANGE)
				ready = sample_index + 1u;
		}
		if (sippymodem_v8_calling_is_ready(&calling))
			break;
	}
	CHECK(dwell_started != 0 && ready - dwell_started ==
					      SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES &&
		      sippymodem_v8_calling_is_ready(&calling),
	      "V.8 calling station did not preserve the one-second dwell");
	return true;
}

static bool test_answer_signal_timeout(void)
{
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu menu;
	int16_t silence[SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum sippymodem_v8_calling_result result = SIPPYMODEM_V8_CALLING_OK;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_calling_init(&calling, &menu) ==
		      SIPPYMODEM_V8_CALLING_OK,
	      "V.8 timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence)) {
		memset(transmit, 1, sizeof(transmit));
		result = sippymodem_v8_calling_process(
			&calling, silence, transmit, ARRAY_LENGTH(silence));
		CHECK((result == SIPPYMODEM_V8_CALLING_OK ||
		       result == SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT) &&
			      memcmp(transmit, silence, sizeof(transmit)) == 0,
		      "V.8 answer timeout emitted audio or failed early");
	}
	CHECK(result == SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT &&
		      calling.state == SIPPYMODEM_V8_CALLING_FAILED &&
		      !sippymodem_v8_calling_is_ready(&calling) &&
		      sippymodem_v8_calling_process(
			      &calling, silence, transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT,
	      "V.8 calling timeout did not latch");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_calling saved;
	struct sippymodem_v8_menu menu;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_calling_init(&calling, &menu) ==
		      SIPPYMODEM_V8_CALLING_OK,
	      "V.8 calling validation fixture did not initialize");
	saved = calling;
	CHECK(sippymodem_v8_calling_init(NULL, &menu) ==
		      SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT &&
	      sippymodem_v8_calling_process(
		      &calling, &receive, &transmit,
		      SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT &&
	      transmit == 123 && memcmp(&calling, &saved, sizeof(calling)) == 0 &&
	      sippymodem_v8_calling_process(
		      &calling, NULL, &transmit, 1) ==
		      SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT &&
	      sippymodem_v8_calling_process(
		      &calling, NULL, NULL, 0) == SIPPYMODEM_V8_CALLING_OK &&
	      strcmp(sippymodem_v8_calling_result_name(
			     SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT),
		     "answer signal timeout") == 0,
	      "V.8 calling API accepted invalid arguments");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "answer signal dwell", test_answer_signal_dwell },
		{ "answer signal timeout", test_answer_signal_timeout },
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
