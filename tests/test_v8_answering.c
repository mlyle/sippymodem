// SPDX-License-Identifier: BSD-3-Clause

#include <v8_answering.h>
#include <v8_calling.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash ^= word & UINT16_C(0xff);
	hash *= UINT64_C(1099511628211);
	hash ^= word >> 8;
	return hash * UINT64_C(1099511628211);
}

static bool endpoints_init(
	struct v8_calling *calling,
	struct v8_answering *answering,
	uint16_t calling_modulations, uint16_t answering_modulations)
{
	struct v8_menu calling_menu;
	struct v8_menu answering_menu;

	return v8_call_menu_init(
		       &calling_menu, calling_modulations, true) ==
		       V8_MENU_OK &&
	       v8_call_menu_init(
		       &answering_menu, answering_modulations, true) ==
		       V8_MENU_OK &&
	       v8_calling_init(calling, &calling_menu) ==
		       V8_CALLING_OK &&
	       v8_answering_init(answering, &answering_menu) ==
		       V8_ANSWERING_OK;
}

static bool test_call_negotiation(void)
{
	struct v8_answering answering;
	struct v8_calling calling;
	struct v8_menu calling_selection;
	struct v8_menu answering_selection;
	enum v8_modulation calling_modulation;
	enum v8_modulation answering_modulation;
	int16_t calling_transmit = 0;
	size_t completed = 0;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V21,
		      V8_MODULATION_V21),
	      "V.8 endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t answering_transmit;
		enum v8_answering_state before = answering.state;

		CHECK(v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      V8_ANSWERING_OK &&
		      v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 endpoints failed at sample %zu", sample_index);
		if (before == V8_ANSWERING_COMPLETE)
			CHECK(answering_transmit == 0,
			      "V.8 answerer emitted audio after CJ");
		if (before != answering.state &&
			 answering.state == V8_ANSWERING_COMPLETE)
			completed = sample_index + 1u;
		if (v8_calling_is_complete(&calling) &&
		    v8_answering_is_complete(&answering))
			break;
	}
	CHECK(completed != 0 &&
	      v8_calling_is_complete(&calling) &&
	      v8_answering_is_complete(&answering),
	      "V.8 endpoints did not complete the timed handshake");
	CHECK(v8_calling_selection(
		      &calling, &calling_selection, &calling_modulation) ==
		      V8_CALLING_OK &&
	      v8_answering_selection(
		      &answering, &answering_selection,
		      &answering_modulation) == V8_ANSWERING_OK &&
	      calling_modulation == V8_MODULATION_V21 &&
	      answering_modulation == calling_modulation &&
	      answering_selection.modulations ==
		      calling_selection.modulations &&
	      answering_selection.lapm && calling_selection.lapm,
	      "V.8 endpoints disagreed on the selected data mode");
	return true;
}

static bool test_answer_signal_wire(void)
{
	struct v8_answering answering;
	struct v8_menu menu;
	int16_t receive[73] = { 0 };
	int16_t transmit[ARRAY_LENGTH(receive)];
	uint64_t hash = UINT64_C(14695981039346656037);
	size_t first_nonzero = SIZE_MAX;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V34_DUPLEX, false) == V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) == V8_ANSWERING_OK,
	      "V.8 answer-signal oracle did not initialize");
	for (size_t offset = 0; offset < 9000;) {
		size_t count = ARRAY_LENGTH(receive);

		if (count > 9000u - offset)
			count = 9000u - offset;
		CHECK(v8_answering_process(
			      &answering, receive, transmit, count) ==
			      V8_ANSWERING_OK,
		      "V.8 answer signal stopped at %zu", offset);
		for (size_t index = 0; index < count; ++index) {
			if (first_nonzero == SIZE_MAX && transmit[index] != 0)
				first_nonzero = offset + index;
			hash = hash_word(hash, (uint16_t)transmit[index]);
		}
		offset += count;
	}
	CHECK(first_nonzero == V8_ANSWERING_QUIET_SAMPLES &&
		      hash == UINT64_C(0xd927fafa7f14d42f) &&
		      answering.quiet_samples == 0 &&
		      answering.state == V8_ANSWERING_WAIT_CALL_MENU,
	      "V.8 answer wire changed at %zu: %#llx",
	      first_nonzero, (unsigned long long)hash);
	return true;
}

static bool test_no_common_modulation(void)
{
	struct v8_answering answering;
	struct v8_calling calling;
	enum v8_answering_result result =
		V8_ANSWERING_OK;
	int16_t calling_transmit = 0;
	int16_t answering_transmit = 0;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V21,
		      V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		result = v8_answering_process(
			&answering, &calling_transmit, &answering_transmit, 1);
		if (result != V8_ANSWERING_OK)
			break;
		CHECK(v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 incompatible caller failed early");
	}
	CHECK(result == V8_ANSWERING_NO_COMMON_MODULATION &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, &calling_transmit,
		      &answering_transmit, 1) ==
		      V8_ANSWERING_NO_COMMON_MODULATION,
	      "V.8 answerer did not latch the incompatible offer");
	return true;
}

static bool test_call_menu_timeout(void)
{
	struct v8_answering answering;
	struct v8_menu menu;
	int16_t silence[V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum v8_answering_result result =
		V8_ANSWERING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) ==
		      V8_ANSWERING_OK,
	      "V.8 answerer timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == V8_ANSWERING_CALL_MENU_TIMEOUT &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      V8_ANSWERING_CALL_MENU_TIMEOUT,
	      "V.8 call-menu timeout did not latch");
	return true;
}

static bool test_cj_timeout(void)
{
	struct v8_answering answering;
	struct v8_calling calling;
	int16_t calling_transmit = 0;
	int16_t silence[V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum v8_answering_result result =
		V8_ANSWERING_OK;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V21,
		      V8_MODULATION_V21),
	      "V.8 CJ-timeout endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 4u * V8_ANSAM_SAMPLE_RATE &&
	     answering.state == V8_ANSWERING_WAIT_CALL_MENU;
	     ++sample_index) {
		int16_t answering_transmit;

		CHECK(v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      V8_ANSWERING_OK &&
		      v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 CJ-timeout endpoints failed during CM");
	}
	CHECK(answering.state == V8_ANSWERING_SEND_JOINT_MENU,
	      "V.8 answerer did not reach the CJ wait");
	for (size_t offset = 0;
	     offset < V8_ANSWERING_CJ_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == V8_ANSWERING_CJ_TIMEOUT &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      V8_ANSWERING_CJ_TIMEOUT,
	      "V.8 CJ timeout did not latch");
	return true;
}

static bool reach_cj_exchange(
	struct v8_calling *calling, struct v8_answering *answering)
{
	int16_t calling_transmit = 0;

	for (size_t sample = 0;
	     sample < 4u * V8_ANSAM_SAMPLE_RATE; ++sample) {
		int16_t answering_transmit;

		if (v8_answering_process(
			    answering, &calling_transmit,
			    &answering_transmit, 1) !=
			    V8_ANSWERING_OK ||
		    v8_calling_process(
			    calling, &answering_transmit,
			    &calling_transmit, 1) != V8_CALLING_OK)
			return false;
		if (calling->state == V8_CALLING_SEND_CJ &&
		    answering->state == V8_ANSWERING_SEND_JOINT_MENU)
			return true;
	}
	return false;
}

static bool drive_damaged_cj(
	struct v8_calling *calling, struct v8_answering *answering,
	size_t damaged_bit, bool expect_fallback)
{
	uint8_t transmit_bit = 1;
	size_t next_bit = 0;
	bool saw_guard_boundary = false;

	for (size_t sample = 0;
	     sample < 2u * V8_ANSAM_SAMPLE_RATE; ++sample) {
		bool consumed;
		int16_t receive;
		int16_t transmit;

		if (v8_fsk_can_accept_bit(&calling->fsk)) {
			if (next_bit < V8_CJ_BITS) {
				if (v8_cj_bit(next_bit, &transmit_bit) !=
				    V8_FRAMING_OK)
					return false;
				if (next_bit == damaged_bit)
					transmit_bit ^= 1u;
			} else {
				transmit_bit = 1;
			}
			next_bit++;
		}
		if (v8_fsk_transmit_sample(
			    &calling->fsk, transmit_bit, &receive,
			    &consumed) != V8_FSK_OK ||
		    v8_answering_process(
			    answering, &receive, &transmit, 1) !=
			    V8_ANSWERING_OK)
			return false;
		if (answering->cj_fallback_armed &&
		    answering->cj_fallback_bits ==
			    V8_ANSWERING_CJ_FALLBACK_BITS - 1u) {
			if (answering->state !=
			    V8_ANSWERING_SEND_JOINT_MENU)
				return false;
			saw_guard_boundary = true;
		}
		if (answering->state == V8_ANSWERING_COMPLETE)
			return expect_fallback && saw_guard_boundary;
		if (!expect_fallback && next_bit > V8_CJ_BITS +
							 2u * V8_FRAMED_OCTET_BITS)
			return !answering->cj_fallback_armed;
	}
	return false;
}

static bool test_cj_completion_fallback(void)
{
	struct v8_answering answering;
	struct v8_calling calling;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V34_DUPLEX,
		      V8_MODULATION_V34_DUPLEX) &&
	      reach_cj_exchange(&calling, &answering) &&
	      drive_damaged_cj(
		      &calling, &answering, V8_CJ_BITS - 1u, true),
	      "V.8 answerer did not bound a damaged final CJ stop bit");
	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V34_DUPLEX,
		      V8_MODULATION_V34_DUPLEX) &&
	      reach_cj_exchange(&calling, &answering) &&
	      drive_damaged_cj(
		      &calling, &answering,
		      2u * V8_FRAMED_OCTET_BITS - 1u, false),
	      "V.8 answerer accepted fewer than two complete CJ octets");
	return true;
}

static bool test_validation(void)
{
	struct v8_answering answering;
	struct v8_answering saved;
	struct v8_menu menu;
	struct v8_menu selected_menu;
	enum v8_modulation selected =
		V8_MODULATION_V34_DUPLEX;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) ==
		      V8_ANSWERING_OK,
	      "V.8 answering validation fixture did not initialize");
	saved = answering;
	CHECK(v8_answering_init(NULL, &menu) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      v8_answering_process(
		      &answering, &receive, &transmit,
		      V8_ANSWERING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      transmit == 123 &&
	      memcmp(&answering, &saved, sizeof(answering)) == 0 &&
	      v8_answering_process(
		      &answering, NULL, &transmit, 1) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      v8_answering_process(
		      &answering, NULL, NULL, 0) ==
		      V8_ANSWERING_OK &&
	      v8_answering_selection(
		      &answering, &selected_menu, &selected) ==
		      V8_ANSWERING_NOT_READY &&
	      selected == V8_MODULATION_V34_DUPLEX &&
	      strcmp(v8_answering_result_name(
			     V8_ANSWERING_CJ_TIMEOUT),
		     "CJ timeout") == 0,
	      "V.8 answering API accepted invalid arguments");
	answering.cj_fallback_armed = true;
	saved = answering;
	CHECK(v8_answering_process(
		      &answering, &receive, &transmit, 1) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      memcmp(&answering, &saved, sizeof(answering)) == 0,
	      "V.8 answering accepted a fallback outside the CJ wait");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "answer signal wire", test_answer_signal_wire },
		{ "call negotiation", test_call_negotiation },
		{ "no common modulation", test_no_common_modulation },
		{ "call menu timeout", test_call_menu_timeout },
		{ "CJ timeout", test_cj_timeout },
		{ "CJ completion fallback", test_cj_completion_fallback },
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
