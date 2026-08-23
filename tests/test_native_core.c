// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <modem_param.h>
#include <datapumps.h>
#include <fsk.h>
#include <single_tone.h>
#include <v21.h>
#include <v42_xid.h>
#include <datapump.h>
#include <line_startup.h>
#include <v8_datapump.h>
#include <v34_datapump.h>
#include <v90_datapump.h>
#include <v90_profile.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

extern int process_at_command(struct modem *modem, char *command);
extern int modem_dial(struct modem *modem);

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

static bool test_modem_events_are_instance_owned(void)
{
	struct modem_driver driver = {
		.name = "event deadline test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *first = modem_create(&driver, "first event modem");
	struct modem *second = modem_create(&driver, "second event modem");
	const unsigned test_event = 1u << 31;

	CHECK(first != NULL && second != NULL,
	      "event deadline modems could not be created");
	first->new_event = test_event;
	first->event_deadline = get_time() + MODEM_HZ;
	first->event_scheduled = 1;
	CHECK(modem_service_event(first) == 0 &&
		      first->new_event == test_event && first->event_scheduled == 1,
	      "future modem event ran before its deadline");
	CHECK(modem_service_event(second) == 0 && second->event == 0 &&
		      second->new_event == 0 && second->event_scheduled == 0,
	      "one modem's event altered another modem");
	first->event_deadline = get_time();
	CHECK(modem_service_event(first) == 1 && first->event == 0 &&
		      first->new_event == 0 && first->event_scheduled == 0,
	      "due modem event was not promoted and serviced");
	CHECK(modem_service_event(NULL) == -1,
	      "null modem event service was accepted");
	modem_delete(second);
	modem_delete(first);
	return true;
}

static bool test_escape_discards_async_transmit_fragment(void)
{
	enum { TEST_STATE_MODEM_ONLINE = 5 };
	struct modem_driver driver = {
		.name = "escape framing test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "escape framing test");
	static const char escape[] = "+++";

	CHECK(modem != NULL, "escape framing modem could not be created");
	modem->state = TEST_STATE_MODEM_ONLINE;
	modem->cfg.ec = 0;
	modem->command = 0;
	modem->last_esc_check = get_time();
	CHECK(modem_write(modem, escape, 3) == 3 &&
		      modem->escape_count == 0 && modem->event_scheduled == 0 &&
		      modem->xmit.count == 3,
	      "unguarded escape characters were withheld from line data");
	modem->xmit.head = modem->xmit.tail = modem->xmit.count = 0;
	modem->last_esc_check = 0;
	CHECK(modem_write(modem, escape, 3) == 3 &&
		      modem->escape_count == 3 && modem->event_scheduled != 0 &&
		      modem->xmit.count == 3,
	      "guarded escape did not schedule command mode");
	CHECK(modem_service_event(modem) == 0 && modem->command == 0,
	      "guarded escape entered command mode before its postguard");
	/* Model a fragment already removed from xmit by the asynchronous packer. */
	modem->xmit.head = modem->xmit.tail = modem->xmit.count = 0;
	modem->pack.bit = 7;
	modem->pack.data = 0x1234;
	modem->event_deadline = get_time() - 1;
	CHECK(modem_service_event(modem) == 1 && modem->command == 1 &&
		      modem->pack.bit == 0 && modem->pack.data == 0 &&
		      modem->xmit.count == 0,
	      "confirmed escape remained queued as asynchronous line data");
	modem_delete(modem);
	return true;
}

static struct {
	int result;
	unsigned int calls;
	size_t length;
	uint8_t data[256];
} lapm_sink;

static struct {
	const uint8_t *data;
	size_t length;
	size_t offset;
} lapm_source;

static int scripted_lapm_source(
	struct modem *modem, char *data, int capacity)
{
	size_t available;
	size_t copied;

	(void)modem;
	if (data == NULL || capacity < 0 || lapm_source.offset > lapm_source.length ||
	    (lapm_source.length != 0 && lapm_source.data == NULL))
		return -1;
	available = lapm_source.length - lapm_source.offset;
	copied = available < (size_t)capacity ? available : (size_t)capacity;
	if (copied != 0)
		memcpy(data, lapm_source.data + lapm_source.offset, copied);
	lapm_source.offset += copied;
	return (int)copied;
}

static int scripted_lapm_sink(
	struct modem *modem, const char *data, int length)
{
	int consumed = lapm_sink.result;

	(void)modem;
	lapm_sink.calls++;
	if (consumed < 0)
		return consumed;
	if (consumed > length)
		return consumed;
	if ((size_t)consumed > sizeof(lapm_sink.data) - lapm_sink.length)
		return -1;
	memcpy(lapm_sink.data + lapm_sink.length, data, (size_t)consumed);
	lapm_sink.length += (size_t)consumed;
	return consumed;
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

static struct {
	struct dp datapump;
	unsigned int retrain_calls;
	int retrain_result;
	int process_result;
} retrain_dp;

static struct dp *create_retrain_dp(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations == NULL || operations->use_count != 0)
		return NULL;
	retrain_dp.datapump = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_CONNECT,
		.op = operations,
	};
	operations->use_count++;
	return &retrain_dp.datapump;
}

static int delete_retrain_dp(struct dp *datapump)
{
	if (datapump != &retrain_dp.datapump || datapump->op == NULL ||
	    datapump->op->use_count != 1)
		return -1;
	datapump->op->use_count--;
	memset(&retrain_dp.datapump, 0, sizeof(retrain_dp.datapump));
	return 0;
}

static int process_retrain_dp(
	struct dp *datapump, void *receive, void *transmit, int count)
{
	(void)receive;
	if (datapump != &retrain_dp.datapump || transmit == NULL || count <= 0)
		return DPSTAT_ERROR;
	memset(transmit, 0, (size_t)count * sizeof(int16_t));
	datapump->status = (unsigned int)retrain_dp.process_result;
	return retrain_dp.process_result;
}

static int request_retrain_dp(struct dp *datapump)
{
	if (datapump != &retrain_dp.datapump)
		return -1;
	retrain_dp.retrain_calls++;
	if (retrain_dp.retrain_result == 0)
		datapump->status = DPSTAT_OK;
	return retrain_dp.retrain_result;
}

static bool test_ato1_waits_for_datapump_retrain(void)
{
	enum {
		TEST_STATE_MODEM_ONLINE = 5,
		TEST_STATE_COMMAND_ONLINE = 6,
	};
	struct modem_driver driver = {
		.name = "ATO1 lifecycle test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations operations = {
		.name = "ATO1 lifecycle test",
		.create = create_retrain_dp,
		.delete = delete_retrain_dp,
		.process = process_retrain_dp,
	};
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];
	struct modem *modem;
	char unsupported[] = "ATO1";
	char invalid[] = "ATO2";
	char retrain[] = "ATO1";
	char resume[] = "ATO0";

	memset(&retrain_dp, 0, sizeof(retrain_dp));
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "ATO1 test data pump did not register");
	modem = modem_create(&driver, "ATO1 lifecycle test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "ATO1 test call did not start");
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
	CHECK(process_at_command(modem, unsupported) == -1 &&
		      modem->state == TEST_STATE_COMMAND_ONLINE &&
		      modem->command == 1 && modem->retrain_pending == 0 &&
		      retrain_dp.retrain_calls == 0,
	      "ATO1 without data-pump support changed the link");
	operations.retrain = request_retrain_dp;
	retrain_dp.retrain_result = -1;
	CHECK(process_at_command(modem, retrain) == -1 &&
		      modem->state == TEST_STATE_COMMAND_ONLINE &&
		      modem->command == 1 && modem->retrain_pending == 0 &&
		      retrain_dp.retrain_calls == 1,
	      "rejected ATO1 was not transactional");
	CHECK(process_at_command(modem, invalid) == -1 &&
		      retrain_dp.retrain_calls == 1 &&
		      modem->state == TEST_STATE_COMMAND_ONLINE,
	      "invalid ATO mode reached the data pump");
	retrain_dp.retrain_result = 0;
	retrain_dp.process_result = DPSTAT_CONNECT;
	CHECK(process_at_command(modem, retrain) == 1 &&
		      retrain_dp.retrain_calls == 2 &&
		      modem->state == TEST_STATE_MODEM_ONLINE &&
		      modem->command == 0 && modem->retrain_pending == 1 &&
		      retrain_dp.datapump.status == DPSTAT_OK,
	      "accepted ATO1 reported completion before retraining");
	modem_process(modem, receive, transmit, MODEM_FRAG);
	CHECK(modem->state == TEST_STATE_MODEM_ONLINE &&
		      modem->retrain_pending == 0 &&
		      retrain_dp.datapump.status == DPSTAT_CONNECT,
	      "data-pump CONNECT did not complete ATO1");
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
	CHECK(process_at_command(modem, resume) == 1 &&
		      retrain_dp.retrain_calls == 2 &&
		      modem->state == TEST_STATE_MODEM_ONLINE &&
		      modem->retrain_pending == 0,
	      "ATO0 incorrectly invoked retraining");
	CHECK(modem_stop_now(modem) == 0 && operations.use_count == 0,
	      "ATO1 test data pump did not stop");
	modem_delete(modem);
	CHECK(modem_dp_deregister(DP_CALLPROG, &operations) == 0,
	      "ATO1 test data pump did not deregister");
	return true;
}

static struct {
	struct dp datapump;
	const uint8_t *codewords;
	enum g711_law law;
	unsigned int linear_calls;
	unsigned int g711_calls;
	bool first_fragment_only;
} g711_dp;

static struct dp *create_g711_dp(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations == NULL || operations->use_count != 0)
		return NULL;
	g711_dp.datapump = (struct dp){
		.id = id,
		.modem = modem,
		.op = operations,
	};
	operations->use_count++;
	return &g711_dp.datapump;
}

static int delete_g711_dp(struct dp *datapump)
{
	if (datapump != &g711_dp.datapump || datapump->op == NULL ||
	    datapump->op->use_count != 1)
		return -1;
	datapump->op->use_count--;
	memset(&g711_dp.datapump, 0, sizeof(g711_dp.datapump));
	return 0;
}

static int process_g711_dp(
	struct dp *datapump, void *receive, void *transmit, int count)
{
	(void)receive;
	if (datapump != &g711_dp.datapump || transmit == NULL || count <= 0)
		return DPSTAT_ERROR;
	g711_dp.linear_calls++;
	memset(transmit, 0, (size_t)count * sizeof(int16_t));
	return DPSTAT_OK;
}

static int process_g711_codewords(
	struct dp *datapump, enum g711_law law,
	const uint8_t *receive_codewords, void *receive,
	uint8_t *transmit_codewords, void *transmit,
	int count, bool *exact_output)
{
	(void)receive;
	if (datapump != &g711_dp.datapump || receive_codewords == NULL ||
	    transmit_codewords == NULL || transmit == NULL || count <= 0 ||
	    exact_output == NULL)
		return DPSTAT_ERROR;
	g711_dp.g711_calls++;
	g711_dp.law = law;
	g711_dp.codewords = receive_codewords;
	for (int index = 0; index < count; ++index)
		transmit_codewords[index] = (uint8_t)~receive_codewords[index];
	memset(transmit, 0, (size_t)count * sizeof(int16_t));
	*exact_output = !g711_dp.first_fragment_only || g711_dp.g711_calls == 1;
	return DPSTAT_OK;
}

static bool test_g711_datapump_input(void)
{
	struct modem_driver driver = {
		.name = "G.711 data pump test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations operations = {
		.name = "G.711 input test",
		.create = create_g711_dp,
		.delete = delete_g711_dp,
		.process = process_g711_dp,
		.process_g711 = process_g711_codewords,
	};
	uint8_t codewords[MODEM_FRAG + 1];
	uint8_t transmit_codewords[MODEM_FRAG + 1] = { 0 };
	int16_t receive[MODEM_FRAG + 1] = { 0 };
	int16_t transmit[MODEM_FRAG + 1];
	struct modem *modem;
	bool exact_output = false;

	memset(&g711_dp, 0, sizeof(g711_dp));
	memset(codewords, 0x7f, sizeof(codewords));
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "G.711 test data pump did not register");
	modem = modem_create(&driver, "G.711 data pump test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "G.711 test call did not start");
	modem_process_g711(
		modem, G711_LAW_PCMU, codewords, receive,
		transmit_codewords, transmit, MODEM_FRAG, &exact_output);
	CHECK(g711_dp.g711_calls == 1 && g711_dp.linear_calls == 0 &&
		      g711_dp.law == G711_LAW_PCMU &&
		      g711_dp.codewords == codewords && exact_output,
	      "exact codewords did not traverse the opt-in data pump");
	for (size_t index = 0; index < MODEM_FRAG; ++index) {
		CHECK(transmit_codewords[index] == (uint8_t)~codewords[index],
		      "exact transmit codeword changed at sample %zu", index);
	}
	g711_dp.g711_calls = 0;
	g711_dp.first_fragment_only = true;
	exact_output = true;
	modem_process_g711(
		modem, G711_LAW_PCMA, codewords, receive,
		transmit_codewords, transmit, ARRAY_LENGTH(codewords),
		&exact_output);
	CHECK(g711_dp.g711_calls == 2 && !exact_output,
	      "one exact fragment incorrectly made the whole frame exact");
	modem_process(modem, receive, transmit, MODEM_FRAG);
	CHECK(g711_dp.g711_calls == 2 && g711_dp.linear_calls == 1,
	      "linear processing did not retain its legacy path");
	CHECK(modem_stop_now(modem) == 0 && operations.use_count == 0,
	      "G.711 test data pump did not stop cleanly");
	modem_delete(modem);
	CHECK(modem_dp_deregister(DP_CALLPROG, &operations) == 0,
	      "G.711 test data pump did not deregister");
	return true;
}

static bool test_datapump_error_control_policy(void)
{
	struct modem_driver driver = {
		.name = "data-pump error-control test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	static const struct {
		enum DP_ID id;
		bool lapm;
	} cases[] = {
		{ DP_B103, false },
		{ DP_V21, false },
		{ DP_V23, false },
		{ DP_BAUDOT, false },
		{ DP_B212, true },
		{ DP_V22, true },
		{ DP_V22BIS, true },
		{ DP_V32, true },
		{ DP_V34, true },
		{ DP_V90, true },
		{ DP_V92, true },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct modem *modem = modem_create(
			&driver, "data-pump error-control test");
		struct dp datapump = {
			.id = cases[index].id,
			.modem = modem,
			.status = DPSTAT_CONNECT,
		};

		CHECK(modem != NULL,
		      "error-control modem %zu could not be created", index);
		modem->dp = &datapump;
		modem->caller = 1;
		modem->rx_rate = modem->tx_rate = 1200;
		modem->cfg.ec = 1;
		modem->cfg.ec_detector = 1;

		modem_update_status(modem, STATUS_DP_LINK);
		if (cases[index].lapm) {
			CHECK(modem->cfg.ec == 1 &&
				      modem->get_bits == modem_detector_get_bits &&
				      modem->put_bits == modem_detector_put_bits,
			      "data pump %u discarded its V.42 detector",
			      cases[index].id);
		} else {
			CHECK(modem->cfg.ec == 0 &&
				      modem->get_bits == modem_async_get_bits &&
				      modem->put_bits == modem_async_put_bits,
			      "data pump %u did not select raw transport",
			      cases[index].id);
		}

		modem->dp = NULL;
		modem_delete(modem);

		modem = modem_create(
			&driver, "raw-answerer error-control test");
		CHECK(modem != NULL,
		      "raw-answerer modem %zu could not be created", index);
		datapump.modem = modem;
		modem->dp = &datapump;
		modem->caller = 0;
		modem->rx_rate = modem->tx_rate = 1200;
		modem->cfg.ec = 0;
		modem->cfg.ec_detector = 0;

		modem_update_status(modem, STATUS_DP_LINK);
		if (cases[index].lapm) {
			CHECK(modem->get_bits == modem_detector_get_bits &&
				      modem->put_bits == modem_detector_put_bits &&
				      modem->packer.detector.tx_pattern[1] == 0x00,
			      "raw answerer %u did not select ADP-V.14",
			      cases[index].id);
			modem->bit_timer_func(modem);
			CHECK(modem->get_bits == modem_async_get_bits &&
				      modem->put_bits == modem_async_put_bits &&
				      modem->packer.async.transmit_mark_bits == 0,
			      "raw answerer %u did not complete V.14 fallback",
			      cases[index].id);
		} else {
			CHECK(modem->get_bits == modem_async_get_bits &&
				      modem->put_bits == modem_async_put_bits,
			      "raw-only answerer %u entered V.42 detection",
			      cases[index].id);
		}

		modem->dp = NULL;
		modem_delete(modem);
	}
	return true;
}

static bool test_modulation_report_identity(void)
{
	struct modem modem = { 0 };
	struct dp datapump = { .id = DP_V32 };
	static const struct {
		unsigned int rate;
		const char *name;
	} cases[] = {
		{ 4800, "V32" },
		{ 7200, "V32bis" },
		{ 9600, "V32" },
		{ 12000, "V32bis" },
		{ 14400, "V32bis" },
	};

	CHECK(modem_active_modulation_name(NULL) == NULL &&
		      modem_active_modulation_name(&modem) == NULL,
	      "inactive modem reported a modulation");
	modem.dp = &datapump;
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		modem.rx_rate = cases[index].rate;
		modem.tx_rate = cases[index].rate;
		CHECK(strcmp(modem_active_modulation_name(&modem),
			     cases[index].name) == 0,
		      "rate %u reported the wrong modulation family",
		      cases[index].rate);
	}
	return true;
}

static bool test_v8_datapump_mapping(void)
{
	CHECK(v8_datapump_modulation(DP_V21) ==
		      V8_MODULATION_V21 &&
		      v8_datapump_modulation(DP_V22) ==
		      V8_MODULATION_V22 &&
		      v8_datapump_modulation(DP_V22BIS) ==
		      V8_MODULATION_V22 &&
		      v8_datapump_modulation(DP_V23) ==
		      V8_MODULATION_V23_DUPLEX &&
		      v8_datapump_modulation(DP_V32) ==
		      V8_MODULATION_V32 &&
		      v8_datapump_modulation(DP_V32BIS) ==
			      V8_MODULATION_V32 &&
		      v8_datapump_modulation(DP_B103) ==
			      V8_MODULATION_NONE &&
		      v8_datapump_modulation(DP_B212) ==
			      V8_MODULATION_NONE &&
		      v8_datapump_modulation(DP_BAUDOT) ==
			      V8_MODULATION_NONE &&
		      v8_datapump_modulation(DP_V34) ==
			      V8_MODULATION_V34_DUPLEX &&
		      v8_datapump_modulation(DP_V90) ==
			      V8_MODULATION_V34_DUPLEX &&
		      v8_datapump_modulation(DP_V92) ==
			      V8_MODULATION_V34_DUPLEX &&
		      v8_datapump_uses_pcm(DP_V90) &&
		      v8_datapump_uses_pcm(DP_V90_NO_V8BIS) &&
		      v8_datapump_uses_pcm(DP_V92) &&
		      !v8_datapump_uses_pcm(DP_V34),
	      "V.8 datapump modulation mapping changed");
	return true;
}

static bool test_v8_pcm_handoff(void)
{
	static const enum DP_ID requested_ids[] = { DP_V90, DP_V92 };
	struct modem_driver driver = {
		.name = "V.8 PCM handoff test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct datapumps datapumps = { 0 };
	struct modem *caller;
	struct modem *answerer;
	struct dp_operations *operations;
	struct dp *calling_dp;
	struct dp *answering_dp;
	int16_t calling_audio[MODEM_FRAG] = { 0 };
	int16_t answering_audio[MODEM_FRAG] = { 0 };
	int calling_status = DPSTAT_OK;
	int answering_status = DPSTAT_OK;

	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK,
	      "V.8 PCM handoff registrations did not start");
	operations = modem_dp_get_registered_ops(DP_V8);
	CHECK(operations != NULL,
	      "V.8 PCM handoff operation did not initialize");
	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(requested_ids); ++selection) {
		enum DP_ID requested = requested_ids[selection];

		caller = modem_create(&driver, "V.8 PCM caller");
		answerer = modem_create(&driver, "V.8 PCM answerer");
		CHECK(caller != NULL && answerer != NULL,
		      "V.8 PCM handoff endpoints did not initialize");
		CHECK(process_at_command(caller, "AT+V90D=0") == 0 &&
			      process_at_command(answerer, "AT+V90D=1") == 0,
		      "V.8 PCM endpoints did not accept complementary roles");
		calling_dp = operations->create(
			caller, requested, 1, MODEM_RATE, MODEM_FRAG, operations);
		answering_dp = operations->create(
			answerer, requested, 0, MODEM_RATE, MODEM_FRAG,
			operations);
		CHECK(calling_dp != NULL && answering_dp != NULL &&
			      operations->use_count == 2,
		      "V.8 PCM handshakes were not independently owned");
		memset(calling_audio, 0, sizeof(calling_audio));
		memset(answering_audio, 0, sizeof(answering_audio));
		calling_status = DPSTAT_OK;
		answering_status = DPSTAT_OK;
		for (size_t frame = 0;
		     frame < 7u * MODEM_RATE / MODEM_FRAG + 1u;
		     ++frame) {
			answering_status = operations->process(
				answering_dp, calling_audio, answering_audio,
				MODEM_FRAG);
			calling_status = operations->process(
				calling_dp, answering_audio, calling_audio,
				MODEM_FRAG);
			CHECK((answering_status == DPSTAT_OK ||
			       answering_status == DPSTAT_CHANGEDP) &&
			      (calling_status == DPSTAT_OK ||
			       calling_status == DPSTAT_CHANGEDP),
			      "V.8 PCM negotiation failed at frame %zu", frame);
			if (calling_status == DPSTAT_CHANGEDP &&
			    answering_status == DPSTAT_CHANGEDP)
				break;
		}
		CHECK(calling_status == DPSTAT_CHANGEDP &&
			      answering_status == DPSTAT_CHANGEDP &&
			      caller->dp_requested == requested &&
			      answerer->dp_requested == requested &&
			      caller->dsp_info.pcm_role == MODEM_PCM_ROLE_ANALOG &&
			      answerer->dsp_info.pcm_role == MODEM_PCM_ROLE_DIGITAL,
		      "V.8 PCM peers did not agree on handoff %u",
		      (unsigned int)requested);
		CHECK(operations->delete(calling_dp) == 0 &&
			      operations->delete(answering_dp) == 0 &&
			      operations->use_count == 0,
		      "V.8 PCM handshakes did not release their state");
		modem_delete(caller);
		modem_delete(answerer);
	}
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.8 PCM handoff registrations did not stop");
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
		      modem->frag == MODEM_FRAG && modem->dp == NULL,
	      "source-built modem core did not expose its native initial state");
	for (size_t index = 0; index < ARRAY_LENGTH(receive); ++index) {
		receive[index] = (int16_t)(index + 1);
		transmit[index] = INT16_MAX;
	}
	modem_process(modem, receive, transmit, (int)ARRAY_LENGTH(receive));
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "idle native core did not mute its transmit samples");
	CHECK(modem->count == ARRAY_LENGTH(receive),
	      "native core did not advance its source-owned sample state");
	modem_delete(modem);
	return true;
}

static bool test_at_dial_destinations_are_bounded(void)
{
	char oversized[sizeof(((struct modem *)0)->dial_string) + 4];
	static struct {
		char command[64];
		const char *destination;
	} cases[] = {
		{ "ATDextension", "extension" },
		{ "ATDuser@example.invalid", "user@example.invalid" },
		{ "ATDsip:peer@example.invalid", "sip:peer@example.invalid" },
	};
	struct datapumps datapumps = { 0 };
	struct modem_driver driver = {
		.name = "AT dial test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_OK,
	      "AT dial test registrations did not start");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct modem *modem = modem_create(&driver, "AT dial test");

		CHECK(modem != NULL,
		      "AT dial modem %zu could not be created", index);
		CHECK(process_at_command(modem, cases[index].command) == 1 &&
			      strcmp(modem->dial_string,
				     cases[index].destination) == 0,
		      "AT dial destination %zu changed", index);
		modem_delete(modem);
	}
	{
		struct modem *modem = modem_create(&driver, "AT dial bound test");

		CHECK(modem != NULL, "bounded AT dial modem could not be created");
		memcpy(oversized, "ATD", 3);
		memset(oversized + 3, 'x', sizeof(modem->dial_string));
		oversized[sizeof(oversized) - 1] = '\0';
		CHECK(process_at_command(modem, oversized) == -1 &&
			      modem->dial_string[0] == '\0' && !modem->started,
		      "oversized AT destination was truncated or started a call");
		modem_delete(modem);
	}
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_OK,
	      "AT dial test registrations did not stop");
	return true;
}

static bool test_at_compression_profile(void)
{
	struct modem_driver driver = {
		.name = "AT compression test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "AT compression test");
	char enable[] = "AT+DS=3,1";
	char disable[] = "AT+DS=0,0";
	char disable_ec[] = "AT\\N0";
	char require_lapm[] = "AT\\N4";
	char chained[] = "AT+DS=2,1%C0";
	char select_v44[] = "AT+DSC=2";
	char unsupported[] = "AT+DS=3,4";

	CHECK(modem != NULL &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_DEFAULT,
	      "AT compression modem did not have safe defaults");
	CHECK(modem_set_sreg(modem, SREG_COMP, 0) == 0 &&
		      process_at_command(modem, enable) == 0 &&
		      modem_get_sreg(modem, SREG_COMP) == 3 &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_V42BIS,
	      "AT+DS did not enable bidirectional V.42bis compression");
	CHECK(process_at_command(modem, chained) == 0 &&
		      modem_get_sreg(modem, SREG_COMP) == 0 &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_V42BIS,
	      "AT+DS did not preserve concatenated command parsing");
	CHECK(modem_set_sreg(modem, SREG_COMP, 3) == 0 &&
		      process_at_command(modem, disable) == 0 &&
		      modem_get_sreg(modem, SREG_COMP) == 0 &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_NONE,
	      "AT+DS did not accept the raw-mode compression setting");
	CHECK(process_at_command(modem, select_v44) == 0 &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_V44,
	      "AT+DSC did not select V.44 compression");
	CHECK(modem_set_sreg(modem, SREG_EC, 3) == 0 &&
		      process_at_command(modem, disable_ec) == 0 &&
		      modem_get_sreg(modem, SREG_EC) == 0,
	      "AT\\N did not accept the raw-mode error-correction setting");
	CHECK(process_at_command(modem, require_lapm) == 0 &&
		      modem_get_sreg(modem, SREG_EC) == 4,
	      "AT\\N did not accept the LAPM-only setting");
	modem->cfg.ec = 1;
	modem->cfg.comp = 3;
	modem->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
	CHECK(modem_ec_active_compression(modem) ==
		      MODEM_EC_COMPRESSION_V42BIS,
	      "legacy compression state was not reported explicitly");
	modem->cfg.ec = 0;
	CHECK(modem_ec_active_compression(modem) ==
		      MODEM_EC_COMPRESSION_NONE,
	      "disabled error correction reported active compression");
	CHECK(modem_set_sreg(modem, SREG_COMP, 2) == 0 &&
		      modem_set_sreg(
			      modem, SREG_COMP_TYPES, MODEM_COMP_TYPE_V44) == 0 &&
		      process_at_command(modem, unsupported) == -1 &&
		      modem_get_sreg(modem, SREG_COMP) == 2 &&
		      modem_get_sreg(modem, SREG_COMP_TYPES) ==
			      MODEM_COMP_TYPE_V44,
	      "AT+DS accepted an unsupported compression type or changed state");
	modem_delete(modem);
	return true;
}

static bool test_at_pcm_profile_controls(void)
{
	struct modem_driver driver = {
		.name = "AT PCM profile test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, driver.name);

	CHECK(modem != NULL &&
		      modem_get_param(modem, MDMPRM_V90_DIGITAL_INDICATION) == 0 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 1 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem->v90_upstream_min_rate == 0 &&
		      modem->v90_upstream_max_rate == 0 &&
		      modem->v90_upstream_max_baud == 0,
	      "PCM profile controls did not have deterministic defaults");
	CHECK(process_at_command(modem, "AT+V90D=1") == 0 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == 1 &&
		      process_at_command(modem, "AT+V90D=2") == -1 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == 1,
	      "AT+V90D was not transactional");
	CHECK(process_at_command(
		      modem, "AT+V90U=14400,31200,3429") == 0 &&
		      modem->v90_upstream_min_rate == 14400 &&
		      modem->v90_upstream_max_rate == 31200 &&
		      modem->v90_upstream_max_baud == 3429,
	      "AT+V90U did not retain its rate policy");
	CHECK(process_at_command(
		      modem, "AT+V90U=33600,14400,3200") == -1 &&
		      modem->v90_upstream_min_rate == 14400 &&
		      modem->v90_upstream_max_rate == 31200 &&
		      modem->v90_upstream_max_baud == 3429 &&
		      process_at_command(
			      modem, "AT+V90U=14400,31200,3360") == -1 &&
		      modem->v90_upstream_max_baud == 3429,
	      "invalid AT+V90U changed its retained policy");
	CHECK(process_at_command(modem, "AT+V92Q=0") == 0 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 0 &&
		      process_at_command(modem, "AT+V92Q=2") == -1 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 0,
	      "AT+V92Q was not transactional");
	CHECK(process_at_command(modem, "AT+V92U=1,28000") == 0 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 1 &&
		      modem_get_sreg(
			      modem, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 4,
	      "AT+V92U did not retain its exact PCM-upstream rate");
	CHECK(process_at_command(modem, "AT+V92U=1,28100") == -1 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 1 &&
		      modem_get_sreg(
			      modem, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 4 &&
		      process_at_command(modem, "AT+V92U=0") == 0 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem_get_sreg(
			      modem, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 0,
	      "AT+V92U did not reject invalid rates or reset cleanly");
	CHECK(process_at_command(modem, "ATZ") == 0 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == 0 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 1 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem->v90_upstream_min_rate == 0 &&
		      modem->v90_upstream_max_rate == 0 &&
		      modem->v90_upstream_max_baud == 0,
	      "modem reset retained a forced PCM profile");
	modem_delete(modem);
	return true;
}

static bool test_at_v34_symbol_rate(void)
{
	static const unsigned int supported[] = {
		2400, 2743, 2800, 3000, 3200, 3429,
	};
	struct modem_driver driver = {
		.name = "AT V.34 symbol-rate test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, driver.name);
	char command[64];

	CHECK(modem != NULL && modem->v34_symbol_rate == 0,
	      "V.34 symbol-rate control did not default to automatic");
	for (size_t index = 0; index < ARRAY_LENGTH(supported); ++index) {
		int length = snprintf(
			command, sizeof(command), "AT+V34B=%u", supported[index]);

		CHECK(length > 0 && (size_t)length < sizeof(command) &&
			      process_at_command(modem, command) == 0 &&
			      modem->v34_symbol_rate == supported[index],
		      "AT+V34B rejected %u symbol/s", supported[index]);
	}
	strcpy(command, "AT+V34B=2600");
	CHECK(process_at_command(modem, command) == -1 &&
		      modem->v34_symbol_rate == 3429,
	      "AT+V34B accepted an unsupported cadence or changed state");
	strcpy(command, "AT+V34B=2400+V34B=2800");
	CHECK(process_at_command(modem, command) == 0 &&
		      modem->v34_symbol_rate == 2800,
	      "AT+V34B did not preserve concatenated command parsing");
	strcpy(command, "ATZ");
	CHECK(process_at_command(modem, command) == 0 &&
		      modem->v34_symbol_rate == 0,
	      "modem reset retained a forced V.34 symbol rate");
	modem_delete(modem);
	return true;
}

static bool test_lapm_xid_envelopes(void)
{
	enum {
		XID_CONTROL = 0xaf,
		SABME_CONTROL = 0x7f,
		UA_CONTROL = 0x73,
		UNNUMBERED_CONTROL_MASK = 0xef,
		LAPM_ESTABLISHING_STATE = 1,
		LAPM_DATA_STATE = 2,
	};
	struct modem_driver driver = {
		.name = "LAPM XID test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *caller = modem_create(&driver, "LAPM XID caller");
	struct modem *answerer = modem_create(&driver, "LAPM XID answerer");
	struct v42_xid_fields fields;
	struct v44_offer v44_offer;
	uint8_t command[LAPM_MAX_INFO_SIZE + 4];
	uint8_t received[LAPM_MAX_INFO_SIZE + 4];
	uint8_t payload[96];
	uint8_t discarded[80];
	uint8_t recovery[112];
	frame_t incoming = { .buf = received };
	frame_t *outgoing;
	int command_size;

	CHECK(caller != NULL && answerer != NULL,
	      "LAPM XID peers could not be created");
	caller->caller = 1;
	answerer->caller = 0;
	CHECK(modem_ec_init(caller) == 0 && modem_ec_init(answerer) == 0,
	      "LAPM XID peers could not initialize");
	modem_ec_start(caller);
	modem_ec_start(answerer);
	CHECK(caller->bit_timer_func != NULL,
	      "LAPM caller did not schedule XID negotiation");
	caller->bit_timer_func(caller);
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size > 2 &&
		      outgoing->buf[1] == XID_CONTROL &&
		      v42_xid_decode(
			      outgoing->buf + 2, (size_t)outgoing->size - 2U,
			      &fields) == V42_XID_OK &&
		      fields.parameter_negotiation_length == 19 &&
		      fields.private_negotiation_length == 15 &&
		      fields.user_data != NULL &&
		      v44_xid_decode(
			      fields.user_data, fields.user_data_length,
			      &v44_offer) == V44_XID_OK &&
		      v44_offer.transmit.enabled && v44_offer.receive.enabled &&
		      caller->ec.lapm.v44_session.phase ==
			      LAPM_V44_COMMAND_PENDING,
	      "LAPM caller did not emit a bounded V.44 XID offer");
	command_size = outgoing->size;
	memcpy(command, outgoing->buf, (size_t)command_size);
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);
	{
		static const uint8_t truncated[] = {
			0x82, 0x80, 0x00, 0x03, 0x03, 0x01,
		};

		received[0] = caller->ec.lapm.cmd_addr;
		received[1] = XID_CONTROL;
		memcpy(received + 2, truncated, sizeof(truncated));
		incoming.count = 2 + sizeof(truncated);
		caller->packer.hdlc.rx_complete(
			caller->packer.hdlc.framer, &incoming);
		CHECK(caller->ec.lapm.config == 1 &&
			      caller->ec.lapm.v44_session.phase ==
				      LAPM_V44_COMMAND_PENDING,
		      "malformed XID response completed LAPM negotiation");
	}
	caller->bit_timer_func(caller);
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size == command_size &&
		      memcmp(outgoing->buf, command, (size_t)command_size) == 0,
	      "LAPM XID retransmission changed its frozen wire image");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);
	{
		frame_t *const saved_next = answerer->ec.lapm.ctrl_list->next;
		bool unchanged;

		answerer->ec.lapm.tx_info_size = 96;
		answerer->ec.lapm.rx_info_size = 96;
		answerer->ec.lapm.ctrl_list->next = answerer->ec.lapm.tx_ctrl;
		answerer->packer.hdlc.rx_complete(
			answerer->packer.hdlc.framer, &incoming);
		unchanged = answerer->ec.lapm.tx_info_size == 96 &&
			    answerer->ec.lapm.rx_info_size == 96 &&
			    answerer->ec.lapm.v44_session.phase ==
				    LAPM_V44_READY &&
			    answerer->ec.lapm.v44_path == NULL &&
			    answerer->packer.hdlc.get_tx_frame(
				    answerer->packer.hdlc.framer) == NULL;
		answerer->ec.lapm.ctrl_list->next = saved_next;
		CHECK(unchanged,
		      "XID response queue pressure changed negotiated state");
	}
	answerer->packer.hdlc.rx_complete(
		answerer->packer.hdlc.framer, &incoming);
	CHECK(answerer->ec.lapm.tx_info_size == 128 &&
		      answerer->ec.lapm.rx_info_size == 128 &&
		      answerer->ec.lapm.v44_session.phase ==
			      LAPM_V44_NEGOTIATED &&
		      answerer->ec.lapm.v44_path != NULL &&
		      answerer->cfg.comp == 0,
	      "LAPM answerer did not own the negotiated V.44 path");
	outgoing = answerer->packer.hdlc.get_tx_frame(
		answerer->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->buf[1] == XID_CONTROL &&
		      v42_xid_decode(
			      outgoing->buf + 2, (size_t)outgoing->size - 2U,
			      &fields) == V42_XID_OK,
	      "LAPM answerer did not return a bounded XID envelope");
	CHECK(fields.private_negotiation == NULL && fields.user_data != NULL,
	      "LAPM answerer selected more than one compression method");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	caller->packer.hdlc.rx_complete(
		caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.config == 0 &&
		      caller->ec.lapm.v44_session.phase ==
			      LAPM_V44_NEGOTIATED &&
		      caller->ec.lapm.v44_path != NULL && caller->cfg.comp == 0 &&
		      modem_ec_active_compression(caller) ==
			      MODEM_EC_COMPRESSION_V44,
	      "valid XID response did not activate the caller V.44 path");
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL,
	      "completed XID negotiation did not queue link establishment");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);
	answerer->packer.hdlc.rx_complete(
		answerer->packer.hdlc.framer, &incoming);
	CHECK(answerer->ec.lapm.v44_session.phase ==
		      LAPM_V44_NEGOTIATED &&
		      answerer->ec.lapm.v44_path != NULL,
	      "LAPM sequence reset erased completed V.44 negotiation");
	outgoing = answerer->packer.hdlc.get_tx_frame(
		answerer->packer.hdlc.framer);
	CHECK(outgoing != NULL,
	      "LAPM answerer did not acknowledge link establishment");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	caller->packer.hdlc.rx_complete(
		caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.state == LAPM_DATA_STATE &&
		      answerer->ec.lapm.state == LAPM_DATA_STATE,
	      "LAPM peers did not enter the data state");

	{
		static const uint8_t truncated[] = {
			0x82, 0x80, 0x00, 0x03, 0x03, 0x01,
		};
		const unsigned int tx_size = answerer->ec.lapm.tx_info_size;
		const unsigned int rx_size = answerer->ec.lapm.rx_info_size;
		const uint8_t compression = answerer->cfg.comp;

		received[0] = answerer->ec.lapm.rsp_addr;
		received[1] = XID_CONTROL;
		memcpy(received + 2, truncated, sizeof(truncated));
		incoming.count = 2 + sizeof(truncated);
		answerer->packer.hdlc.rx_complete(
			answerer->packer.hdlc.framer, &incoming);
		CHECK(answerer->ec.lapm.tx_info_size == tx_size &&
			      answerer->ec.lapm.rx_info_size == rx_size &&
			      answerer->cfg.comp == compression &&
			      answerer->ec.lapm.v44_session.phase ==
				      LAPM_V44_NEGOTIATED &&
			      answerer->ec.lapm.v44_path != NULL &&
			      answerer->packer.hdlc.get_tx_frame(
				      answerer->packer.hdlc.framer) == NULL,
		      "truncated LAPM XID changed state or produced a response");
	}
	for (size_t index = 0; index < sizeof(payload); ++index)
		payload[index] = (uint8_t)('A' + (index / 16U) % 3U);
	lapm_source.data = payload;
	lapm_source.length = sizeof(payload);
	lapm_source.offset = 0;
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	caller->get_chars = scripted_lapm_source;
	answerer->put_chars = scripted_lapm_sink;
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size > 3 &&
		      outgoing->size < (int)sizeof(payload) + 3 &&
		      lapm_source.offset == sizeof(payload),
	      "V.44 did not produce a bounded compressed information field");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);
	answerer->packer.hdlc.rx_complete(
		answerer->packer.hdlc.framer, &incoming);
	CHECK(lapm_sink.length == 0 && answerer->ec.lapm.busy &&
		      answerer->packer_process != NULL,
	      "V.44 receive backpressure did not suspend LAPM input");
	lapm_sink.result = 1;
	answerer->packer_process(answerer, 0);
	CHECK(lapm_sink.length == sizeof(payload) &&
		      memcmp(lapm_sink.data, payload, sizeof(payload)) == 0 &&
		      !answerer->ec.lapm.busy && answerer->ec.lapm.rx_count == 0 &&
		      answerer->packer_process == NULL,
	      "V.44 did not resume and deliver the information field exactly once");

	/*
	 * Advance only the encoder, then establish a fresh LAPM sequence. Both
	 * peers must perform C-INIT before subsequent compressed data can agree.
	 */
	for (size_t index = 0; index < sizeof(discarded); ++index)
		discarded[index] = (uint8_t)(index * 29U + 17U);
	lapm_source.data = discarded;
	lapm_source.length = sizeof(discarded);
	lapm_source.offset = 0;
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && (outgoing->buf[1] & 1U) == 0 &&
		      lapm_source.offset == sizeof(discarded),
	      "LAPM reset fixture did not create an unacknowledged V.44 frame");
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);

	received[0] = answerer->ec.lapm.rsp_addr;
	received[1] = SABME_CONTROL;
	incoming.count = 2;
	answerer->packer.hdlc.rx_complete(
		answerer->packer.hdlc.framer, &incoming);
	CHECK(answerer->ec.lapm.state == LAPM_DATA_STATE &&
		      answerer->ec.lapm.v44_path != NULL &&
		      answerer->ec.lapm.rx_count == 0,
	      "received SABME did not reinitialize the V.44 receive path");

	caller->ec.lapm.state = LAPM_ESTABLISHING_STATE;
	outgoing = NULL;
	for (size_t index = 0; index < LAPM_CTRL_FRAMES; ++index) {
		frame_t *candidate = answerer->packer.hdlc.get_tx_frame(
			answerer->packer.hdlc.framer);

		CHECK(candidate != NULL,
		      "LAPM answerer did not queue establishment acknowledgement");
		if ((candidate->buf[1] & UNNUMBERED_CONTROL_MASK) ==
		    (UA_CONTROL & UNNUMBERED_CONTROL_MASK)) {
			outgoing = candidate;
			break;
		}
		answerer->packer.hdlc.tx_complete(
			answerer->packer.hdlc.framer, candidate);
	}
	CHECK(outgoing != NULL,
	      "LAPM answerer control queue did not contain UA");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	caller->packer.hdlc.rx_complete(
		caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.state == LAPM_DATA_STATE &&
		      caller->ec.lapm.v44_path != NULL &&
		      caller->ec.lapm.sent_count == 0 &&
		      caller->ec.lapm.tx_count == 0 &&
		      caller->ec.lapm.rx_count == 0 &&
		      caller->packer_process == NULL,
	      "received UA retained discarded frames or V.44 stream state");

	for (size_t index = 0; index < sizeof(recovery); ++index)
		recovery[index] = (uint8_t)('a' + (index / 11U) % 5U);
	lapm_source.data = recovery;
	lapm_source.length = sizeof(recovery);
	lapm_source.offset = 0;
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	lapm_sink.result = 1;
	outgoing = caller->packer.hdlc.get_tx_frame(
		caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size > 3 &&
		      lapm_source.offset == sizeof(recovery),
	      "re-established LAPM path did not produce compressed data");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(
		caller->packer.hdlc.framer, outgoing);
	answerer->packer.hdlc.rx_complete(
		answerer->packer.hdlc.framer, &incoming);
	CHECK(lapm_sink.length == sizeof(recovery) &&
		      memcmp(lapm_sink.data, recovery, sizeof(recovery)) == 0,
	      "V.44 transfer did not recover after LAPM re-establishment");
	memset(&lapm_source, 0, sizeof(lapm_source));
	modem_ec_exit(caller);
	modem_ec_exit(answerer);
	modem_delete(caller);
	modem_delete(answerer);
	return true;
}

static bool test_lapm_receive_backpressure(void)
{
	enum { LAPM_DATA_STATE = 2 };
	struct modem_driver driver = {
		.name = "LAPM receive test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "LAPM receive test");
	uint8_t wire[] = { 0, 0, 0, 'a', 'b', 'c' };
	frame_t incoming = {
		.buf = wire,
		.count = sizeof(wire),
	};

	CHECK(modem != NULL && modem_ec_init(modem) == 0,
	      "LAPM receive fixture could not initialize");
	modem_ec_start(modem);
	modem->ec.lapm.state = LAPM_DATA_STATE;
	modem->put_chars = scripted_lapm_sink;
	wire[0] = modem->ec.lapm.rsp_addr;
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	lapm_sink.result = 1;
	modem->packer.hdlc.rx_complete(
		modem->packer.hdlc.framer, &incoming);
	CHECK(lapm_sink.calls == 1 && lapm_sink.length == 1 &&
		      lapm_sink.data[0] == 'a' && modem->ec.lapm.busy &&
		      modem->ec.lapm.rx_count == 2 &&
		      modem->packer_process != NULL,
	      "LAPM did not retain partially consumed receive data");
	modem->ec.lapm.state = 0;
	lapm_sink.result = 2;
	modem->packer_process(modem, 0);
	CHECK(lapm_sink.calls == 1 && modem->ec.lapm.rx_count == 2,
	      "deferred LAPM output ran outside the data state");
	modem->ec.lapm.state = LAPM_DATA_STATE;
	modem->packer_process(modem, 0);
	CHECK(lapm_sink.calls == 2 && lapm_sink.length == 3 &&
		      memcmp(lapm_sink.data, "abc", 3) == 0 &&
		      !modem->ec.lapm.busy && modem->ec.lapm.rx_count == 0 &&
		      modem->packer_process == NULL,
	      "LAPM did not drain retained receive data exactly once");
	wire[1] = 2;
	lapm_sink.result = -1;
	modem->packer.hdlc.rx_complete(
		modem->packer.hdlc.framer, &incoming);
	CHECK(lapm_sink.calls == 3 && modem->ec.lapm.vr == 1 &&
		      modem->ec.lapm.rx_count == 0 &&
		      modem->packer_process == NULL,
	      "failed LAPM sink advanced sequence or buffered invalid data");
	modem_ec_exit(modem);
	modem_delete(modem);
	return true;
}

static bool samples_are_silent(const int16_t *samples, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		if (samples[index] != 0)
			return false;
	return true;
}

static bool test_v21_originator_answers_brief_mark(void)
{
	enum {
		BRIEF_MARK_SAMPLES = MODEM_RATE * 15 / 1000,
		ORIGINATOR_GUARD_SAMPLES = MODEM_RATE * 160 / 1000,
		TEST_FSK_AMPLITUDE = 8192,
	};
	struct datapumps datapumps = { 0 };
	struct modem_driver driver = {
		.name = "V.21 mark acquisition test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct fsk_transmitter answerer;
	struct dp_operations *operations;
	struct modem *modem;
	struct dp *datapump;
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];

	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_OK,
	      "V.21 mark acquisition registrations did not start");
	operations = modem_dp_get_registered_ops(DP_V21);
	modem = modem_create(&driver, "V.21 mark acquisition test");
	CHECK(operations != NULL && modem != NULL &&
		      fsk_transmitter_init(
			      &answerer, &v21_channel_2,
			      TEST_FSK_AMPLITUDE) == FSK_OK,
	      "V.21 mark acquisition fixture did not initialize");
	datapump = operations->create(
		modem, DP_V21, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(datapump != NULL, "V.21 originator did not initialize");
	for (size_t presented = 0; presented < BRIEF_MARK_SAMPLES;) {
		size_t remaining = BRIEF_MARK_SAMPLES - presented;
		size_t count = remaining < MODEM_FRAG ? remaining : MODEM_FRAG;

		for (size_t index = 0; index < count; ++index) {
			bool consumed;

			CHECK(fsk_transmitter_sample(
				      &answerer, 1, &receive[index], &consumed) ==
				      FSK_OK,
			      "V.21 answer mark generation failed");
		}
		memset(transmit, 1, sizeof(transmit));
		CHECK(operations->process(
			      datapump, receive, transmit, (int)count) ==
			      DPSTAT_OK &&
			      samples_are_silent(transmit, count),
		      "V.21 originator transmitted before acquiring answer mark");
		presented += count;
	}
	for (size_t guarded = 0; guarded < ORIGINATOR_GUARD_SAMPLES;) {
		size_t remaining = ORIGINATOR_GUARD_SAMPLES - guarded;
		size_t count = remaining < MODEM_FRAG ? remaining : MODEM_FRAG;

		memset(receive, 0, sizeof(receive));
		memset(transmit, 1, sizeof(transmit));
		CHECK(operations->process(
			      datapump, receive, transmit, (int)count) ==
			      DPSTAT_OK &&
			      samples_are_silent(transmit, count),
		      "V.21 originator transmitted during its 160 ms guard");
		guarded += count;
	}
	memset(transmit, 0, sizeof(transmit));
	CHECK(operations->process(
		      datapump, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      !samples_are_silent(transmit, MODEM_FRAG),
	      "V.21 originator did not answer a 15 ms mark");
	CHECK(operations->delete(datapump) == 0,
	      "V.21 originator did not release its data pump");
	modem_delete(modem);
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_OK,
	      "V.21 mark acquisition registrations did not stop");
	return true;
}

static bool test_owns_source_built_datapump_registrations(void)
{
	struct datapumps datapumps = { 0 };
	struct datapumps rival = { 0 };
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
	struct dp_operations *v32bis_operations;
	struct dp_operations *v34_operations;
	struct dp_operations *v90_operations;
	struct dp_operations *v92_operations;
	struct dp_operations *baudot_operations;
	struct dp *call_progress;
	struct dp *v8;
	struct dp *baudot;
	struct dp *v22;
	struct dp *v22bis;
	struct dp *bell212;
	struct dp *v32;
	struct dp *v32bis;
	struct dp *v34;
	struct dp *v90;
	struct dp *v92;
	struct single_tone_transmitter v32_line_artifact;
	struct single_tone_transmitter v32_answer_tone;
	int16_t receive[MODEM_FRAG];
	int16_t transmit[MODEM_FRAG];
	uint64_t v32_calling_power = 0;

	CHECK(modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V8) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_V32BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_V34) == NULL &&
		      modem_dp_get_registered_ops(DP_V90) == NULL &&
		      modem_dp_get_registered_ops(DP_V92) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
		      modem_dp_register(DP_V90, &conflict_operations) == 0 &&
		      datapumps_start(&datapumps) ==
			      DATAPUMPS_REGISTRATION_CONFLICT &&
		      modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V8) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_V32BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_V34) == NULL &&
		      modem_dp_get_registered_ops(DP_V90) ==
			      &conflict_operations &&
		      modem_dp_get_registered_ops(DP_V92) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
		      modem_dp_deregister(DP_V90, &conflict_operations) == 0 &&
		      datapumps_start(&datapumps) ==
			      DATAPUMPS_OK &&
		      datapumps_are_started(&datapumps),
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
	v32bis_operations = modem_dp_get_registered_ops(DP_V32BIS);
	v34_operations = modem_dp_get_registered_ops(DP_V34);
	v90_operations = modem_dp_get_registered_ops(DP_V90);
	v92_operations = modem_dp_get_registered_ops(DP_V92);
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
		      v32bis_operations == v32_operations &&
		      v34_operations != NULL &&
		      v34_operations != v21_operations &&
		      v34_operations != v23_operations &&
		      v34_operations != v22_operations &&
		      v34_operations != v32_operations &&
		      v34_operations->create != NULL &&
		      v34_operations->delete != NULL &&
		      v34_operations->process != NULL &&
		      v34_operations->use_count == 0 &&
		      v90_operations != NULL &&
		      v90_operations != v34_operations &&
		      v90_operations->create != NULL &&
		      v90_operations->delete != NULL &&
		      v90_operations->process != NULL &&
		      v90_operations->process_g711 != NULL &&
		      v90_operations->use_count == 0 &&
		      v92_operations == v90_operations &&
		      baudot_operations != NULL &&
		      baudot_operations != v21_operations &&
		      baudot_operations != v23_operations &&
		      baudot_operations != v22_operations &&
		      baudot_operations->create != NULL &&
		      baudot_operations->delete != NULL &&
		      baudot_operations->process != NULL &&
		      baudot_operations->use_count == 0,
	      "source-built modem operations were incomplete");
	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_ALREADY_STARTED &&
		      datapumps_start(&rival) ==
			      DATAPUMPS_REGISTRATION_CONFLICT,
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
		      datapumps_stop(&datapumps) ==
			      DATAPUMPS_BUSY &&
		      v8_operations->delete(v8) == 0 &&
		      v8_operations->use_count == 0,
	      "V.8 did not own and release its calling handshake");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "V.8 caller emitted audio before the answer signal");
	modem->min_rate = modem->max_rate = 600;
	v22 = v22_operations->create(
		modem, DP_V22, 1, MODEM_RATE, MODEM_FRAG,
		v22_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v22 != NULL &&
		      v22_operations->process(
			      v22, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      samples_are_silent(transmit, MODEM_FRAG) &&
		      v22_operations->delete(v22) == 0 &&
		      v22_operations->use_count == 0,
	      "V.22 600 data pump did not wait silently and release cleanly");
	modem->min_rate = modem->max_rate = 1200;
	v22 = v22_operations->create(
		modem, DP_V22, 1, MODEM_RATE, MODEM_FRAG, v22_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v22 != NULL &&
		      v22_operations->process(
			      v22, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      datapumps_stop(&datapumps) ==
			      DATAPUMPS_BUSY &&
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
	modem->min_rate = modem->max_rate = V32_4800_BIT_RATE;
	v32 = v32_operations->create(
		modem, DP_V32, 1, MODEM_RATE, MODEM_FRAG, v32_operations);
	CHECK(v32 != NULL && v32->id == DP_V32 &&
		      single_tone_transmitter_init(
			      &v32_line_artifact, 50, 512) ==
			      SINGLE_TONE_OK &&
		      single_tone_transmitter_init(
			      &v32_answer_tone,
			      V32_ANSWER_TONE_HZ, 12000) ==
			      SINGLE_TONE_OK &&
		      v32_operations->use_count == 1 &&
		      datapumps_stop(&datapumps) ==
			      DATAPUMPS_BUSY,
		      "V.32 data pump did not retain its registration");
	for (size_t presented = 0; presented < MODEM_RATE * 3u / 20u;) {
		size_t remaining = MODEM_RATE * 3u / 20u - presented;
		size_t count = remaining < MODEM_FRAG ? remaining : MODEM_FRAG;

		for (size_t index = 0; index < count; ++index)
			CHECK(single_tone_transmitter_sample(
				      &v32_line_artifact, &receive[index]) ==
				      SINGLE_TONE_OK,
			      "V.32 line-artifact generation failed");
		memset(transmit, 1, sizeof(transmit));
		CHECK(v32_operations->process(
			      v32, receive, transmit, (int)count) == DPSTAT_OK &&
			      samples_are_silent(transmit, count),
		      "V.32 caller treated a low-frequency artifact as QAM");
		presented += count;
	}
	for (size_t presented = 0; presented < MODEM_RATE;) {
		size_t remaining = MODEM_RATE - presented;
		size_t count = remaining < MODEM_FRAG ? remaining : MODEM_FRAG;

		for (size_t index = 0; index < count; ++index)
			CHECK(single_tone_transmitter_sample(
				      &v32_answer_tone, &receive[index]) ==
				      SINGLE_TONE_OK,
			      "V.32 answer tone generation failed");
		memset(transmit, 1, sizeof(transmit));
		CHECK(v32_operations->process(
			      v32, receive, transmit, (int)count) == DPSTAT_OK &&
			      samples_are_silent(transmit, count),
		      "V.32 caller transmitted before its answer-tone guard");
		presented += count;
	}
	for (size_t presented = 0; presented < MODEM_RATE / 20u;) {
		size_t remaining = MODEM_RATE / 20u - presented;
		size_t count = remaining < MODEM_FRAG ? remaining : MODEM_FRAG;

		for (size_t index = 0; index < count; ++index)
			CHECK(single_tone_transmitter_sample(
				      &v32_answer_tone, &receive[index]) ==
				      SINGLE_TONE_OK,
			      "V.32 guarded answer tone generation failed");
		CHECK(v32_operations->process(
			      v32, receive, transmit, (int)count) == DPSTAT_OK,
		      "V.32 calling-carrier frame failed");
		for (size_t index = 0; index < count; ++index)
			v32_calling_power +=
				(uint64_t)((int32_t)transmit[index] * transmit[index]);
		presented += count;
	}
	CHECK(v32_calling_power > UINT64_C(1000000000),
	      "V.32 caller omitted its guarded carrier (%llu)",
	      (unsigned long long)v32_calling_power);
	CHECK(v32_operations->delete(v32) == 0 &&
		      v32_operations->use_count == 0,
	      "V.32 data pump did not release after line startup");
	modem->min_rate = modem->max_rate = 14400;
	v32bis = v32bis_operations->create(
		modem, DP_V32BIS, 1, MODEM_RATE, MODEM_FRAG,
		v32bis_operations);
	CHECK(v32bis != NULL && v32bis->id == DP_V32BIS &&
		      v32bis_operations->use_count == 1 &&
		      v32bis_operations->delete(v32bis) == 0 &&
		      v32bis_operations->use_count == 0,
	      "V.32bis alias did not share the V.32 data pump");
	modem->min_rate = V34_MAX_BIT_RATE + 1;
	modem->max_rate = V34_MAX_BIT_RATE + V34_RATE_STEP;
	CHECK(v34_operations->create(
		      modem, DP_V34, 1, MODEM_RATE, MODEM_FRAG,
		      v34_operations) == NULL &&
		      v34_operations->use_count == 0,
	      "V.34 accepted a range above its implemented rates");
	modem->min_rate = modem->max_rate = V34_MAX_BIT_RATE;
	v34 = v34_operations->create(
		modem, DP_V34, 1, MODEM_RATE, MODEM_FRAG, v34_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v34 != NULL && v34->id == DP_V34 &&
		      v34_operations->use_count == 1 &&
		      v34_operations->process(
			      v34, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      datapumps_stop(&datapumps) == DATAPUMPS_BUSY &&
		      v34_operations->delete(v34) == 0 &&
		      v34_operations->use_count == 0,
	      "V.34 data pump did not retain and release its call state");
	{
		char command[] = "AT+MS=90,0,28000,28000";

		CHECK(process_at_command(modem, command) == 0 &&
			      MODEM_DP(modem) == DP_V90 &&
			      modem->min_rate ==
				      V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE &&
			      modem->max_rate ==
				      V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
		      "AT+MS did not expose the registered V.90 profile");
	}
	modem->dsp_info.pcm_role = MODEM_PCM_ROLE_ANALOG;
	v90 = v90_operations->create(
		modem, DP_V90, 1, MODEM_RATE, MODEM_FRAG, v90_operations);
	CHECK(v90 != NULL && v90->id == DP_V90 &&
		      v90_operations->use_count == 1 &&
		      datapumps_stop(&datapumps) == DATAPUMPS_BUSY &&
		      v90_operations->delete(v90) == 0 &&
		      v90_operations->use_count == 0,
	      "V.90 data pump did not retain and release its call state");
	{
		char pcm_upstream[] = "AT+V92Q=0+V92U=1,24000";
		char selection[] = "AT+MS=92,0,28000,28000";

		CHECK(process_at_command(modem, pcm_upstream) == 0 &&
		      process_at_command(modem, selection) == 0 &&
		      MODEM_DP(modem) == DP_V92,
		      "AT+MS did not expose the shared V.92 PCM profile");
	}
	v92 = v92_operations->create(
		modem, DP_V92, 1, MODEM_RATE, MODEM_FRAG, v92_operations);
	CHECK(v92 != NULL && v92->id == DP_V92 &&
		      v92_operations->use_count == 1 &&
		      datapumps_stop(&datapumps) == DATAPUMPS_BUSY &&
		      v92_operations->delete(v92) == 0 &&
		      v92_operations->use_count == 0,
	      "V.92 did not retain the shared PCM call state");
	modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
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
		      datapumps_stop(&datapumps) ==
			      DATAPUMPS_BUSY &&
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
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_BUSY &&
		      operations->delete(call_progress) == 0 &&
		      operations->use_count == 0,
	      "live call progress did not protect its registration");
	modem_delete(modem);
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_OK &&
		      modem_dp_get_registered_ops(DP_CALLPROG) == NULL &&
		      modem_dp_get_registered_ops(DP_V8) == NULL &&
		      modem_dp_get_registered_ops(DP_V21) == NULL &&
		      modem_dp_get_registered_ops(DP_B103) == NULL &&
		      modem_dp_get_registered_ops(DP_V23) == NULL &&
		      modem_dp_get_registered_ops(DP_V22) == NULL &&
		      modem_dp_get_registered_ops(DP_V22BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_B212) == NULL &&
		      modem_dp_get_registered_ops(DP_V32) == NULL &&
		      modem_dp_get_registered_ops(DP_V32BIS) == NULL &&
		      modem_dp_get_registered_ops(DP_V34) == NULL &&
		      modem_dp_get_registered_ops(DP_V90) == NULL &&
		      modem_dp_get_registered_ops(DP_V92) == NULL &&
		      modem_dp_get_registered_ops(DP_BAUDOT) == NULL &&
		      !datapumps_are_started(&datapumps) &&
		      datapumps_stop(&datapumps) ==
			      DATAPUMPS_NOT_STARTED &&
		      strcmp(datapumps_result_name(
			     DATAPUMPS_BUSY),
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
		{ "instance-owned modem events",
		  test_modem_events_are_instance_owned },
		{ "escape discards async transmit fragment",
		  test_escape_discards_async_transmit_fragment },
		{ "V.8 datapump mapping", test_v8_datapump_mapping },
		{ "V.8 PCM handoff", test_v8_pcm_handoff },
		{ "modulation report identity", test_modulation_report_identity },
		{ "data-pump error-control policy",
		  test_datapump_error_control_policy },
		{ "source-built core lifecycle",
		  test_core_has_no_external_dsp_owner },
		{ "G.711 data-pump input", test_g711_datapump_input },
		{ "truthful ATO1 lifecycle",
		  test_ato1_waits_for_datapump_retrain },
		{ "bounded AT dial destinations",
		  test_at_dial_destinations_are_bounded },
		{ "AT compression profile", test_at_compression_profile },
		{ "AT PCM profile controls", test_at_pcm_profile_controls },
		{ "AT V.34 symbol rate", test_at_v34_symbol_rate },
		{ "bounded LAPM XID envelopes", test_lapm_xid_envelopes },
		{ "LAPM receive backpressure", test_lapm_receive_backpressure },
		{ "V.21 brief answer mark",
		  test_v21_originator_answers_brief_mark },
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
