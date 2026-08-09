// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <sippymodem_datapumps.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static int unused_start(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_stop(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_control(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	(void)modem;
	(void)command;
	(void)argument;
	return 0;
}

static struct dp *unused_dp_create(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	(void)modem;
	(void)id;
	(void)caller;
	(void)sample_rate;
	(void)max_fragment;
	(void)operations;
	return NULL;
}

static int unused_dp_delete(struct dp *dp)
{
	(void)dp;
	return 0;
}

static int unused_dp_process(
	struct dp *dp, void *receive, void *transmit, int count)
{
	(void)dp;
	(void)receive;
	(void)transmit;
	(void)count;
	return DPSTAT_ERROR;
}

static bool test_core_has_no_external_dsp_owner(void)
{
	struct modem_driver driver = {
		.name = "native core test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "native core test");
	int16_t receive[MODEM_FRAG];
	int16_t transmit[MODEM_FRAG];

	CHECK(modem != NULL, "source-built modem core could not be created");
	CHECK(modem->srate == MODEM_RATE && modem->format == MODEM_FORMAT &&
		      modem->frag == MODEM_FRAG && modem->dp == NULL &&
		      modem->dc_filter.phase == SIPPYMODEM_DC_FILTER_WARMUP,
	      "source-built modem core did not expose its native initial state");
	for (size_t index = 0; index < ARRAY_LENGTH(receive); ++index) {
		receive[index] = (int16_t)(index + 1);
		transmit[index] = INT16_MAX;
	}
	modem_process(modem, receive, transmit, (int)ARRAY_LENGTH(receive));
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "idle native core did not mute its transmit samples");
	CHECK(modem->count == ARRAY_LENGTH(receive) &&
		      modem->dc_filter.sample_count == ARRAY_LENGTH(receive),
	      "native core did not advance its source-owned sample state");
	modem_delete(modem);
	return true;
}

static bool test_owns_source_built_datapump_registrations(void)
{
	struct sippymodem_datapumps datapumps = { 0 };
	struct sippymodem_datapumps rival = { 0 };
	struct modem_driver driver = {
		.name = "call progress test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations conflict_operations = {
		.name = "registration conflict",
		.create = unused_dp_create,
		.delete = unused_dp_delete,
		.process = unused_dp_process,
	};
	struct modem *modem;
	struct dp_operations *operations;
	struct dp_operations *v21_operations;
	struct dp_operations *bell103_operations;
	struct dp_operations *v23_operations;
	struct dp *call_progress;
	int16_t receive[MODEM_FRAG];
	int16_t transmit[MODEM_FRAG];

	CHECK(modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_register(DP_B103, &conflict_operations) == 0 &&
		      sippymodem_datapumps_start(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT &&
		      modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) ==
			      &conflict_operations &&
		      modem_dp_deregister(DP_B103, &conflict_operations) == 0 &&
		      sippymodem_datapumps_start(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_OK &&
		      sippymodem_datapumps_are_started(&datapumps),
	      "source-built data-pump registration did not start cleanly");
	operations = modem_dp_get_registered_ops(DP_CALLPROG);
	v21_operations = modem_dp_get_registered_ops(DP_V21);
	bell103_operations = modem_dp_get_registered_ops(DP_B103);
	v23_operations = modem_dp_get_registered_ops(DP_V23);
	CHECK(operations != NULL && operations->create != NULL &&
		      operations->delete != NULL && operations->process != NULL &&
		      operations->hangup == NULL && operations->use_count == 0 &&
		      v21_operations != NULL && v21_operations->create != NULL &&
		      v21_operations->delete != NULL &&
		      v21_operations->process != NULL &&
		      v21_operations->use_count == 0 &&
		      bell103_operations == v21_operations &&
		      v23_operations != NULL &&
		      v23_operations != v21_operations &&
		      v23_operations->create != NULL &&
		      v23_operations->delete != NULL &&
		      v23_operations->process != NULL &&
		      v23_operations->use_count == 0,
	      "source-built modem operations were incomplete");
	CHECK(sippymodem_datapumps_start(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_ALREADY_STARTED &&
		      sippymodem_datapumps_start(&rival) ==
			      SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT,
	      "process-global call-progress ownership was ambiguous");
	modem = modem_create(&driver, "call progress test");
	CHECK(modem != NULL, "call-progress modem could not be created");
	call_progress = operations->create(
		modem, DP_CALLPROG, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(call_progress != NULL && call_progress->modem == modem &&
		      call_progress->status == DPSTAT_OK &&
		      operations->use_count == 1,
	      "SIP call-progress instance was not owned by its modem");
	for (size_t index = 0; index < ARRAY_LENGTH(receive); ++index) {
		receive[index] = (int16_t)(index + 1);
		transmit[index] = INT16_MAX;
	}
	CHECK(operations->process(
		      call_progress, receive, transmit, MODEM_FRAG) == DPSTAT_OK,
	      "SIP call-progress frame failed");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "SIP call progress leaked audio while awaiting signaling");
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_BUSY &&
		      operations->delete(call_progress) == 0 &&
		      operations->use_count == 0,
	      "live call progress did not protect its registration");
	modem_delete(modem);
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_OK &&
		      modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      !sippymodem_datapumps_are_started(&datapumps) &&
		      sippymodem_datapumps_stop(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_NOT_STARTED &&
		      strcmp(sippymodem_datapumps_result_name(
			     SIPPYMODEM_DATAPUMPS_BUSY),
			     "data pump is active") == 0,
	      "SIP call-progress registration did not stop cleanly");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "source-built core lifecycle",
		  test_core_has_no_external_dsp_owner },
		{ "source-built data-pump ownership",
		  test_owns_source_built_datapump_registrations },
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
