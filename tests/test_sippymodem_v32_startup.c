// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_startup.h>

#include "test.h"

#include <string.h>

enum {
	TEST_MAX_STARTUP_DIBITS = 4096,
};

static bool test_role_exchange(void)
{
	struct sippymodem_v32_startup caller;
	struct sippymodem_v32_startup answerer;
	uint32_t caller_phases = 0;
	uint32_t answerer_phases = 0;
	bool caller_connected = false;
	bool answerer_connected = false;
	size_t completed_at = SIZE_MAX;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      sippymodem_v32_startup_init(
			      &answerer, SIPPYMODEM_V32_ANSWERER) ==
			      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 startup peers did not initialize");
	for (size_t index = 0; index < TEST_MAX_STARTUP_DIBITS; ++index) {
		enum sippymodem_v32_startup_output caller_output;
		enum sippymodem_v32_startup_output answerer_output;
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
		CHECK(sippymodem_v32_startup_observe(
			      &caller, answerer_dibit, &caller_connected) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      sippymodem_v32_startup_observe(
				      &answerer, caller_dibit,
				      &answerer_connected) ==
				      SIPPYMODEM_V32_STARTUP_OK,
		      "V.32 startup could not receive at dibit %zu", index);
		if (caller_connected && answerer_connected) {
			completed_at = index;
			break;
		}
		CHECK(caller_output == SIPPYMODEM_V32_STARTUP_DIBIT ||
			      sippymodem_v32_startup_is_complete(&caller),
		      "V.32 caller requested data before startup completed");
		CHECK(answerer_output == SIPPYMODEM_V32_STARTUP_DIBIT ||
			      sippymodem_v32_startup_is_complete(&answerer),
		      "V.32 answerer requested data before startup completed");
	}
	caller_phases |= UINT32_C(1) << caller.phase;
	answerer_phases |= UINT32_C(1) << answerer.phase;
	CHECK(completed_at != SIZE_MAX &&
		      sippymodem_v32_startup_is_complete(&caller) &&
		      sippymodem_v32_startup_is_complete(&answerer),
	      "V.32 peers did not complete startup");
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
	return true;
}

static bool test_training_boundary(void)
{
	struct sippymodem_v32_startup answerer;

	CHECK(sippymodem_v32_startup_init(
		      &answerer, SIPPYMODEM_V32_ANSWERER) ==
		      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 training fixture did not initialize");
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS; ++index) {
		enum sippymodem_v32_startup_output output;
		uint8_t dibit = UINT8_MAX;

		CHECK(sippymodem_v32_startup_next_transmit(
			      &answerer, &output, &dibit) ==
			      SIPPYMODEM_V32_STARTUP_OK &&
			      output == SIPPYMODEM_V32_STARTUP_DIBIT && dibit == 3,
		      "V.32 answerer changed training dibit %zu", index);
	}
	CHECK(answerer.phase == SIPPYMODEM_V32_ANSWERER_SEND_OFFER,
	      "V.32 answerer did not finish bounded training");
	return true;
}

static bool test_no_common_rate(void)
{
	struct sippymodem_v32_startup caller;
	struct sippymodem_v32_startup saved;
	bool connected = true;

	CHECK(sippymodem_v32_startup_init(
		      &caller, SIPPYMODEM_V32_CALLER) ==
		      SIPPYMODEM_V32_STARTUP_OK,
	      "V.32 incompatible-rate fixture did not initialize");
	for (unsigned int repeat = 0; repeat < 2; ++repeat) {
		for (unsigned int index = 0;
		     index < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS; ++index) {
			uint8_t dibit;

			CHECK(sippymodem_v32_rate_signal_dibit(
				      0x0311, index, &dibit) ==
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
		      &startup, SIPPYMODEM_V32_CALLER) ==
		      SIPPYMODEM_V32_STARTUP_OK &&
		      sippymodem_v32_startup_is_valid(&startup),
	      "V.32 validation startup did not initialize");
	saved = startup;
	CHECK(sippymodem_v32_startup_init(
		      &startup, (enum sippymodem_v32_endpoint_role)2) ==
		      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0 &&
		      sippymodem_v32_startup_init(NULL, SIPPYMODEM_V32_CALLER) ==
			      SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT,
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
		{ "role exchange", test_role_exchange },
		{ "training boundary", test_training_boundary },
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
