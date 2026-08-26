// SPDX-License-Identifier: BSD-3-Clause

#include <v8_calling.h>

#include "test.h"

#include <string.h>

enum remote_answer_state {
	REMOTE_ANSWER_SIGNAL,
	REMOTE_JOINT_MENU,
	REMOTE_SILENCE,
};

struct remote_answerer {
	struct v8_ansam_generator answer_signal;
	struct v8_fsk fsk;
	struct v8_menu_detector call_menu_detector;
	struct v8_control_generator joint_menu_bits;
	struct v8_cj_detector cj_detector;
	struct v8_menu local_menu;
	struct v8_menu joint_menu;
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

	if (v8_ansam_generator_init(
		    &initialized.answer_signal, 10000, true) !=
		    V8_ANSAM_OK ||
	    v8_fsk_init(
		    &initialized.fsk, V8_ANSWERING_STATION) !=
		    V8_FSK_OK ||
	    v8_menu_detector_init(
		    &initialized.call_menu_detector) != V8_MENU_OK ||
	    v8_cj_detector_init(&initialized.cj_detector) !=
		    V8_FRAMING_OK ||
	    v8_call_menu_init(
		    &initialized.local_menu, modulations, true) !=
		    V8_MENU_OK)
		return false;
	*answerer = initialized;
	return true;
}

static bool remote_answerer_transmit(
	struct remote_answerer *answerer, int16_t *sample)
{
	bool consumed;

	if (answerer->state == REMOTE_ANSWER_SIGNAL)
		return v8_ansam_generate(
			       &answerer->answer_signal, sample, 1) ==
		       V8_ANSAM_OK;
	if (answerer->state == REMOTE_JOINT_MENU) {
		if (v8_fsk_can_accept_bit(&answerer->fsk)) {
			bool sequence_complete;

			if (v8_control_next_bit(
				    &answerer->joint_menu_bits,
				    &answerer->transmit_bit,
				    &sequence_complete) !=
			    V8_FRAMING_OK)
				return false;
		}
		return v8_fsk_transmit_sample(
			       &answerer->fsk, answerer->transmit_bit, sample,
			       &consumed) == V8_FSK_OK;
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
	if (v8_fsk_receive_sample(
		    &answerer->fsk, sample, &bit, &ready) !=
	    V8_FSK_OK)
		return false;
	if (!ready)
		return true;
	if (answerer->state == REMOTE_ANSWER_SIGNAL) {
		struct v8_menu call_menu;
		uint8_t information[V8_MAX_INFORMATION_OCTETS];
		size_t information_count;
		bool confirmed;

		if (v8_menu_detect_bit(
			    &answerer->call_menu_detector, bit, &call_menu,
			    &confirmed) != V8_MENU_OK)
			return false;
		if (!confirmed)
			return true;
		if (v8_joint_menu_select(
			    &call_menu, &answerer->local_menu,
			    &answerer->joint_menu) != V8_MENU_OK ||
		    v8_menu_encode(
			    &answerer->joint_menu, information,
			    sizeof(information), &information_count) !=
			    V8_MENU_OK ||
		    v8_control_generator_init(
			    &answerer->joint_menu_bits, information,
			    information_count) != V8_FRAMING_OK)
			return false;
		answerer->call_menu_confirmed = true;
		answerer->state = REMOTE_JOINT_MENU;
		return true;
	}
	if (v8_cj_detect_bit(
		    &answerer->cj_detector, bit, &answerer->cj_detected) !=
	    V8_FRAMING_OK)
		return false;
	if (answerer->cj_detected)
		answerer->state = REMOTE_SILENCE;
	return true;
}

static bool test_call_negotiation(void)
{
	struct v8_calling calling;
	struct v8_menu local_menu;
	struct v8_menu negotiated_menu;
	struct remote_answerer answerer;
	enum v8_modulation modulation;
	size_t exchange_started = 0;
	size_t completed = 0;

	CHECK(v8_call_menu_init(
		      &local_menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &local_menu) ==
		      V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, V8_MODULATION_V21),
	      "V.8 negotiation endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t receive;
		int16_t transmit;
		enum v8_calling_state before = calling.state;

		CHECK(remote_answerer_transmit(&answerer, &receive) &&
		      v8_calling_process(
			      &calling, &receive, &transmit, 1) ==
		      V8_CALLING_OK,
		      "V.8 calling station failed at sample %zu", sample_index);
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 answering station failed at sample %zu", sample_index);
		if (before == V8_CALLING_WAIT_ANSWER_SIGNAL ||
		    before == V8_CALLING_COMPLETE)
			CHECK(transmit == 0,
			      "V.8 silent state emitted audio at sample %zu",
			      sample_index);
		if (before != calling.state) {
			if (calling.state ==
				 V8_CALLING_EXCHANGE_MENUS)
				exchange_started = sample_index + 1u;
			else if (calling.state ==
				 V8_CALLING_COMPLETE)
				completed = sample_index + 1u;
		}
		if (v8_calling_is_complete(&calling) &&
		    answerer.cj_detected)
			break;
	}
	CHECK(exchange_started >
		      V8_ANSAM_POST_DETECT_DWELL_BLOCKS *
			      V8_ANSAM_BLOCK_SAMPLES &&
	      completed != 0 &&
	      v8_calling_is_complete(&calling) &&
	      answerer.call_menu_confirmed && answerer.cj_detected,
	      "V.8 calling station did not complete the timed exchange");
	CHECK(v8_calling_selection(
		      &calling, &negotiated_menu, &modulation) ==
		      V8_CALLING_OK &&
	      modulation == V8_MODULATION_V21 &&
	      negotiated_menu.modulations == V8_MODULATION_V21 &&
	      negotiated_menu.lapm,
	      "V.8 calling station did not expose the selected V.21 menu");
	return true;
}

static bool test_no_common_modulation(void)
{
	struct v8_calling calling;
	struct v8_menu local_menu;
	struct remote_answerer answerer;
	enum v8_calling_result result = V8_CALLING_OK;
	int16_t receive = 0;
	int16_t transmit = 0;

	CHECK(v8_call_menu_init(
		      &local_menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &local_menu) ==
		      V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		CHECK(remote_answerer_transmit(&answerer, &receive),
		      "V.8 incompatible answerer failed to transmit");
		result = v8_calling_process(
			&calling, &receive, &transmit, 1);
		if (result != V8_CALLING_OK)
			break;
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 incompatible answerer failed to receive");
	}
	CHECK(result == V8_CALLING_NO_COMMON_MODULATION &&
	      calling.state == V8_CALLING_FAILED &&
	      answerer.call_menu_confirmed && !answerer.cj_detected &&
	      v8_calling_process(
		      &calling, &receive, &transmit, 1) ==
		      V8_CALLING_NO_COMMON_MODULATION,
	      "V.8 incompatible modulation result did not latch");
	return true;
}

static bool test_answer_signal_timeout(void)
{
	struct v8_calling calling;
	struct v8_menu menu;
	int16_t silence[V8_CALLING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum v8_calling_result result = V8_CALLING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence)) {
		memset(transmit, 1, sizeof(transmit));
		result = v8_calling_process(
			&calling, silence, transmit, ARRAY_LENGTH(silence));
		CHECK((result == V8_CALLING_OK ||
		       result == V8_CALLING_ANSWER_TIMEOUT) &&
			      memcmp(transmit, silence, sizeof(transmit)) == 0,
		      "V.8 answer timeout emitted audio or failed early");
	}
	CHECK(result == V8_CALLING_ANSWER_TIMEOUT &&
		      calling.state == V8_CALLING_FAILED &&
		      !v8_calling_is_complete(&calling) &&
		      v8_calling_process(
			      &calling, silence, transmit, 1) ==
			      V8_CALLING_ANSWER_TIMEOUT,
	      "V.8 calling timeout did not latch");
	return true;
}

static bool test_menu_timeout(void)
{
	struct v8_ansam_generator answer_signal;
	struct v8_calling calling;
	struct v8_menu menu;
	int16_t receive[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	int16_t transmit[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum v8_calling_result result = V8_CALLING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_ansam_generator_init(
		      &answer_signal, 10000, true) == V8_ANSAM_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 menu-timeout fixture did not initialize");
	while (calling.state != V8_CALLING_EXCHANGE_MENUS) {
		CHECK(v8_ansam_generate(
			      &answer_signal, receive, ARRAY_LENGTH(receive)) ==
			      V8_ANSAM_OK &&
		      v8_calling_process(
			      &calling, receive, transmit,
			      ARRAY_LENGTH(receive)) == V8_CALLING_OK,
		      "V.8 menu-timeout fixture did not reach menu exchange");
	}
	memset(receive, 0, sizeof(receive));
	for (size_t offset = 0;
	     offset < V8_CALLING_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(receive))
		result = v8_calling_process(
			&calling, receive, transmit, ARRAY_LENGTH(receive));
	CHECK(result == V8_CALLING_MENU_TIMEOUT &&
	      calling.state == V8_CALLING_FAILED &&
	      v8_calling_process(
		      &calling, receive, transmit, 1) ==
		      V8_CALLING_MENU_TIMEOUT,
	      "V.8 menu timeout did not latch");
	return true;
}

static bool test_validation(void)
{
	struct v8_calling calling;
	struct v8_calling saved;
	struct v8_menu menu;
	struct v8_menu selected_menu;
	enum v8_modulation selected =
		V8_MODULATION_V34_DUPLEX;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 calling validation fixture did not initialize");
	saved = calling;
	CHECK(v8_calling_init(NULL, &menu) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      v8_calling_process(
		      &calling, &receive, &transmit,
		      V8_CALLING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      transmit == 123 && memcmp(&calling, &saved, sizeof(calling)) == 0 &&
	      v8_calling_process(
		      &calling, NULL, &transmit, 1) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      v8_calling_process(
		      &calling, NULL, NULL, 0) == V8_CALLING_OK &&
	      v8_calling_selection(
		      &calling, &selected_menu, &selected) ==
		      V8_CALLING_NOT_READY &&
	      selected == V8_MODULATION_V34_DUPLEX &&
	      v8_calling_selection(
		      &calling, NULL, &selected) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      strcmp(v8_calling_result_name(
			     V8_CALLING_ANSWER_TIMEOUT),
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
