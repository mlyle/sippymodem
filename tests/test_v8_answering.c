// SPDX-License-Identifier: BSD-3-Clause

#include <v8_answering.h>
#include <v8_calling.h>

#include "test.h"

#include <string.h>

static bool endpoints_init(
	struct sippymodem_v8_calling *calling,
	struct sippymodem_v8_answering *answering,
	uint16_t calling_modulations, uint16_t answering_modulations)
{
	struct sippymodem_v8_menu calling_menu;
	struct sippymodem_v8_menu answering_menu;

	return sippymodem_v8_call_menu_init(
		       &calling_menu, calling_modulations, true) ==
		       SIPPYMODEM_V8_MENU_OK &&
	       sippymodem_v8_call_menu_init(
		       &answering_menu, answering_modulations, true) ==
		       SIPPYMODEM_V8_MENU_OK &&
	       sippymodem_v8_calling_init(calling, &calling_menu) ==
		       SIPPYMODEM_V8_CALLING_OK &&
	       sippymodem_v8_answering_init(answering, &answering_menu) ==
		       SIPPYMODEM_V8_ANSWERING_OK;
}

static bool test_call_negotiation(void)
{
	struct sippymodem_v8_answering answering;
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu calling_selection;
	struct sippymodem_v8_menu answering_selection;
	enum sippymodem_v8_modulation calling_modulation;
	enum sippymodem_v8_modulation answering_modulation;
	int16_t calling_transmit = 0;
	size_t silence_started = 0;
	size_t completed = 0;

	CHECK(endpoints_init(
		      &calling, &answering, SIPPYMODEM_V8_MODULATION_V21,
		      SIPPYMODEM_V8_MODULATION_V21),
	      "V.8 endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t answering_transmit;
		enum sippymodem_v8_answering_state before = answering.state;

		CHECK(sippymodem_v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      SIPPYMODEM_V8_ANSWERING_OK &&
		      sippymodem_v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_OK,
		      "V.8 endpoints failed at sample %zu", sample_index);
		if (before == SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE ||
		    before == SIPPYMODEM_V8_ANSWERING_COMPLETE)
			CHECK(answering_transmit == 0,
			      "V.8 answerer emitted audio after CJ");
		if (before != answering.state &&
		    answering.state ==
			    SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE)
			silence_started = sample_index + 1u;
		else if (before != answering.state &&
			 answering.state == SIPPYMODEM_V8_ANSWERING_COMPLETE)
			completed = sample_index + 1u;
		if (sippymodem_v8_calling_is_complete(&calling) &&
		    sippymodem_v8_answering_is_complete(&answering))
			break;
	}
	CHECK(silence_started != 0 &&
	      completed - silence_started ==
		      SIPPYMODEM_V8_ANSWERING_POST_CJ_SILENCE_SAMPLES &&
	      sippymodem_v8_calling_is_complete(&calling) &&
	      sippymodem_v8_answering_is_complete(&answering),
	      "V.8 endpoints did not complete the timed handshake");
	CHECK(sippymodem_v8_calling_selection(
		      &calling, &calling_selection, &calling_modulation) ==
		      SIPPYMODEM_V8_CALLING_OK &&
	      sippymodem_v8_answering_selection(
		      &answering, &answering_selection,
		      &answering_modulation) == SIPPYMODEM_V8_ANSWERING_OK &&
	      calling_modulation == SIPPYMODEM_V8_MODULATION_V21 &&
	      answering_modulation == calling_modulation &&
	      answering_selection.modulations ==
		      calling_selection.modulations &&
	      answering_selection.lapm && calling_selection.lapm,
	      "V.8 endpoints disagreed on the selected data mode");
	return true;
}

static bool test_no_common_modulation(void)
{
	struct sippymodem_v8_answering answering;
	struct sippymodem_v8_calling calling;
	enum sippymodem_v8_answering_result result =
		SIPPYMODEM_V8_ANSWERING_OK;
	int16_t calling_transmit = 0;
	int16_t answering_transmit = 0;

	CHECK(endpoints_init(
		      &calling, &answering, SIPPYMODEM_V8_MODULATION_V21,
		      SIPPYMODEM_V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		result = sippymodem_v8_answering_process(
			&answering, &calling_transmit, &answering_transmit, 1);
		if (result != SIPPYMODEM_V8_ANSWERING_OK)
			break;
		CHECK(sippymodem_v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_OK,
		      "V.8 incompatible caller failed early");
	}
	CHECK(result == SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION &&
	      answering.state == SIPPYMODEM_V8_ANSWERING_FAILED &&
	      sippymodem_v8_answering_process(
		      &answering, &calling_transmit,
		      &answering_transmit, 1) ==
		      SIPPYMODEM_V8_ANSWERING_NO_COMMON_MODULATION,
	      "V.8 answerer did not latch the incompatible offer");
	return true;
}

static bool test_call_menu_timeout(void)
{
	struct sippymodem_v8_answering answering;
	struct sippymodem_v8_menu menu;
	int16_t silence[SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum sippymodem_v8_answering_result result =
		SIPPYMODEM_V8_ANSWERING_OK;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_answering_init(&answering, &menu) ==
		      SIPPYMODEM_V8_ANSWERING_OK,
	      "V.8 answerer timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = sippymodem_v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT &&
	      answering.state == SIPPYMODEM_V8_ANSWERING_FAILED &&
	      sippymodem_v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      SIPPYMODEM_V8_ANSWERING_CALL_MENU_TIMEOUT,
	      "V.8 call-menu timeout did not latch");
	return true;
}

static bool test_cj_timeout(void)
{
	struct sippymodem_v8_answering answering;
	struct sippymodem_v8_calling calling;
	int16_t calling_transmit = 0;
	int16_t silence[SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum sippymodem_v8_answering_result result =
		SIPPYMODEM_V8_ANSWERING_OK;

	CHECK(endpoints_init(
		      &calling, &answering, SIPPYMODEM_V8_MODULATION_V21,
		      SIPPYMODEM_V8_MODULATION_V21),
	      "V.8 CJ-timeout endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 4u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE &&
	     answering.state == SIPPYMODEM_V8_ANSWERING_WAIT_CALL_MENU;
	     ++sample_index) {
		int16_t answering_transmit;

		CHECK(sippymodem_v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      SIPPYMODEM_V8_ANSWERING_OK &&
		      sippymodem_v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_OK,
		      "V.8 CJ-timeout endpoints failed during CM");
	}
	CHECK(answering.state == SIPPYMODEM_V8_ANSWERING_SEND_JOINT_MENU,
	      "V.8 answerer did not reach the CJ wait");
	for (size_t offset = 0;
	     offset < SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = sippymodem_v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT &&
	      answering.state == SIPPYMODEM_V8_ANSWERING_FAILED &&
	      sippymodem_v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT,
	      "V.8 CJ timeout did not latch");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v8_answering answering;
	struct sippymodem_v8_answering saved;
	struct sippymodem_v8_menu menu;
	struct sippymodem_v8_menu selected_menu;
	enum sippymodem_v8_modulation selected =
		SIPPYMODEM_V8_MODULATION_V34_DUPLEX;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_answering_init(&answering, &menu) ==
		      SIPPYMODEM_V8_ANSWERING_OK,
	      "V.8 answering validation fixture did not initialize");
	saved = answering;
	CHECK(sippymodem_v8_answering_init(NULL, &menu) ==
		      SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT &&
	      sippymodem_v8_answering_process(
		      &answering, &receive, &transmit,
		      SIPPYMODEM_V8_ANSWERING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT &&
	      transmit == 123 &&
	      memcmp(&answering, &saved, sizeof(answering)) == 0 &&
	      sippymodem_v8_answering_process(
		      &answering, NULL, &transmit, 1) ==
		      SIPPYMODEM_V8_ANSWERING_INVALID_ARGUMENT &&
	      sippymodem_v8_answering_process(
		      &answering, NULL, NULL, 0) ==
		      SIPPYMODEM_V8_ANSWERING_OK &&
	      sippymodem_v8_answering_selection(
		      &answering, &selected_menu, &selected) ==
		      SIPPYMODEM_V8_ANSWERING_NOT_READY &&
	      selected == SIPPYMODEM_V8_MODULATION_V34_DUPLEX &&
	      strcmp(sippymodem_v8_answering_result_name(
			     SIPPYMODEM_V8_ANSWERING_CJ_TIMEOUT),
		     "CJ timeout") == 0,
	      "V.8 answering API accepted invalid arguments");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "call negotiation", test_call_negotiation },
		{ "no common modulation", test_no_common_modulation },
		{ "call menu timeout", test_call_menu_timeout },
		{ "CJ timeout", test_cj_timeout },
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
