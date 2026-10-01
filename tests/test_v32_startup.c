// SPDX-License-Identifier: BSD-3-Clause

#include <startup.h>

#include "test.h"

#include <string.h>

enum {
	TEST_MAX_STARTUP_DIBITS = 4096,
};

static bool observe_word(
	struct v32_startup *startup, uint16_t word)
{
	for (unsigned int index = 0;
	     index < V32_RATE_SIGNAL_DIBITS; ++index) {
		uint8_t dibit;
		bool connected;

		CHECK(v32_rate_signal_dibit(word, index, &dibit) ==
			      V32_RATE_SIGNAL_OK &&
			      v32_startup_observe(
				      startup, dibit, &connected) ==
				      V32_STARTUP_OK &&
			      !connected,
		      "V.32 startup could not observe word 0x%04x", word);
	}
	return true;
}

static enum v32_startup_output expected_output(
	enum v32_startup_phase phase)
{
	if (phase == V32_DATA)
		return V32_STARTUP_DATA;
	if (phase == V32_CONFIRM)
		return V32_STARTUP_SELECTED_RATE_MARK;
	if (phase == V32_CALLER_WAIT_OFFER ||
	    phase == V32_ANSWERER_TRAIN ||
	    phase == V32_ANSWERER_RETRAIN)
		return V32_STARTUP_TRN_DIBIT;
	return V32_STARTUP_DIFFERENTIAL_DIBIT;
}

static bool check_coding_exchange(
	uint8_t caller_rates, uint8_t answerer_rates, uint8_t expected_rate,
	bool caller_uncoded, bool answerer_uncoded)
{
	struct v32_startup caller;
	struct v32_startup answerer;
	struct v32_rate_offer selected = {
		.rates = expected_rate,
		.trellis_at_highest_rate =
			expected_rate == V32_RATE_CAPABILITY_7200 ||
			expected_rate == V32_RATE_CAPABILITY_9600 ||
			expected_rate == V32_RATE_CAPABILITY_12000 ||
			expected_rate == V32_RATE_CAPABILITY_14400,
	};
	struct v32_rate_offer caller_response = {
		.rates = caller_rates & answerer_rates,
		.trellis_at_highest_rate =
			expected_rate == V32_RATE_CAPABILITY_7200 ||
			expected_rate == V32_RATE_CAPABILITY_9600 ||
			expected_rate == V32_RATE_CAPABILITY_12000 ||
			expected_rate == V32_RATE_CAPABILITY_14400,
	};
	struct v32_rate_offer initial = { 0 };
	uint32_t caller_phases = 0;
	uint32_t answerer_phases = 0;
	uint16_t expected_rate_word;
	uint16_t expected_response_word;
	uint16_t expected_end_word;
	bool caller_connected = false;
	bool answerer_connected = false;
	bool answerer_counted_early_marks = false;
	size_t completed_at = SIZE_MAX;

	CHECK(v32_startup_init(
		      &caller, V32_CALLER, caller_rates) ==
		      V32_STARTUP_OK &&
		      v32_startup_init(
			      &answerer, V32_ANSWERER,
			      answerer_rates) ==
		      V32_STARTUP_OK,
	      "V.32 startup peers 0x%x/0x%x did not initialize", caller_rates,
	      answerer_rates);
	if (caller_uncoded) {
		initial.rates = caller_rates;
		CHECK(v32_rate_signal_encode(V32_RATE_SIGNAL, &initial,
			      &caller.rate_word) == V32_RATE_SIGNAL_OK,
		      "uncoded caller offer did not initialize");
	}
	if (answerer_uncoded) {
		initial.rates = answerer_rates;
		CHECK(v32_rate_signal_encode(V32_RATE_SIGNAL, &initial,
			      &answerer.rate_word) == V32_RATE_SIGNAL_OK,
		      "uncoded answerer offer did not initialize");
	}
	if (expected_rate == V32_RATE_CAPABILITY_9600 &&
	    (caller_uncoded || answerer_uncoded)) {
		selected.trellis_at_highest_rate = false;
		caller_response.trellis_at_highest_rate = false;
	}
	for (size_t index = 0; index < TEST_MAX_STARTUP_DIBITS; ++index) {
		enum v32_startup_output caller_output;
		enum v32_startup_output answerer_output;
		enum v32_startup_phase caller_phase = caller.phase;
		enum v32_startup_phase answerer_phase = answerer.phase;
		uint8_t caller_dibit = 3;
		uint8_t answerer_dibit = 3;

		if (caller.phase == V32_CALLER_SEND_RESPONSE &&
		    answerer.phase ==
			    V32_ANSWERER_SEND_SELECTION &&
		    caller.selected_rate == 0 && !caller.selection_expected)
			CHECK(v32_startup_expect_selection(&caller) ==
				      V32_STARTUP_OK,
			      "V.32 caller could not accept R3 at dibit %zu",
			      index);
		caller_phases |= UINT32_C(1) << caller.phase;
		answerer_phases |= UINT32_C(1) << answerer.phase;
		CHECK(v32_startup_next_transmit(
			      &caller, &caller_output, &caller_dibit) ==
			      V32_STARTUP_OK &&
			      v32_startup_next_transmit(
				      &answerer, &answerer_output,
				      &answerer_dibit) ==
				      V32_STARTUP_OK,
		      "V.32 startup could not transmit at dibit %zu", index);
		CHECK(caller_output == expected_output(caller_phase) &&
			      answerer_output == expected_output(answerer_phase),
		      "V.32 startup selected the wrong framing at dibit %zu",
		      index);
		CHECK(v32_startup_observe(
			      &caller, answerer_dibit, &caller_connected) ==
			      V32_STARTUP_OK &&
			      v32_startup_observe(
				      &answerer, caller_dibit,
				      &answerer_connected) ==
				      V32_STARTUP_OK,
		      "V.32 startup could not receive at dibit %zu", index);
		if (answerer.end_received &&
		    answerer.phase == V32_ANSWERER_SEND_SELECTION &&
		    answerer.receive_mark_dibits != 0)
			answerer_counted_early_marks = true;
		if (caller_connected && answerer_connected) {
			completed_at = index;
			break;
		}
	}
	caller_phases |= UINT32_C(1) << caller.phase;
	answerer_phases |= UINT32_C(1) << answerer.phase;
	CHECK(completed_at != SIZE_MAX &&
		      v32_startup_is_complete(&caller) &&
		      v32_startup_is_complete(&answerer) &&
		      caller.supported_rates == caller_rates &&
		      answerer.supported_rates == answerer_rates &&
		      caller.transmitted_rates == caller_response.rates &&
		      answerer.transmitted_rates == expected_rate &&
		      caller.selected_rate == expected_rate &&
		      answerer.selected_rate == expected_rate &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &caller_response,
			      &expected_response_word) ==
			      V32_RATE_SIGNAL_OK &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &selected,
			      &expected_rate_word) ==
			      V32_RATE_SIGNAL_OK &&
		      v32_rate_signal_encode(
			      V32_END_SIGNAL, &selected,
			      &expected_end_word) ==
			      V32_RATE_SIGNAL_OK &&
		      caller.rate_word == expected_response_word &&
		      answerer.rate_word == expected_rate_word &&
		      caller.end_word == expected_end_word &&
		      answerer.end_word == expected_end_word,
	      "V.32 peers 0x%x/0x%x did not select 0x%x", caller_rates,
	      answerer_rates, expected_rate);
	CHECK((caller_phases &
	       ((UINT32_C(1) << V32_CALLER_WAIT_OFFER) |
		(UINT32_C(1) << V32_CALLER_SEND_RESPONSE) |
		(UINT32_C(1) << V32_CALLER_SEND_END) |
		(UINT32_C(1) << V32_CONFIRM) |
		(UINT32_C(1) << V32_DATA))) ==
		      ((UINT32_C(1) << V32_CALLER_WAIT_OFFER) |
		       (UINT32_C(1) << V32_CALLER_SEND_RESPONSE) |
		       (UINT32_C(1) << V32_CALLER_SEND_END) |
		       (UINT32_C(1) << V32_CONFIRM) |
		       (UINT32_C(1) << V32_DATA)),
	      "V.32 caller skipped a startup phase");
	CHECK((answerer_phases &
	       ((UINT32_C(1) << V32_ANSWERER_TRAIN) |
		(UINT32_C(1) << V32_ANSWERER_SEND_OFFER) |
		(UINT32_C(1) << V32_ANSWERER_RETRAIN) |
		(UINT32_C(1) << V32_ANSWERER_SEND_SELECTION) |
		(UINT32_C(1) << V32_ANSWERER_SEND_END) |
		(UINT32_C(1) << V32_CONFIRM) |
		(UINT32_C(1) << V32_DATA))) ==
		      ((UINT32_C(1) << V32_ANSWERER_TRAIN) |
		       (UINT32_C(1) << V32_ANSWERER_SEND_OFFER) |
		       (UINT32_C(1) << V32_ANSWERER_RETRAIN) |
		       (UINT32_C(1) << V32_ANSWERER_SEND_SELECTION) |
		       (UINT32_C(1) << V32_ANSWERER_SEND_END) |
		       (UINT32_C(1) << V32_CONFIRM) |
		       (UINT32_C(1) << V32_DATA)),
	      "V.32 answerer skipped a startup phase");
	CHECK(answerer_counted_early_marks,
	      "V.32 answerer discarded post-E marks during selection");
	return true;
}

static bool check_role_exchange(
	uint8_t caller_rates, uint8_t answerer_rates, uint8_t expected_rate)
{
	return check_coding_exchange(caller_rates, answerer_rates, expected_rate,
				     false, false);
}

static bool test_rate_selection(void)
{
	uint8_t v32_rates = V32_RATE_CAPABILITY_4800 |
			   V32_RATE_CAPABILITY_9600;
	uint8_t through_7200 = V32_RATE_CAPABILITY_4800 |
			       V32_RATE_CAPABILITY_7200;
	uint8_t through_9600 =
		through_7200 | V32_RATE_CAPABILITY_9600;
	uint8_t through_12000 =
		through_9600 | V32_RATE_CAPABILITY_12000;
	uint8_t through_14400 =
		through_12000 | V32_RATE_CAPABILITY_14400;

	return check_role_exchange(
		       through_14400, through_14400,
		       V32_RATE_CAPABILITY_14400) &&
	       check_role_exchange(through_14400, through_12000,
				   V32_RATE_CAPABILITY_12000) &&
	       check_role_exchange(through_12000, through_14400,
				   V32_RATE_CAPABILITY_12000) &&
	       check_role_exchange(
		       through_12000, through_12000,
		       V32_RATE_CAPABILITY_12000) &&
	       check_role_exchange(through_12000, through_9600,
				   V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(through_9600, through_12000,
				   V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(v32_rates, v32_rates,
				   V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(through_7200, through_7200,
				   V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(through_9600, through_7200,
				   V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(through_7200, through_9600,
				   V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(v32_rates,
				   V32_RATE_CAPABILITY_4800,
				   V32_RATE_CAPABILITY_4800) &&
	       check_role_exchange(V32_RATE_CAPABILITY_4800,
				   v32_rates,
				   V32_RATE_CAPABILITY_4800) &&
	       check_role_exchange(V32_RATE_CAPABILITY_9600,
				   V32_RATE_CAPABILITY_9600,
				   V32_RATE_CAPABILITY_9600);
}

static bool test_uncoded_9600_negotiation(void)
{
	static const uint8_t masks[] = {
		V32_RATE_CAPABILITY_4800 | V32_RATE_CAPABILITY_9600,
		V32_RATE_CAPABILITY_9600,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(masks); ++index)
		CHECK(check_coding_exchange(masks[index], masks[index],
			      V32_RATE_CAPABILITY_9600, false, true) &&
		      check_coding_exchange(masks[index], masks[index],
			      V32_RATE_CAPABILITY_9600, true, false) &&
		      check_coding_exchange(masks[index], masks[index],
			      V32_RATE_CAPABILITY_9600, true, true),
		      "V.32 peers did not retain uncoded 9600 for mask %x", masks[index]);
	return true;
}

static bool test_coding_selection_is_retained(void)
{
	const struct v32_rate_offer uncoded = { .rates = V32_RATE_CAPABILITY_9600 };
	const struct v32_rate_offer coded = {
		.rates = V32_RATE_CAPABILITY_9600, .trellis_at_highest_rate = true,
	};
	struct v32_startup caller, saved;
	struct v32_rate_offer selected = { .rates = V32_RATE_CAPABILITY_4800 };
	struct v32_rate_offer saved_offer = selected;

	CHECK(v32_startup_init(&caller, V32_CALLER, uncoded.rates) == V32_STARTUP_OK &&
	      !v32_startup_selected_offer(&caller, &selected) &&
	      v32_rate_offers_match(&selected, &saved_offer) &&
	      v32_startup_note_rate_signal(&caller, &uncoded) == V32_STARTUP_OK &&
	      caller.rate_word == 0x0311 &&
	      v32_startup_expect_selection(&caller) == V32_STARTUP_OK,
	      "uncoded R1 was not preserved in the caller's R2");
	saved = caller;
	CHECK(v32_startup_note_rate_signal(&caller, &coded) == V32_STARTUP_NO_COMMON_RATE &&
	      memcmp(&caller, &saved, sizeof(caller)) == 0 &&
	      v32_startup_note_rate_signal(&caller, &uncoded) == V32_STARTUP_OK &&
	      caller.end_word == 0xf311 &&
	      v32_startup_selected_offer(&caller, &selected) &&
	      v32_rate_offers_match(&selected, &uncoded),
	      "R3 changed the coding offered by R2 or lost the uncoded E selection");
	saved = caller;
	CHECK(v32_startup_note_end_signal(&caller, &coded) == V32_STARTUP_NO_COMMON_RATE &&
	      memcmp(&caller, &saved, sizeof(caller)) == 0,
	      "a mismatched peer E changed the selected coding");
	caller.end_word ^= 0x0080;
	CHECK(!v32_startup_is_valid(&caller),
	      "locally stored E contradicted the coding in R2");
	caller = saved;
	caller.end_word |= 0x0002;
	saved_offer = selected;
	CHECK(!v32_startup_is_valid(&caller) &&
	      !v32_startup_selected_offer(&caller, &selected) &&
	      v32_rate_offers_match(&selected, &saved_offer) &&
	      !v32_startup_selected_offer(NULL, &selected) &&
	      !v32_startup_selected_offer(&saved, NULL),
	      "invalid selected-word lookup changed its output");
	CHECK(v32_startup_init(&caller, V32_CALLER, coded.rates) == V32_STARTUP_OK &&
	      v32_startup_note_rate_signal(&caller, &coded) == V32_STARTUP_OK &&
	      caller.rate_word == 0x0391 &&
	      v32_startup_expect_selection(&caller) == V32_STARTUP_OK &&
	      v32_startup_note_rate_signal(&caller, &uncoded) == V32_STARTUP_OK &&
	      caller.end_word == 0xf311 &&
	      v32_startup_selected_offer(&caller, &selected) &&
	      v32_rate_offers_match(&selected, &uncoded),
	      "trellis availability prevented the peer from selecting uncoded 9600");
	return true;
}

static bool test_higher_rate_offer_fallback(void)
{
	struct v32_rate_offer offer = {
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_7200 |
			 V32_RATE_CAPABILITY_9600 |
			 V32_RATE_CAPABILITY_12000 |
			 V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct v32_startup caller;
	uint16_t word;

	CHECK(v32_startup_init(
		      &caller, V32_CALLER,
		      V32_RATE_CAPABILITY_4800 |
			      V32_RATE_CAPABILITY_7200 |
			      V32_RATE_CAPABILITY_9600) ==
		      V32_STARTUP_OK &&
	      v32_rate_signal_encode(
		      V32_RATE_SIGNAL, &offer, &word) ==
		      V32_RATE_SIGNAL_OK &&
	      observe_word(&caller, word) && observe_word(&caller, word) &&
	      caller.transmitted_rates ==
		      (V32_RATE_CAPABILITY_4800 |
		       V32_RATE_CAPABILITY_7200 |
		       V32_RATE_CAPABILITY_9600) &&
	      caller.selected_rate == 0 && caller.rate_word == 0x0fd1 &&
	      caller.end_word == 0,
	      "V.32 startup did not select its highest supported fallback");
	return true;
}

static bool test_training_boundary(void)
{
	struct v32_startup answerer;

	CHECK(v32_startup_init(
		      &answerer, V32_ANSWERER,
		      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_OK,
	      "V.32 training fixture did not initialize");
	for (size_t index = 0;
	     index < V32_STARTUP_TRAINING_DIBITS; ++index) {
		enum v32_startup_output output;
		uint8_t dibit = UINT8_MAX;

		CHECK(v32_startup_next_transmit(
			      &answerer, &output, &dibit) ==
			      V32_STARTUP_OK &&
			      output == V32_STARTUP_TRN_DIBIT &&
			      dibit == 3,
		      "V.32 answerer changed training dibit %zu", index);
	}
	CHECK(answerer.phase == V32_ANSWERER_SEND_OFFER,
	      "V.32 answerer did not finish bounded training");
	return true;
}

static bool test_selection_readiness_and_rate_boundary(void)
{
	struct v32_startup caller;

	CHECK(v32_startup_init(
		      &caller, V32_CALLER,
		      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_OK &&
		      observe_word(&caller, 0x0d11) &&
		      observe_word(&caller, 0x0d11) &&
		      caller.phase == V32_CALLER_SEND_RESPONSE,
	      "V.32 boundary fixture did not acquire R1");
	for (unsigned int index = 0; index < 3; ++index) {
		enum v32_startup_output output;
		uint8_t dibit;
		uint8_t expected;

		CHECK(v32_startup_next_transmit(
			      &caller, &output, &dibit) ==
			      V32_STARTUP_OK &&
			      v32_rate_signal_dibit(
				      caller.rate_word, index, &expected) ==
				      V32_RATE_SIGNAL_OK &&
			      output ==
				      V32_STARTUP_DIFFERENTIAL_DIBIT &&
			      dibit == expected,
		      "V.32 boundary fixture changed R2 dibit %u", index);
	}
	for (unsigned int index = 0; index < 40; ++index) {
		bool connected;

		CHECK(v32_startup_observe(
			      &caller, 3, &connected) ==
			      V32_STARTUP_OK &&
			      !connected,
		      "V.32 boundary fixture could not observe conditioning");
	}
	CHECK(observe_word(&caller, 0x0d11) &&
		      observe_word(&caller, 0x0d11) &&
		      caller.selected_rate == 0 && !caller.end_pending &&
		      v32_startup_expect_selection(&caller) ==
		      V32_STARTUP_OK &&
		      observe_word(&caller, 0x0d11) &&
		      observe_word(&caller, 0x0d11) && caller.end_pending &&
		      caller.selected_rate ==
			      V32_RATE_CAPABILITY_4800 &&
		      caller.phase == V32_CALLER_SEND_RESPONSE &&
		      caller.word_dibit == 3,
	      "V.32 startup did not defer E at a partial R2 word");
	for (unsigned int index = 3;
	     index < V32_RATE_SIGNAL_DIBITS; ++index) {
		enum v32_startup_output output;
		uint8_t dibit;
		uint8_t expected;

		CHECK(v32_startup_next_transmit(
			      &caller, &output, &dibit) ==
			      V32_STARTUP_OK &&
			      v32_rate_signal_dibit(
				      caller.rate_word, index, &expected) ==
				      V32_RATE_SIGNAL_OK &&
			      output ==
				      V32_STARTUP_DIFFERENTIAL_DIBIT &&
			      dibit == expected,
		      "V.32 startup truncated R2 at dibit %u", index);
	}
	CHECK(caller.phase == V32_CALLER_SEND_END &&
		      !caller.end_pending,
	      "V.32 startup did not enter E on the R2 boundary");
	return true;
}

static bool test_repeated_initial_offer(void)
{
	const struct v32_rate_offer broad_offer = {
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_7200 |
			 V32_RATE_CAPABILITY_9600 |
			 V32_RATE_CAPABILITY_12000 |
			 V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct v32_startup caller;
	uint16_t broad_word;

	CHECK(v32_startup_init(
		      &caller, V32_CALLER,
		      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_OK &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &broad_offer,
			      &broad_word) == V32_RATE_SIGNAL_OK &&
		      observe_word(&caller, broad_word) &&
		      observe_word(&caller, broad_word),
	      "V.32 repeated-offer fixture did not acquire R1");
	for (unsigned int index = 0; index < 40; ++index) {
		bool connected;

		CHECK(v32_startup_observe(
			      &caller, 3, &connected) ==
			      V32_STARTUP_OK &&
			      !connected,
		      "V.32 repeated-offer fixture could not observe conditioning");
	}
	CHECK(observe_word(&caller, broad_word) &&
		      observe_word(&caller, broad_word) &&
		      caller.phase == V32_CALLER_SEND_RESPONSE &&
		      caller.selected_rate == 0 &&
		      !caller.end_pending,
	      "V.32 caller mistook repeated R1 for the final selection");
	return true;
}

static bool test_multi_rate_selection(void)
{
	static const uint8_t masks[] = {
		V32_RATE_CAPABILITY_4800 | V32_RATE_CAPABILITY_7200,
		V32_RATE_CAPABILITY_4800 | V32_RATE_CAPABILITY_7200 |
			V32_RATE_CAPABILITY_9600,
		V32_RATE_CAPABILITY_4800 | V32_RATE_CAPABILITY_9600 |
			V32_RATE_CAPABILITY_12000 | V32_RATE_CAPABILITY_14400,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(masks); ++index) {
		struct v32_rate_offer offer = {
			.rates = masks[index],
			.trellis_at_highest_rate = true,
		};
		struct v32_startup caller;
		struct v32_rate_offer decoded;
		enum v32_rate_signal_type type;
		uint8_t highest = v32_rate_highest(offer.rates);
		uint16_t word;

		CHECK(v32_startup_init(&caller, V32_CALLER, offer.rates) ==
			      V32_STARTUP_OK &&
		      v32_rate_signal_encode(V32_RATE_SIGNAL, &offer, &word) ==
			      V32_RATE_SIGNAL_OK &&
		      observe_word(&caller, word) && observe_word(&caller, word) &&
		      caller.selected_rate == 0 && !caller.end_pending &&
		      v32_startup_expect_selection(&caller) == V32_STARTUP_OK &&
		      observe_word(&caller, word) && observe_word(&caller, word) &&
		      caller.selected_rate == highest && caller.end_pending &&
		      !caller.selection_expected && caller.rate_word == word,
		      "V.32 caller rejected multi-rate R3 mask %x", offer.rates);
		CHECK(v32_rate_signal_decode(caller.end_word, &type, &decoded) ==
			      V32_RATE_SIGNAL_OK && type == V32_END_SIGNAL &&
		      decoded.rates == highest && decoded.trellis_at_highest_rate,
		      "V.32 caller did not emit a single-rate E after mask %x",
		      offer.rates);
	}
	{
		const struct v32_rate_offer enabled = {
			.rates = V32_RATE_CAPABILITY_4800,
		};
		const struct v32_rate_offer outside_response = {
			.rates = V32_RATE_CAPABILITY_4800 |
				 V32_RATE_CAPABILITY_9600,
			.trellis_at_highest_rate = true,
		};
		struct v32_startup caller, before;

		CHECK(v32_startup_init(&caller, V32_CALLER, enabled.rates) ==
			      V32_STARTUP_OK &&
		      v32_startup_note_rate_signal(&caller, &enabled) ==
			      V32_STARTUP_OK &&
		      v32_startup_expect_selection(&caller) == V32_STARTUP_OK,
		      "V.32 out-of-response selection fixture did not initialize");
		before = caller;
		CHECK(v32_startup_note_rate_signal(&caller, &outside_response) ==
			      V32_STARTUP_NO_COMMON_RATE &&
		      memcmp(&caller, &before, sizeof(caller)) == 0,
		      "V.32 caller changed state for R3 above its R2 ceiling");
	}
	return true;
}

static bool test_answerer_selects_lower_rate(void)
{
	const uint8_t rates = V32_RATE_CAPABILITY_4800 |
			      V32_RATE_CAPABILITY_7200 |
			      V32_RATE_CAPABILITY_9600 |
			      V32_RATE_CAPABILITY_12000 |
			      V32_RATE_CAPABILITY_14400;
	const struct v32_rate_offer offer = {
		.rates = rates,
		.trellis_at_highest_rate = true,
	};
	const struct v32_rate_offer selection = {
		.rates = V32_RATE_CAPABILITY_7200,
		.trellis_at_highest_rate = true,
	};
	struct v32_startup caller;
	uint16_t offer_word;
	uint16_t selection_word;
	uint16_t end_word;

	CHECK(v32_startup_init(
		      &caller, V32_CALLER, rates) ==
		      V32_STARTUP_OK &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &offer, &offer_word) ==
			      V32_RATE_SIGNAL_OK &&
		      v32_rate_signal_encode(
			      V32_RATE_SIGNAL, &selection,
			      &selection_word) == V32_RATE_SIGNAL_OK &&
		      v32_rate_signal_encode(
			      V32_END_SIGNAL, &selection,
			      &end_word) == V32_RATE_SIGNAL_OK &&
		      observe_word(&caller, offer_word) &&
		      observe_word(&caller, offer_word) &&
		      caller.transmitted_rates == rates &&
		      caller.selected_rate == 0 && caller.end_word == 0,
	      "V.32 caller did not retain its R2 rate intersection");
	for (unsigned int index = 0; index < 40; ++index) {
		bool connected;

		CHECK(v32_startup_observe(
			      &caller, 3, &connected) ==
			      V32_STARTUP_OK &&
			      !connected,
		      "V.32 lower-selection fixture could not observe conditioning");
	}
	CHECK(v32_startup_expect_selection(&caller) ==
		      V32_STARTUP_OK &&
		      observe_word(&caller, selection_word) &&
		      observe_word(&caller, selection_word) &&
		      caller.transmitted_rates == rates &&
		      caller.rate_word == offer_word &&
		      caller.selected_rate ==
			      V32_RATE_CAPABILITY_7200 &&
		      caller.end_word == end_word && caller.end_pending,
	      "V.32 caller rejected the answerer's lower R3 selection");
	return true;
}

static bool transmit_word(struct v32_startup *startup)
{
	for (unsigned int index = 0;
	     index < V32_RATE_SIGNAL_DIBITS; ++index) {
		enum v32_startup_output output;
		uint8_t dibit;

		if (v32_startup_next_transmit(
			    startup, &output, &dibit) != V32_STARTUP_OK ||
		    output != V32_STARTUP_DIFFERENTIAL_DIBIT || dibit >= 4)
			return false;
	}
	return true;
}

static bool test_decoded_event_exchange(void)
{
	const uint8_t rates = V32_RATE_CAPABILITY_4800 |
			      V32_RATE_CAPABILITY_7200 |
			      V32_RATE_CAPABILITY_9600;
	const struct v32_rate_offer offer = {
		.rates = rates,
		.trellis_at_highest_rate = true,
	};
	const struct v32_rate_offer selected = {
		.rates = V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = true,
	};
	struct v32_startup caller;
	struct v32_startup answerer;
	bool caller_connected = false;
	bool answerer_connected = false;

	CHECK(v32_startup_init(&caller, V32_CALLER, rates) ==
		      V32_STARTUP_OK &&
	      v32_startup_init(&answerer, V32_ANSWERER, rates) ==
		      V32_STARTUP_OK &&
	      v32_startup_finish_conditioning(&answerer) ==
		      V32_STARTUP_OK &&
	      answerer.phase == V32_ANSWERER_SEND_OFFER &&
	      v32_startup_note_rate_signal(&caller, &offer) ==
		      V32_STARTUP_OK &&
	      caller.phase == V32_CALLER_SEND_RESPONSE &&
	      v32_startup_note_rate_signal(&answerer, &offer) ==
		      V32_STARTUP_OK &&
	      answerer.phase == V32_ANSWERER_RETRAIN &&
	      answerer.selected_rate == V32_RATE_CAPABILITY_9600 &&
	      v32_startup_finish_conditioning(&answerer) ==
		      V32_STARTUP_OK &&
	      answerer.phase == V32_ANSWERER_SEND_SELECTION &&
	      v32_startup_expect_selection(&caller) == V32_STARTUP_OK &&
	      v32_startup_note_rate_signal(&caller, &selected) ==
		      V32_STARTUP_OK &&
	      caller.selected_rate == V32_RATE_CAPABILITY_9600 &&
	      caller.end_pending,
	      "decoded V.32 events did not reach the selected-rate exchange");
	CHECK(transmit_word(&caller) && caller.phase == V32_CALLER_SEND_END &&
	      transmit_word(&caller) && caller.phase == V32_CONFIRM &&
	      v32_startup_note_end_signal(&answerer, &selected) ==
		      V32_STARTUP_OK &&
	      answerer.end_received && answerer.end_pending &&
	      transmit_word(&answerer) &&
	      answerer.phase == V32_ANSWERER_SEND_END &&
	      transmit_word(&answerer) && answerer.phase == V32_CONFIRM &&
	      v32_startup_note_end_signal(&caller, &selected) ==
		      V32_STARTUP_OK &&
	      caller.end_received,
	      "decoded V.32 events did not reach confirmation");
	for (unsigned int index = 0;
	     index < V32_STARTUP_CONFIRMATION_DIBITS; ++index) {
		enum v32_startup_output caller_output;
		enum v32_startup_output answerer_output;
		uint8_t caller_dibit;
		uint8_t answerer_dibit;

		CHECK(v32_startup_next_transmit(
			      &caller, &caller_output, &caller_dibit) ==
			      V32_STARTUP_OK &&
		      v32_startup_next_transmit(
			      &answerer, &answerer_output,
			      &answerer_dibit) == V32_STARTUP_OK &&
		      caller_output == V32_STARTUP_SELECTED_RATE_MARK &&
		      answerer_output == V32_STARTUP_SELECTED_RATE_MARK &&
		      caller_dibit == 3 && answerer_dibit == 3 &&
		      v32_startup_note_confirmation(
			      &caller, answerer_dibit,
			      &caller_connected) == V32_STARTUP_OK &&
		      v32_startup_note_confirmation(
			      &answerer, caller_dibit,
			      &answerer_connected) == V32_STARTUP_OK,
		      "decoded V.32 confirmation failed at dibit %u", index);
	}
	CHECK(caller_connected && answerer_connected &&
	      v32_startup_is_complete(&caller) &&
	      v32_startup_is_complete(&answerer) &&
	      caller.selected_rate == answerer.selected_rate &&
	      caller.selected_rate == V32_RATE_CAPABILITY_9600,
	      "decoded V.32 events did not enter data");
	return true;
}

static bool check_no_common_rate(enum v32_endpoint_role role, uint16_t word)
{
	struct v32_startup caller;
	struct v32_startup saved;
	bool connected = true;

	CHECK(v32_startup_init(
		      &caller, role,
		      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_OK,
	      "V.32 incompatible-rate fixture did not initialize");
	if (role == V32_ANSWERER)
		CHECK(v32_startup_finish_conditioning(&caller) == V32_STARTUP_OK,
		      "answerer could not receive the peer rate");
	for (unsigned int repeat = 0; repeat < 2; ++repeat) {
		for (unsigned int index = 0;
		     index < V32_RATE_SIGNAL_DIBITS; ++index) {
			uint8_t dibit;

			CHECK(v32_rate_signal_dibit(
				      word, index, &dibit) ==
				      V32_RATE_SIGNAL_OK,
			      "V.32 incompatible rate word did not decode");
			if (repeat == 1 &&
			    index + 1 == V32_RATE_SIGNAL_DIBITS) {
				saved = caller;
				connected = true;
				CHECK(v32_startup_observe(
					      &caller, dibit, &connected) ==
					      V32_STARTUP_NO_COMMON_RATE &&
					      memcmp(&caller, &saved, sizeof(caller)) == 0 &&
					      connected,
				      "V.32 startup accepted an incompatible rate");
			} else {
				connected = false;
				CHECK(v32_startup_observe(
					      &caller, dibit, &connected) ==
					      V32_STARTUP_OK &&
					      !connected,
				      "V.32 startup rejected a partial rate word");
			}
		}
	}
	return true;
}

static bool test_no_common_rate(void)
{
	return check_no_common_rate(V32_CALLER, 0x0b11) &&
	       check_no_common_rate(V32_CALLER, 0x0111) &&
	       check_no_common_rate(V32_ANSWERER, 0x0197);
}

static bool test_validation(void)
{
	const struct v32_rate_offer invalid_offer = { 0 };
	struct v32_startup startup;
	struct v32_startup saved;
	bool connected = true;
	enum v32_startup_output output =
		V32_STARTUP_DATA;
	uint8_t dibit = UINT8_MAX;

	CHECK(v32_startup_init(
		      &startup, V32_CALLER,
		      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_OK &&
		      v32_startup_is_valid(&startup),
	      "V.32 validation startup did not initialize");
	saved = startup;
	CHECK(v32_startup_expect_selection(&startup) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_finish_conditioning(&startup) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_rate_signal(NULL, &invalid_offer) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_rate_signal(&startup, NULL) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_rate_signal(&startup, &invalid_offer) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_end_signal(NULL, &invalid_offer) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_end_signal(&startup, NULL) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_end_signal(&startup, &invalid_offer) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_confirmation(NULL, 3, &connected) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_confirmation(&startup, 4, &connected) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      v32_startup_note_confirmation(&startup, 3, NULL) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
	      memcmp(&startup, &saved, sizeof(startup)) == 0 && connected &&
		      v32_startup_init(
			      &startup, (enum v32_endpoint_role)2,
			      V32_RATE_CAPABILITY_4800) ==
		      V32_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0 &&
		      v32_startup_init(
			      NULL, V32_CALLER,
			      V32_RATE_CAPABILITY_4800) ==
			      V32_STARTUP_INVALID_ARGUMENT &&
		      v32_startup_init(
			      &startup, V32_CALLER, 0) ==
			      V32_STARTUP_INVALID_ARGUMENT &&
		      v32_startup_init(
			      &startup, V32_CALLER, UINT8_C(0x80)) ==
			      V32_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0,
	      "V.32 startup initialization changed state on failure");
	startup.phase_dibits =
		V32_STARTUP_CONFIRMATION_DIBITS + 1;
	CHECK(!v32_startup_is_valid(&startup) &&
		      v32_startup_next_transmit(
			      &startup, &output, &dibit) ==
			      V32_STARTUP_INVALID_ARGUMENT &&
		      output == V32_STARTUP_DATA &&
		      dibit == UINT8_MAX &&
		      strcmp(v32_startup_phase_name(
			     V32_ANSWERER_SEND_SELECTION),
			     "answerer rate selection") == 0,
	      "V.32 startup accepted corrupt state or changed diagnostics");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "highest common rate", test_rate_selection },
		{ "uncoded 9600 negotiation", test_uncoded_9600_negotiation },
		{ "retained coding selection", test_coding_selection_is_retained },
		{ "higher-rate offer fallback", test_higher_rate_offer_fallback },
		{ "training boundary", test_training_boundary },
		{ "selection readiness and rate boundary",
		  test_selection_readiness_and_rate_boundary },
		{ "repeated initial offer", test_repeated_initial_offer },
		{ "multi-rate final selection", test_multi_rate_selection },
		{ "answerer selects lower rate", test_answerer_selects_lower_rate },
		{ "decoded event exchange", test_decoded_event_exchange },
		{ "no common rate", test_no_common_rate },
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
