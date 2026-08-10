// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v8_calling.h>

#include "test.h"

#include <string.h>

enum remote_answer_state {
	REMOTE_ANSWER_SIGNAL,
	REMOTE_JOINT_MENU,
	REMOTE_SILENCE,
};

struct remote_answerer {
	struct sippymodem_v8_ansam_generator answer_signal;
	struct sippymodem_v8_fsk fsk;
	struct sippymodem_v8_menu_detector call_menu_detector;
	struct sippymodem_v8_control_generator joint_menu_bits;
	struct sippymodem_v8_cj_detector cj_detector;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu joint_menu;
	enum remote_answer_state state;
	uint8_t transmit_bit;
	bool call_menu_confirmed;
	bool cj_detected;
};

static bool remote_answerer_init(
	struct remote_answerer *answerer, uint16_t modulations)
{
	struct remote_answerer initialized = {
		.transmit_bit = 1,
	};

	if (sippymodem_v8_ansam_generator_init(
		    &initialized.answer_signal, 10000, true) !=
		    SIPPYMODEM_V8_ANSAM_OK ||
	    sippymodem_v8_fsk_init(
		    &initialized.fsk, SIPPYMODEM_V8_ANSWERING_STATION) !=
		    SIPPYMODEM_V8_FSK_OK ||
	    sippymodem_v8_menu_detector_init(
		    &initialized.call_menu_detector) != SIPPYMODEM_V8_MENU_OK ||
	    sippymodem_v8_cj_detector_init(&initialized.cj_detector) !=
		    SIPPYMODEM_V8_FRAMING_OK ||
	    sippymodem_v8_call_menu_init(
		    &initialized.local_menu, modulations, true) !=
		    SIPPYMODEM_V8_MENU_OK)
		return false;
	*answerer = initialized;
	return true;
}

static bool remote_answerer_transmit(
	struct remote_answerer *answerer, int16_t *sample)
{
	bool consumed;

	if (answerer->state == REMOTE_ANSWER_SIGNAL)
		return sippymodem_v8_ansam_generate(
			       &answerer->answer_signal, sample, 1) ==
		       SIPPYMODEM_V8_ANSAM_OK;
	if (answerer->state == REMOTE_JOINT_MENU) {
		if (sippymodem_v8_fsk_can_accept_bit(&answerer->fsk)) {
			bool sequence_complete;

			if (sippymodem_v8_control_next_bit(
				    &answerer->joint_menu_bits,
				    &answerer->transmit_bit,
				    &sequence_complete) !=
			    SIPPYMODEM_V8_FRAMING_OK)
				return false;
		}
		return sippymodem_v8_fsk_transmit_sample(
			       &answerer->fsk, answerer->transmit_bit, sample,
			       &consumed) == SIPPYMODEM_V8_FSK_OK;
	}
	*sample = 0;
	return true;
}

static bool remote_answerer_receive(
	struct remote_answerer *answerer, int16_t sample)
{
	uint8_t bit;
	bool ready;

	if (answerer->state == REMOTE_SILENCE)
		return true;
	if (sippymodem_v8_fsk_receive_sample(
		    &answerer->fsk, sample, &bit, &ready) !=
	    SIPPYMODEM_V8_FSK_OK)
		return false;
	if (!ready)
		return true;
	if (answerer->state == REMOTE_ANSWER_SIGNAL) {
		struct sippymodem_v8_menu call_menu;
		uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
		size_t information_count;
		bool confirmed;

		if (sippymodem_v8_menu_detect_bit(
			    &answerer->call_menu_detector, bit, &call_menu,
			    &confirmed) != SIPPYMODEM_V8_MENU_OK)
			return false;
		if (!confirmed)
			return true;
		if (sippymodem_v8_joint_menu_select(
			    &call_menu, &answerer->local_menu,
			    &answerer->joint_menu) != SIPPYMODEM_V8_MENU_OK ||
		    sippymodem_v8_menu_encode(
			    &answerer->joint_menu, information,
			    sizeof(information), &information_count) !=
			    SIPPYMODEM_V8_MENU_OK ||
		    sippymodem_v8_control_generator_init(
			    &answerer->joint_menu_bits, information,
			    information_count) != SIPPYMODEM_V8_FRAMING_OK)
			return false;
		answerer->call_menu_confirmed = true;
		answerer->state = REMOTE_JOINT_MENU;
		return true;
	}
	if (sippymodem_v8_cj_detect_bit(
		    &answerer->cj_detector, bit, &answerer->cj_detected) !=
	    SIPPYMODEM_V8_FRAMING_OK)
		return false;
	if (answerer->cj_detected)
		answerer->state = REMOTE_SILENCE;
	return true;
}

static bool test_call_negotiation(void)
{
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu negotiated_menu;
	struct remote_answerer answerer;
	enum sippymodem_v8_modulation modulation;
	size_t dwell_started = 0;
	size_t exchange_started = 0;
	size_t post_cj_started = 0;
	size_t completed = 0;

	CHECK(sippymodem_v8_call_menu_init(
		      &local_menu, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_calling_init(&calling, &local_menu) ==
		      SIPPYMODEM_V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, SIPPYMODEM_V8_MODULATION_V21),
	      "V.8 negotiation endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t receive;
		int16_t transmit;
		enum sippymodem_v8_calling_state before = calling.state;

		CHECK(remote_answerer_transmit(&answerer, &receive) &&
		      sippymodem_v8_calling_process(
			      &calling, &receive, &transmit, 1) ==
		      SIPPYMODEM_V8_CALLING_OK,
		      "V.8 calling station failed at sample %zu", sample_index);
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 answering station failed at sample %zu", sample_index);
		if (before == SIPPYMODEM_V8_CALLING_WAIT_ANSWER_SIGNAL ||
		    before == SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL ||
		    before == SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE ||
		    before == SIPPYMODEM_V8_CALLING_COMPLETE)
			CHECK(transmit == 0,
			      "V.8 silent state emitted audio at sample %zu",
			      sample_index);
		if (before != calling.state) {
			if (calling.state ==
			    SIPPYMODEM_V8_CALLING_ECHO_CONTROL_DWELL)
				dwell_started = sample_index + 1u;
			else if (calling.state ==
				 SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS)
				exchange_started = sample_index + 1u;
			else if (calling.state ==
				 SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE)
				post_cj_started = sample_index + 1u;
			else if (calling.state ==
				 SIPPYMODEM_V8_CALLING_COMPLETE)
				completed = sample_index + 1u;
		}
		if (sippymodem_v8_calling_is_complete(&calling))
			break;
	}
	CHECK(dwell_started != 0 &&
	      exchange_started - dwell_started ==
		      SIPPYMODEM_V8_ECHO_CONTROL_DWELL_SAMPLES &&
	      post_cj_started != 0 &&
	      completed - post_cj_started ==
		      SIPPYMODEM_V8_CALLING_POST_CJ_SILENCE_SAMPLES &&
	      sippymodem_v8_calling_is_complete(&calling) &&
	      answerer.call_menu_confirmed && answerer.cj_detected,
	      "V.8 calling station did not complete the timed exchange");
	CHECK(sippymodem_v8_calling_selection(
		      &calling, &negotiated_menu, &modulation) ==
		      SIPPYMODEM_V8_CALLING_OK &&
	      modulation == SIPPYMODEM_V8_MODULATION_V21 &&
	      negotiated_menu.modulations == SIPPYMODEM_V8_MODULATION_V21 &&
	      negotiated_menu.lapm,
	      "V.8 calling station did not expose the selected V.21 menu");
	return true;
}

static bool test_no_common_modulation(void)
{
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu local_menu;
	struct remote_answerer answerer;
	enum sippymodem_v8_calling_result result = SIPPYMODEM_V8_CALLING_OK;
	int16_t receive = 0;
	int16_t transmit = 0;

	CHECK(sippymodem_v8_call_menu_init(
		      &local_menu, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_calling_init(&calling, &local_menu) ==
		      SIPPYMODEM_V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, SIPPYMODEM_V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		CHECK(remote_answerer_transmit(&answerer, &receive),
		      "V.8 incompatible answerer failed to transmit");
		result = sippymodem_v8_calling_process(
			&calling, &receive, &transmit, 1);
		if (result != SIPPYMODEM_V8_CALLING_OK)
			break;
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 incompatible answerer failed to receive");
	}
	CHECK(result == SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION &&
	      calling.state == SIPPYMODEM_V8_CALLING_FAILED &&
	      answerer.call_menu_confirmed && !answerer.cj_detected &&
	      sippymodem_v8_calling_process(
		      &calling, &receive, &transmit, 1) ==
		      SIPPYMODEM_V8_CALLING_NO_COMMON_MODULATION,
	      "V.8 incompatible modulation result did not latch");
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
		      !sippymodem_v8_calling_is_complete(&calling) &&
		      sippymodem_v8_calling_process(
			      &calling, silence, transmit, 1) ==
			      SIPPYMODEM_V8_CALLING_ANSWER_TIMEOUT,
	      "V.8 calling timeout did not latch");
	return true;
}

static bool test_menu_timeout(void)
{
	struct sippymodem_v8_ansam_generator answer_signal;
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_menu menu;
	int16_t receive[SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES];
	int16_t transmit[SIPPYMODEM_V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum sippymodem_v8_calling_result result = SIPPYMODEM_V8_CALLING_OK;

	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_ansam_generator_init(
		      &answer_signal, 10000, true) == SIPPYMODEM_V8_ANSAM_OK &&
	      sippymodem_v8_calling_init(&calling, &menu) ==
		      SIPPYMODEM_V8_CALLING_OK,
	      "V.8 menu-timeout fixture did not initialize");
	while (calling.state != SIPPYMODEM_V8_CALLING_EXCHANGE_MENUS) {
		CHECK(sippymodem_v8_ansam_generate(
			      &answer_signal, receive, ARRAY_LENGTH(receive)) ==
			      SIPPYMODEM_V8_ANSAM_OK &&
		      sippymodem_v8_calling_process(
			      &calling, receive, transmit,
			      ARRAY_LENGTH(receive)) == SIPPYMODEM_V8_CALLING_OK,
		      "V.8 menu-timeout fixture did not reach menu exchange");
	}
	memset(receive, 0, sizeof(receive));
	for (size_t offset = 0;
	     offset < SIPPYMODEM_V8_CALLING_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(receive))
		result = sippymodem_v8_calling_process(
			&calling, receive, transmit, ARRAY_LENGTH(receive));
	CHECK(result == SIPPYMODEM_V8_CALLING_MENU_TIMEOUT &&
	      calling.state == SIPPYMODEM_V8_CALLING_FAILED &&
	      sippymodem_v8_calling_process(
		      &calling, receive, transmit, 1) ==
		      SIPPYMODEM_V8_CALLING_MENU_TIMEOUT,
	      "V.8 menu timeout did not latch");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v8_calling calling;
	struct sippymodem_v8_calling saved;
	struct sippymodem_v8_menu menu;
	struct sippymodem_v8_menu selected_menu;
	enum sippymodem_v8_modulation selected =
		SIPPYMODEM_V8_MODULATION_V34_DUPLEX;
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
	      sippymodem_v8_calling_selection(
		      &calling, &selected_menu, &selected) ==
		      SIPPYMODEM_V8_CALLING_NOT_READY &&
	      selected == SIPPYMODEM_V8_MODULATION_V34_DUPLEX &&
	      sippymodem_v8_calling_selection(
		      &calling, NULL, &selected) ==
		      SIPPYMODEM_V8_CALLING_INVALID_ARGUMENT &&
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
		{ "call negotiation", test_call_negotiation },
		{ "no common modulation", test_no_common_modulation },
		{ "answer signal timeout", test_answer_signal_timeout },
		{ "menu timeout", test_menu_timeout },
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
