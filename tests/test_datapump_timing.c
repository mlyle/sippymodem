// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_timing.h>
#include <training_session.h>

#include "test.h"

#include <string.h>

int modem_get_sreg(struct modem *modem, unsigned int number)
{
	if (modem == NULL || number >= ARRAY_LENGTH(modem->sregs))
		return -1;
	return modem->sregs[number];
}

static bool test_default_and_configured_wait(void)
{
	struct modem modem = { 0 };

	CHECK(datapump_carrier_wait_ticks(&modem) ==
		      UINT64_C(60) * MODEM_RATE,
	      "zero S7 did not select the bounded default carrier wait");
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 1;
	CHECK(datapump_carrier_wait_ticks(&modem) == MODEM_RATE,
	      "one-second carrier wait was not converted to media ticks");
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = UINT8_MAX;
	CHECK(datapump_carrier_wait_ticks(&modem) ==
		      UINT64_C(255) * MODEM_RATE,
	      "full-width S7 carrier wait was truncated");
	return true;
}

static bool test_invalid_modem(void)
{
	CHECK(datapump_carrier_wait_ticks(NULL) == 0,
	      "missing modem produced a carrier-wait deadline");
	CHECK(datapump_carrier_loss_ticks(NULL) == 0,
	      "missing modem produced a carrier-loss deadline");
	return true;
}

static bool test_configured_carrier_loss(void)
{
	struct modem modem = { 0 };

	CHECK(datapump_carrier_loss_ticks(&modem) == 1,
	      "zero S10 did not request immediate carrier-loss handling");
	modem.sregs[SREG_CARRIER_LOSS_DISCONNECT_TIME] = 7;
	CHECK(datapump_carrier_loss_ticks(&modem) == MODEM_RATE * 7 / 10,
	      "S10 was not converted from tenths to media ticks");
	modem.sregs[SREG_CARRIER_LOSS_DISCONNECT_TIME] = UINT8_MAX;
	CHECK(datapump_carrier_loss_ticks(&modem) ==
		      UINT32_C(255) * MODEM_RATE / 10,
	      "full-width S10 carrier-loss interval was truncated");
	return true;
}

static bool test_bounded_sample_count(void)
{
	uint32_t samples = 0;

	datapump_add_samples_bounded(&samples, 3, 10);
	CHECK(samples == 3, "sample count did not advance below its limit");
	datapump_add_samples_bounded(&samples, 7, 10);
	CHECK(samples == 10, "sample count missed its exact limit");
	datapump_add_samples_bounded(&samples, UINT32_MAX, 10);
	CHECK(samples == 10, "sample count wrapped past its limit");
	samples = 9;
	datapump_add_samples_bounded(&samples, UINT32_MAX, 10);
	CHECK(samples == 10, "large increment did not saturate");
	samples = UINT32_MAX;
	datapump_add_samples_bounded(&samples, 1, 10);
	CHECK(samples == 10, "count above limit did not normalize");
	samples = UINT32_MAX - 1u;
	datapump_add_samples_bounded(&samples, 2, UINT32_MAX);
	CHECK(samples == UINT32_MAX, "full-width limit wrapped");
	samples = 7;
	datapump_add_samples_bounded(&samples, 1, 0);
	CHECK(samples == 0, "zero limit did not clear the count");
	return true;
}

static bool test_qam_line_power(void)
{
	int16_t samples[MODEM_FRAG + 1];

	memset(samples, 0, sizeof(samples));
	CHECK(!datapump_qam_line_power_present(NULL, 1) &&
		      !datapump_qam_line_power_present(samples, 0) &&
		      !datapump_qam_line_power_present(
			      samples, MODEM_FRAG + 1),
	      "invalid line-power window was accepted");
	samples[0] = 999;
	CHECK(!datapump_qam_line_power_present(samples, 1),
	      "carrier floor accepted a subthreshold sample");
	samples[0] = 1000;
	CHECK(datapump_qam_line_power_present(samples, 1) &&
		      !datapump_qam_line_power_present(samples, 2),
	      "carrier floor did not use mean-square power");
	CHECK(!datapump_qam_line_power_above(samples, 1, 0u) &&
	      !datapump_qam_line_power_above(NULL, 1, 10000u),
	      "invalid calibrated carrier gate was accepted");
	samples[0] = 100;
	CHECK(datapump_qam_line_power_above(samples, 1, 10000u) &&
	      !datapump_qam_line_power_above(samples, 1, 10001u) &&
	      !datapump_qam_line_power_above(samples, 2, 10000u),
	      "calibrated carrier floor did not use mean-square power");
	samples[0] = INT16_MIN;
	CHECK(datapump_qam_line_power_present(samples, MODEM_FRAG),
	      "full-width sample square overflowed");
	return true;
}

static bool test_initial_training_lifecycle(void)
{
	struct training_session session;

	CHECK(training_session_init(&session, 100) == TRAINING_SESSION_OK &&
	      training_session_is_valid(&session),
	      "initial training session did not initialize");
	CHECK(session.state == TRAINING_SESSION_ACTIVE &&
	      session.origin == TRAINING_INITIAL && session.generation == 1,
	      "initial training session selected the wrong lifecycle");
	CHECK(!training_session_is_online(&session) &&
	      !training_session_has_failed(&session) &&
	      !training_session_is_retraining(&session),
	      "initial training session reported a terminal state");
	CHECK(training_session_advance(&session, 40) == TRAINING_SESSION_OK &&
	      session.elapsed_ticks == 40 && session.stalled_ticks == 40,
	      "initial training clock did not advance");
	CHECK(training_session_note_progress(&session) ==
		      TRAINING_SESSION_OK &&
	      session.progress_events == 1 && session.stalled_ticks == 0,
	      "training progress did not reset the stall clock");
	CHECK(training_session_advance(&session, 60) == TRAINING_SESSION_OK &&
	      session.elapsed_ticks == 100 && session.stalled_ticks == 60 &&
	      session.state == TRAINING_SESSION_ACTIVE,
	      "exact training deadline was not inclusive");
	CHECK(training_session_advance(&session, 1) ==
		      TRAINING_SESSION_DEADLINE &&
	      session.elapsed_ticks == 100 && session.stalled_ticks == 60 &&
	      session.state == TRAINING_SESSION_FAILED &&
	      training_session_is_valid(&session) &&
	      training_session_has_failed(&session) &&
	      !training_session_is_online(&session),
	      "training deadline did not fail closed");
	return true;
}

static bool test_retraining_lifecycle(void)
{
	struct training_session session;
	struct training_session unchanged;

	CHECK(training_session_init(&session, 200) == TRAINING_SESSION_OK &&
	      training_session_connect(&session) == TRAINING_SESSION_OK &&
	      session.state == TRAINING_SESSION_ONLINE &&
	      training_session_is_online(&session),
	      "initial training did not enter online state");
	unchanged = session;
	CHECK(training_session_start_retrain(&session, TRAINING_INITIAL) ==
		      TRAINING_SESSION_INVALID_STATE &&
	      memcmp(&session, &unchanged, sizeof(session)) == 0,
	      "invalid initial retrain mutated the session");
	CHECK(training_session_start_retrain(
		      &session, TRAINING_LOCAL_RETRAIN) == TRAINING_SESSION_OK &&
	      session.state == TRAINING_SESSION_ACTIVE &&
	      session.origin == TRAINING_LOCAL_RETRAIN &&
	      session.generation == 2 && session.elapsed_ticks == 0 &&
	      training_session_is_retraining(&session),
	      "local retrain did not start a fresh generation");
	CHECK(training_session_advance(&session, 25) == TRAINING_SESSION_OK &&
	      training_session_note_progress(&session) ==
		      TRAINING_SESSION_OK &&
	      training_session_connect(&session) == TRAINING_SESSION_OK,
	      "local retrain did not reconnect");
	CHECK(training_session_start_retrain(
		      &session, TRAINING_REMOTE_RETRAIN) == TRAINING_SESSION_OK &&
	      session.origin == TRAINING_REMOTE_RETRAIN &&
	      session.generation == 3 && session.progress_events == 0,
	      "remote retrain did not reset transaction state");
	CHECK(training_session_fail(&session) == TRAINING_SESSION_OK &&
	      training_session_fail(&session) == TRAINING_SESSION_OK &&
	      session.state == TRAINING_SESSION_FAILED &&
	      training_session_has_failed(&session) &&
	      !training_session_is_retraining(&session),
	      "training failure was not idempotent");
	return true;
}

static bool test_training_owner_transfer(void)
{
	struct training_session session;
	struct training_session unchanged;

	CHECK(training_session_init(&session, 100) == TRAINING_SESSION_OK &&
	      training_session_advance(&session, 90) == TRAINING_SESSION_OK &&
	      training_session_note_progress(&session) == TRAINING_SESSION_OK &&
	      training_session_advance(&session, 5) == TRAINING_SESSION_OK &&
	      training_session_transfer(&session) == TRAINING_SESSION_OK &&
	      session.deadline_ticks == 100 && session.elapsed_ticks == 0 &&
	      session.stalled_ticks == 0 && session.progress_events == 0 &&
	      session.generation == 1 && session.origin == TRAINING_INITIAL &&
	      session.state == TRAINING_SESSION_ACTIVE,
	      "initial owner transfer did not start a bounded interval");
	CHECK(training_session_advance(&session, 100) == TRAINING_SESSION_OK &&
	      training_session_connect(&session) == TRAINING_SESSION_OK,
	      "transferred initial training did not connect at its deadline");
	unchanged = session;
	CHECK(training_session_transfer(&session) ==
		      TRAINING_SESSION_INVALID_STATE &&
	      memcmp(&session, &unchanged, sizeof(session)) == 0,
	      "online owner transfer changed the session");
	CHECK(training_session_start_retrain(
		      &session, TRAINING_REMOTE_RETRAIN) == TRAINING_SESSION_OK &&
	      training_session_advance(&session, 75) == TRAINING_SESSION_OK &&
	      training_session_transfer(&session) == TRAINING_SESSION_OK &&
	      session.elapsed_ticks == 75 && session.stalled_ticks == 0 &&
	      session.progress_events == 0 && session.generation == 2 &&
	      session.origin == TRAINING_REMOTE_RETRAIN &&
	      training_session_is_retraining(&session),
	      "retrain owner transfer lost its deadline, generation or origin");
	for (unsigned transfer = 0; transfer < 25; ++transfer) {
		CHECK(training_session_advance(&session, 1) == TRAINING_SESSION_OK &&
		      training_session_transfer(&session) == TRAINING_SESSION_OK &&
		      session.elapsed_ticks == 76 + transfer,
		      "repeated owner transfer extended a retrain deadline");
	}
	CHECK(training_session_advance(&session, 1) == TRAINING_SESSION_DEADLINE &&
	      training_session_has_failed(&session) && session.elapsed_ticks == 100,
	      "retrain owner changes prevented deadline expiry");
	CHECK(training_session_transfer(NULL) ==
		      TRAINING_SESSION_INVALID_ARGUMENT,
	      "missing owner transfer was accepted");
	return true;
}

static bool test_invalid_and_full_width_sessions(void)
{
	struct training_session session = { .deadline_ticks = 7 };
	struct training_session unchanged = session;

	CHECK(training_session_init(&session, 0) ==
		      TRAINING_SESSION_INVALID_ARGUMENT &&
	      memcmp(&session, &unchanged, sizeof(session)) == 0,
	      "invalid initialization mutated its destination");
	CHECK(training_session_init(&session, UINT64_MAX) ==
		      TRAINING_SESSION_OK &&
	      training_session_advance(&session, UINT64_MAX) ==
		      TRAINING_SESSION_OK &&
	      session.elapsed_ticks == UINT64_MAX &&
	      session.stalled_ticks == UINT64_MAX,
	      "full-width training deadline overflowed");
	CHECK(training_session_advance(&session, 1) ==
		      TRAINING_SESSION_DEADLINE &&
	      training_session_is_valid(&session),
	      "expired full-width session became invalid");
	CHECK(training_session_init(NULL, 1) ==
		      TRAINING_SESSION_INVALID_ARGUMENT &&
	      !training_session_is_valid(NULL) &&
	      !training_session_is_online(NULL) &&
	      !training_session_has_failed(NULL) &&
	      !training_session_is_retraining(NULL),
	      "missing session was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "default and configured wait", test_default_and_configured_wait },
		{ "configured carrier loss", test_configured_carrier_loss },
		{ "bounded sample count", test_bounded_sample_count },
		{ "shared QAM line power", test_qam_line_power },
		{ "invalid modem", test_invalid_modem },
		{ "initial training lifecycle", test_initial_training_lifecycle },
		{ "retraining lifecycle", test_retraining_lifecycle },
		{ "training owner transfer", test_training_owner_transfer },
		{ "invalid and full-width sessions",
		  test_invalid_and_full_width_sessions },
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
