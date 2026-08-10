// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <sippymodem_datapumps.h>
#include <sippymodem_v32_datapump.h>
#include <sippymodem_v8_datapump.h>

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

static bool test_v8_datapump_mapping(void)
{
	CHECK(sippymodem_v8_datapump_modulation(DP_V21) ==
		      SIPPYMODEM_V8_MODULATION_V21 &&
		      sippymodem_v8_datapump_modulation(DP_V22) ==
		      SIPPYMODEM_V8_MODULATION_V22 &&
		      sippymodem_v8_datapump_modulation(DP_V22BIS) ==
		      SIPPYMODEM_V8_MODULATION_V22 &&
		      sippymodem_v8_datapump_modulation(DP_V23) ==
		      SIPPYMODEM_V8_MODULATION_V23_DUPLEX &&
		      sippymodem_v8_datapump_modulation(DP_V32) ==
		      SIPPYMODEM_V8_MODULATION_V32 &&
		      sippymodem_v8_datapump_modulation(DP_B103) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_B212) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_BAUDOT) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_V32BIS) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_V34) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_V90) ==
			      SIPPYMODEM_V8_MODULATION_NONE &&
		      sippymodem_v8_datapump_modulation(DP_V92) ==
			      SIPPYMODEM_V8_MODULATION_NONE,
	      "V.8 datapump modulation mapping changed");
	return true;
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
	struct dp_operations *v8_operations;
	struct dp_operations *v21_operations;
	struct dp_operations *bell103_operations;
	struct dp_operations *v23_operations;
	struct dp_operations *v22_operations;
	struct dp_operations *v22bis_operations;
	struct dp_operations *bell212_operations;
	struct dp_operations *v32_operations;
	struct dp_operations *baudot_operations;
	struct dp *call_progress;
	struct dp *v8;
	struct dp *baudot;
	struct dp *v22;
	struct dp *v22bis;
	struct dp *bell212;
	struct dp *v32;
	int16_t receive[MODEM_FRAG];
	int16_t transmit[MODEM_FRAG];

	CHECK(modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V8) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
		      modem_dp_register(DP_V8, &conflict_operations) == 0 &&
		      sippymodem_datapumps_start(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT &&
		      modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V8) ==
			      &conflict_operations &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
		      modem_dp_deregister(DP_V8, &conflict_operations) == 0 &&
		      sippymodem_datapumps_start(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_OK &&
		      sippymodem_datapumps_are_started(&datapumps),
	      "source-built data-pump registration did not start cleanly");
	operations = modem_dp_get_registered_ops(DP_CALLPROG);
	v8_operations = modem_dp_get_registered_ops(DP_V8);
	v21_operations = modem_dp_get_registered_ops(DP_V21);
	bell103_operations = modem_dp_get_registered_ops(DP_B103);
	v23_operations = modem_dp_get_registered_ops(DP_V23);
	v22_operations = modem_dp_get_registered_ops(DP_V22);
	v22bis_operations = modem_dp_get_registered_ops(DP_V22BIS);
	bell212_operations = modem_dp_get_registered_ops(DP_B212);
	v32_operations = modem_dp_get_registered_ops(DP_V32);
	baudot_operations = modem_dp_get_registered_ops(DP_BAUDOT);
	CHECK(operations != NULL && operations->create != NULL &&
		      operations->delete != NULL && operations->process != NULL &&
		      operations->hangup == NULL && operations->use_count == 0 &&
		      v8_operations != NULL &&
		      v8_operations != operations &&
		      v8_operations->create != NULL &&
		      v8_operations->delete != NULL &&
		      v8_operations->process != NULL &&
		      v8_operations->use_count == 0 &&
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
		      v23_operations->use_count == 0 &&
		      v22_operations != NULL &&
		      v22_operations != v21_operations &&
		      v22_operations != v23_operations &&
		      v22_operations->create != NULL &&
		      v22_operations->delete != NULL &&
		      v22_operations->process != NULL &&
		      v22_operations->use_count == 0 &&
		      v22bis_operations == v22_operations &&
		      bell212_operations == v22_operations &&
		      v32_operations != NULL &&
		      v32_operations != v21_operations &&
		      v32_operations != v23_operations &&
		      v32_operations != v22_operations &&
		      v32_operations->create != NULL &&
		      v32_operations->delete != NULL &&
		      v32_operations->process != NULL &&
		      v32_operations->use_count == 0 &&
		      baudot_operations != NULL &&
		      baudot_operations != v21_operations &&
		      baudot_operations != v23_operations &&
		      baudot_operations != v22_operations &&
		      baudot_operations->create != NULL &&
		      baudot_operations->delete != NULL &&
		      baudot_operations->process != NULL &&
		      baudot_operations->use_count == 0,
	      "source-built modem operations were incomplete");
	CHECK(sippymodem_datapumps_start(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_ALREADY_STARTED &&
		      sippymodem_datapumps_start(&rival) ==
			      SIPPYMODEM_DATAPUMPS_REGISTRATION_CONFLICT,
	      "process-global call-progress ownership was ambiguous");
	modem = modem_create(&driver, "call progress test");
	CHECK(modem != NULL, "call-progress modem could not be created");
	v8 = v8_operations->create(
		modem, DP_V21, 1, MODEM_RATE, MODEM_FRAG, v8_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v8 != NULL && v8->id == DP_V8 &&
		      v8_operations->process(
			      v8, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      sippymodem_datapumps_stop(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_BUSY &&
		      v8_operations->delete(v8) == 0 &&
		      v8_operations->use_count == 0,
	      "V.8 did not own and release its calling handshake");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "V.8 caller emitted audio before the answer signal");
	modem->min_rate = modem->max_rate = 600;
	CHECK(v22_operations->create(
		      modem, DP_V22, 1, MODEM_RATE, MODEM_FRAG,
		      v22_operations) == NULL &&
		      v22_operations->use_count == 0,
	      "V.22 accepted a range that excludes 1200 bit/s");
	modem->min_rate = modem->max_rate = 1200;
	v22 = v22_operations->create(
		modem, DP_V22, 1, MODEM_RATE, MODEM_FRAG, v22_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v22 != NULL &&
		      v22_operations->process(
			      v22, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      sippymodem_datapumps_stop(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_BUSY &&
		      v22_operations->delete(v22) == 0 &&
		      v22_operations->use_count == 0,
	      "V.22 data pump did not wait silently and release cleanly");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "originating V.22 data pump leaked pre-answer audio");
	bell212 = bell212_operations->create(
		modem, DP_B212, 1, MODEM_RATE, MODEM_FRAG,
		bell212_operations);
	CHECK(bell212 != NULL && bell212->id == DP_B212 &&
		      bell212_operations->use_count == 1 &&
		      bell212_operations->delete(bell212) == 0 &&
		      bell212_operations->use_count == 0,
	      "Bell 212A did not preserve its registered modem identity");
	modem->min_rate = modem->max_rate = 1200;
	CHECK(v22bis_operations->create(
		      modem, DP_V22BIS, 1, MODEM_RATE, MODEM_FRAG,
		      v22bis_operations) == NULL,
	      "V.22bis accepted a range that excludes 2400 bit/s");
	modem->min_rate = modem->max_rate = 2400;
	v22bis = v22bis_operations->create(
		modem, DP_V22BIS, 1, MODEM_RATE, MODEM_FRAG,
		v22bis_operations);
	CHECK(v22bis != NULL && v22bis->id == DP_V22BIS &&
		      v22bis_operations->use_count == 1 &&
		      v22bis_operations->delete(v22bis) == 0 &&
		      v22bis_operations->use_count == 0,
	      "V.22bis did not preserve its registered modem identity");
	modem->min_rate = modem->max_rate = 2400;
	CHECK(v32_operations->create(
		      modem, DP_V32, 1, MODEM_RATE, MODEM_FRAG,
		      v32_operations) == NULL,
	      "V.32 accepted a range that excludes 4800 bit/s");
	modem->min_rate = modem->max_rate = SIPPYMODEM_V32_4800_BIT_RATE;
	v32 = v32_operations->create(
		modem, DP_V32, 1, MODEM_RATE, MODEM_FRAG, v32_operations);
	CHECK(v32 != NULL && v32->id == DP_V32 &&
		      v32_operations->use_count == 1 &&
		      sippymodem_datapumps_stop(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_BUSY &&
		      v32_operations->delete(v32) == 0 &&
		      v32_operations->use_count == 0,
	      "V.32 data pump did not retain and release its registration");
	modem->min_rate = 46;
	modem->max_rate = 49;
	CHECK(baudot_operations->create(
		      modem, DP_BAUDOT, 1, MODEM_RATE, MODEM_FRAG,
		      baudot_operations) == NULL &&
		      baudot_operations->use_count == 0,
	      "Baudot accepted a range containing neither supported rate");
	modem->min_rate = modem->max_rate = 45;
	baudot = baudot_operations->create(
		modem, DP_BAUDOT, 1, MODEM_RATE, MODEM_FRAG,
		baudot_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(baudot != NULL &&
		      baudot_operations->process(
			      baudot, receive, transmit, MODEM_FRAG) ==
			      DPSTAT_CONNECT &&
		      baudot->status == DPSTAT_CONNECT && modem->tx_rate == 45 &&
		      modem->rx_rate == 45 &&
		      sippymodem_datapumps_stop(&datapumps) ==
			      SIPPYMODEM_DATAPUMPS_BUSY &&
		      baudot_operations->delete(baudot) == 0 &&
		      baudot_operations->use_count == 0,
	      "Baudot data pump did not link and release cleanly");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "idle Baudot data pump did not mute its output");
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
		      modem_dp_get_registered_ops(DP_V8) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
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
		{ "V.8 datapump mapping", test_v8_datapump_mapping },
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
