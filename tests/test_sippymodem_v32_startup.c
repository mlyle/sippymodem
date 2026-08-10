// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_startup.h>

#include "test.h"

#include <string.h>

enum {
	TEST_MAX_STARTUP_DIBITS = 4096,
};

static bool observe_word(
	struct sippymodem_v32_startup *startup, uint16_t word)
{
	for (unsigned int index = 0;
	     index < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS; ++index) {
		uint8_t dibit;
		bool connected;

		CHECK(sippymodem_v32_rate_signal_dibit(word, index, &dibit) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      sippymodem_v32_startup_observe(
				      startup, dibit, &connected) ==
				      SIPPYMODEM_V32_STARTUP_OK &&
			      !connected,
		      "V.32 startup could not observe word 0x%04x", word);
	}
	return true;
}

static enum sippymodem_v32_startup_output expected_output(
	enum sippymodem_v32_startup_phase phase)
{
	if (phase == SIPPYMODEM_V32_DATA)
		return SIPPYMODEM_V32_STARTUP_DATA;
	if (phase == SIPPYMODEM_V32_CONFIRM)
		return SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK;
	if (phase == SIPPYMODEM_V32_CALLER_WAIT_OFFER ||
	    phase == SIPPYMODEM_V32_ANSWERER_TRAIN ||
	    phase == SIPPYMODEM_V32_ANSWERER_RETRAIN)
		return SIPPYMODEM_V32_STARTUP_TRN_DIBIT;
	return SIPPYMODEM_V32_STARTUP_DIFFERENTIAL_DIBIT;
}

static bool check_role_exchange(
	uint8_t caller_rates, uint8_t answerer_rates, uint8_t expected_rate)
{
	struct sippymodem_v32_startup caller;
	struct sippymodem_v32_startup answerer;
	struct sippymodem_v32_rate_offer selected = {
		.rates = expected_rate,
		.trellis_at_highest_rate =
			expected_rate == SIPPYMODEM_V32_RATE_CAPABILITY_7200 ||
			expected_rate == SIPPYMODEM_V32_RATE_CAPABILITY_12000,
	};
	uint32_t caller_phases = 0;
	uint32_t answerer_phases = 0;
	uint16_t expected_rate_word;
	uint16_t expected_end_word;
	bool caller_connected = false;
	bool answerer_connected = false;
	bool answerer_counted_early_marks = false;
	size_t completed_at = SIZE_MAX;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER, caller_rates) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      sippymodem_v32_startup_init(
			      &answerer, SIPPYMODEM_V32_ANSWERER,
			      answerer_rates) ==
		      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 startup peers 0x%x/0x%x did not initialize", caller_rates,
	      answerer_rates);
	for (size_t index = 0; index < TEST_MAX_STARTUP_DIBITS; ++index) {
		enum sippymodem_v32_startup_output caller_output;
		enum sippymodem_v32_startup_output answerer_output;
		enum sippymodem_v32_startup_phase caller_phase = caller.phase;
		enum sippymodem_v32_startup_phase answerer_phase = answerer.phase;
		uint8_t caller_dibit = 3;
		uint8_t answerer_dibit = 3;

		caller_phases |= UINT32_C(1) << caller.phase;
		answerer_phases |= UINT32_C(1) << answerer.phase;
		CHECK(sippymodem_v32_startup_next_transmit(
			      &caller, &caller_output, &caller_dibit) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      sippymodem_v32_startup_next_transmit(
				      &answerer, &answerer_output,
				      &answerer_dibit) ==
				      SIPPYMODEM_V32_STARTUP_OK,
		      "V.32 startup could not transmit at dibit %zu", index);
		CHECK(caller_output == expected_output(caller_phase) &&
			      answerer_output == expected_output(answerer_phase),
		      "V.32 startup selected the wrong framing at dibit %zu",
		      index);
		CHECK(sippymodem_v32_startup_observe(
			      &caller, answerer_dibit, &caller_connected) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      sippymodem_v32_startup_observe(
				      &answerer, caller_dibit,
				      &answerer_connected) ==
				      SIPPYMODEM_V32_STARTUP_OK,
		      "V.32 startup could not receive at dibit %zu", index);
		if (answerer.end_received &&
		    answerer.phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION &&
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
		      sippymodem_v32_startup_is_complete(&caller) &&
		      sippymodem_v32_startup_is_complete(&answerer) &&
		      caller.supported_rates == caller_rates &&
		      answerer.supported_rates == answerer_rates &&
		      caller.selected_rate == expected_rate &&
		      answerer.selected_rate == expected_rate &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_RATE_SIGNAL, &selected,
			      &expected_rate_word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_END_SIGNAL, &selected,
			      &expected_end_word) ==
			      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      caller.rate_word == expected_rate_word &&
		      answerer.rate_word == expected_rate_word &&
		      caller.end_word == expected_end_word &&
		      answerer.end_word == expected_end_word,
	      "V.32 peers 0x%x/0x%x did not select 0x%x", caller_rates,
	      answerer_rates, expected_rate);
	CHECK((caller_phases &
	       ((UINT32_C(1) << SIPPYMODEM_V32_CALLER_WAIT_OFFER) |
		(UINT32_C(1) << SIPPYMODEM_V32_CALLER_SEND_RESPONSE) |
		(UINT32_C(1) << SIPPYMODEM_V32_CALLER_SEND_END) |
		(UINT32_C(1) << SIPPYMODEM_V32_CONFIRM) |
		(UINT32_C(1) << SIPPYMODEM_V32_DATA))) ==
		      ((UINT32_C(1) << SIPPYMODEM_V32_CALLER_WAIT_OFFER) |
		       (UINT32_C(1) << SIPPYMODEM_V32_CALLER_SEND_RESPONSE) |
		       (UINT32_C(1) << SIPPYMODEM_V32_CALLER_SEND_END) |
		       (UINT32_C(1) << SIPPYMODEM_V32_CONFIRM) |
		       (UINT32_C(1) << SIPPYMODEM_V32_DATA)),
	      "V.32 caller skipped a startup phase");
	CHECK((answerer_phases &
	       ((UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_TRAIN) |
		(UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_OFFER) |
		(UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_RETRAIN) |
		(UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_SELECTION) |
		(UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_END) |
		(UINT32_C(1) << SIPPYMODEM_V32_CONFIRM) |
		(UINT32_C(1) << SIPPYMODEM_V32_DATA))) ==
		      ((UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_TRAIN) |
		       (UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_OFFER) |
		       (UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_RETRAIN) |
		       (UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_SELECTION) |
		       (UINT32_C(1) << SIPPYMODEM_V32_ANSWERER_SEND_END) |
		       (UINT32_C(1) << SIPPYMODEM_V32_CONFIRM) |
		       (UINT32_C(1) << SIPPYMODEM_V32_DATA)),
	      "V.32 answerer skipped a startup phase");
	CHECK(answerer_counted_early_marks,
	      "V.32 answerer discarded post-E marks during selection");
	return true;
}

static bool test_rate_selection(void)
{
	uint8_t both = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
		       SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	uint8_t v32bis = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_7200;
	uint8_t all = v32bis | SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	uint8_t through_12000 = all | SIPPYMODEM_V32_RATE_CAPABILITY_12000;

	return check_role_exchange(
		       through_12000, through_12000,
		       SIPPYMODEM_V32_RATE_CAPABILITY_12000) &&
	       check_role_exchange(through_12000, all,
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(all, through_12000,
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(both, both,
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600) &&
	       check_role_exchange(v32bis, v32bis,
				   SIPPYMODEM_V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(all, v32bis,
				   SIPPYMODEM_V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(v32bis, all,
				   SIPPYMODEM_V32_RATE_CAPABILITY_7200) &&
	       check_role_exchange(both, SIPPYMODEM_V32_RATE_CAPABILITY_4800,
				   SIPPYMODEM_V32_RATE_CAPABILITY_4800) &&
	       check_role_exchange(SIPPYMODEM_V32_RATE_CAPABILITY_4800, both,
				   SIPPYMODEM_V32_RATE_CAPABILITY_4800) &&
	       check_role_exchange(SIPPYMODEM_V32_RATE_CAPABILITY_9600,
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600,
				   SIPPYMODEM_V32_RATE_CAPABILITY_9600);
}

static bool test_uncoded_fallback(void)
{
	struct sippymodem_v32_rate_offer trellis_offer = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_startup caller;
	uint16_t word;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			      SIPPYMODEM_V32_RATE_CAPABILITY_9600) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      sippymodem_v32_rate_signal_encode(
			      SIPPYMODEM_V32_RATE_SIGNAL, &trellis_offer,
			      &word) == SIPPYMODEM_V32_RATE_SIGNAL_OK &&
		      observe_word(&caller, word) && observe_word(&caller, word) &&
		      caller.phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE &&
		      caller.selected_rate ==
			      SIPPYMODEM_V32_RATE_CAPABILITY_4800 &&
		      caller.rate_word == 0x0d11 && caller.end_word == 0xfd11,
	      "V.32 startup did not fall back from trellis-only 9600");
	return true;
}

static bool test_higher_rate_offer_fallback(void)
{
	struct sippymodem_v32_rate_offer offer = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_9600 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_12000 |
			 SIPPYMODEM_V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct sippymodem_v32_startup caller;
	uint16_t word;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
			      SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
			      SIPPYMODEM_V32_RATE_CAPABILITY_9600) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
	      sippymodem_v32_rate_signal_encode(
		      SIPPYMODEM_V32_RATE_SIGNAL, &offer, &word) ==
		      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
	      observe_word(&caller, word) && observe_word(&caller, word) &&
	      caller.selected_rate == SIPPYMODEM_V32_RATE_CAPABILITY_9600 &&
	      caller.rate_word == 0x0b11 && caller.end_word == 0xfb11,
	      "V.32 startup did not select its highest supported fallback");
	return true;
}

static bool test_training_boundary(void)
{
	struct sippymodem_v32_startup answerer;

	CHECK(sippymodem_v32_startup_init(
		      &answerer, SIPPYMODEM_V32_ANSWERER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
		      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 training fixture did not initialize");
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS; ++index) {
		enum sippymodem_v32_startup_output output;
		uint8_t dibit = UINT8_MAX;

		CHECK(sippymodem_v32_startup_next_transmit(
			      &answerer, &output, &dibit) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      output == SIPPYMODEM_V32_STARTUP_TRN_DIBIT &&
			      dibit == 3,
		      "V.32 answerer changed training dibit %zu", index);
	}
	CHECK(answerer.phase == SIPPYMODEM_V32_ANSWERER_SEND_OFFER,
	      "V.32 answerer did not finish bounded training");
	return true;
}

static bool test_end_waits_for_rate_boundary(void)
{
	struct sippymodem_v32_startup caller;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      observe_word(&caller, 0x0d11) &&
		      observe_word(&caller, 0x0d11) &&
		      caller.phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE,
	      "V.32 boundary fixture did not acquire R1");
	for (unsigned int index = 0; index < 3; ++index) {
		enum sippymodem_v32_startup_output output;
		uint8_t dibit;
		uint8_t expected;

		CHECK(sippymodem_v32_startup_next_transmit(
			      &caller, &output, &dibit) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      sippymodem_v32_rate_signal_dibit(
				      caller.rate_word, index, &expected) ==
				      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      output ==
				      SIPPYMODEM_V32_STARTUP_DIFFERENTIAL_DIBIT &&
			      dibit == expected,
		      "V.32 boundary fixture changed R2 dibit %u", index);
	}
	for (unsigned int index = 0; index < 40; ++index) {
		bool connected;

		CHECK(sippymodem_v32_startup_observe(
			      &caller, 3, &connected) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      !connected,
		      "V.32 boundary fixture could not observe its rate gap");
	}
	CHECK(observe_word(&caller, 0x0d11) &&
		      observe_word(&caller, 0x0d11) && caller.end_pending &&
		      caller.phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE &&
		      caller.word_dibit == 3,
	      "V.32 startup did not defer E at a partial R2 word");
	for (unsigned int index = 3;
	     index < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS; ++index) {
		enum sippymodem_v32_startup_output output;
		uint8_t dibit;
		uint8_t expected;

		CHECK(sippymodem_v32_startup_next_transmit(
			      &caller, &output, &dibit) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      sippymodem_v32_rate_signal_dibit(
				      caller.rate_word, index, &expected) ==
				      SIPPYMODEM_V32_RATE_SIGNAL_OK &&
			      output ==
				      SIPPYMODEM_V32_STARTUP_DIFFERENTIAL_DIBIT &&
			      dibit == expected,
		      "V.32 startup truncated R2 at dibit %u", index);
	}
	CHECK(caller.phase == SIPPYMODEM_V32_CALLER_SEND_END &&
		      !caller.end_pending,
	      "V.32 startup did not enter E on the R2 boundary");
	return true;
}

static bool test_no_common_rate(void)
{
	struct sippymodem_v32_startup caller;
	struct sippymodem_v32_startup saved;
	bool connected = true;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
		      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 incompatible-rate fixture did not initialize");
	for (unsigned int repeat = 0; repeat < 2; ++repeat) {
		for (unsigned int index = 0;
		     index < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS; ++index) {
			uint8_t dibit;

			CHECK(sippymodem_v32_rate_signal_dibit(
				      0x0b11, index, &dibit) ==
				      SIPPYMODEM_V32_RATE_SIGNAL_OK,
			      "V.32 incompatible rate word did not decode");
			if (repeat == 1 &&
			    index + 1 == SIPPYMODEM_V32_RATE_SIGNAL_DIBITS) {
				saved = caller;
				connected = true;
				CHECK(sippymodem_v32_startup_observe(
					      &caller, dibit, &connected) ==
					      SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE &&
					      memcmp(&caller, &saved, sizeof(caller)) == 0 &&
					      connected,
				      "V.32 startup accepted an incompatible rate");
			} else {
				connected = false;
				CHECK(sippymodem_v32_startup_observe(
					      &caller, dibit, &connected) ==
					      SIPPYMODEM_V32_STARTUP_OK &&
					      !connected,
				      "V.32 startup rejected a partial rate word");
			}
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v32_startup startup;
	struct sippymodem_v32_startup saved;
	enum sippymodem_v32_startup_output output =
		SIPPYMODEM_V32_STARTUP_DATA;
	uint8_t dibit = UINT8_MAX;

	CHECK(sippymodem_v32_startup_init(
		      &startup, SIPPYMODEM_V32_CALLER,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      sippymodem_v32_startup_is_valid(&startup),
	      "V.32 validation startup did not initialize");
	saved = startup;
	CHECK(sippymodem_v32_startup_init(
		      &startup, (enum sippymodem_v32_endpoint_role)2,
		      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
		      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0 &&
		      sippymodem_v32_startup_init(
			      NULL, SIPPYMODEM_V32_CALLER,
			      SIPPYMODEM_V32_RATE_CAPABILITY_4800) ==
			      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      sippymodem_v32_startup_init(
			      &startup, SIPPYMODEM_V32_CALLER, 0) ==
			      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      sippymodem_v32_startup_init(
			      &startup, SIPPYMODEM_V32_CALLER, UINT8_C(0x80)) ==
			      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0,
	      "V.32 startup initialization changed state on failure");
	startup.phase_dibits =
		SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS + 1;
	CHECK(!sippymodem_v32_startup_is_valid(&startup) &&
		      sippymodem_v32_startup_next_transmit(
			      &startup, &output, &dibit) ==
			      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      output == SIPPYMODEM_V32_STARTUP_DATA &&
		      dibit == UINT8_MAX &&
		      strcmp(sippymodem_v32_startup_phase_name(
			     SIPPYMODEM_V32_ANSWERER_SEND_SELECTION),
			     "answerer rate selection") == 0 &&
		      strcmp(sippymodem_v32_startup_result_name(
			     SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE),
			     "no common rate") == 0,
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
		{ "uncoded fallback", test_uncoded_fallback },
		{ "higher-rate offer fallback", test_higher_rate_offer_fallback },
		{ "training boundary", test_training_boundary },
		{ "end waits for rate boundary",
		  test_end_waits_for_rate_boundary },
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
