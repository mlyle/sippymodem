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
#include "lapm_xid_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

extern int process_at_command(struct modem *modem, char *command);
extern int modem_dial(struct modem *modem);
extern int modem_recv_from_tty(struct modem *modem, char *buffer, int count);

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

static void bit_timer_expired(struct modem *modem)
{
	modem->ring_count++;
	modem->bit_timer = 0;
}

static int arm_bit_timer_on_transmit(
	struct modem *modem, int bits_per_group, uint8_t *bits, int count)
{
	(void)bits_per_group;
	memset(bits, 1, (size_t)count);
	if (modem->bit_timer == 0) {
		modem->bit_timer = 32;
		modem->bit_timer_func = bit_timer_expired;
	}
	return count;
}

static int arm_bit_timer_on_receive(
	struct modem *modem, int bits_per_group, uint8_t *bits, int count)
{
	(void)bits_per_group;
	(void)bits;
	if (modem->bit_timer == 0) {
		modem->bit_timer = 32;
		modem->bit_timer_func = bit_timer_expired;
	}
	return count;
}

static bool check_bit_timer_transition(bool transmit)
{
	struct modem modem = { 0 };
	uint8_t bits[64] = { 0 };

	if (transmit)
		modem.get_bits = arm_bit_timer_on_transmit;
	else
		modem.put_bits = arm_bit_timer_on_receive;
	if (transmit)
		CHECK(modem_get_bits(&modem, 1, bits, (int)sizeof(bits)) ==
			      (int)sizeof(bits),
		      "transmit timer fixture produced an unexpected length");
	else
		CHECK(modem_put_bits(&modem, 1, bits, (int)sizeof(bits)) ==
			      (int)sizeof(bits),
		      "receive timer fixture consumed an unexpected length");
	CHECK(modem.bit_timer == 32 && modem.ring_count == 0,
	      "new %s timer was charged for its transition batch",
	      transmit ? "transmit" : "receive");
	if (transmit)
		modem_get_bits(&modem, 1, bits, (int)sizeof(bits));
	else
		modem_put_bits(&modem, 1, bits, (int)sizeof(bits));
	CHECK(modem.bit_timer == 0 && modem.ring_count == 1,
	      "%s timer did not consume the following batch",
	      transmit ? "transmit" : "receive");
	return true;
}

static bool test_bit_timers_follow_duplex_time(void)
{
	CHECK(check_bit_timer_transition(true) &&
		      check_bit_timer_transition(false),
	      "link timer transition accounting failed");
	return true;
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

static bool test_escape_holds_candidate_bytes(void)
{
	enum { TEST_STATE_MODEM_ONLINE = 5 };
	struct modem_driver driver = {
		.name = "escape hold test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "escape hold test");
	char received[8];
	uint8_t bits[32];

	CHECK(modem != NULL, "escape hold modem could not be created");
	for (unsigned ec = 0; ec < 2; ec++) {
		modem->state = TEST_STATE_MODEM_ONLINE;
		modem->command = 0;
		modem->cfg.ec = ec;
		modem->get_chars = modem_recv_from_tty;
		modem->last_esc_check = 0;
		for (unsigned index = 0; index < 3; index++) {
			CHECK(modem_write(modem, "+", 1) == 1 &&
				      modem->escape_count == index + 1,
			      "split escape candidate was not retained");
			CHECK(modem_recv_from_tty(modem, received, sizeof(received)) == 0,
			      "escape candidate reached the character source (EC %u)", ec);
		}
		if (!ec) {
			CHECK(modem_async_get_bits(modem, 1, bits, sizeof(bits)) == sizeof(bits),
			      "raw escape guard did not produce idle bits");
			for (unsigned index = 0; index < sizeof(bits); index++)
				CHECK(bits[index] == 1, "escape candidate reached raw framing");
		}
		modem->event_deadline = get_time() - 1;
		CHECK(modem_service_event(modem) == 1 && modem->command == 1 &&
			      modem->xmit.count == 0,
		      "confirmed escape was not consumed as command syntax");
	}
	modem->state = TEST_STATE_MODEM_ONLINE;
	modem->command = 0;
	for (unsigned count = 1; count <= 2; count++) {
		modem->last_esc_check = 0;
		CHECK(modem_write(modem, "++", count) == (int)count &&
			      modem_recv_from_tty(modem, received, sizeof(received)) == 0,
		      "incomplete escape was not held");
		CHECK(modem->event_scheduled, "incomplete escape has no expiry");
		modem->event_deadline = get_time() - 1;
		CHECK(modem_service_event(modem) == 1 && modem->command == 0 &&
			      modem_recv_from_tty(modem, received, sizeof(received)) == (int)count &&
			      memcmp(received, "++", count) == 0,
		      "expired incomplete escape did not return to line data");
	}
	for (unsigned count = 2; count <= 3; count++) {
		modem->last_esc_check = 0;
		CHECK(modem_write(modem, "+++", count) == (int)count &&
			      modem_write(modem, "x", 1) == 1 &&
			      modem_recv_from_tty(modem, received, sizeof(received)) == (int)count + 1 &&
			      memcmp(received, "+++", count) == 0 && received[count] == 'x',
		      "cancelled escape changed line data");
		modem->event_deadline = get_time() - 1;
		CHECK(modem_service_event(modem) == 1 && modem->command == 0,
		      "cancelled escape entered command mode");
	}
	/* Earlier data may cross the ring boundary while its escape suffix waits. */
	modem->xmit.head = modem->xmit.tail = modem->xmit.size - 2;
	CHECK(modem_write(modem, "old", 3) == 3, "earlier line data was not queued");
	modem->last_esc_check = 0;
	CHECK(modem_write(modem, "+++", 3) == 3 &&
		      modem_recv_from_tty(modem, received, sizeof(received)) == 3 &&
		      memcmp(received, "old", 3) == 0 && modem->xmit.count == 3,
	      "escape hold blocked or consumed earlier ring data");
	modem->event_deadline = get_time() - 1;
	CHECK(modem_service_event(modem) == 1 && modem->command == 1,
	      "wrapped escape did not enter command mode");
	/* Backpressure must count only accepted bytes toward an escape. */
	modem->state = TEST_STATE_MODEM_ONLINE;
	modem->command = 0;
	modem->xmit.tail = 0;
	modem->xmit.head = modem->xmit.count = modem->xmit.size - 1;
	memset(modem->xmit.buf, 'o', modem->xmit.count);
	modem->last_esc_check = 0;
	CHECK(modem_write(modem, "+++", 3) == 1 && modem->escape_count == 1 &&
		      modem_write(modem, "++", 2) == 0 && modem->escape_count == 1,
	      "unaccepted input changed the escape candidate");
	while (modem->xmit.count > 1) {
		int count = modem_recv_from_tty(modem, received, sizeof(received));

		CHECK(count > 0, "escape candidate blocked older data under backpressure");
		for (int index = 0; index < count; index++)
			CHECK(received[index] == 'o', "unconfirmed escape escaped a full queue");
	}
	modem->event_deadline = get_time() - 1;
	CHECK(modem_service_event(modem) == 1 && modem->command == 0 &&
		      modem_recv_from_tty(modem, received, sizeof(received)) == 1 &&
		      received[0] == '+',
	      "partial write did not release its incomplete escape");
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
	unsigned int hangup_calls;
	int retrain_result;
	int hangup_result;
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

static int process_stalled_lapm_dp(
	struct dp *datapump, void *receive, void *transmit, int count)
{
	unsigned before = retrain_dp.retrain_calls;
	uint8_t bit;

	/* Force a real bit-timer expiry inside the datapump's source callback. */
	datapump->modem->bit_timer = 1;
	modem_get_bits(datapump->modem, 1, &bit, 1);
	if (retrain_dp.retrain_calls != before)
		return DPSTAT_ERROR;
	return process_retrain_dp(datapump, receive, transmit, count);
}

static int request_hangup_dp(struct dp *datapump)
{
	if (datapump != &retrain_dp.datapump)
		return -1;
	retrain_dp.hangup_calls++;
	return retrain_dp.hangup_result;
}

static int process_hangup_g711(
	struct dp *datapump, enum g711_law law, const uint8_t *receive_codewords,
	void *receive, const bool *received_mask, uint8_t *transmit_codewords, void *transmit, int count,
	bool *exact_output)
{
	int result = process_retrain_dp(datapump, receive, transmit, count);

	(void)receive_codewords;
	(void)received_mask;
	/* Preserve the signed minimum-magnitude codeword through the last
	 * datapump fragment, including PCMU's otherwise lost negative zero. */
	uint8_t octet = law == G711_LAW_PCMA ? 0x55 : 0x7f;
	memset(transmit_codewords, octet, count);
	for (int index = 0; index < count; ++index)
		((int16_t *)transmit)[index] = law == G711_LAW_PCMA ?
			g711_decode_pcma(octet) : g711_decode_pcmu(octet);
	*exact_output = true;
	return result;
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
	char fallback_response[256];
	char unsupported[] = "ATO1";
	char invalid[] = "ATO2";
	char retrain[] = "ATO1";
	char resume[] = "ATO0";
	ssize_t fallback_length;
	int fallback_pipe[2];

	memset(&retrain_dp, 0, sizeof(retrain_dp));
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "ATO1 test data pump did not register");
	modem = modem_create(&driver, "ATO1 lifecycle test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "ATO1 test call did not start");
	CHECK(strcmp(modem_call_phase(modem), "call-progress") == 0,
	      "dialing phase missing from diagnostics");
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
	CHECK(pipe(fallback_pipe) == 0,
	      "ATO1 fallback response pipe failed");
	modem->pty = fallback_pipe[1];
	CHECK(process_at_command(modem, unsupported) == 1 &&
		      modem->state == TEST_STATE_MODEM_ONLINE &&
		      modem->command == 0 && modem->retrain_pending == 0 &&
		      retrain_dp.retrain_calls == 0,
	      "ATO1 without data-pump support did not resume the link");
	close(fallback_pipe[1]);
	modem->pty = -1;
	fallback_length = read(
		fallback_pipe[0], fallback_response,
		sizeof(fallback_response) - 1u);
	close(fallback_pipe[0]);
	CHECK(fallback_length > 0,
	      "ATO1 fallback produced no terminal response");
	fallback_response[fallback_length] = '\0';
	CHECK(strstr(fallback_response, "WARNING: Retrain unavailable") != NULL &&
		      strstr(fallback_response, "CONNECT") != NULL &&
		      strstr(fallback_response, "ERROR") == NULL,
	      "ATO1 fallback response was not truthful: %s",
	      fallback_response);
	operations.retrain = request_retrain_dp;
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
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
	CHECK(strcmp(modem_call_phase(modem), "local-retrain") == 0,
	      "pending ATO1 was reported online");
	modem_process(modem, receive, transmit, MODEM_FRAG);
	CHECK(modem->state == TEST_STATE_MODEM_ONLINE &&
		      modem->retrain_pending == 0 &&
		      retrain_dp.datapump.status == DPSTAT_CONNECT,
	      "data-pump CONNECT did not complete ATO1");
	CHECK(strcmp(modem_call_phase(modem), "online") == 0,
	      "completed ATO1 retained retrain phase");
	retrain_dp.datapump.status = DPSTAT_OK;
	CHECK(strcmp(modem_call_phase(modem), "retraining") == 0,
	      "automatic datapump retraining was reported online");
	retrain_dp.datapump.status = DPSTAT_CONNECT;
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

static unsigned int hangup_driver_stop_calls;

static int count_hangup_driver_stop(struct modem *modem)
{
	(void)modem;
	hangup_driver_stop_calls++;
	return 0;
}

enum ath0_test_completion {
	ATH0_ACKNOWLEDGED,
	ATH0_TIMED_OUT,
	ATH0_SIMULTANEOUS,
	ATH0_REMOTE_END,
	ATH0_RESET,
	PEER_DISC,
	PEER_DISC_TIMEOUT,
	HANGUP_WITHOUT_COMMAND,
};

static bool run_ath0_cleardown(enum ath0_test_completion completion, enum g711_law law)
{
	enum {
		TEST_STATE_MODEM_IDLE = 1,
		TEST_STATE_COMMAND_ONLINE = 6,
		TEST_STATE_EC_DISC = 7,
		TEST_STATE_DP_DISC = 9,
		LAPM_DATA_STATE = 2,
		LAPM_DISC_STATE = 3,
		UA_CONTROL = 0x73,
	};
	struct modem_driver driver = {
		.name = "ATH0 cleardown test",
		.start = unused_start,
		.stop = count_hangup_driver_stop,
		.ioctl = unused_control,
	};
	struct dp_operations operations = {
		.name = "ATH0 cleardown test",
		.create = create_retrain_dp,
		.delete = delete_retrain_dp,
		.process = process_retrain_dp,
		.process_g711 = process_hangup_g711,
		.hangup = request_hangup_dp,
	};
	int16_t receive[MODEM_FRAG * 4] = { 0 };
	int16_t transmit[ARRAY_LENGTH(receive)];
	uint8_t receive_codewords[ARRAY_LENGTH(receive)] = { 0 };
	uint8_t transmit_codewords[ARRAY_LENGTH(receive)];
	bool exact_output;
	uint8_t response_bytes[2];
	frame_t response = {
		.buf = response_bytes,
		.count = sizeof(response_bytes),
	};
	frame_t *disconnect;
	struct modem *modem;
	char command[] = "ATH0";
	int result_pipe[2];
	char result_text[32];
	bool peer = completion == PEER_DISC || completion == PEER_DISC_TIMEOUT;
	const char *expected_result =
		peer || completion == ATH0_RESET || completion == HANGUP_WITHOUT_COMMAND ?
			"NO CARRIER\r\n" : "OK\r\n";

	memset(&retrain_dp, 0, sizeof(retrain_dp));
	hangup_driver_stop_calls = 0;
	retrain_dp.process_result = DPSTAT_OK;
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "ATH0 test data pump did not register");
	modem = modem_create(&driver, "ATH0 cleardown test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "ATH0 test call did not start");
	CHECK(pipe(result_pipe) == 0 &&
	      fcntl(result_pipe[0], F_SETFL, O_NONBLOCK) == 0,
	      "ATH0 result pipe could not be opened");
	modem->pty = result_pipe[1];
	/* Build an already-established answer-side LAPM owner without running a
	 * second datapump through the negotiation fixture. */
	modem_ec_exit(modem);
	modem->caller = 0;
	modem->cfg.ec = 1;
	modem->cfg.comp = 0;
	modem->cfg.ec_tx_win_size = 15;
	modem->cfg.ec_rx_win_size = 15;
	modem->cfg.ec_tx_info_size = 128;
	modem->cfg.ec_rx_info_size = 128;
	CHECK(modem_ec_init(modem) == 0,
	      "ATH0 LAPM owner did not initialize");
	modem_hdlc_start(modem);
	modem_ec_start(modem);
	modem->ec.lapm.state = LAPM_DATA_STATE;
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
	modem->hook = MODEM_HOOK_OFF;
	if (peer) {
		uint8_t serialized[32];
		response_bytes[0] = modem->ec.lapm.rsp_addr;
		response_bytes[1] = 0x53;
		modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &response);
		CHECK(modem->state == TEST_STATE_DP_DISC && modem->ec.lapm.state == 0 &&
		      modem->get_bits == modem_hdlc_get_bits && modem->put_bits == modem_hdlc_put_bits &&
		      retrain_dp.hangup_calls == 0 && hangup_driver_stop_calls == 0 &&
		      modem->sample_timer > modem->count &&
		      modem->ec.lapm.tx_ctrl != modem->ec.lapm.ctrl_list &&
		      modem->ec.lapm.tx_ctrl->buf[1] == UA_CONTROL,
		      "peer DISC lost its queued UA or prematurely cleared the modem");
		CHECK(modem_hdlc_get_bits(modem, 8, serialized, sizeof(serialized)) == sizeof(serialized) &&
		      modem->packer.hdlc.tx_frame == NULL &&
		      modem->ec.lapm.tx_ctrl == modem->ec.lapm.ctrl_list,
		      "peer UA did not drain through the retained HDLC serializer");
		goto wait_datapump;
	}
	if (completion == ATH0_ACKNOWLEDGED) {
		uint8_t poll_bytes[] = { modem->ec.lapm.rsp_addr, 0x01, 0x01 };
		frame_t poll = { .buf = poll_bytes, .count = sizeof(poll_bytes) };

		/* Hold one RR in HDLC and fill the remaining control queue. */
		for (unsigned int i = 0; i < LAPM_CTRL_FRAMES; ++i) {
			modem->packer.hdlc.rx_complete(
				modem->packer.hdlc.framer, &poll);
			if (i == 0)
				modem->packer.hdlc.tx_frame =
					modem->packer.hdlc.get_tx_frame(
						modem->packer.hdlc.framer);
		}
		CHECK(modem->ec.lapm.ctrl_list->next == modem->ec.lapm.tx_ctrl,
		      "ATH0 fixture did not fill the LAPM control queue");
		modem->ec.lapm.rtx_count = 3;
	}
	if (completion == HANGUP_WITHOUT_COMMAND)
		modem_hangup(modem);
	else
		CHECK(process_at_command(modem, command) == 1,
		      "ATH0 was acknowledged before its line shutdown");
	CHECK(modem->state == TEST_STATE_EC_DISC &&
		      modem->ec.lapm.state == LAPM_DISC_STATE &&
		      retrain_dp.hangup_calls == 0 &&
		      hangup_driver_stop_calls == 0 &&
		      modem->hook == MODEM_HOOK_OFF &&
		      modem->sample_timer > modem->count,
	      "ATH0 did not wait for LAPM disconnect");
	CHECK(read(result_pipe[0], result_text, sizeof(result_text)) == -1 &&
	      errno == EAGAIN,
	      "ATH0 emitted a result before LAPM release");
	CHECK(modem->ec.lapm.rtx_count == 0,
	      "LAPM disconnect inherited the data retry counter");
	if (completion == ATH0_ACKNOWLEDGED) {
		frame_t *active = modem->packer.hdlc.tx_frame;

		CHECK(active != NULL && active->size == 3 &&
		      active->buf[0] == modem->ec.lapm.rsp_addr &&
		      active->buf[1] == 0x01 && active->buf[2] == 0x01,
		      "LAPM disconnect overwrote the control frame still in HDLC");
		modem->packer.hdlc.tx_complete(modem->packer.hdlc.framer, active);
		modem->packer.hdlc.tx_frame = NULL;
	}
	{
		unsigned long deadline = modem->sample_timer;

		modem_update_status(modem, STATUS_DP_LINK);
		modem_update_status(modem, STATUS_PACK_LINK);
		modem_update_status(modem, STATUS_EC_LINK);
		CHECK((completion == HANGUP_WITHOUT_COMMAND ||
		       process_at_command(modem, command) == 1) &&
		      modem->sample_timer == deadline &&
		      modem->state == TEST_STATE_EC_DISC &&
		      retrain_dp.hangup_calls == 0,
		      "late link status or repeated ATH0 changed LAPM disconnect");
	}
	if (completion == ATH0_ACKNOWLEDGED) {
		static const uint8_t controls[] = { 0x00, 0x01, 0x05, 0x09 };
		uint8_t late_bytes[] = { modem->ec.lapm.rsp_addr, 0, 0 };
		frame_t late = { .buf = late_bytes, .count = sizeof(late_bytes) };
		unsigned long deadline = modem->sample_timer;

		for (size_t i = 0; i < ARRAY_LENGTH(controls); ++i) {
			late_bytes[1] = controls[i];
			modem->packer.hdlc.rx_complete(
				modem->packer.hdlc.framer, &late);
			CHECK(modem->state == TEST_STATE_EC_DISC &&
			      modem->ec.lapm.state == LAPM_DISC_STATE &&
			      modem->sample_timer == deadline &&
			      retrain_dp.hangup_calls == 0,
			      "ordinary frame 0x%02x acknowledged LAPM DISC", controls[i]);
		}
		response_bytes[0] = modem->ec.lapm.cmd_addr;
		response_bytes[1] = UA_CONTROL & ~0x10u;
		modem->packer.hdlc.rx_complete(
			modem->packer.hdlc.framer, &response);
		CHECK(modem->state == TEST_STATE_EC_DISC &&
		      modem->ec.lapm.state == LAPM_DISC_STATE &&
		      modem->sample_timer == deadline && retrain_dp.hangup_calls == 0,
		      "UA without the final bit acknowledged LAPM DISC");
	}
	if (completion == ATH0_REMOTE_END || completion == ATH0_RESET) {
		char reset[] = "ATZ";

		CHECK((completion == ATH0_REMOTE_END ?
		       modem_report_remote_call_end(modem) :
		       process_at_command(modem, reset)) == 0,
		      "external call termination failed during ATH0");
		goto stopped;
	}
	disconnect = modem->packer.hdlc.get_tx_frame(
		modem->packer.hdlc.framer);
	CHECK(disconnect != NULL &&
		      (disconnect->buf[1] & 0xefu) == 0x43u,
	      "ATH0 did not queue LAPM DISC");
	modem->packer.hdlc.tx_complete(
		modem->packer.hdlc.framer, disconnect);
	if (completion == ATH0_SIMULTANEOUS) {
		unsigned long deadline = modem->sample_timer;
		uint8_t serialized[32];

		response_bytes[0] = modem->ec.lapm.rsp_addr;
		response_bytes[1] = 0x53;
		modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &response);
		CHECK(modem->state == TEST_STATE_EC_DISC &&
		      modem->ec.lapm.state == LAPM_DISC_STATE &&
		      modem->sample_timer == deadline && retrain_dp.hangup_calls == 0 &&
		      modem->ec.lapm.tx_ctrl != modem->ec.lapm.ctrl_list &&
		      modem->ec.lapm.tx_ctrl->buf[1] == UA_CONTROL,
		      "simultaneous DISC lost its UA or changed the local deadline");
		CHECK(modem_hdlc_get_bits(modem, 8, serialized, sizeof(serialized)) == sizeof(serialized) &&
		      modem->packer.hdlc.tx_frame == NULL &&
		      modem->ec.lapm.tx_ctrl == modem->ec.lapm.ctrl_list,
		      "simultaneous DISC did not serialize its UA before cleardown");
	}
	if (completion == ATH0_ACKNOWLEDGED) {
		response_bytes[0] = modem->ec.lapm.cmd_addr;
		response_bytes[1] = UA_CONTROL;
		modem->packer.hdlc.rx_complete(
			modem->packer.hdlc.framer, &response);
	} else {
		unsigned int samples = 0;

		while (modem->state == TEST_STATE_EC_DISC && samples < MODEM_RATE * 2) {
			modem_process(modem, receive, transmit, ARRAY_LENGTH(receive));
			samples += ARRAY_LENGTH(receive);
		}
		CHECK(samples >= MODEM_RATE * 3 / 2 && samples < MODEM_RATE * 2,
		      "LAPM disconnect did not respect its bounded wait");
	}
	CHECK(modem->state == TEST_STATE_DP_DISC &&
		      retrain_dp.hangup_calls == 1 &&
		      hangup_driver_stop_calls == 0 &&
		      modem->hook == MODEM_HOOK_OFF &&
		      modem->sample_timer > modem->count,
	      "LAPM UA did not begin datapump cleardown");
wait_datapump:
	modem_process(modem, receive, transmit, ARRAY_LENGTH(receive));
	CHECK(modem->state == TEST_STATE_DP_DISC &&
		      hangup_driver_stop_calls == 0,
	      "ATH0 released the call before datapump completion");
	if (completion == ATH0_TIMED_OUT || completion == PEER_DISC_TIMEOUT) {
		unsigned int samples = ARRAY_LENGTH(receive);

		while (modem->started && samples < MODEM_RATE * 4) {
			modem_process(modem, receive, transmit, ARRAY_LENGTH(receive));
			samples += ARRAY_LENGTH(receive);
		}
		CHECK(samples >= MODEM_RATE * 3 && samples < MODEM_RATE * 4,
		      "datapump disconnect did not respect its bounded wait");
		goto stopped;
	}
	retrain_dp.process_result = DPSTAT_DISCONNECT;
	modem_process_g711(modem, law, receive_codewords, receive, NULL,
		transmit_codewords, transmit, ARRAY_LENGTH(receive), &exact_output);
	CHECK(modem->state == TEST_STATE_DP_DISC && modem->process == NULL &&
		      hangup_driver_stop_calls == 0 && exact_output &&
		      transmit_codewords[MODEM_FRAG - 1] == (law == G711_LAW_PCMA ? 0x55 : 0x7f) &&
		      transmit_codewords[MODEM_FRAG] == (law == G711_LAW_PCMA ? 0xd5 : 0xff),
	      "datapump completion did not leave its final media frame to drain");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == (law == G711_LAW_PCMA ?
			g711_decode_pcma(transmit_codewords[index]) : g711_decode_pcmu(transmit_codewords[index])),
		      "final exact-media fragment disagreed with its codeword at %zu", index);
	CHECK(read(result_pipe[0], result_text, sizeof(result_text)) == -1 &&
	      errno == EAGAIN,
	      "ATH0 emitted a result before the final media frame drained");
	modem_process(modem, receive, transmit, ARRAY_LENGTH(receive));
stopped:
	CHECK(modem->state == TEST_STATE_MODEM_IDLE &&
		      hangup_driver_stop_calls == 1 &&
		      modem->sample_timer == 0 && modem->sample_timer_func == NULL &&
		      modem->hook == MODEM_HOOK_ON && operations.use_count == 0,
	      "datapump cleardown did not complete ATH0: state=%u stop=%u "
	      "hook=%u uses=%d timer=%lu count=%lu process=%p",
	      modem->state, hangup_driver_stop_calls, modem->hook,
	      operations.use_count, modem->sample_timer, modem->count,
	      (void *)modem->process);
	CHECK(modem->hangup_command_pending == 0 &&
	      modem->result_code == 0 &&
	      read(result_pipe[0], result_text, sizeof(result_text)) ==
		      (ssize_t)strlen(expected_result) &&
	      memcmp(result_text, expected_result, strlen(expected_result)) == 0,
	      "hangup did not emit exactly its final command/carrier result");
	CHECK(read(result_pipe[0], result_text, sizeof(result_text)) == -1 &&
	      errno == EAGAIN,
	      "hangup repeated its final result");
	modem->pty = -1;
	close(result_pipe[0]);
	close(result_pipe[1]);
	modem_delete(modem);
	CHECK(modem_dp_deregister(DP_CALLPROG, &operations) == 0,
	      "ATH0 test data pump did not deregister");
	return true;
}

static bool test_ath0_drains_link_cleardown(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum ath0_test_completion completion = ATH0_ACKNOWLEDGED;
		     completion <= HANGUP_WITHOUT_COMMAND; ++completion)
			CHECK(run_ath0_cleardown(completion, law), "ATH0 law %u case %u failed", law, completion);
	return true;
}

static struct {
	struct dp datapump;
	const uint8_t *codewords;
	const bool *received_mask;
	unsigned int missing_samples;
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
	const uint8_t *receive_codewords, void *receive, const bool *received_mask,
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
	g711_dp.received_mask = received_mask;
	for (int index = 0; received_mask != NULL && index < count; ++index)
		g711_dp.missing_samples += !received_mask[index];
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
	bool received_mask[MODEM_FRAG + 1];
	uint8_t transmit_codewords[MODEM_FRAG + 1] = { 0 };
	int16_t receive[MODEM_FRAG + 1] = { 0 };
	int16_t transmit[MODEM_FRAG + 1];
	struct modem *modem;
	bool exact_output = false;

	memset(&g711_dp, 0, sizeof(g711_dp));
	memset(codewords, 0x7f, sizeof(codewords));
	for (size_t index = 0; index < ARRAY_LENGTH(received_mask); ++index)
		received_mask[index] = index != 0 && index != MODEM_FRAG;
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "G.711 test data pump did not register");
	modem = modem_create(&driver, "G.711 data pump test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "G.711 test call did not start");
	modem_process_g711(
		modem, G711_LAW_PCMU, codewords, receive, received_mask,
		transmit_codewords, transmit, MODEM_FRAG, &exact_output);
	CHECK(g711_dp.g711_calls == 1 && g711_dp.linear_calls == 0 &&
		      g711_dp.law == G711_LAW_PCMU &&
		      g711_dp.codewords == codewords && exact_output &&
		      g711_dp.received_mask == received_mask && g711_dp.missing_samples == 1,
	      "exact codewords did not traverse the opt-in data pump");
	for (size_t index = 0; index < MODEM_FRAG; ++index) {
		CHECK(transmit_codewords[index] == (uint8_t)~codewords[index],
		      "exact transmit codeword changed at sample %zu", index);
	}
	g711_dp.g711_calls = 0;
	g711_dp.first_fragment_only = true;
	exact_output = true;
	modem_process_g711(
		modem, G711_LAW_PCMA, codewords, receive, received_mask,
		transmit_codewords, transmit, ARRAY_LENGTH(codewords),
		&exact_output);
	CHECK(g711_dp.g711_calls == 2 && !exact_output &&
	      g711_dp.received_mask == received_mask + MODEM_FRAG && g711_dp.missing_samples == 3,
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

static bool test_receive_dc_frontend(void)
{
	struct modem_driver driver = {
		.name = "receive DC front-end test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct dp_operations operations = {
		.name = "receive DC front-end test",
		.create = create_g711_dp,
		.delete = delete_g711_dp,
		.process = process_g711_dp,
		.process_g711 = process_g711_codewords,
	};
	static const struct {
		enum DP_ID id;
		enum modem_pcm_role role;
		bool pcm_upstream;
		int16_t expected;
		const char *name;
	} cases[] = {
		{ DP_CALLPROG, MODEM_PCM_ROLE_NONE, false, 0,
		  "conventional receive" },
		{ DP_V90, MODEM_PCM_ROLE_ANALOG, false, 1400,
		  "V.90 downstream PCM" },
		{ DP_V90, MODEM_PCM_ROLE_DIGITAL, false, 0,
		  "V.90 conventional upstream" },
		{ DP_V92, MODEM_PCM_ROLE_DIGITAL, true, 1400,
		  "V.92 PCM upstream" },
		{ DP_V92, MODEM_PCM_ROLE_DIGITAL, false, 0,
		  "V.92 conventional upstream" },
	};
	uint8_t codeword = 0xff;
	uint8_t transmit_codeword = 0;
	int16_t receive;
	int16_t transmit;
	struct modem *modem;
	bool exact_output;

	memset(&g711_dp, 0, sizeof(g711_dp));
	CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
	      "DC front-end data pump did not register");
	modem = modem_create(&driver, "receive DC front-end test");
	CHECK(modem != NULL && modem_dial(modem) == 0,
	      "DC front-end call did not start");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		modem->dp->id = cases[index].id;
		modem->dsp_info.pcm_role = cases[index].role;
		modem->sregs[SREG_V92_PCM_UPSTREAM] = cases[index].pcm_upstream;
		modem->receive_dc = (struct dc_remover){
			.level = 1400,
			.stage = DC_REMOVER_TRACKING,
		};
		receive = 1400;
		transmit = 1;
		exact_output = false;
		modem_process_g711(
			modem, G711_LAW_PCMU, &codeword, &receive, NULL,
			&transmit_codeword, &transmit, 1, &exact_output);
		CHECK(receive == cases[index].expected && exact_output,
		      "%s DC policy changed sample to %d, expected %d",
		      cases[index].name, receive, cases[index].expected);
	}
	modem->dp->id = DP_CALLPROG;
	CHECK(modem_stop_now(modem) == 0 && operations.use_count == 0,
	      "DC front-end data pump did not stop cleanly");
	modem_delete(modem);
	CHECK(modem_dp_deregister(DP_CALLPROG, &operations) == 0,
	      "DC front-end data pump did not deregister");
	return true;
}

static bool test_connect_reports_observed_startup(void)
{
	struct modem_driver driver = {
		.name = "startup report test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	static const struct {
		enum modem_startup startup;
		const char *text;
	} cases[] = {
		{ MODEM_STARTUP_UNKNOWN, NULL },
		{ MODEM_STARTUP_QC, "Startup: QC\r\n" },
		{ MODEM_STARTUP_V8, "Startup: V8\r\n" },
		{ MODEM_STARTUP_V8_FALLBACK, "Startup: V8-FALLBACK\r\n" },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct modem *modem = modem_create(&driver, driver.name);
		char response[256];
		int result_pipe[2];
		ssize_t length;

		CHECK(modem != NULL && pipe(result_pipe) == 0,
		      "startup report fixture failed");
		modem->pty = result_pipe[1];
		modem->sregs[SREG_CONNNECT_MSG_FORMAT] = 1;
		modem->sregs[SREG_V92_QUICK_CONNECT] = 1;
		modem->dsp_info.startup = cases[index].startup;
		modem_update_status(modem, STATUS_EC_LINK);
		close(result_pipe[1]);
		modem->pty = -1;
		length = read(result_pipe[0], response, sizeof(response) - 1);
		close(result_pipe[0]);
		CHECK(length > 0, "CONNECT produced no startup report");
		response[length] = '\0';
		CHECK(strstr(response, "CONNECT") != NULL &&
		      (cases[index].text != NULL ? strstr(response, cases[index].text) != NULL :
		       strstr(response, "Startup:") == NULL),
		      "CONNECT substituted configuration for observed startup: %s", response);
		modem_delete(modem);
	}
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

		modem_dp_connected(modem);
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
		{
			int (*get_bits)(struct modem *, int, u8 *, int) =
				modem->get_bits;
			int (*put_bits)(struct modem *, int, u8 *, int) =
				modem->put_bits;
			unsigned int state = modem->state;

			modem_dp_connected(modem);
			CHECK(modem->get_bits == get_bits &&
				      modem->put_bits == put_bits &&
				      modem->state == state,
			      "duplicate data-pump link notification changed transport %u",
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

		modem_dp_connected(modem);
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

static bool test_v92_negotiated_lapm_bypasses_detection(void)
{
	struct datapumps datapumps = { 0 };
	struct modem_driver driver = {
		.name = "negotiated LAPM test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	static const struct {
		enum DP_ID id;
		bool selected;
		bool bypass;
	} cases[] = {
		{ DP_V92, true, true },
		{ DP_V92, false, false },
		{ DP_V90, true, false },
		{ DP_V34, true, false },
	};

	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK,
	      "negotiated LAPM registrations did not start");
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		for (unsigned caller = 0; caller <= 1; ++caller) {
			struct modem *modem = modem_create(&driver, "negotiated LAPM");
			struct dp datapump = { .id = cases[i].id, .modem = modem };

			CHECK(modem != NULL, "negotiated LAPM modem allocation failed");
			modem->dp = &datapump;
			modem->caller = caller;
			modem->rx_rate = 26400;
			modem->tx_rate = 53333;
			modem->cfg.ec = modem->cfg.ec_detector = 1;
			modem->dsp_info.v8_lapm = cases[i].selected;
			modem->dsp_info.upstream_mode = MODEM_UPSTREAM_V34;
			CHECK(modem_ec_init(modem) == 0, "LAPM initialization failed");
			modem_dp_connected(modem);
			for (unsigned epoch = 0; epoch < 2; ++epoch) {
				CHECK(modem->get_bits == (cases[i].bypass ?
					      modem_hdlc_get_bits : modem_detector_get_bits) &&
				      modem->put_bits == (cases[i].bypass ?
					      modem_hdlc_put_bits : modem_detector_put_bits),
				      "LAPM detection policy changed: case %zu role %u epoch %u",
				      i, caller, epoch);
				if (cases[i].bypass) {
					CHECK(modem->bit_timer == 0 &&
					      modem->packer.hdlc.framer == &modem->ec.lapm &&
					      (modem->packer.hdlc.tx_preamble_flags != 0) ==
						      (caller != 0),
					      "negotiated LAPM lost its role-specific HDLC startup");
				}
				modem_dp_reconnected(modem);
			}
			modem->dp = NULL;
			modem->dsp_info.startup = MODEM_STARTUP_QC;
			CHECK(process_at_command(modem, "ATZ") == 0 &&
			      process_at_command(modem, "ATDpeer") == 1 &&
			      modem->dsp_info.v8_lapm == 0 &&
			      modem->dsp_info.startup == MODEM_STARTUP_UNKNOWN,
			      "a new call inherited the previous V.8 LAPM selection");
			modem_delete(modem);
		}
	}
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "negotiated LAPM registrations did not stop");
	return true;
}

static bool complete_raw_v14_detection(struct modem *answerer)
{
	uint8_t bits[576];
	struct modem pattern_source = {
		.name = "raw detection pattern source",
	};

	pattern_source.packer.detector.tx_count = 1;
	pattern_source.packer.detector.tx_pattern[0] = 0x88;
	pattern_source.packer.detector.tx_pattern[1] = 0x89;
	CHECK(modem_detector_get_bits(
		      &pattern_source, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits) &&
		      answerer->packer.detector.rx_count == 4 &&
		      answerer->packer.detector.ec_enable == 0,
	      "raw answerer did not accept two ODP patterns");
	CHECK(modem_get_bits(
		      answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits),
	      "raw answerer did not transmit 16 ADP-V.14 patterns");
	CHECK(answerer->get_bits == modem_async_get_bits &&
		      answerer->put_bits == modem_async_put_bits &&
		      answerer->cfg.ec == 0,
	      "raw answerer did not complete V.14 selection");
	return true;
}

static bool feed_async_characters(
	struct modem *receiver, const uint8_t *characters, size_t count)
{
	uint8_t bits[128];
	struct modem source = {
		.name = "raw detection residue source",
		.get_chars = scripted_lapm_source,
	};
	size_t bit_count = count * 10u;

	CHECK(bit_count <= ARRAY_LENGTH(bits),
	      "raw detection residue fixture overflowed");
	lapm_source.data = characters;
	lapm_source.length = count;
	lapm_source.offset = 0;
	modem_async_start(&source);
	CHECK(modem_async_get_bits(
		      &source, 1, bits, (int)bit_count) == (int)bit_count &&
		      lapm_source.offset == count &&
		      receiver->put_bits(receiver, 1, bits, (int)bit_count) ==
			      (int)bit_count,
	      "raw detection residue stream was not transferred");
	return true;
}

static bool test_raw_answerer_discards_detection_residue(void)
{
	static const uint8_t residue_then_data[] = {
		0x91, 0x11, 0x91, 0x11, 'R',
	};
	static const uint8_t lone_detection_character[] = { 0x11, 'S' };
	static const struct {
		const uint8_t *input;
		size_t input_length;
		const uint8_t *expected;
		size_t expected_length;
	} cases[] = {
		{ residue_then_data, ARRAY_LENGTH(residue_then_data),
		  (const uint8_t *)"R", 1 },
		{ lone_detection_character, ARRAY_LENGTH(lone_detection_character),
		  lone_detection_character, ARRAY_LENGTH(lone_detection_character) },
	};
	struct modem_driver driver = {
		.name = "raw detection residue test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct modem *answerer = modem_create(
			&driver, "raw detection residue test");
		struct dp datapump = {
			.id = DP_V34,
			.modem = answerer,
			.status = DPSTAT_CONNECT,
		};

		CHECK(answerer != NULL,
		      "raw detection residue modem could not be created");
		answerer->dp = &datapump;
		answerer->rx_rate = answerer->tx_rate = 9600;
		answerer->cfg.ec = 0;
		answerer->cfg.ec_detector = 0;
		modem_dp_connected(answerer);
		CHECK(complete_raw_v14_detection(answerer),
		      "raw answerer detection setup failed");
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		answerer->put_chars = scripted_lapm_sink;
		CHECK(feed_async_characters(
			      answerer, cases[index].input,
			      cases[index].input_length),
		      "raw answerer residue fixture failed");
		CHECK(lapm_sink.length == cases[index].expected_length &&
			      memcmp(lapm_sink.data, cases[index].expected,
				     cases[index].expected_length) == 0,
		      "raw answerer produced %zu bytes in residue case %zu",
		      lapm_sink.length, index);
		answerer->dp = NULL;
		modem_delete(answerer);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
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

static bool test_v8_handoff(void)
{
	static const struct {
		enum DP_ID caller_requested;
		enum DP_ID answerer_requested;
		enum DP_ID handoff;
		bool caller_digital;
		bool answerer_digital;
		enum modem_pcm_role caller_role;
		enum modem_pcm_role answerer_role;
		bool caller_v34_fallback;
		bool caller_lapm;
		bool answerer_lapm;
	} selections[] = {
		{ DP_V34, DP_V34, DP_V34,
		  false, false, MODEM_PCM_ROLE_NONE, MODEM_PCM_ROLE_NONE,
		  false, true, true },
		{ DP_V90, DP_V90, DP_V90,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true },
		{ DP_V90, DP_V90, DP_V90,
		  true, false, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true },
		{ DP_V92, DP_V92, DP_V92,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true },
		{ DP_V92, DP_V92, DP_V92,
		  true, false, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true },
		{ DP_V92, DP_V92, DP_V92,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, false, true },
		{ DP_V90, DP_V34, DP_V34,
		  true, false, MODEM_PCM_ROLE_NONE, MODEM_PCM_ROLE_NONE,
		  true, true, true },
		{ DP_V90, DP_V90, DP_V90,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, false },
	};
	struct modem_driver driver = {
		.name = "V.8 handoff test",
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
	struct single_tone_detector legacy_carrier;
	int16_t calling_audio[MODEM_FRAG] = { 0 };
	int16_t answering_audio[MODEM_FRAG] = { 0 };
	int calling_status = DPSTAT_OK;
	int answering_status = DPSTAT_OK;

	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK,
	      "V.8 handoff registrations did not start");
	operations = modem_dp_get_registered_ops(DP_V8);
	CHECK(operations != NULL,
	      "V.8 handoff operation did not initialize");
	for (size_t run = 0; run < 2 * ARRAY_LENGTH(selections); ++run) {
		size_t selection = run % ARRAY_LENGTH(selections);
		enum DP_ID caller_requested =
			selections[selection].caller_requested;
		enum DP_ID answerer_requested =
			selections[selection].answerer_requested;

		caller = modem_create(&driver, "V.8 caller");
		answerer = modem_create(&driver, "V.8 answerer");
		CHECK(caller != NULL && answerer != NULL,
		      "V.8 handoff endpoints did not initialize");
		MODEM_AUTOMODE(caller) = run >= ARRAY_LENGTH(selections);
		MODEM_AUTOMODE(answerer) = MODEM_AUTOMODE(caller);
		CHECK(process_at_command(
			      caller, selections[selection].caller_digital ?
				      "AT+V90D=1" : "AT+V90D=0") == 0 &&
			      process_at_command(
				      answerer,
				      selections[selection].answerer_digital ?
					      "AT+V90D=1" : "AT+V90D=0") == 0,
		      "V.8 endpoints did not accept PCM role configuration");
		caller->cfg.ec = selections[selection].caller_lapm;
		answerer->cfg.ec = selections[selection].answerer_lapm;
		CHECK(process_at_command(
			      caller, selections[selection].caller_lapm ?
				      "AT\\N4" : "AT\\N0") == 0 &&
			      process_at_command(
				      answerer, selections[selection].answerer_lapm ?
					      "AT\\N4" : "AT\\N0") == 0,
		      "V.8 endpoints did not accept error-control configuration");
		calling_dp = operations->create(
			caller, caller_requested, 1, MODEM_RATE, MODEM_FRAG,
			operations);
		answering_dp = operations->create(
			answerer, answerer_requested, 0, MODEM_RATE, MODEM_FRAG,
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
			      !v8_datapump_v32_handoff(calling_dp, &legacy_carrier) &&
			      !v8_datapump_v32_handoff(answering_dp, &legacy_carrier) &&
			      caller->dp_requested == selections[selection].handoff &&
			      answerer->dp_requested == selections[selection].handoff &&
			      caller->dsp_info.pcm_role ==
				      selections[selection].caller_role &&
			      answerer->dsp_info.pcm_role ==
				      selections[selection].answerer_role &&
			      caller->cfg.ec == selections[selection].caller_lapm &&
			      answerer->cfg.ec == selections[selection].answerer_lapm &&
			      caller->dsp_info.v8_lapm ==
				      (selections[selection].caller_lapm &&
				       selections[selection].answerer_lapm) &&
			      answerer->dsp_info.v8_lapm ==
				      (selections[selection].caller_lapm &&
				       selections[selection].answerer_lapm) &&
			      v8_datapump_selected_v34_fallback(calling_dp) ==
				      selections[selection].caller_v34_fallback &&
			      !v8_datapump_selected_v34_fallback(answering_dp),
		      "V.8 peers did not agree on handoff %u",
		      (unsigned int)selections[selection].handoff);
		CHECK(operations->delete(calling_dp) == 0 &&
			      operations->delete(answering_dp) == 0 &&
			      operations->use_count == 0,
		      "V.8 PCM handshakes did not release their state");
		modem_delete(caller);
		modem_delete(answerer);
	}
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.8 handoff registrations did not stop");
	return true;
}

static bool test_v8_fallback_limits(void)
{
	struct modem_driver driver = {
		.name = "V.8 fallback limits",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	struct datapumps datapumps = { 0 };

	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK,
	      "fallback limit registrations failed");
	for (unsigned int restricted = 0; restricted < 3; ++restricted) {
		struct modem *caller = modem_create(&driver, "restricted caller");
		struct modem *answerer = modem_create(&driver, "automatic answerer");
		struct dp_operations *ops = modem_dp_get_registered_ops(DP_V8);
		int16_t tx[MODEM_FRAG] = { 0 }, rx[MODEM_FRAG] = { 0 };
		int result = DPSTAT_OK;
		struct dp *calling, *answering;

		CHECK(caller != NULL && answerer != NULL && ops != NULL,
		      "fallback limit endpoints failed");
		/* V.21 must not bypass a fixed family or a 1200-bit/s floor.
		 * Conversely, selecting V.21 must not offer the higher V.32 family. */
		MODEM_AUTOMODE(answerer) = restricted != 0;
		answerer->min_rate = restricted == 1 ? 1200 : 300;
		calling = ops->create(caller, restricted == 2 ? DP_V32 : DP_V21,
			1, MODEM_RATE, MODEM_FRAG, ops);
		answering = ops->create(answerer, restricted == 2 ? DP_V21 : DP_V34,
			0, MODEM_RATE, MODEM_FRAG, ops);
		CHECK(calling != NULL && answering != NULL,
		      "restricted V.8 handshakes failed to initialize");
		for (unsigned int frame = 0; result == DPSTAT_OK &&
		     frame < 12u * MODEM_RATE / MODEM_FRAG; ++frame) {
			result = ops->process(answering, tx, rx, MODEM_FRAG);
			if (result == DPSTAT_OK)
				result = ops->process(calling, rx, tx, MODEM_FRAG);
		}
		CHECK(result == DPSTAT_ERROR && caller->dp_requested == 0 &&
		      answerer->dp_requested == 0,
		      "V.8 escaped configured fallback limit %u", restricted);
		CHECK(ops->delete(calling) == 0 && ops->delete(answering) == 0,
		      "restricted V.8 handshakes leaked state");
		modem_delete(caller);
		modem_delete(answerer);
	}
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "fallback limit registrations leaked");
	return true;
}

static bool test_v8_legacy_v32(void)
{
	struct modem_driver driver = {
		.name = "legacy V.32 selection",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	struct datapumps datapumps = { 0 };

	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK,
	      "legacy V.32 registrations failed");
	for (unsigned int scenario = 0; scenario < 6; ++scenario) {
		struct modem *modem = modem_create(&driver, "automatic answerer");
		struct dp_operations *ops = modem_dp_get_registered_ops(DP_V8);
		struct single_tone_transmitter tone;
		struct single_tone_detector carrier;
		struct v8_pcm_handoff pcm;
		struct dp *dp;
		int result = DPSTAT_OK;

		CHECK(modem != NULL && ops != NULL &&
		      single_tone_transmitter_init(&tone, scenario == 4 ? 1650 : 1800,
			      12000) == SINGLE_TONE_OK,
		      "legacy V.32 fixture failed");
		MODEM_AUTOMODE(modem) = scenario != 1;
		modem->min_rate = scenario == 2 ? 16800 : 300;
		modem->max_rate = 56000;
		modem->cfg.ec = 1;
		dp = ops->create(modem, DP_V92, scenario == 3,
			MODEM_RATE, MODEM_FRAG, ops);
		CHECK(dp != NULL && !v8_datapump_v32_handoff(dp, &carrier),
		      "legacy V.32 handoff was available before detection");
		/* A 300-ms fragment followed by 100 ms silence must not count
		 * towards the subsequent half-second continuous confirmation. */
		for (unsigned int sample = 0; sample < MODEM_RATE &&
		     result == DPSTAT_OK; sample += MODEM_FRAG) {
			int16_t rx[MODEM_FRAG], tx[MODEM_FRAG];

			for (unsigned int i = 0; i < MODEM_FRAG; ++i) {
				CHECK(single_tone_transmitter_sample(&tone, &rx[i]) == SINGLE_TONE_OK,
				      "legacy V.32 stimulus failed");
				if (sample + i >= MODEM_RATE * 3 / 10 &&
				    sample + i < MODEM_RATE * 4 / 10)
					rx[i] = 0;
			}
			result = ops->process(dp, rx, scenario == 5 ? rx : tx, MODEM_FRAG);
			CHECK(result == DPSTAT_OK || result == DPSTAT_CHANGEDP,
			      "legacy detector failed scenario %u", scenario);
			CHECK(result != DPSTAT_CHANGEDP || sample >= MODEM_RATE * 9 / 10,
			      "legacy detector accumulated disconnected carrier fragments");
		}
		CHECK((scenario == 0 || scenario == 5) == (result == DPSTAT_CHANGEDP) &&
		      (scenario == 0 || scenario == 5) == v8_datapump_v32_handoff(dp, &carrier) &&
		      modem->dp_requested == (scenario == 0 || scenario == 5 ? DP_V32 : 0),
		      "legacy V.32 escaped selection limits in scenario %u", scenario);
		if (scenario == 0 || scenario == 5)
			CHECK(single_tone_detector_is_valid(&carrier) && carrier.tone_present &&
			      carrier.profile.frequency_hz == 1800 &&
			      !v8_datapump_pcm_handoff(dp, &pcm) &&
			      !v8_datapump_v32_handoff(dp, NULL) &&
			      modem->cfg.ec == 1 && !modem->dsp_info.v8_lapm &&
			      modem->dsp_info.startup == MODEM_STARTUP_UNKNOWN,
			      "legacy handoff lost carrier history or invented V.8 agreement");
		CHECK(ops->delete(dp) == 0, "legacy negotiation state leaked");
		modem_delete(modem);
	}
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "legacy V.32 registrations leaked");
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
		int tone;
	} cases[] = {
		{ "ATDextension", "extension", -1 },
		{ "ATDuser@example.invalid", "user@example.invalid", -1 },
		{ "ATDsip:peer@example.invalid", "sip:peer@example.invalid", -1 },
		{ "ATD5551234", "5551234", -1 },
		{ "ATDT5551234", "5551234", 1 },
		{ "ATDP5551234", "5551234", 0 },
		{ "atdt5551234", "5551234", 1 },
		{ "atdp5551234", "5551234", 0 },
		{ "ATDTsip:Test@example.invalid", "sip:Test@example.invalid", 1 },
		{ "ATDPsip:Peer@example.invalid", "sip:Peer@example.invalid", 0 },
		{ "ATDsip:Test@example.invalid", "sip:Test@example.invalid", -1 },
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
		if (cases[index].tone >= 0)
			modem_set_sreg(modem, SREG_TONE_OR_PULSE, !cases[index].tone);
		CHECK(process_at_command(modem, cases[index].command) == 1 &&
			      strcmp(modem->dial_string,
				     cases[index].destination) == 0 &&
			      modem_get_sreg(modem, SREG_TONE_OR_PULSE) ==
				      (cases[index].tone < 0 ? 1 : cases[index].tone),
		      "AT dial destination %zu changed", index);
		CHECK(modem_stop_now(modem) == 0, "AT dial modem did not stop");
		{
			char redial[] = "ATDL";

			CHECK(process_at_command(modem, redial) == 1 &&
			      strcmp(modem->dial_string, cases[index].destination) == 0,
			      "redial changed AT dial destination %zu", index);
		}
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
	CHECK(modem->v92_upstream_lu_ucode == 0,
	      "PCM upstream did not default to the profile training level");
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
	CHECK(process_at_command(modem, "AT+V92U=1,40000,69") == 0 &&
		      modem->v92_upstream_lu_ucode == 69 &&
		      modem_get_sreg(modem, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 13,
	      "AT+V92U did not retain its rate and training level");
	char invalid_levels[][32] = {
		"AT+V92U=1,40000,128", "AT+V92U=1,40000,-1",
		"AT+V92U=1,40000,", "AT+V92U=1,40000,69,1",
		"AT+V92U=0,40000,69", "AT+V92U=1,28100,70",
	};
	for (size_t index = 0; index < ARRAY_LENGTH(invalid_levels); ++index)
		CHECK(process_at_command(modem, invalid_levels[index]) == -1 &&
			      modem->v92_upstream_lu_ucode == 69 &&
			      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 1 &&
			      modem_get_sreg(modem, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 13,
		      "invalid AT+V92U level changed the retained profile");
	CHECK(process_at_command(modem, "AT+V92U=1,40000,0") == 0 &&
		      modem->v92_upstream_lu_ucode == 0 &&
		      process_at_command(modem, "AT+V92U=1,40000,127") == 0 &&
		      modem->v92_upstream_lu_ucode == 127 &&
		      process_at_command(modem, "AT+V92U=1,40000") == 0 &&
		      modem->v92_upstream_lu_ucode == 0 &&
		      process_at_command(modem, "AT+V92U=1,40000,1") == 0 &&
		      modem->v92_upstream_lu_ucode == 1 &&
		      process_at_command(modem, "AT+V92U=0") == 0 &&
		      modem->v92_upstream_lu_ucode == 0 &&
		      process_at_command(modem, "AT+V92U=1,40000,69") == 0,
	      "AT+V92U did not accept or reset its bounded training level");
	int response_pipe[2];
	char response[64] = { 0 };

	CHECK(pipe(response_pipe) == 0, "PCM profile response pipe failed");
	modem->pty = response_pipe[1];
	CHECK(process_at_command(modem, "AT+V92U?") == 0,
	      "PCM upstream profile query failed");
	close(response_pipe[1]);
	modem->pty = -1;
	ssize_t response_length = read(response_pipe[0], response, sizeof(response) - 1u);
	close(response_pipe[0]);
	CHECK(response_length > 0 && strstr(response, "1,40000,69\r\n") != NULL,
	      "PCM upstream query omitted the selected training level");
	CHECK(process_at_command(modem, "ATZ") == 0 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == 0 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 1 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem->v90_upstream_min_rate == 0 &&
		      modem->v90_upstream_max_rate == 0 &&
		      modem->v90_upstream_max_baud == 0,
	      "modem reset retained a forced PCM profile");
	CHECK(modem->v92_upstream_lu_ucode == 0,
	      "modem reset retained the PCM upstream training level");
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

static bool test_lapm_dual_compression_retry(void)
{
	struct modem_driver driver = {
		.name = "LAPM compression retry test",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *caller = modem_create(&driver, "LAPM compression retry caller");
	struct lapm_v44_session before;
	struct v42_xid_fields fields;
	uint8_t preamble[109], received[LAPM_MAX_INFO_SIZE + 4];
	frame_t incoming = { .buf = received };
	frame_t *outgoing;

	CHECK(caller != NULL, "compression retry caller could not be created");
	caller->caller = 1;
	caller->rx_rate = caller->tx_rate = 2400;
	CHECK(modem_ec_init(caller) == 0, "compression retry initialization failed");
	modem_ec_start(caller);
	CHECK(modem_hdlc_get_bits(caller, 8, preamble, sizeof(preamble)) == sizeof(preamble),
	      "compression retry preamble failed");
	outgoing = caller->packer.hdlc.tx_frame;
	caller->packer.hdlc.tx_frame = NULL;
	CHECK(outgoing != NULL, "compression retry initial XID missing");
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	before = caller->ec.lapm.v44_session;
	received[0] = caller->ec.lapm.cmd_addr;
	received[1] = 0xaf;
	memcpy(received + 2, lapm_dual_compression_response, sizeof(lapm_dual_compression_response));
	incoming.count = 2 + sizeof(lapm_dual_compression_response);
	caller->bit_timer = 1234;
	caller->ec.lapm.rtx_count = 3;
	{
		frame_t *const saved_next = caller->ec.lapm.ctrl_list->next;
		bool unchanged;

		caller->ec.lapm.ctrl_list->next = caller->ec.lapm.tx_ctrl;
		caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
		unchanged = memcmp(&before, &caller->ec.lapm.v44_session, sizeof(before)) == 0 &&
			caller->bit_timer == 1234 && caller->ec.lapm.rtx_count == 3;
		caller->ec.lapm.ctrl_list->next = saved_next;
		CHECK(unchanged, "retry queue pressure changed the command or its deadline");
	}
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.config == 1 && caller->ec.lapm.v44_path == NULL &&
	      caller->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING &&
	      caller->bit_timer == 14400 && caller->ec.lapm.rtx_count == 0 &&
	      caller->ec.lapm.tx_info_size == 128,
	      "dual selection applied settings or failed to start the new XID deadline");
	outgoing = caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
	      v42_xid_decode(outgoing->buf + 2, outgoing->size - 2, &fields) == V42_XID_OK &&
	      fields.private_negotiation != NULL && fields.user_data == NULL,
	      "dual selection did not queue a V.42bis-only command");
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	before = caller->ec.lapm.v44_session;
	caller->bit_timer = 2345;
	caller->ec.lapm.rtx_count = 2;
	incoming.count--;
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(memcmp(&before, &caller->ec.lapm.v44_session, sizeof(before)) == 0 &&
	      caller->bit_timer == 2345 && caller->ec.lapm.rtx_count == 2 &&
	      caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer) == NULL,
	      "malformed response restarted negotiation");
	incoming.count++;
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.config == 0 && caller->ec.lapm.v44_path == NULL &&
	      caller->ec.lapm.v44_session.phase == LAPM_V44_DECLINED &&
	      caller->cfg.comp == 3 && caller->bit_timer == 14400,
	      "cached reply to a V.42bis-only command did not finish XID negotiation");
	outgoing = caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
	      "compression retry did not proceed to SABME");
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	received[0] = caller->ec.lapm.cmd_addr;
	received[1] = 0x73;
	incoming.count = 2;
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.state == 2, "compression retry did not establish LAPM");
	modem_delete(caller);
	return true;
}

static bool test_lapm_xid_parameters(void)
{
	static const uint8_t expected_parameters[] = {
		0x03, 0x04, 0x8a, 0x89, 0x00, 0x00,
		0x05, 0x02, 0x02, 0x98, /* transmit N401: 83 octets, 664 bits */
		0x06, 0x02, 0x03, 0xb8, /* receive N401: 119 octets, 952 bits */
		0x07, 0x01, 0x07,
		0x08, 0x01, 0x0b,
	};
	static const uint8_t expected_private[] = {
		0x00, 0x03, 'V', '4', '2',
		0x01, 0x01, 0x03,
		0x02, 0x02, 0x04, 0x00,
		0x03, 0x01, 0x0c,
	};
	static const unsigned int compression_types[] = {
		MODEM_COMP_TYPE_NONE,
		MODEM_COMP_TYPE_V42BIS,
		MODEM_COMP_TYPE_V44,
		MODEM_COMP_TYPE_V42BIS | MODEM_COMP_TYPE_V44,
	};
	struct modem_driver driver = {
		.name = "LAPM XID parameters",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(compression_types); ++index) {
		struct modem *modem = modem_create(&driver, "LAPM XID parameters");
		struct v42_xid_fields fields;
		uint8_t preamble[109];
		frame_t *outgoing;

		CHECK(modem != NULL, "XID parameter modem could not be created");
		modem->caller = 1;
		modem->rx_rate = modem->tx_rate = 2400;
		modem->cfg.ec_tx_info_size = 83;
		modem->cfg.ec_rx_info_size = 119;
		modem->cfg.ec_tx_win_size = 7;
		modem->cfg.ec_rx_win_size = 11;
		modem->cfg.comp = 3;
		modem->cfg.comp_types = compression_types[index];
		modem->cfg.comp_dict_size = 1024;
		modem->cfg.comp_max_string = 12;
		CHECK(modem_ec_init(modem) == 0,
		      "XID parameter modem could not initialize");
		modem_ec_start(modem);
		CHECK(modem_hdlc_get_bits(modem, 8, preamble, sizeof(preamble)) ==
			      (int)sizeof(preamble),
		      "XID parameter modem did not finish its preamble");
		outgoing = modem->packer.hdlc.tx_frame;
		CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
			      v42_xid_decode(outgoing->buf + 2,
				      (size_t)outgoing->size - 2U, &fields) == V42_XID_OK &&
			      fields.parameter_negotiation_length == sizeof(expected_parameters) &&
			      memcmp(fields.parameter_negotiation, expected_parameters,
				      sizeof(expected_parameters)) == 0,
		      "XID did not encode the V.42 mask and directional parameters");
		if ((compression_types[index] & MODEM_COMP_TYPE_V42BIS) != 0) {
			CHECK(fields.private_negotiation_length == sizeof(expected_private) &&
				      memcmp(fields.private_negotiation, expected_private,
					      sizeof(expected_private)) == 0,
			      "XID did not encode the V.42bis parameters");
		} else {
			CHECK(fields.private_negotiation == NULL,
			      "XID offered V.42bis without the configured capability");
		}
		modem_delete(modem);
	}
	return true;
}

static bool test_lapm_xid_directions(void)
{
	static const uint8_t peer_parameters[] = {
		0x03, 0x04, 0x8a, 0x89, 0x00, 0x00,
		0x05, 0x02, 0x02, 0x98, /* peer transmit: 83 octets */
		0x06, 0x02, 0x03, 0xb8, /* peer receive: 119 octets */
		0x07, 0x01, 0x07,
		0x08, 0x01, 0x0b,
	};
	static const uint8_t local_parameters[] = {
		0x03, 0x04, 0x8a, 0x89, 0x00, 0x00,
		0x05, 0x02, 0x03, 0xb8,
		0x06, 0x02, 0x02, 0x98,
		0x07, 0x01, 0x0b,
		0x08, 0x01, 0x07,
	};
	struct modem_driver driver = {
		.name = "LAPM XID directions",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

	/* Initial command reception and response confirmation use peer directions. */
	for (unsigned int response = 0; response < 2; ++response) {
		struct modem *modem = modem_create(&driver, driver.name);
		uint8_t preamble[109];
		uint8_t wire[26] = { 0, 0xaf, 0x82, 0x80, 0, 20 };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		struct v42_xid_fields fields;
		frame_t *outgoing;

		CHECK(modem != NULL, "directional XID modem could not be created");
		modem->caller = response;
		modem->cfg.comp = 0;
		modem->rx_rate = modem->tx_rate = 2400;
		modem->cfg.ec_tx_info_size = response ? 83 : 70;
		modem->cfg.ec_rx_info_size = response ? 119 : 70;
		modem->cfg.ec_tx_win_size = response ? 7 : 2;
		modem->cfg.ec_rx_win_size = response ? 11 : 2;
		CHECK(modem_ec_init(modem) == 0,
		      "directional XID modem could not initialize");
		modem_ec_start(modem);
		if (modem->caller) {
			CHECK(modem_hdlc_get_bits(modem, 8, preamble, sizeof(preamble)) ==
				      (int)sizeof(preamble), "directional XID preamble failed");
			outgoing = modem->packer.hdlc.tx_frame;
			modem->packer.hdlc.tx_frame = NULL;
			CHECK(outgoing != NULL, "directional XID command was missing");
			modem->packer.hdlc.tx_complete(modem->packer.hdlc.framer, outgoing);
		}
		wire[0] = response ? modem->ec.lapm.cmd_addr : modem->ec.lapm.rsp_addr;
		memcpy(wire + 6, response ? local_parameters : peer_parameters, 20);
		modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &incoming);
		CHECK(modem->ec.lapm.tx_info_size == (response ? 83u : 119u) &&
			      modem->ec.lapm.rx_info_size == (response ? 119u : 83u) &&
			      modem->ec.lapm.tx_win_size == (response ? 7u : 11u) &&
			      modem->ec.lapm.rx_win_size == (response ? 11u : 7u),
		      "XID case %u did not reverse directions: N401 %u/%u, window %u/%u",
		      response, modem->ec.lapm.tx_info_size, modem->ec.lapm.rx_info_size,
		      modem->ec.lapm.tx_win_size, modem->ec.lapm.rx_win_size);
		outgoing = modem->packer.hdlc.get_tx_frame(modem->packer.hdlc.framer);
		CHECK(outgoing != NULL, "directional XID did not queue its reply");
		if (response) {
			CHECK(outgoing->buf[1] == 0x7f && modem->ec.lapm.config == 0,
			      "directional XID response did not finish negotiation");
		} else {
			CHECK(outgoing->buf[1] == 0xaf &&
				      v42_xid_decode(outgoing->buf + 2,
					      (size_t)outgoing->size - 2U, &fields) == V42_XID_OK &&
				      fields.parameter_negotiation_length == sizeof(local_parameters) &&
				      memcmp(fields.parameter_negotiation, local_parameters,
					      sizeof(local_parameters)) == 0,
			      "XID response did not encode the local frame and window directions");
		}
		modem->packer.hdlc.tx_complete(modem->packer.hdlc.framer, outgoing);
		modem_delete(modem);
	}
	return true;
}

static bool test_lapm_repeated_xid_parameters(void)
{
	struct modem_driver driver = {
		.name = "LAPM repeated XID parameters",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, driver.name);
	/* A 127-octet information field containing 41 valid parameter items. */
	uint8_t wire[129] = { 0, 0xaf, 0x82, 0x80, 0, 123 };
	frame_t incoming = { .buf = wire, .count = sizeof(wire) };
	struct v42_xid_fields fields;
	frame_t *response;

	CHECK(modem != NULL, "repeated XID modem could not be created");
	modem->cfg.comp = 0;
	CHECK(modem_ec_init(modem) == 0,
	      "repeated XID modem could not initialize");
	modem_ec_start(modem);
	wire[0] = modem->ec.lapm.rsp_addr;
	for (size_t index = 6; index < sizeof(wire); index += 3) {
		wire[index] = 7;
		wire[index + 1] = 1;
		wire[index + 2] = 15;
	}
	modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &incoming);
	response = modem->packer.hdlc.get_tx_frame(modem->packer.hdlc.framer);
	CHECK(response != NULL && response->buf[1] == 0xaf &&
		      v42_xid_decode(response->buf + 2,
			      (size_t)response->size - 2U, &fields) == V42_XID_OK &&
		      fields.parameter_negotiation_length == 20 &&
		      modem->ec.lapm.tx_win_size == 15 &&
		      modem->ec.lapm.rx_win_size == 15,
	      "bounded repeated XID parameters did not produce a valid response");
	modem->packer.hdlc.tx_complete(modem->packer.hdlc.framer, response);
	modem_delete(modem);
	return true;
}

static bool test_lapm_xid_envelopes(void)
{
	static const uint8_t expected_parameters[] = {
		0x03, 0x04, 0x8a, 0x89, 0x00, 0x00,
		0x05, 0x02, 0x04, 0x00,
		0x06, 0x02, 0x04, 0x00,
		0x07, 0x01, 0x0f,
		0x08, 0x01, 0x0f,
	};
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
	uint8_t preamble[109];
	uint8_t expected_preamble[109];
	frame_t incoming = { .buf = received };
	frame_t *outgoing;
	int command_size;

	CHECK(caller != NULL && answerer != NULL,
	      "LAPM XID peers could not be created");
	caller->caller = 1;
	answerer->caller = 0;
	caller->rx_rate = caller->tx_rate = 2400;
	CHECK(modem_ec_init(caller) == 0 && modem_ec_init(answerer) == 0,
	      "LAPM XID peers could not initialize");
	modem_ec_start(caller);
	modem_ec_start(answerer);
	CHECK(caller->packer.hdlc.tx_preamble_flags == 109u &&
		      caller->packer.hdlc.tx_preamble_complete != NULL &&
		      caller->packer.hdlc.get_tx_frame == NULL &&
		      caller->bit_timer == 0,
	      "LAPM caller did not schedule a transmitted-flag preamble");
	memset(expected_preamble, 0x7e, sizeof(expected_preamble));
	CHECK(modem_hdlc_get_bits(
		      caller, 8, preamble, (int)sizeof(preamble)) ==
		      (int)sizeof(preamble) &&
		      memcmp(preamble, expected_preamble, sizeof(preamble)) == 0 &&
		      caller->packer.hdlc.tx_preamble_flags == 0 &&
		      caller->packer.hdlc.tx_preamble_complete == NULL,
	      "LAPM caller did not emit the exact transmitted-flag preamble");
	outgoing = caller->packer.hdlc.tx_frame;
	caller->packer.hdlc.tx_frame = NULL;
	CHECK(outgoing != NULL && outgoing->size > 2 &&
		      outgoing->buf[1] == XID_CONTROL &&
		      v42_xid_decode(
			      outgoing->buf + 2, (size_t)outgoing->size - 2U,
			      &fields) == V42_XID_OK &&
		      fields.parameter_negotiation_length == sizeof(expected_parameters) &&
		      memcmp(fields.parameter_negotiation, expected_parameters,
			      sizeof(expected_parameters)) == 0 &&
		      fields.private_negotiation_length == 15 &&
		      fields.user_data != NULL &&
		      v44_xid_decode(
			      fields.user_data, fields.user_data_length,
			      &v44_offer) == V44_XID_OK &&
		      v44_offer.transmit.enabled && v44_offer.receive.enabled &&
		      caller->ec.lapm.v44_session.phase ==
			      LAPM_V44_COMMAND_PENDING,
	      "LAPM caller did not emit a bounded V.44 XID offer");
	CHECK(caller->bit_timer ==
		      (int)(3u * (caller->rx_rate + caller->tx_rate)),
	      "LAPM response timer did not provide three elapsed seconds");
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
	caller->rx_rate = caller->tx_rate = 300;
	caller->bit_timer_func(caller);
	CHECK(caller->bit_timer == 3300,
	      "LAPM response timer did not allow slow-frame serialization");
	caller->rx_rate = caller->tx_rate = 2400;
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
	CHECK(fields.parameter_negotiation_length == sizeof(expected_parameters) &&
		      memcmp(fields.parameter_negotiation, expected_parameters,
			      sizeof(expected_parameters)) == 0,
	      "LAPM XID response did not encode the V.42 mandatory mask");
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

static bool run_lapm_startup_pair(unsigned tx_rate, unsigned rx_rate, bool long_adp)
{
	enum { LAPM_DATA_STATE = 2 };
	struct modem_driver driver = {
		.name = "LAPM directional startup",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *peers[] = {
		modem_create(&driver, "caller"), modem_create(&driver, "answerer"),
	};
	struct dp pumps[] = {
		{ .id = DP_V90, .modem = peers[0] },
		{ .id = DP_V90, .modem = peers[1] },
	};
	unsigned phase[2] = { 0 };
	unsigned answer_pattern_bits = 0;
	bool connected = false;

	for (unsigned side = 0; side < 2; ++side) {
		struct modem *m = peers[side];

		CHECK(m != NULL, "directional LAPM allocation failed");
		m->dp = &pumps[side];
		m->caller = side == 0;
		m->tx_rate = side == 0 ? tx_rate : rx_rate;
		m->rx_rate = side == 0 ? rx_rate : tx_rate;
		m->cfg.ec = m->cfg.ec_detector = 1;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "directional LAPM initialization failed");
		modem_dp_connected(m);
	}
	/* Independent wire clocks expose a first XID that outruns the slower
	 * answerer's ADP. Do not let a later T401 retry conceal that lost frame. */
	for (unsigned tick = 0; tick < 8000 && !connected; ++tick) {
		for (unsigned side = 0; side < 2; ++side) {
			struct modem *m = peers[side];

			phase[side] += m->tx_rate;
			while (phase[side] >= 8000) {
				uint8_t bit;

				phase[side] -= 8000;
				/* Extend each native 18-bit character group with eight
				 * marks, exercising V.42's allowed 16-mark spacing. The
				 * answerer's detection timer is stopped during its reply. */
				bool extra_mark = side == 1 && long_adp &&
					m->get_bits == modem_detector_get_bits &&
					m->packer.detector.tx_count > 0;
				if (extra_mark)
					extra_mark = answer_pattern_bits++ % 26 < 8;
				if (extra_mark) {
					CHECK(m->bit_timer == 0, "ADP padding skipped an active timer");
					bit = 1;
				} else {
					CHECK(modem_get_bits(m, 1, &bit, 1) == 1,
					      "directional LAPM source failed");
				}
				CHECK(modem_put_bits(peers[1 - side], 1, &bit, 1) == 1,
				      "directional LAPM exchange failed");
				if (side == 0 && m->get_bits == modem_hdlc_get_bits &&
				    m->packer.hdlc.tx_frame != NULL)
					CHECK(peers[1]->put_bits == modem_hdlc_put_bits,
					      "first XID outran peer ADP: tx=%u rx=%u tick=%u",
					      tx_rate, rx_rate, tick);
			}
		}
		connected = peers[0]->ec.lapm.state == LAPM_DATA_STATE &&
			peers[1]->ec.lapm.state == LAPM_DATA_STATE;
	}
	CHECK(connected, "directional LAPM needed an establishment retry: tx=%u rx=%u",
	      tx_rate, rx_rate);
	for (unsigned side = 0; side < 2; ++side) {
		peers[side]->dp = NULL;
		modem_delete(peers[side]);
	}
	return true;
}

static bool test_lapm_directional_preamble(void)
{
	static const struct {
		unsigned tx, rx, flags;
	} cases[] = {
		{ 0, 0, 109 }, { 52000, 0, 109 }, { 0, 24000, 109 },
		{ 31200, 31200, 109 }, { 52000, 24000, 218 },
		{ 24000, 52000, 59 }, { 56000, 4800, 1101 },
		{ 4800, 56000, 24 }, { 56000, 48000, 125 },
		{ 53333, 26400, 204 },
		{ UINT_MAX, UINT_MAX, 109 }, { UINT_MAX, 1, UINT_MAX },
		{ 1, UINT_MAX, 17 },
	};
	struct modem_driver driver = {
		.name = "LAPM preamble bounds",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct modem *m = modem_create(&driver, "LAPM preamble bounds");

		CHECK(m != NULL, "LAPM preamble allocation failed");
		m->caller = 1;
		m->tx_rate = cases[index].tx;
		m->rx_rate = cases[index].rx;
		m->cfg.ec = 1;
		CHECK(modem_ec_init(m) == 0, "LAPM preamble initialization failed");
		modem_hdlc_start(m);
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		modem_ec_start(m);
		CHECK(m->packer.hdlc.tx_preamble_flags == cases[index].flags &&
		      m->packer.hdlc.tx_preamble_complete != NULL &&
		      m->packer.hdlc.get_tx_frame == NULL && m->bit_timer == 0,
		      "wrong LAPM preamble: tx=%u rx=%u flags=%u/%u",
		      cases[index].tx, cases[index].rx,
		      m->packer.hdlc.tx_preamble_flags, cases[index].flags);
		uint8_t bit;
		CHECK(modem_get_bits(m, 1, &bit, 1) == 1,
		      "LAPM preamble could not begin a flag");
		m->tx_rate = 52000;
		m->rx_rate = 24000;
		CHECK(modem_ec_resume_after_retrain(m) == 0 &&
		      m->packer.hdlc.tx_preamble_flags == 218 &&
		      m->packer.hdlc.tx_preamble_complete != NULL &&
		      m->packer.hdlc.get_tx_frame == NULL && m->bit_timer == 0,
		      "unfinished LAPM preamble ignored the retrained rates");
		modem_delete(m);
		if (cases[index].tx >= 1200 && cases[index].tx <= 56000 &&
		    cases[index].rx >= 1200 && cases[index].rx <= 56000)
			CHECK(run_lapm_startup_pair(cases[index].tx, cases[index].rx, false) &&
			      run_lapm_startup_pair(cases[index].tx, cases[index].rx, true),
			      "directional LAPM startup failed");
	}
	return true;
}

static bool test_lapm_peer_flag_preamble(void)
{
	struct modem_driver driver = {
		.name = "LAPM peer flags",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, "LAPM peer flags");
	uint8_t bit;

	CHECK(m != NULL, "LAPM peer-flag allocation failed");
	m->caller = 1;
	m->tx_rate = m->rx_rate = 1200;
	m->cfg.ec = 1;
	CHECK(modem_ec_init(m) == 0, "LAPM peer-flag initialization failed");
	modem_hdlc_start(m);
	m->get_bits = modem_hdlc_get_bits;
	m->put_bits = modem_hdlc_put_bits;
	modem_ec_start(m);
	CHECK(m->packer.hdlc.tx_preamble_flags == 109,
	      "wrong conservative LAPM preamble");
	/* The HDLC decoder holds 18 bits of lookahead. Five wire flags
	 * expose at least two complete, consecutive peer flags to it. */
	for (unsigned i = 0; i < 3 * 8; ++i) {
		bit = (0x7e >> (i % 8)) & 1;
		CHECK(modem_hdlc_put_bits(m, 1, &bit, 1) == 1,
		      "peer flag input failed");
	}
	CHECK(m->packer.hdlc.tx_preamble_flags == 109,
	      "one peer flag prematurely released the ADP wait");
	for (unsigned i = 3 * 8; i < 5 * 8; ++i) {
		bit = (0x7e >> (i % 8)) & 1;
		CHECK(modem_hdlc_put_bits(m, 1, &bit, 1) == 1,
		      "peer flag input failed");
	}
	CHECK(m->packer.hdlc.tx_preamble_flags == 16 &&
	      m->packer.hdlc.tx_preamble_complete != NULL,
	      "peer flags did not release the ADP wait");
	for (unsigned i = 0; i < 16 * 8; ++i)
		CHECK(modem_hdlc_get_bits(m, 1, &bit, 1) == 1,
		      "minimum LAPM flag preamble failed");
	CHECK(m->packer.hdlc.tx_preamble_flags == 0 &&
	      m->packer.hdlc.tx_preamble_complete == NULL &&
	      m->packer.hdlc.get_tx_frame != NULL,
	      "first XID did not follow 16 transmitted flags");
	modem_delete(m);
	return true;
}

static bool test_lapm_receive_frame_boundaries(void)
{
	struct modem_driver driver = {
		.name = "LAPM receive boundaries", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const uint8_t controls[] = { 0, 1, 5, 9, 0x73 };

	for (unsigned caller = 0; caller < 2; ++caller) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, 0, 254, 'X' };
		frame_t incoming = { .buf = wire };

		CHECK(m != NULL, "receive boundary allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "receive boundary role initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		for (unsigned state = 0; state < 4; ++state) {
			l->state = state;
			m->bit_timer = 1234;
			struct lapm_state before = *l;
			unsigned event = m->new_event;

			for (size_t control = 0; control < ARRAY_LENGTH(controls); ++control)
			for (unsigned length = 0; length < 3; ++length) {
				wire[0] = l->cmd_addr;
				wire[1] = controls[control];
				incoming.count = length;
				if (length == 2 && wire[1] == 0x73)
					continue; /* Complete U header. */
				m->packer.hdlc.rx_complete(l, &incoming);
				CHECK(memcmp(l, &before, sizeof(before)) == 0 &&
				      m->bit_timer == 1234 && m->new_event == event,
				      "truncated frame changed link state: state=%u control=%u length=%u",
				      state, wire[1], length);
			}
			incoming.count = 3;
			for (size_t control = 0; control < ARRAY_LENGTH(controls); ++control) {
				wire[0] = 0x05; /* Unimplemented DLCI. */
				wire[1] = controls[control];
				m->packer.hdlc.rx_complete(l, &incoming);
				CHECK(memcmp(l, &before, sizeof(before)) == 0 && m->bit_timer == 1234,
				      "unimplemented address changed link state");
				if ((state == 0 || state == 3) && wire[1] != 0x73) {
					wire[0] = l->rsp_addr;
					m->packer.hdlc.rx_complete(l, &incoming);
					CHECK(memcmp(l, &before, sizeof(before)) == 0 &&
					      m->bit_timer == 1234 && m->new_event == event,
					      "I/S traffic established an idle or releasing link");
				}
			}
		}
		/* Pending XID and rejected payloads cannot supply a missing UA. */
		l->state = 1;
		l->config = 1;
		wire[0] = l->rsp_addr;
		wire[1] = wire[2] = 0;
		incoming.count = sizeof(wire);
		struct lapm_state before = *l;
		unsigned event = m->new_event;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(memcmp(l, &before, sizeof(before)) == 0 &&
		      m->bit_timer == 1234 && m->new_event == event,
		      "early information bypassed XID negotiation");
		l->config = 0;
		l->rx_info_size = 0;
		before = *l;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(memcmp(l, &before, sizeof(before)) == 0 &&
		      m->bit_timer == 1234 && m->new_event == event,
		      "rejected information completed establishment");
		l->rx_info_size = LAPM_MAX_INFO_SIZE;
		l->state = 1;
		incoming.count = 2;
		wire[0] = l->cmd_addr;
		wire[1] = 0x63; /* UA F=0 cannot acknowledge SABME P=1. */
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 1 && m->bit_timer == 1234,
		      "unsolicited UA completed establishment");
		wire[1] = 0x73;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->bit_timer > 1234,
		      "solicited UA did not complete establishment");
		for (size_t control = 0; control < ARRAY_LENGTH(controls) - 1; ++control) {
			l->state = 1;
			l->vs = l->va = l->vr = 17;
			l->rtx_count = 2;
			m->bit_timer = 1234;
			m->put_chars = scripted_lapm_sink;
			memset(&lapm_sink, 0, sizeof(lapm_sink));
			lapm_sink.result = 1;
			wire[0] = l->rsp_addr;
			wire[1] = controls[control];
			wire[2] = 0;
			incoming.count = control == 0 ? sizeof(wire) : 3;
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->state == 2 && l->vs == 0 && l->va == 0 &&
			      l->vr == (control == 0) && !l->rtx_count &&
			      l->peer_busy == (wire[1] == 5) && m->bit_timer > 1234,
			      "valid I/S frame did not recover a lost UA: control=%u", wire[1]);
			CHECK(lapm_sink.length == (control == 0) &&
			      (control != 0 || lapm_sink.data[0] == 'X'),
			      "lost-UA recovery dropped or repeated information");
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_receive_information_limit(void)
{
	struct modem_driver driver = {
		.name = "LAPM information limit", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const unsigned limits[] = { 8, LAPM_MAX_INFO_SIZE, UINT_MAX };

	for (unsigned caller = 0; caller < 2; ++caller)
	for (size_t limit = 0; limit < ARRAY_LENGTH(limits); ++limit)
	for (unsigned oversized = 0; oversized < 2; ++oversized) {
		struct modem *m = modem_create(&driver, driver.name);
		unsigned capacity = limits[limit] < LAPM_MAX_INFO_SIZE ?
			limits[limit] : LAPM_MAX_INFO_SIZE;
		uint8_t wire[LAPM_MAX_INFO_SIZE + 4];
		frame_t incoming = { .buf = wire, .count = capacity + oversized + 3 };

		CHECK(m != NULL, "information limit allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "information limit initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		l->state = 2;
		m->state = 5;
		l->rx_info_size = limits[limit];
		memset(l->rx_buf, 0x55, sizeof(l->rx_buf));
		uint8_t saved_buffer[sizeof(l->rx_buf)];
		memcpy(saved_buffer, l->rx_buf, sizeof(saved_buffer));
		frame_t saved_frame = l->info_buf[0].frame;
		memset(wire, 'X', sizeof(wire));
		wire[0] = l->rsp_addr;
		wire[1] = wire[2] = 0;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		m->put_chars = scripted_lapm_sink; /* Host blocks all payload. */
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(memcmp(&saved_frame, &l->info_buf[0].frame, sizeof(saved_frame)) == 0,
		      "receive payload overwrote information queue metadata");
		if (oversized) {
			CHECK(l->state == 0 && l->vr == 0 && l->rx_count == 0 && !l->busy &&
			      lapm_sink.calls == 0 && m->packer_process == NULL &&
			      memcmp(l->rx_buf, saved_buffer, sizeof(saved_buffer)) == 0 &&
			      l->tx_ctrl == l->ctrl_list && m->bit_timer == 0 &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "oversized information was acknowledged, decoded, or retained");
		} else {
			CHECK(l->state == 2 && l->vr == 1 && l->rx_count == (int)capacity && l->busy &&
			      lapm_sink.calls == 1 && m->packer_process != NULL &&
			      memcmp(l->rx_buf, wire + 3, capacity) == 0,
			      "legal maximum payload was not retained under backpressure");
			lapm_sink.result = capacity;
			m->packer_process(m, 0);
			CHECK(!l->busy && !l->rx_count && lapm_sink.length == capacity &&
			      memcmp(lapm_sink.data, wire + 3, capacity) == 0,
			      "legal maximum payload did not drain exactly once");
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_peer_ready_dm(void)
{
	struct modem_driver driver = {
		.name = "LAPM peer-ready DM", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	uint8_t wire[] = { 0, 0x0f, 0 };
	frame_t incoming = { .buf = wire, .count = 2 };
	frame_t *outgoing;

	CHECK(m != NULL, "peer-ready allocation failed");
	m->cfg.ec = 1;
	m->cfg.comp = 0;
	m->tx_rate = m->rx_rate = 28800;
	CHECK(modem_ec_init(m) == 0, "peer-ready initialization failed");
	modem_hdlc_start(m);
	modem_ec_start(m);
	m->get_bits = modem_hdlc_get_bits;
	m->put_bits = modem_hdlc_put_bits;
	m->state = 4; /* Host is waiting for error-control establishment. */
	struct lapm_state *l = &m->ec.lapm;
	wire[0] = l->cmd_addr;
	l->config = 1;
	m->bit_timer = 1234;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 0 && l->config && m->state == 4 && m->bit_timer == 1234 &&
	      l->tx_ctrl == l->ctrl_list, "peer-ready DM bypassed pending XID");
	l->config = 0;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 1 && m->state == 4 && m->bit_timer > 1234,
	      "peer-ready DM reported CONNECT without establishing LAPM");
	outgoing = m->packer.hdlc.get_tx_frame(l);
	CHECK(outgoing != NULL && outgoing->buf[0] == l->cmd_addr &&
	      outgoing->buf[1] == 0x7f && outgoing->size == 2,
	      "peer-ready DM did not send SABME P=1");
	m->packer.hdlc.tx_complete(l, outgoing);
	int timer = m->bit_timer;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 1 && m->state == 4 && m->bit_timer == timer,
	      "colliding DM F=0 changed establishment");
	wire[1] = 0x73; /* UA F=1 completes the SABME exchange. */
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 2 && m->state == 5, "SABME acknowledgement did not connect");
	CHECK(modem_write(m, "A", 1) == 1, "established LAPM rejected terminal data");
	outgoing = m->packer.hdlc.get_tx_frame(l);
	CHECK(outgoing != NULL && outgoing->size == 4 && outgoing->buf[1] == 0 &&
	      outgoing->buf[3] == 'A' && m->xmit.count == 0,
	      "peer-ready link did not transmit the queued keystroke");
	m->packer.hdlc.tx_complete(l, outgoing);
	wire[1] = 1;
	wire[2] = 2; /* RR N(R)=1 acknowledges the information frame. */
	incoming.count = 3;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->va == l->vs && m->packer.hdlc.get_tx_frame(l) == NULL,
	      "peer-ready exchange repeated acknowledged terminal data");
	modem_delete(m);

	/* An originator must finish its wire preamble and XID first. */
	m = modem_create(&driver, driver.name);
	CHECK(m != NULL, "originator peer-ready allocation failed");
	m->caller = 1;
	m->cfg.comp = 0;
	CHECK(modem_ec_init(m) == 0, "originator peer-ready initialization failed");
	modem_hdlc_start(m);
	modem_ec_start(m);
	l = &m->ec.lapm;
	struct lapm_state before = *l;
	wire[0] = l->cmd_addr;
	wire[1] = 0x0f;
	incoming.count = 2;
	m->state = 4;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(memcmp(l, &before, sizeof(before)) == 0 && m->state == 4 &&
	      m->packer.hdlc.tx_preamble_complete != NULL && m->bit_timer == 0,
	      "peer-ready DM bypassed the originator preamble");
	modem_delete(m);
	static const struct { unsigned state, ec; } unwilling[] = {
		{ 0, 1 }, { 7, 1 }, { 8, 1 }, { 4, 0 },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(unwilling); ++i) {
		m = modem_create(&driver, driver.name);
		CHECK(m != NULL, "closing peer-ready allocation failed");
		m->cfg.ec = unwilling[i].ec;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "closing peer-ready initialization failed");
		modem_ec_start(m);
		l = &m->ec.lapm;
		wire[0] = l->cmd_addr;
		m->state = unwilling[i].state;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 0 && l->tx_ctrl == l->ctrl_list && m->bit_timer == 0 &&
		      m->state == unwilling[i].state, "peer-ready DM reopened an unwilling host");
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_unnumbered_responses(void)
{
	struct modem_driver driver = {
		.name = "LAPM link responses", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const struct {
		unsigned state, control, poll, next;
	} cases[] = {
		{ 0, 0x1f, 0, 0 }, /* Solicited DM in disconnected state is ignored. */
		{ 1, 0x0f, 0, 1 }, { 1, 0x1f, 0, 0 },
		{ 3, 0x0f, 0, 3 }, { 3, 0x1f, 0, 0 },
		{ 2, 0x0f, 0, 0 }, { 2, 0x1f, 0, 2 }, { 2, 0x1f, 1, 0 },
		{ 3, 0x63, 0, 3 }, { 3, 0x73, 0, 0 },
		{ 0, 0x87, 0, 0 }, { 1, 0x87, 0, 1 },
		{ 2, 0x87, 0, 0 }, { 3, 0x87, 0, 3 },
		{ 0, 0x97, 0, 0 }, { 1, 0x97, 0, 1 },
		{ 2, 0x97, 0, 0 }, { 3, 0x97, 0, 3 },
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[7] = { 0 };
		bool frmr = (cases[i].control & 0xef) == 0x87;
		frame_t incoming = { .buf = wire, .count = frmr ? 7 : 2 };

		CHECK(m != NULL, "link response allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		m->tx_rate = m->rx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "link response initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		l->state = cases[i].state;
		l->rtx_count = cases[i].poll;
		l->vr = 7;
		m->bit_timer = 1234;
		m->state = 5;
		wire[0] = l->cmd_addr;
		wire[1] = cases[i].control;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == (int)cases[i].next,
		      "wrong response transition: role=%u state=%u control=%02x poll=%u next=%d",
		      caller, cases[i].state, cases[i].control, cases[i].poll, l->state);
		if (cases[i].state == cases[i].next) {
			CHECK(l->vr == 7 && l->rtx_count == (int)cases[i].poll && m->bit_timer == 1234,
			      "ignored response changed the deadline or sequence");
		} else {
			CHECK(l->vr == 0 && l->rtx_count == 0 && m->bit_timer == 0 &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "released link retained its sequence or recovery timer");
		}
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_disconnected_disc(void)
{
	struct modem_driver driver = {
		.name = "LAPM disconnected DISC", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned poll = 0; poll < 2; ++poll) {
		struct modem *m = modem_create(&driver, driver.name);
		CHECK(m != NULL, "disconnected DISC allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "disconnected DISC initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		uint8_t wire[] = { l->rsp_addr, 0x43 | poll << 4 };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 0 && l->tx_ctrl != l->ctrl_list &&
		      l->tx_ctrl->size == 2 && l->tx_ctrl->buf[0] == l->rsp_addr &&
		      l->tx_ctrl->buf[1] == (0x0f | poll << 4),
		      "disconnected DISC response did not echo P in F: role=%u P=%u", caller, poll);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_mode_command_collisions(void)
{
	struct modem_driver driver = {
		.name = "LAPM mode collision", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned disconnect = 0; disconnect < 2; ++disconnect)
	for (unsigned identical = 0; identical < 2; ++identical)
	for (unsigned poll = 0; poll < 2; ++poll)
	for (unsigned pressure = 0; pressure < 2; ++pressure) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[LAPM_MAX_INFO_SIZE + 2] = { 0 };
		frame_t incoming = { .buf = wire, .count = 2 };
		frame_t *outgoing;

		CHECK(m != NULL, "mode collision allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "mode collision initialization failed");
		modem_ec_start(m);
		m->state = 4; /* Host requests error-control establishment. */
		struct lapm_state *l = &m->ec.lapm;
		if (caller) {
			void (*complete)(struct modem *) = m->packer.hdlc.tx_preamble_complete;
			m->packer.hdlc.tx_preamble_complete = NULL;
			m->packer.hdlc.tx_preamble_flags = 0;
			complete(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size <= (int)sizeof(wire),
			      "collision originator did not send XID");
			incoming.count = outgoing->size;
			memcpy(wire, outgoing->buf, (size_t)incoming.count);
			wire[0] = l->cmd_addr;
			m->packer.hdlc.tx_complete(l, outgoing);
		} else {
			wire[0] = l->cmd_addr;
			wire[1] = 0x0f; /* Peer-ready DM starts the local SABME. */
		}
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(l->state == 1 && outgoing != NULL && outgoing->buf[1] == 0x7f,
		      "collision fixture did not start a local SABME transaction");
		m->packer.hdlc.tx_complete(l, outgoing);
		incoming.count = 2;
		if (disconnect) {
			wire[0] = l->cmd_addr;
			wire[1] = 0x73;
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->state == 2 && m->state == 5, "collision fixture did not connect");
			m->state = 7; /* Host requests error-control release. */
			modem_ec_stop(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(l->state == 3 && outgoing != NULL && outgoing->buf[1] == 0x53,
			      "collision fixture did not start a local DISC transaction");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		int pending = l->state;
		unsigned host = m->state;
		void (*timeout)(struct modem *) = m->bit_timer_func;
		m->bit_timer = 1234; /* Preserve the remaining transaction deadline. */
		l->rtx_count = 2;
		wire[0] = l->rsp_addr;
		wire[1] = ((disconnect != identical) ? 0x6f : 0x43) | poll << 4;
		for (unsigned repeat = 0; repeat < (pressure ? LAPM_CTRL_FRAMES : 2); ++repeat) {
			m->packer.hdlc.rx_complete(l, &incoming);
			if (pressure && repeat == LAPM_CTRL_FRAMES - 1)
				break;
			CHECK(l->state == pending && m->state == host && l->rtx_count == 2 &&
			      m->bit_timer == 1234 && m->bit_timer_func == timeout,
			      "collision replaced pending transaction: role=%u local=%d identical=%u P=%u",
			      caller, pending, identical, poll);
			if (pressure)
				continue;
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 2 &&
			      outgoing->buf[0] == l->rsp_addr &&
			      outgoing->buf[1] == ((identical ? 0x63 : 0x0f) | poll << 4),
			      "collision response did not use UA/DM with F=P");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		if (pressure) {
			CHECK(l->state == 0 && m->bit_timer == 0 &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "unreplyable collision retained a pending link");
			modem_delete(m);
			continue;
		}
		wire[0] = l->cmd_addr;
		wire[1] = identical ? 0x63 : 0x0f;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == pending && m->state == host && m->bit_timer == 1234,
		      "F=0 response completed a local P=1 transaction");
		timeout(m);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(l->state == pending && l->rtx_count == 3 && outgoing != NULL &&
		      outgoing->buf[1] == (disconnect ? 0x53 : 0x7f),
		      "collision timeout retransmitted a different mode command");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[1] |= 0x10;
		m->packer.hdlc.rx_complete(l, &incoming);
		if (identical && !disconnect) {
			CHECK(l->state == 2 && m->state == 5 && !l->rtx_count && m->bit_timer > 1234,
			      "matching SABME collision did not complete on UA F=1");
			CHECK(modem_write(m, "A", 1) == 1, "connected collision rejected terminal data");
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4 && outgoing->buf[3] == 'A',
			      "connected collision did not transmit terminal data");
			m->packer.hdlc.tx_complete(l, outgoing);
		} else {
			CHECK(l->state == 0 && !m->bit_timer &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "collision resolution did not release the physical link");
		}
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_numbered_frame_rejection(void)
{
	struct modem_driver driver = {
		.name = "LAPM numbered rejection", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const struct {
		unsigned control, length;
		bool wrong_nr, response_only;
	} cases[] = {
		{ 0x0d, 3, false, false }, { 0x11, 3, false, false },
		{ 0xfd, 3, false, false },
		{ 1, 4, false, false }, { 5, 4, false, false }, { 9, 4, false, false },
		{ 0, 4, true, false }, { 1, 3, true, false },
		{ 5, 3, true, false }, { 9, 3, true, false },
		{ 0, 4, false, true }, /* I frames are commands. */
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned state = 1; state <= 2; ++state)
	for (unsigned response = 0; response < 2; ++response)
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		if (cases[i].response_only && !response)
			continue;
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, cases[i].control, 0, 'X' };
		frame_t incoming = { .buf = wire, .count = cases[i].length };

		CHECK(m != NULL, "numbered rejection allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "numbered rejection initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		l->state = state;
		l->va = l->vs = state == 2 ? 7 : 0;
		l->vr = 7;
		l->busy = l->peer_busy = 1;
		l->rx_count = 1;
		l->rx_buf[0] = 'R';
		m->state = state == 1 ? 4 : 5;
		m->bit_timer = 1234;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		wire[0] = response ? l->cmd_addr : l->rsp_addr;
		wire[2] = (uint8_t)((l->va + cases[i].wrong_nr) << 1);
		struct lapm_state before = *l;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(lapm_sink.calls == 0, "rejected numbered frame delivered terminal data");
		if (state == 1) {
			CHECK(memcmp(l, &before, sizeof(before)) == 0 && m->state == 4 &&
			      m->bit_timer == 1234,
			      "rejected frame completed establishment: role=%u response=%u control=%02x length=%u nr=%u",
			      caller, response, wire[1], cases[i].length, wire[2] >> 1);
		} else {
			CHECK(l->state == 0 && !l->rx_count && !l->busy && !l->peer_busy &&
			      !m->bit_timer && l->tx_ctrl == l->ctrl_list &&
			      l->rx_buf[0] == 'R' && strcmp(modem_call_phase(m), "cleardown") == 0,
			      "rejected frame retained the physical link: role=%u response=%u control=%02x length=%u nr=%u",
			      caller, response, wire[1], cases[i].length, wire[2] >> 1);
		}
		modem_delete(m);
	}
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_unnumbered_frame_rejection(void)
{
	struct modem_driver driver = {
		.name = "LAPM unnumbered rejection", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	/* V.42 Table 8 and 8.2.4: fixed headers have no information field,
	 * except FRMR, whose information field contains exactly five octets. */
	static const struct {
		unsigned control, length;
		bool command;
	} fixed[] = {
		{ 0x73, 2, false }, { 0x63, 2, false },
		{ 0x7f, 2, true }, { 0x6f, 2, true },
		{ 0x53, 2, true }, { 0x43, 2, true },
		{ 0x1f, 2, false }, { 0x0f, 2, false },
		{ 0x87, 7, false }, { 0x97, 7, false },
	};
	/* UI/XID require P/F=0; TEST is a command with P=0. Other control
	 * encodings are undefined, regardless of their direction or length. */
	static const unsigned invalid[] = { 0x13, 0xbf, 0xf3, 0xff, 0x23 };

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned state = 0; state < 4; ++state)
	for (unsigned response = 0; response < 2; ++response)
	for (size_t i = 0; i < ARRAY_LENGTH(fixed) + ARRAY_LENGTH(invalid) + 1; ++i)
	for (unsigned length = 2; length <= 8; ++length) {
		unsigned control = i < ARRAY_LENGTH(fixed) ? fixed[i].control :
			i < ARRAY_LENGTH(fixed) + ARRAY_LENGTH(invalid) ?
			invalid[i - ARRAY_LENGTH(fixed)] : 0xe3;
		if (i < ARRAY_LENGTH(fixed) && fixed[i].length == length &&
		    fixed[i].command == !response)
			continue;
		if (control == 0xe3 && !response)
			continue; /* Optional TEST has a variable information field. */
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[8] = { 0, control, 0x82 };
		frame_t incoming = { .buf = wire, .count = length };

		CHECK(m != NULL, "unnumbered rejection allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "unnumbered rejection initialization failed");
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		l->state = state;
		l->va = l->vs = l->vr = 7;
		l->busy = l->peer_busy = 1;
		l->rx_count = 1;
		l->rx_buf[0] = 'R';
		m->state = state == 2 ? 5 : 4;
		m->bit_timer = 1234;
		wire[0] = response ? l->cmd_addr : l->rsp_addr;
		struct lapm_state before = *l;
		struct modem_config configuration = m->cfg;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(memcmp(&configuration, &m->cfg, sizeof(configuration)) == 0,
		      "rejected U frame changed negotiated configuration");
		if (state != 2) {
			CHECK(memcmp(l, &before, sizeof(before)) == 0 && m->state == 4 &&
			      m->bit_timer == 1234,
			      "rejected U frame changed pending link: role=%u state=%u response=%u control=%02x length=%u",
			      caller, state, response, control, length);
		} else {
			CHECK(l->state == 0 && !l->rx_count && !l->busy && !l->peer_busy &&
			      !m->bit_timer && l->tx_ctrl == l->ctrl_list &&
			      l->rx_buf[0] == 'R' && strcmp(modem_call_phase(m), "cleardown") == 0,
			      "rejected U frame retained link: role=%u response=%u control=%02x length=%u",
			      caller, response, control, length);
		}
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_acknowledgement_queue_consistency(void)
{
	struct modem_driver driver = {
		.name = "LAPM acknowledgement consistency", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	static const uint8_t payload = 'A';

	CHECK(m != NULL, "acknowledgement consistency allocation failed");
	m->cfg.ec = 1;
	m->cfg.comp = 0;
	CHECK(modem_ec_init(m) == 0, "acknowledgement consistency initialization failed");
	modem_ec_start(m);
	struct lapm_state *l = &m->ec.lapm;
	l->state = 2;
	l->vs = 1; /* Sequence claims an outstanding frame; its queue is empty. */
	m->state = 5;
	m->get_chars = scripted_lapm_source;
	memset(&lapm_source, 0, sizeof(lapm_source));
	lapm_source.data = &payload;
	lapm_source.length = 1;
	uint8_t wire[] = { l->rsp_addr, 1, 2 };
	frame_t incoming = { .buf = wire, .count = sizeof(wire) };
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 0 && !m->bit_timer && lapm_source.offset == 0 &&
	      strcmp(modem_call_phase(m), "cleardown") == 0,
	      "inconsistent acknowledgement resumed transmission or retained the link");
	modem_delete(m);
	memset(&lapm_source, 0, sizeof(lapm_source));
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

static bool test_lapm_acknowledgement_window(void)
{
	enum { LAPM_DATA_STATE = 2 };
	static const uint8_t starts[] = { 5, 126 };
	static const unsigned int distances[] = { 127, 0, 1, 3, 4 };
	/* I command, then RR/RNR/REJ commands and solicited responses. */
	static const uint8_t controls[] = { 0, 1, 5, 9, 1, 5, 9 };
	static const uint8_t payload[] = { 'a', 'b', 'c' };
	struct modem_driver driver = {
		.name = "LAPM acknowledgement window", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t start = 0; start < ARRAY_LENGTH(starts); ++start)
	for (unsigned int pending = 0; pending <= 3; pending += 3)
	for (size_t kind = 0; kind < ARRAY_LENGTH(controls); ++kind)
	for (size_t index = 0; index < ARRAY_LENGTH(distances); ++index) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_state *l;
		frame_t *outgoing;
		uint8_t retained[sizeof(m->ec.lapm.info_buf)];
		unsigned int distance = distances[index];
		uint8_t nr = (uint8_t)((starts[start] + distance) & 0x7f);
		uint8_t wire[3];
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };

		CHECK(m != NULL, "LAPM acknowledgement fixture allocation failed");
		m->cfg.ec = 1;
		m->cfg.ec_tx_win_size = 3;
		m->cfg.ec_tx_info_size = 1;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "LAPM acknowledgement initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		l = &m->ec.lapm;
		l->state = LAPM_DATA_STATE;
		m->state = 5;
		l->va = l->vs = starts[start];
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		m->get_chars = scripted_lapm_source;
		lapm_source.data = payload;
		lapm_source.length = pending;
		lapm_source.offset = 0;
		for (unsigned int frame = 0; frame < pending; ++frame) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4,
			      "LAPM acknowledgement fixture did not queue information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		/* Exercise the response to a real post-retrain status enquiry,
		 * including the clean-window case with peer flow control retained. */
		l->peer_busy = 1;
		CHECK(modem_ec_resume_after_retrain(m) == 0 && l->retrain_poll,
		      "LAPM acknowledgement fixture did not poll after retrain");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 1,
		      "LAPM acknowledgement fixture lost its poll");
		m->packer.hdlc.tx_complete(l, outgoing);
		memcpy(retained, l->info_buf, sizeof(retained));
		wire[0] = kind < 4 ? l->rsp_addr : l->cmd_addr;
		wire[1] = controls[kind];
		wire[2] = (uint8_t)((nr << 1) | 1u);
		m->packer.hdlc.rx_complete(l, &incoming);
		if (distance <= pending) {
			CHECK(l->state == LAPM_DATA_STATE && l->va == nr,
			      "valid acknowledgement rejected (base=%u pending=%u kind=%zu nr=%u)",
			      starts[start], pending, kind, nr);
			if (kind == 1 || kind == 3) {
				/* An RR/REJ command clears peer busy, but it is not the
				 * final response to our outstanding recovery enquiry. */
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->buf[1] == 1,
				      "recovery command did not get its supervisory reply");
				m->packer.hdlc.tx_complete(l, outgoing);
				lapm_source.offset = 0;
				lapm_source.length = 1;
				CHECK(l->rtx_count && l->retrain_poll && !l->peer_busy &&
				      m->packer.hdlc.get_tx_frame(l) == NULL,
				      "RR/REJ command released information before the final reply");
				wire[0] = l->cmd_addr;
				wire[1] = 1;
				m->packer.hdlc.rx_complete(l, &incoming);
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(!l->rtx_count && !l->retrain_poll && outgoing != NULL &&
				      (outgoing->buf[1] & 1u) == 0,
				      "final recovery reply did not release information");
				m->packer.hdlc.tx_complete(l, outgoing);
			}
		} else {
			CHECK(l->state == 0 && l->va == 0 && l->vs == 0 &&
			      l->sent_info == l->info_list && l->sent_count == 0 &&
			      m->bit_timer == 0 && l->rtx_count == 0 &&
			      memcmp(retained, l->info_buf, sizeof(retained)) == 0 &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "invalid acknowledgement changed payload or retained the physical link "
			      "(base=%u pending=%u kind=%zu nr=%u)", starts[start], pending, kind, nr);
			CHECK(m->packer.hdlc.get_tx_frame(l) == NULL,
			      "invalid acknowledgement queued traffic after release");
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	return true;
}

static bool test_lapm_repeated_sequence_error_poll(void)
{
	enum { LAPM_DATA_STATE = 2, RR = 1, RNR = 5, REJ = 9 };
	static const unsigned int receive_starts[] = { 0, 5, 127 };
	static const uint8_t payload[] = { 'A', 'B' };
	struct modem_driver driver = {
		.name = "LAPM repeated sequence error", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t start = 0; start < ARRAY_LENGTH(receive_starts); ++start)
	for (unsigned int recovery = 0; recovery < 2; ++recovery)
	for (unsigned int busy = 0; busy < 2; ++busy) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_state *l;
		frame_t *retained, *outgoing;
		unsigned int vr = receive_starts[start];
		uint8_t wire[] = { 0, (uint8_t)(((vr + 127) & 127) << 1), 254, 'X' };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		int timer, retries;

		CHECK(m != NULL, "LAPM repeated-poll allocation failed");
		m->cfg.ec = 1;
		m->cfg.ec_tx_win_size = 3;
		m->cfg.ec_tx_info_size = 1;
		m->rx_rate = m->tx_rate = 26400;
		CHECK(modem_ec_init(m) == 0, "LAPM repeated-poll initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		l = &m->ec.lapm;
		l->state = LAPM_DATA_STATE;
		l->vr = vr;
		l->vs = l->va = 126;
		m->get_chars = scripted_lapm_source;
		lapm_source.data = payload;
		lapm_source.length = sizeof(payload);
		lapm_source.offset = 0;
		retained = l->sent_info->next;
		for (unsigned int i = 0; i < sizeof(payload); ++i) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4,
			      "LAPM repeated-poll fixture did not queue information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		m->get_chars = NULL;
		if (recovery) {
			m->bit_timer_func(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[1] == RR &&
			      (outgoing->buf[2] & 1) && l->rtx_count == 1,
			      "LAPM fixture did not enter timer recovery");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		/* An old I frame acknowledges the first of our two pending frames.
		 * Reject its payload, but still process N(R) across the wrap. */
		wire[0] = l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(l->reject && l->vr == vr && l->va == 127 && l->vs == 0 &&
		      l->sent_info == retained && l->sent_count == 1 &&
		      outgoing != NULL && outgoing->buf[1] == REJ && outgoing->buf[2] == 2 * vr,
		      "first LAPM sequence error did not preserve information and send REJ");
		m->packer.hdlc.tx_complete(l, outgoing);
		l->busy = busy;
		timer = m->bit_timer;
		retries = l->rtx_count;
		for (unsigned int repeat = 0; repeat < 4; ++repeat) {
			unsigned int poll = repeat & 1;
			wire[2] = 254 | poll;
			m->packer.hdlc.rx_complete(l, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(l->reject && l->vr == vr && l->va == 127 && l->vs == 0 &&
			      l->sent_info == retained && l->sent_count == 1 &&
			      m->bit_timer == timer && l->rtx_count == retries,
			      "duplicate LAPM information changed retained recovery state");
			if (poll) {
				CHECK(outgoing != NULL && outgoing->buf[0] == l->rsp_addr &&
				      outgoing->buf[1] == (busy ? RNR : RR) && outgoing->buf[2] == 2 * vr + 1,
				      "duplicate I poll has no final reply (vr=%u recovery=%u busy=%u)",
				      vr, recovery, busy);
				m->packer.hdlc.tx_complete(l, outgoing);
			} else {
				CHECK(outgoing == NULL, "unpolled duplicate repeated REJ or sent information");
			}
		}
		/* The expected I frame, not another duplicate, resolves the REJ
		 * exception and delivers its payload exactly once. */
		l->busy = 0;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		wire[1] = (uint8_t)(vr << 1);
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(!l->reject && l->vr == ((vr + 1) & 127) &&
		      lapm_sink.length == 1 && lapm_sink.data[0] == 'X' &&
		      outgoing != NULL && outgoing->buf[1] == RR &&
		      outgoing->buf[2] == (((vr + 1) & 127) * 2 + 1) &&
		      m->bit_timer == timer && l->rtx_count == retries,
		      "expected LAPM information did not resolve REJ while preserving our enquiry");
		m->packer.hdlc.tx_complete(l, outgoing);
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_idle_supervision(void)
{
	struct modem_driver driver = {
		.name = "LAPM idle supervision", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller) {
		struct modem *m = modem_create(&driver, driver.name);
		frame_t *outgoing;
		uint8_t wire[] = { 0, 0x7f, 0 }; /* SABME P=1 */
		frame_t incoming = { .buf = wire, .count = 2 };
		uint8_t bits[16];
		void (*idle_timeout)(struct modem *);
		int remaining;

		CHECK(m != NULL, "idle LAPM allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = 48000;
		m->tx_rate = 26400;
		CHECK(modem_ec_init(m) == 0, "idle LAPM initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		struct lapm_state *l = &m->ec.lapm;
		if (m->packer.hdlc.tx_preamble_complete != NULL) {
			uint8_t response[LAPM_MAX_INFO_SIZE + 2];
			frame_t xid = { .buf = response };
			void (*complete)(struct modem *) = m->packer.hdlc.tx_preamble_complete;

			m->packer.hdlc.tx_preamble_complete = NULL;
			m->packer.hdlc.tx_preamble_flags = 0;
			complete(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size <= (int)sizeof(response),
			      "originator idle fixture did not send XID");
			xid.count = outgoing->size;
			memcpy(response, outgoing->buf, (size_t)xid.count);
			response[0] = l->cmd_addr;
			m->packer.hdlc.tx_complete(l, outgoing);
			m->packer.hdlc.rx_complete(l, &xid);
			CHECK(!l->config, "originator idle fixture did not complete XID");
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
			      "originator idle fixture did not send SABME");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		wire[0] = l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x73,
		      "idle LAPM did not acknowledge SABME");
		m->packer.hdlc.tx_complete(l, outgoing);
		if (caller) {
			CHECK(l->state == 1, "colliding SABME completed before the local UA");
			wire[0] = l->cmd_addr;
			wire[1] = 0x73;
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		CHECK(l->state == 2 && m->bit_timer == 744000 && !l->rtx_count,
		      "established idle link did not start supervision");
		idle_timeout = m->bit_timer_func;
		m->bit_timer = 148800; /* Two seconds remain. */
		CHECK(modem_set_param(m, MDMPRM_TX_RATE, 4800) == 0 &&
		      modem_set_param(m, MDMPRM_RX_RATE, 7200) == 0 &&
		      m->bit_timer == 24000 && m->bit_timer_func == idle_timeout,
		      "rate publication changed the remaining idle interval");
		wire[0] = l->rsp_addr;
		wire[1] = 1; /* RR command P=0 */
		incoming.count = 3;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(m->bit_timer == 120000 && !l->rtx_count &&
		      !modem_ec_take_retrain_request(m),
		      "valid idle traffic did not refresh supervision");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[2] == 0,
		      "idle ready command did not receive its reply");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->bit_timer = 1;
		CHECK(modem_get_bits(m, 1, bits, (int)sizeof(bits)) == (int)sizeof(bits) &&
		      l->rtx_count == 1 && m->bit_timer == 36000 &&
		      m->bit_timer_func != idle_timeout && !modem_ec_take_retrain_request(m),
		      "idle expiry did not enter ordinary enquiry recovery");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[0] == l->cmd_addr &&
		      outgoing->buf[1] == 1 && outgoing->buf[2] == 1,
		      "idle supervision did not send RR P=1");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->bit_timer /= 2;
		remaining = m->bit_timer;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(m->bit_timer == remaining && l->rtx_count == 1,
		      "ordinary ready command postponed an unanswered idle poll");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL, "ready command lost its reply during recovery");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->bit_timer = 1;
		CHECK(modem_get_bits(m, 1, bits, (int)sizeof(bits)) == (int)sizeof(bits) &&
		      l->rtx_count == 2 && l->va == l->vs &&
		      modem_ec_take_retrain_request(m) && !modem_ec_take_retrain_request(m),
		      "unanswered idle polls did not request one physical recovery");
		CHECK(modem_ec_resume_after_retrain(m) == 0 && l->retrain_poll &&
		      l->peer_busy && l->rtx_count == 1 && l->stalled_retrain,
		      "idle retrain did not resynchronize its outstanding enquiry");
		wire[0] = l->cmd_addr;
		wire[2] = 1; /* RR response F=1 */
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(!l->rtx_count && !l->stalled_retrain && m->bit_timer == 120000 &&
		      m->bit_timer_func == idle_timeout,
		      "final ready response did not restore idle supervision");
		while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL)
			m->packer.hdlc.tx_complete(l, outgoing);
		l->busy = 1;
		m->bit_timer = 1;
		CHECK(modem_get_bits(m, 1, bits, (int)sizeof(bits)) == (int)sizeof(bits),
		      "busy idle timer did not advance");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 5 && outgoing->buf[2] == 1,
		      "own-receiver busy did not send RNR P=1");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->bit_timer_func(m);
		CHECK(!modem_ec_take_retrain_request(m),
		      "idle supervision treated receive backpressure as a channel fault");
		modem_ec_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_acknowledgement_progress(void)
{
	enum { DATA = 2, DISC = 3, STALL, ACK, PEER_BUSY, LOCAL_BUSY };
	static const uint8_t starts[] = { 0, 127 };
	static const uint8_t payload[] = { 'X', 'Y' };
	struct modem_driver driver = {
		.name = "LAPM progress", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t start = 0; start < ARRAY_LENGTH(starts); ++start)
	for (unsigned scenario = STALL; scenario <= LOCAL_BUSY; ++scenario) {
		struct modem *m = modem_create(&driver, driver.name);
		frame_t *outgoing;
		unsigned base = starts[start];
		uint8_t wire[] = { 0, 0x7f, 0 }; /* SABME P=1 */
		frame_t incoming = { .buf = wire, .count = 2 };

		CHECK(m != NULL, "progress fixture allocation failed");
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->cfg.ec_tx_info_size = m->cfg.ec_tx_win_size = 1;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "progress fixture initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x73, "progress fixture UA missing");
		m->packer.hdlc.tx_complete(l, outgoing);
		l->va = l->vs = base;
		memset(&lapm_source, 0, sizeof(lapm_source));
		lapm_source.data = payload;
		lapm_source.length = sizeof(payload);
		m->get_chars = scripted_lapm_source;
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->size == 4 && outgoing->buf[3] == 'X',
		      "progress fixture first information missing");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[0] = l->cmd_addr;
		wire[1] = 1; /* RR F=1, unchanged N(R) */
		wire[2] = (uint8_t)((base << 1) | 1);
		incoming.count = 3;
		for (unsigned expiry = 1; expiry <= 6; ++expiry) {
			m->bit_timer_func(m);
			if (expiry == 6) {
				CHECK(l->state == DISC && !modem_ec_take_retrain_request(m),
				      "nonadvancing final replies left an unbounded data stall");
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->buf[1] == 0x53,
				      "exhausted data progress did not send DISC");
				m->packer.hdlc.tx_complete(l, outgoing);
				break;
			}
			CHECK(l->state == DATA && l->stalled_polls == expiry &&
			      modem_ec_take_retrain_request(m) == (expiry == 2) &&
			      !modem_ec_take_retrain_request(m),
			      "nonadvancing enquiries did not request exactly one recovery");
			if (expiry == 2) {
				CHECK(modem_ec_resume_after_retrain(m) == 0 && l->stalled_polls == 2 &&
				      l->stalled_retrain, "physical recovery reset data-progress history");
			}
			while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL) {
				CHECK(outgoing->size == 3 && outgoing->buf[1] == 1 &&
				      outgoing->buf[2] == 1, "progress enquiry changed its wire format");
				m->packer.hdlc.tx_complete(l, outgoing);
			}
			m->packer.hdlc.rx_complete(l, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4 &&
			      outgoing->buf[1] == (uint8_t)(base << 1) && outgoing->buf[3] == 'X' &&
			      l->va == base && lapm_source.offset == 1,
			      "unchanged acknowledgement lost or consumed pending data");
			m->packer.hdlc.tx_complete(l, outgoing);
			if (expiry == 2 && scenario != STALL)
				break;
		}
		if (scenario == ACK) {
			wire[2] = (uint8_t)(((base + 1) & 127) << 1); /* RR F=0 advances V(A). */
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(!l->stalled_polls && !l->stalled_retrain && l->va == ((base + 1) & 127),
			      "real acknowledgement progress did not rearm recovery");
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[3] == 'Y' && lapm_source.offset == 2,
			      "acknowledgement progress did not preserve the next payload");
			m->packer.hdlc.tx_complete(l, outgoing);
			m->bit_timer_func(m);
			CHECK(l->stalled_polls == 1 && !modem_ec_take_retrain_request(m),
			      "new information inherited the previous stall budget");
		} else if (scenario == PEER_BUSY || scenario == LOCAL_BUSY) {
			if (scenario == PEER_BUSY) {
				wire[1] = 5; /* RNR F=0 */
				wire[2] &= ~1;
				m->packer.hdlc.rx_complete(l, &incoming);
			} else
				l->busy = 1;
			wire[2] |= 1;
			for (unsigned expiry = 0; expiry < 10; ++expiry) {
				m->bit_timer_func(m);
				CHECK(l->state == DATA && !l->stalled_polls &&
				      !modem_ec_take_retrain_request(m), "flow control consumed channel recovery");
				while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL)
					m->packer.hdlc.tx_complete(l, outgoing);
				m->packer.hdlc.rx_complete(l, &incoming);
				while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL)
					m->packer.hdlc.tx_complete(l, outgoing);
			}
			CHECK(lapm_source.offset == 1 && l->va == base,
			      "flow control discarded the pending payload");
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	return true;
}

static bool test_lapm_stalled_retrain(void)
{
	enum { ONLINE = 5, LAPM_DATA_STATE = 2, LAPM_DISC_STATE = 3 };
	struct modem_driver driver = {
		.name = "LAPM stalled retrain", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const uint8_t payload[] = { 'a', 'b' };
	int16_t receive[MODEM_FRAG] = { 0 }, transmit[MODEM_FRAG];

	for (int result = -1; result <= 0; ++result) {
		struct dp_operations operations = {
			.name = "LAPM stalled retrain", .create = create_retrain_dp,
			.delete = delete_retrain_dp, .process = process_retrain_dp,
			.retrain = request_retrain_dp,
		};
		memset(&retrain_dp, 0, sizeof(retrain_dp));
		retrain_dp.retrain_result = result;
		CHECK(modem_dp_register(DP_CALLPROG, &operations) == 0,
		      "stall datapump registration failed");
		struct modem *m = modem_create(&driver, driver.name);
		CHECK(m != NULL && modem_dial(m) == 0, "stall fixture did not start");
		m->state = ONLINE;
		m->caller = 0;
		m->cfg.ec = 1;
		m->cfg.ec_tx_win_size = 3;
		m->cfg.ec_tx_info_size = 1;
		m->rx_rate = m->tx_rate = 28800;
		CHECK(modem_ec_init(m) == 0, "stall LAPM initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		struct lapm_state *l = &m->ec.lapm;
		l->state = LAPM_DATA_STATE;
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		m->get_chars = scripted_lapm_source;
		lapm_source.data = payload;
		lapm_source.length = sizeof(payload);
		lapm_source.offset = 0;
		for (unsigned frame = 0; frame < 2; ++frame) {
			frame_t *outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4, "stall fixture lacks I frame");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		frame_t *retained = l->sent_info;
		/* No requests for flow control, establishment, raw mode, shutdown,
		 * an unsupported datapump, an inactive timer, or the first poll. */
		int timer = m->bit_timer;
		for (unsigned excluded = 0; excluded < 10; ++excluded) {
			l->rtx_count = excluded == 7 ? 1 : 2;
			l->busy = excluded == 0;
			l->peer_busy = excluded == 1;
			l->config = excluded == 2;
			l->state = excluded == 3 ? 1 : LAPM_DATA_STATE;
			m->cfg.ec = excluded != 4;
			m->state = excluded == 5 ? 7 : ONLINE;
			operations.retrain = excluded == 6 ? NULL : request_retrain_dp;
			m->get_bits = excluded == 8 ? modem_async_get_bits : modem_hdlc_get_bits;
			m->bit_timer = excluded == 9 ? 0 : timer;
			modem_process(m, receive, transmit, MODEM_FRAG);
			CHECK(!retrain_dp.retrain_calls && !l->stalled_retrain,
			      "excluded stall %u requested retrain", excluded);
		}
		m->bit_timer = timer;
		l->rtx_count = 0;
		l->va = 0;
		operations.process = process_stalled_lapm_dp;
		modem_process(m, receive, transmit, MODEM_FRAG);
		CHECK(l->rtx_count == 1 && !retrain_dp.retrain_calls, "retrained before two polls");
		modem_process(m, receive, transmit, MODEM_FRAG);
		CHECK(l->rtx_count == 2 && retrain_dp.retrain_calls == 1 && l->stalled_retrain &&
		      m->state == ONLINE && !m->command && !m->retrain_pending &&
		      l->sent_info == retained && l->sent_count == 2 && l->va == 0 && l->vs == 2,
		      "deferred stall retrain lost state or ran inside the datapump");
		operations.process = process_retrain_dp;
		for (unsigned frame = 0; frame < 20; ++frame)
			modem_process(m, receive, transmit, MODEM_FRAG);
		CHECK(retrain_dp.retrain_calls == 1, "stall retrain repeated on each media chunk");
		CHECK(modem_ec_resume_after_retrain(m) == 0 && l->stalled_retrain,
		      "physical retrain rearmed its own recovery allowance");
		uint8_t reply[] = { l->cmd_addr, 1, 5 }; /* RR F=1, N(R)=2 */
		frame_t incoming = { .buf = reply, .count = sizeof(reply) };
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(!l->stalled_retrain && !l->rtx_count && l->sent_count == 0 && l->va == 2,
		      "successful poll did not acknowledge and rearm recovery");
		lapm_source.offset = 0;
		/* Drain the retained poll, then place a new information window. */
		for (unsigned frame = 0; frame < LAPM_CTRL_FRAMES + 2 && l->vs == l->va; ++frame) {
			frame_t *outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL, "no frame after recovered poll");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		CHECK(l->vs != l->va, "recovered poll did not start the new information window");
		operations.process = process_stalled_lapm_dp;
		for (unsigned expiry = 0; expiry < 8 && l->state == LAPM_DATA_STATE; ++expiry)
			modem_process(m, receive, transmit, MODEM_FRAG);
		CHECK(retrain_dp.retrain_calls == 2 && l->state == LAPM_DISC_STATE,
		      "absent peer was not bounded by normal LAPM retry exhaustion");
		CHECK(modem_stop_now(m) == 0 && operations.use_count == 0, "stall fixture did not stop");
		modem_delete(m);
		CHECK(modem_dp_deregister(DP_CALLPROG, &operations) == 0, "stall deregistration failed");
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	return true;
}

static bool test_lapm_peer_busy_acknowledgement(void)
{
	enum { LAPM_DATA_STATE = 2, RR = 1, RNR = 5, REJ = 9 };
	static const uint8_t starts[] = { 0, 126 };
	static const uint8_t payload[] = { 'A', 'B', 'C' };
	struct modem_driver driver = {
		.name = "LAPM busy acknowledgement", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t start = 0; start < ARRAY_LENGTH(starts); ++start)
	for (unsigned int acknowledged = 1; acknowledged <= 2; ++acknowledged)
	for (unsigned int recovery = 0; recovery < 2; ++recovery)
	for (unsigned int sequenced = 0; sequenced < 2; ++sequenced) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_state *l;
		frame_t *outgoing;
		unsigned int base = starts[start];
		unsigned int nr = (base + acknowledged) & 127;
		uint8_t wire[] = { 0, RNR, (uint8_t)(base << 1), 'X' };
		frame_t incoming = { .buf = wire, .count = 3 };
		int timer;

		CHECK(m != NULL, "LAPM busy-acknowledgement allocation failed");
		m->cfg.ec = 1;
		m->cfg.ec_tx_info_size = 1;
		m->cfg.ec_tx_win_size = 3;
		m->rx_rate = m->tx_rate = 26400;
		CHECK(modem_ec_init(m) == 0, "LAPM busy-acknowledgement initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		l = &m->ec.lapm;
		l->state = LAPM_DATA_STATE;
		l->va = l->vs = base;
		m->get_chars = scripted_lapm_source;
		lapm_source.data = payload;
		lapm_source.length = 2;
		lapm_source.offset = 0;
		for (unsigned int i = 0; i < 2; ++i) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4,
			      "LAPM busy-acknowledgement fixture did not queue information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		/* RNR may close the peer's receive window while it continues to
		 * send us I frames acknowledging our previously transmitted data. */
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->peer_busy && !l->rtx_count && m->bit_timer > 0,
		      "RNR did not start the peer-busy enquiry timer");
		if (recovery) {
			m->bit_timer_func(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[1] == RR &&
			      (outgoing->buf[2] & 1), "peer-busy enquiry was not transmitted");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		m->bit_timer /= 2;
		timer = m->bit_timer;
		wire[0] = l->rsp_addr;
		wire[1] = sequenced ? 0 : 254;
		wire[2] = (uint8_t)((nr << 1) | 1);
		incoming.count = sizeof(wire);
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->va == nr && l->vs == ((base + 2) & 127) &&
		      l->vr == sequenced && l->peer_busy && l->sent_count == 2 - (int)acknowledged &&
		      m->bit_timer == timer && l->rtx_count == (int)recovery &&
		      lapm_sink.length == sequenced && (!sequenced || lapm_sink.data[0] == 'X'),
		      "I acknowledgement changed peer-busy enquiry (base=%u ack=%u recovery=%u sequenced=%u timer=%d/%d)",
		      base, acknowledged, recovery, sequenced, m->bit_timer, timer);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[0] == l->rsp_addr &&
		      outgoing->buf[1] == (sequenced ? RR : REJ) &&
		      outgoing->buf[2] == 2 * sequenced + 1,
		      "I poll did not receive its supervisory final reply");
		m->packer.hdlc.tx_complete(l, outgoing);
		lapm_source.length = sizeof(payload);
		CHECK(m->packer.hdlc.get_tx_frame(l) == NULL && lapm_source.offset == 2,
		      "I acknowledgement cleared peer busy or consumed queued terminal data");
		/* If the peer's eventual ready indication is lost, the preserved
		 * timer must still be able to ask and release the retained window. */
		m->bit_timer_func(m);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[0] == l->cmd_addr &&
		      outgoing->buf[1] == RR && outgoing->buf[2] == 2 * sequenced + 1 &&
		      l->rtx_count == (int)recovery + 1 && m->bit_timer > 0,
		      "acknowledged busy link lost its status enquiry");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[0] = l->cmd_addr;
		wire[1] = RR;
		incoming.count = 3;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(!l->peer_busy && !l->rtx_count && m->bit_timer > 0 &&
		      l->va == nr && l->vs == nr,
		      "RR final did not release the acknowledged busy link");
		for (unsigned int i = acknowledged; i < sizeof(payload); ++i) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4 &&
			      outgoing->buf[1] == (((base + i) & 127) << 1) &&
			      outgoing->buf[3] == payload[i],
			      "busy recovery did not resume retained and queued bytes in order");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		CHECK(lapm_source.offset == sizeof(payload), "busy recovery lost queued terminal data");
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_peer_busy_timer(void)
{
	enum { LAPM_DATA_STATE = 2, RR = 1, RNR = 5 };
	static const uint8_t payload[] = { 'A', 'B' };
	struct modem_driver driver = {
		.name = "LAPM busy timer", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned int pending = 0; pending <= 2; pending += 2)
	for (unsigned int acknowledged = 0; acknowledged <= pending; ++acknowledged)
	for (unsigned int command = 0; command < 2; ++command)
	for (unsigned int poll = 0; poll < 2; ++poll)
	for (unsigned int recovery = 0; recovery < 2; ++recovery) {
		struct modem *m;
		struct lapm_state *l;
		frame_t *outgoing;
		uint8_t wire[] = { 0, RNR, (uint8_t)((acknowledged << 1) | poll) };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		int remaining, expected;

		/* A final response is solicited only during timer recovery. */
		if ((!command && poll && !recovery) || (!pending && recovery))
			continue;
		m = modem_create(&driver, driver.name);
		CHECK(m != NULL, "LAPM busy-timer allocation failed");
		m->cfg.ec = 1;
		m->cfg.ec_tx_info_size = 1;
		m->cfg.ec_tx_win_size = 3;
		m->rx_rate = m->tx_rate = 26400;
		CHECK(modem_ec_init(m) == 0, "LAPM busy-timer initialization failed");
		modem_hdlc_start(m);
		modem_ec_start(m);
		l = &m->ec.lapm;
		l->state = LAPM_DATA_STATE;
		m->get_chars = scripted_lapm_source;
		lapm_source.data = payload;
		lapm_source.length = pending;
		lapm_source.offset = 0;
		for (unsigned int i = 0; i < pending; ++i) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4,
			      "LAPM busy-timer fixture did not transmit information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		if (recovery) {
			m->bit_timer_func(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[1] == RR &&
			      (outgoing->buf[2] & 1), "LAPM busy-timer fixture did not poll");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		m->bit_timer /= 2;
		remaining = m->bit_timer;
		expected = recovery && (command || !poll) ? remaining :
			3 * (int)(m->rx_rate + m->tx_rate);
		wire[0] = command ? l->rsp_addr : l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->peer_busy && l->va == acknowledged &&
		      l->vs == (recovery && !command && poll ? acknowledged : pending) &&
		      m->bit_timer == expected &&
		      l->rtx_count == (int)(recovery && (command || !poll)),
		      "RNR lost its enquiry timer (pending=%u ack=%u command=%u poll=%u recovery=%u timer=%d/%d)",
		      pending, acknowledged, command, poll, recovery, m->bit_timer, expected);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		if (command && poll) {
			CHECK(outgoing != NULL && outgoing->buf[0] == l->rsp_addr &&
			      outgoing->buf[1] == RR && outgoing->buf[2] == 1,
			      "RNR command poll did not receive RR final");
			m->packer.hdlc.tx_complete(l, outgoing);
		} else {
			CHECK(outgoing == NULL, "RNR unexpectedly released information");
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	return true;
}

static bool test_lapm_retrain_preserves_link_state(void)
{
	enum {
		TEST_STATE_MODEM_ONLINE = 5,
		LAPM_DATA_STATE = 2,
		FRAME_S_RR = 0x01,
	};
	static const uint8_t retained_payload[] = { 'P', 'I', 'N', 'G' };
	static const uint8_t queued_payload[] = { 'N', 'E', 'X', 'T' };
	struct modem_driver driver = {
		.name = "LAPM retrain resume test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "LAPM retrain resume test");
	struct lapm_state *lapm;
	uint8_t response[3];
	frame_t incoming = { .buf = response, .count = sizeof(response) };
	frame_t *outgoing;
	frame_t *retained_frame;
	frame_t *saved_next;
	struct lapm_v44_session retained_v44_session;
	struct v44_character_path *v44_path;
	uint8_t serialized_bit;

	CHECK(modem != NULL, "LAPM retrain modem could not be created");
	modem->caller = 0;
	modem->cfg.ec = 1;
	modem->rx_rate = modem->tx_rate = 26400;
	CHECK(modem_ec_init(modem) == 0,
	      "LAPM retrain modem could not initialize");
	modem_hdlc_start(modem);
	modem_ec_start(modem);
	lapm = &modem->ec.lapm;
	lapm->state = LAPM_DATA_STATE;
	lapm->peer_busy = 1;
	lapm->vs = lapm->va = 5;
	lapm->vr = 6;
	v44_path = lapm->v44_path;
	modem->state = TEST_STATE_MODEM_ONLINE;
	modem->retrain_pending = 1;
	modem->bit_timer = 0;
	modem->get_bits = modem_hdlc_get_bits;
	modem->put_bits = modem_hdlc_put_bits;
	saved_next = lapm->ctrl_list->next;
	lapm->ctrl_list->next = lapm->tx_ctrl;
	CHECK(modem_ec_resume_after_retrain(NULL) == -1 &&
		      modem_ec_resume_after_retrain(modem) == -1 &&
		      modem->bit_timer == 0 && lapm->rtx_count == 0 &&
		      lapm->peer_busy == 1 && lapm->vs == 5 && lapm->va == 5 &&
		      lapm->vr == 6,
	      "failed LAPM retrain poll changed the retained link");
	lapm->ctrl_list->next = saved_next;

	for (unsigned int local = 0; local < 2; ++local) {
		lapm->peer_busy = 1;
		lapm->vr = 6;
		modem->retrain_pending = local;
		modem->bit_timer = 0;
		modem_dp_reconnected(modem);
		CHECK(modem->retrain_pending == 0 &&
			      lapm->state == LAPM_DATA_STATE &&
			      lapm->peer_busy == 1 && lapm->vs == 5 &&
			      lapm->va == 5 && lapm->vr == 6 &&
			      lapm->v44_path == v44_path &&
			      lapm->retrain_poll == 1 &&
			      modem->bit_timer == 158400 && lapm->rtx_count == 1,
		      "LAPM %s retrain did not retain and poll peer flow control",
		      local ? "local" : "remote");
		modem_dp_connected(modem);
		CHECK(modem->bit_timer == 158400 && lapm->rtx_count == 1,
		      "duplicate LAPM %s reconnect restarted its status enquiry",
		      local ? "local" : "remote");
		outgoing = modem->packer.hdlc.get_tx_frame(
			modem->packer.hdlc.framer);
		CHECK(outgoing != NULL && outgoing->size == 3 &&
			      outgoing->buf[0] == lapm->cmd_addr &&
			      outgoing->buf[1] == FRAME_S_RR &&
			      outgoing->buf[2] ==
				      (uint8_t)((lapm->vr << 1) | 1),
		      "LAPM %s retrain did not emit an RR status enquiry",
		      local ? "local" : "remote");
		modem->packer.hdlc.tx_complete(
			modem->packer.hdlc.framer, outgoing);
		CHECK(modem->packer.hdlc.get_tx_frame(
			      modem->packer.hdlc.framer) == NULL,
		      "duplicate LAPM %s reconnect queued a second status enquiry",
		      local ? "local" : "remote");
		if (local == 0) {
			modem->bit_timer_func(modem);
			CHECK(modem->bit_timer == 158400 && lapm->rtx_count == 2,
			      "LAPM retrain did not retain its status retry");
			lapm->peer_busy = 0;
			modem->pack.bit = 3;
			modem->unpack.bit = 5;
			modem->packer.hdlc.tx_ones = 2;
			modem->packer.hdlc.rx_ones = 4;
			modem->packer.hdlc.rx_error = 0;
			modem->packer.hdlc.rx_frame->count = 2;
			modem->retrain_pending = 1;
			modem_dp_reconnected(modem);
			CHECK(modem->retrain_pending == 0 &&
				      lapm->state == LAPM_DATA_STATE &&
				      lapm->peer_busy == 1 && lapm->vs == 5 &&
				      lapm->va == 5 && lapm->vr == 6 &&
				      lapm->v44_path == v44_path &&
				      lapm->retrain_poll == 1 &&
				      modem->bit_timer == 158400 &&
				      lapm->rtx_count == 2 &&
				      modem->pack.bit == 0 &&
				      modem->unpack.bit == 0 &&
				      modem->packer.hdlc.tx_ones == 0 &&
				      modem->packer.hdlc.rx_ones == 0 &&
				      modem->packer.hdlc.rx_error == 1 &&
				      modem->packer.hdlc.rx_frame->count == 0,
			      "repeated LAPM retrain did not reframe the retained link");
			modem->pack.bit = 3;
			modem_dp_connected(modem);
			CHECK(modem->pack.bit == 3 && modem->bit_timer == 158400 &&
				      lapm->rtx_count == 2,
			      "duplicate LAPM reconnect reframed the active link");
			modem->pack.bit = 0;
			outgoing = modem->packer.hdlc.get_tx_frame(
				modem->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->size == 3 &&
				      outgoing->buf[0] == lapm->cmd_addr &&
				      outgoing->buf[1] == FRAME_S_RR &&
				      outgoing->buf[2] ==
					      (uint8_t)((lapm->vr << 1) | 1),
			      "repeated LAPM retrain lost its queued status retry");
			modem->packer.hdlc.tx_complete(
				modem->packer.hdlc.framer, outgoing);
			CHECK(modem->packer.hdlc.get_tx_frame(
				      modem->packer.hdlc.framer) == NULL,
			      "repeated LAPM retrain duplicated its status retry");
		}

		response[0] = local == 0 ? lapm->rsp_addr : lapm->cmd_addr;
		response[1] = local == 0 ? (uint8_t)(lapm->vr << 1) :
			FRAME_S_RR;
		response[2] = (uint8_t)(lapm->va << 1) |
			(local == 0 ? 0 : 1);
		modem->packer.hdlc.rx_complete(
			modem->packer.hdlc.framer, &incoming);
		CHECK(lapm->peer_busy == 0 && !lapm->retrain_poll &&
			      modem->bit_timer > 0 &&
			      lapm->rtx_count == 0 && lapm->vs == 5 &&
			      lapm->va == 5 && lapm->vr == 6 + (local == 0),
		      "LAPM %s retrain did not accept peer progress",
		      local ? "supervisory" : "information");
		if (local == 0) {
			outgoing = modem->packer.hdlc.get_tx_frame(
				modem->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->size == 3 &&
				      outgoing->buf[1] == FRAME_S_RR,
			      "LAPM information recovery did not acknowledge the peer");
			modem->packer.hdlc.tx_complete(
				modem->packer.hdlc.framer, outgoing);
		}
	}

	lapm->peer_busy = 0;
	lapm->vr = 6;
	modem->pack.bit = 3;
	modem->unpack.bit = 5;
	modem->retrain_pending = 1;
	modem_dp_reconnected(modem);
	CHECK(modem->retrain_pending == 0 && !lapm->peer_busy &&
		      !lapm->retrain_poll && lapm->rtx_count == 0 &&
		      modem->bit_timer > 0 && lapm->vs == 5 && lapm->va == 5 &&
		      lapm->vr == 6 && modem->pack.bit == 0 &&
		      modem->unpack.bit == 0,
	      "clean LAPM retrain invented a recovery poll");

	memset(&lapm_source, 0, sizeof(lapm_source));
	lapm_source.data = retained_payload;
	lapm_source.length = sizeof(retained_payload);
	modem->get_chars = scripted_lapm_source;
	CHECK(modem_hdlc_get_bits(modem, 1, &serialized_bit, 1) == 1,
	      "LAPM could not begin the interrupted information frame");
	retained_frame = modem->packer.hdlc.tx_frame;
	CHECK(retained_frame != NULL && retained_frame->count == 0 &&
		      modem->pack.bit == 7 &&
		      retained_frame->size == 3 + sizeof(retained_payload) &&
		      retained_frame->buf[0] == lapm->cmd_addr &&
		      retained_frame->buf[1] == 10 &&
		      retained_frame->buf[2] == 12 &&
		      memcmp(retained_frame->buf + 3, retained_payload,
			     sizeof(retained_payload)) == 0 &&
		      lapm_source.offset == sizeof(retained_payload) &&
		      lapm->vs == 6,
	      "LAPM did not queue the retained information frame");
	retained_v44_session = lapm->v44_session;
	lapm_source.data = queued_payload;
	lapm_source.length = sizeof(queued_payload);
	lapm_source.offset = 0;
	lapm->peer_busy = 0;
	modem->unpack.bit = 11;
	modem->unpack.data = 0x5a5;
	modem->packer.hdlc.rx_error = 0;
	modem->packer.hdlc.rx_ones = 3;
	modem->packer.hdlc.rx_frame->count = 2;
	modem->retrain_pending = 1;
	modem->bit_timer = 0;
	modem_dp_reconnected(modem);
	CHECK(lapm->state == LAPM_DATA_STATE && lapm->vs == 6 &&
		      lapm->va == 5 && lapm->vr == 6 && lapm->peer_busy == 1 &&
		      lapm->retrain_poll == 1 &&
		      lapm->sent_info == retained_frame &&
		      modem->packer.hdlc.tx_frame == NULL &&
		      modem->pack.bit == 0 && modem->unpack.bit == 0 &&
		      modem->packer.hdlc.tx_ones == 0 &&
		      modem->packer.hdlc.rx_ones == 0 &&
		      modem->packer.hdlc.rx_error == 1 &&
		      modem->packer.hdlc.rx_frame->count == 0 &&
		      retained_frame->size == 3 + sizeof(retained_payload) &&
		      memcmp(retained_frame->buf + 3, retained_payload,
			     sizeof(retained_payload)) == 0 &&
		      lapm_source.offset == 0 && lapm->v44_path == v44_path &&
		      memcmp(&lapm->v44_session, &retained_v44_session,
			     sizeof(retained_v44_session)) == 0,
	      "LAPM retrain changed pending frames or compression state");
	outgoing = modem->packer.hdlc.get_tx_frame(
		modem->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size == 3 &&
		      outgoing->buf[1] == FRAME_S_RR,
	      "LAPM retrain did not poll before resuming pending frames");
	modem->packer.hdlc.tx_complete(
		modem->packer.hdlc.framer, outgoing);
	response[0] = lapm->cmd_addr;
	response[1] = FRAME_S_RR;
	response[2] = (uint8_t)((lapm->va << 1) | 1);
	modem->packer.hdlc.rx_complete(
		modem->packer.hdlc.framer, &incoming);
	CHECK(lapm->peer_busy == 0 && !lapm->retrain_poll &&
		      lapm->vs == 5 && lapm->va == 5 &&
		      lapm->tx_info == retained_frame && lapm_source.offset == 0,
	      "LAPM retrain did not requeue the unacknowledged frame");
	outgoing = modem->packer.hdlc.get_tx_frame(
		modem->packer.hdlc.framer);
	CHECK(outgoing == retained_frame &&
		      outgoing->size == 3 + sizeof(retained_payload) &&
		      outgoing->buf[1] == 10 && outgoing->buf[2] == 12 &&
		      memcmp(outgoing->buf + 3, retained_payload,
			     sizeof(retained_payload)) == 0 &&
		      lapm_source.offset == 0 && lapm->vs == 6,
	      "LAPM retrain did not retransmit the retained frame first");
	modem->packer.hdlc.tx_complete(
		modem->packer.hdlc.framer, outgoing);
	response[2] = (uint8_t)(6u << 1);
	modem->packer.hdlc.rx_complete(
		modem->packer.hdlc.framer, &incoming);
	CHECK(lapm->va == 6 && lapm->sent_info == lapm->tx_info,
	      "LAPM retrain did not acknowledge the retained frame");
	outgoing = modem->packer.hdlc.get_tx_frame(
		modem->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size == 3 + sizeof(queued_payload) &&
		      outgoing->buf[1] == 12 && outgoing->buf[2] == 12 &&
		      memcmp(outgoing->buf + 3, queued_payload,
			     sizeof(queued_payload)) == 0 &&
		      lapm_source.offset == sizeof(queued_payload) && lapm->vs == 7,
	      "LAPM retrain did not resume queued payload after retransmission");
	modem->packer.hdlc.tx_complete(
		modem->packer.hdlc.framer, outgoing);
	memset(&lapm_source, 0, sizeof(lapm_source));
	modem_ec_exit(modem);
	modem_delete(modem);
	return true;
}

static bool test_retrain_preserves_detection(void)
{
	enum {
		TEST_STATE_DP_ESTAB = 2,
		TEST_STATE_EC_ESTAB = 4,
	};
	struct modem_driver driver = {
		.name = "pre-link retrain test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "pre-link retrain test");
	struct dp datapump = {
		.id = DP_V34,
		.modem = modem,
		.status = DPSTAT_CONNECT,
	};
	struct lapm_state *lapm;

	CHECK(modem != NULL, "pre-link retrain modem could not be created");
	modem->dp = &datapump;
	modem->rx_rate = modem->tx_rate = 4800;
	modem->cfg.ec = 1;
	modem->cfg.ec_detector = 1;
	modem->cfg.ec_tx_win_size = LAPM_MAX_WIN_SIZE;
	modem->cfg.ec_rx_win_size = LAPM_MAX_WIN_SIZE;
	modem->cfg.ec_tx_info_size = LAPM_MAX_INFO_SIZE;
	modem->cfg.ec_rx_info_size = LAPM_MAX_INFO_SIZE;
	CHECK(modem_ec_init(modem) == 0,
	      "pre-link retrain LAPM could not initialize");

	lapm = &modem->ec.lapm;
	for (unsigned caller = 0; caller <= 1; ++caller) {
		void (*timeout)(struct modem *);
		struct detect_state retained;

		modem->caller = caller;
		modem->state = TEST_STATE_DP_ESTAB;
		modem_dp_connected(modem);
		CHECK(modem->state == TEST_STATE_EC_ESTAB &&
		      modem->get_bits == modem_detector_get_bits &&
		      modem->put_bits == modem_detector_put_bits,
		      "V.42 detection did not start for role %u", caller);
		modem->packer.detector.tx_count = 7;
		modem->packer.detector.rx_count = 1;
		modem->packer.detector.ec_enable = 1;
		retained = modem->packer.detector;
		modem->pack.bit = 3;
		modem->unpack.bit = 5;
		modem->bit_timer = 17;
		timeout = modem->bit_timer_func;
		modem_dp_reconnected(modem);
		CHECK(modem->state == TEST_STATE_EC_ESTAB &&
		      lapm->state == 0 && lapm->config == 0 &&
		      modem->get_bits == modem_detector_get_bits &&
		      modem->put_bits == modem_detector_put_bits &&
		      modem->packer.detector.tx_count == retained.tx_count &&
		      modem->packer.detector.ec_enable == retained.ec_enable &&
		      memcmp(modem->packer.detector.tx_pattern,
		             retained.tx_pattern, sizeof(retained.tx_pattern)) == 0 &&
		      memcmp(modem->packer.detector.rx_pattern,
		             retained.rx_pattern, sizeof(retained.rx_pattern)) == 0 &&
		      modem->packer.detector.rx_count == 0 &&
		      modem->pack.bit == 0 && modem->unpack.bit == 0 &&
		      modem->bit_timer == 17 && modem->bit_timer_func == timeout,
		      "retrain changed detection selection or timer for role %u", caller);
		modem->packer.detector.rx_count = 2;
		modem_dp_connected(modem);
		CHECK(modem->packer.detector.rx_count == 2 && modem->bit_timer == 17,
		      "duplicate generic CONNECT restarted detection");
	}

	modem->dp = NULL;
	modem_ec_exit(modem);
	modem_delete(modem);
	return true;
}

static bool test_lapm_timer_follows_rate_changes(void)
{
	struct modem_driver driver = {
		.name = "LAPM timer rate change", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, "LAPM timer rate change");
	uint8_t bits[480];
	void (*timeout)(struct modem *);
	static const struct {
		unsigned parameter;
		int rate;
		int remaining;
	} changes[] = {
		{ MDMPRM_TX_RATE, 24000, 86400 },
		{ MDMPRM_RX_RATE, 24000, 144000 },
		{ MDMPRM_TX_RATE, 4800, 86400 },
		{ MDMPRM_RX_RATE, 4800, 28800 },
	};

	CHECK(m != NULL, "LAPM timer modem allocation failed");
	m->caller = 1;
	m->cfg.ec = 1;
	m->rx_rate = m->tx_rate = 4800;
	CHECK(modem_ec_init(m) == 0, "LAPM timer initialization failed");
	modem_hdlc_start(m);
	m->get_bits = modem_hdlc_get_bits;
	m->put_bits = modem_hdlc_put_bits;
	modem_ec_start(m);
	for (unsigned i = 0; m->bit_timer == 0 && i < 10000; ++i)
		modem_get_bits(m, 1, bits, 1);
	CHECK(m->bit_timer == 28800 && m->ec.lapm.config &&
	      m->ec.lapm.rtx_count == 0, "XID did not start its response timer");
	timeout = m->bit_timer_func;
	for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
		CHECK(modem_set_param(m, changes[i].parameter, changes[i].rate) == 0 &&
		      m->bit_timer == changes[i].remaining &&
		      m->bit_timer_func == timeout && m->ec.lapm.rtx_count == 0,
		      "rate change %zu altered the remaining LAPM timeout", i);
	}

	/* Only half a second remains. Preserve that, not a fresh three seconds,
	 * when both directions change independently to an asymmetric PCM link. */
	m->bit_timer = 4800;
	CHECK(modem_set_param(m, MDMPRM_RX_RATE, 48000) == 0 &&
	      modem_set_param(m, MDMPRM_TX_RATE, 14400) == 0 &&
	      m->bit_timer == 31200, "partial LAPM timeout was restarted or shortened");
	for (unsigned tick = 0; tick < 49; ++tick) {
		CHECK(modem_get_bits(m, 1, bits, 144) == 144,
		      "LAPM timer transmit failed");
		memset(bits, 1, sizeof(bits));
		CHECK(modem_put_bits(m, 1, bits, 480) == 480,
		      "LAPM timer receive failed");
	}
	CHECK(m->bit_timer == 624 && m->ec.lapm.rtx_count == 0 &&
	      modem_ec_resume_after_retrain(m) == 0 && m->bit_timer == 624,
	      "LAPM timeout expired early or changed on physical resume");
	CHECK(modem_get_bits(m, 1, bits, 144) == 144,
	      "final LAPM timer transmit failed");
	memset(bits, 1, sizeof(bits));
	CHECK(modem_put_bits(m, 1, bits, 479) == 479 && m->bit_timer == 1 &&
	      m->ec.lapm.rtx_count == 0 && modem_put_bits(m, 1, bits, 1) == 1 &&
	      m->ec.lapm.rtx_count == 1 && m->bit_timer == 187200,
	      "LAPM retry did not occur at the preserved half-second boundary");

	m->bit_timer = 1;
	CHECK(modem_set_param(m, MDMPRM_RX_RATE, 1) == 0 &&
	      modem_set_param(m, MDMPRM_TX_RATE, 1) == 0 && m->bit_timer == 1,
	      "rate reduction rounded an active timer down to disabled");
	m->bit_timer = INT_MAX;
	CHECK(modem_set_param(m, MDMPRM_TX_RATE, INT_MAX) == 0 &&
	      m->bit_timer == INT_MAX,
	      "large rate change overflowed the LAPM timer");
	CHECK(modem_set_param(m, MDMPRM_RX_RATE, -1) == -1 &&
	      m->rx_rate == 1 && m->bit_timer == INT_MAX,
	      "negative receive rate altered the modem or timer");
	CHECK(modem_set_param(m, MDMPRM_TX_RATE, -1) == -1 &&
	      m->tx_rate == INT_MAX && m->bit_timer == INT_MAX,
	      "negative transmit rate altered the modem or timer");

	m->rx_rate = m->tx_rate = 0;
	m->bit_timer = 57600;
	CHECK(modem_set_param(m, MDMPRM_TX_RATE, 2400) == 0 &&
	      m->bit_timer == 2400 && modem_set_param(m, MDMPRM_TX_RATE, 0) == 0 &&
	      m->bit_timer == 57600, "unknown-rate LAPM timer lost its fallback scale");
	m->bit_timer = 0;
	CHECK(modem_set_param(m, MDMPRM_RX_RATE, 4800) == 0 && m->bit_timer == 0,
	      "rate update restarted a stopped LAPM timer");
	modem_ec_exit(m);
	modem_detector_start(m);
	timeout = m->bit_timer_func;
	CHECK(m->bit_timer == 512 &&
	      modem_set_param(m, MDMPRM_TX_RATE, 24000) == 0 &&
	      m->bit_timer == 512 && m->bit_timer_func == timeout,
	      "rate update rescaled the detector's fixed-bit mark preamble");
	modem_delete(m);
	return true;
}

static bool test_retrain_preserves_lapm_establishment(void)
{
	enum { LAPM_DATA_STATE = 2 };
	struct modem_driver driver = {
		.name = "LAPM establishment retrain",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *peers[] = {
		modem_create(&driver, "caller"), modem_create(&driver, "answerer"),
	};
	struct dp datapumps[] = {
		{ .id = DP_V92, .modem = peers[0] },
		{ .id = DP_V92, .modem = peers[1] },
	};
	unsigned interrupted = 0;

	for (unsigned side = 0; side < 2; ++side) {
		struct modem *m = peers[side];

		CHECK(m != NULL, "establishment retrain allocation failed");
		m->dp = &datapumps[side];
		m->caller = side == 0;
		m->rx_rate = m->tx_rate = 4800;
		m->cfg.ec = 1;
		m->cfg.ec_detector = 1;
		m->dsp_info.v8_lapm = 1;
		CHECK(modem_ec_init(m) == 0, "establishment retrain initialization failed");
		modem_dp_connected(m);
	}
	for (unsigned bit = 0; bit < 200000; ++bit) {
		for (unsigned side = 0; side < 2; ++side) {
			uint8_t value;
			struct modem *m = peers[side];
			frame_t *frame = m->packer.hdlc.tx_frame;
			unsigned point = 0;

			if (side == 0 && bit == 64 &&
			    m->packer.hdlc.tx_preamble_complete != NULL)
				point = 8u;
			else if (frame != NULL && frame->count >= 1) {
				if (frame->buf[1] == 0xaf)
					point = side == 0 ? 1u : 2u;
				else if (side == 0 && frame->buf[1] == 0x7f)
					point = 4u;
			}
			if (point != 0 && (interrupted & point) == 0) {
				for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
					struct modem *resuming = peers[endpoint];
					struct lapm_state *l = &resuming->ec.lapm;
					struct lapm_v44_session session = l->v44_session;
					struct v44_character_path *path = l->v44_path;
					int state = l->state, config = l->config;
					int retries = l->rtx_count, remaining = resuming->bit_timer;
					void (*timeout)(struct modem *) = resuming->bit_timer_func;

					modem_dp_reconnected(resuming);
					CHECK(l->state == state && l->config == config &&
					      l->rtx_count == retries && l->v44_path == path &&
					      memcmp(&l->v44_session, &session, sizeof(session)) == 0 &&
					      resuming->bit_timer == remaining &&
					      resuming->bit_timer_func == timeout &&
					      resuming->get_bits == modem_hdlc_get_bits &&
					      resuming->put_bits == modem_hdlc_put_bits &&
					      resuming->packer.hdlc.tx_frame == NULL &&
					      resuming->packer.hdlc.rx_frame->count == 0,
					      "retrain reset LAPM establishment at point %u side %u",
					      point, endpoint);
				}
				interrupted |= point;
			}
			CHECK(modem_get_bits(m, 1, &value, 1) == 1 &&
			      modem_put_bits(peers[1 - side], 1, &value, 1) == 1,
			      "establishment serial exchange failed");
		}
		if (peers[0]->ec.lapm.state == LAPM_DATA_STATE &&
		    peers[1]->ec.lapm.state == LAPM_DATA_STATE)
			break;
	}
	CHECK(interrupted == 15u && peers[0]->ec.lapm.state == LAPM_DATA_STATE &&
	      peers[1]->ec.lapm.state == LAPM_DATA_STATE,
	      "LAPM did not establish after interrupted preamble, XID, reply and SABME (%u)",
	      interrupted);
	for (unsigned side = 0; side < 2; ++side) {
		peers[side]->dp = NULL;
		modem_delete(peers[side]);
	}
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
		      v21_operations->retrain == NULL &&
		      v21_operations->use_count == 0 &&
		      bell103_operations == v21_operations &&
		      v23_operations != NULL &&
		      v23_operations != v21_operations &&
		      v23_operations->create != NULL &&
		      v23_operations->delete != NULL &&
		      v23_operations->process != NULL &&
		      v23_operations->retrain == NULL &&
		      v23_operations->use_count == 0 &&
		      v22_operations != NULL &&
		      v22_operations != v21_operations &&
		      v22_operations != v23_operations &&
		      v22_operations->create != NULL &&
		      v22_operations->delete != NULL &&
		      v22_operations->process != NULL &&
		      v22_operations->retrain == NULL &&
		      v22_operations->use_count == 0 &&
		      v22bis_operations != NULL &&
		      v22bis_operations != v22_operations &&
		      v22bis_operations->create == v22_operations->create &&
		      v22bis_operations->delete == v22_operations->delete &&
		      v22bis_operations->process == v22_operations->process &&
		      v22bis_operations->retrain != NULL &&
		      v22bis_operations->use_count == 0 &&
		      bell212_operations == v22_operations &&
		      bell212_operations->retrain == NULL &&
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
		{ "duplex link timer accounting",
		  test_bit_timers_follow_duplex_time },
		{ "instance-owned modem events",
		  test_modem_events_are_instance_owned },
		{ "escape discards async transmit fragment",
		  test_escape_discards_async_transmit_fragment },
		{ "escape holds candidate bytes", test_escape_holds_candidate_bytes },
		{ "V.8 datapump mapping", test_v8_datapump_mapping },
		{ "V.8 handoff", test_v8_handoff },
		{ "V.8 fallback limits", test_v8_fallback_limits },
		{ "V.8 legacy V.32 selection", test_v8_legacy_v32 },
		{ "modulation report identity", test_modulation_report_identity },
		{ "observed CONNECT startup", test_connect_reports_observed_startup },
		{ "data-pump error-control policy",
		  test_datapump_error_control_policy },
		{ "V.92 negotiated LAPM detection bypass",
		  test_v92_negotiated_lapm_bypasses_detection },
		{ "raw V.42 detection residue",
		  test_raw_answerer_discards_detection_residue },
		{ "source-built core lifecycle",
		  test_core_has_no_external_dsp_owner },
		{ "G.711 data-pump input", test_g711_datapump_input },
		{ "receive DC front end", test_receive_dc_frontend },
		{ "truthful ATO1 lifecycle",
		  test_ato1_waits_for_datapump_retrain },
		{ "bounded ATH0 link cleardown",
		  test_ath0_drains_link_cleardown },
		{ "bounded AT dial destinations",
		  test_at_dial_destinations_are_bounded },
		{ "AT compression profile", test_at_compression_profile },
		{ "AT PCM profile controls", test_at_pcm_profile_controls },
		{ "AT V.34 symbol rate", test_at_v34_symbol_rate },
		{ "LAPM XID parameter encoding", test_lapm_xid_parameters },
		{ "LAPM XID negotiation directions", test_lapm_xid_directions },
		{ "LAPM repeated XID parameters", test_lapm_repeated_xid_parameters },
		{ "bounded LAPM XID envelopes", test_lapm_xid_envelopes },
		{ "bounded LAPM compression retry", test_lapm_dual_compression_retry },
		{ "LAPM directional preamble", test_lapm_directional_preamble },
		{ "LAPM peer-flag preamble", test_lapm_peer_flag_preamble },
		{ "LAPM receive backpressure", test_lapm_receive_backpressure },
		{ "LAPM receive frame boundaries", test_lapm_receive_frame_boundaries },
		{ "LAPM receive information limit", test_lapm_receive_information_limit },
		{ "LAPM peer-ready DM establishment", test_lapm_peer_ready_dm },
		{ "LAPM unnumbered response states", test_lapm_unnumbered_responses },
		{ "LAPM disconnected DISC response", test_lapm_disconnected_disc },
		{ "LAPM mode command collisions", test_lapm_mode_command_collisions },
		{ "LAPM numbered frame rejection", test_lapm_numbered_frame_rejection },
		{ "LAPM unnumbered frame rejection", test_lapm_unnumbered_frame_rejection },
		{ "LAPM acknowledgement queue consistency", test_lapm_acknowledgement_queue_consistency },
		{ "LAPM acknowledgement window", test_lapm_acknowledgement_window },
		{ "LAPM repeated sequence error poll", test_lapm_repeated_sequence_error_poll },
		{ "LAPM peer-busy acknowledgement", test_lapm_peer_busy_acknowledgement },
		{ "LAPM peer-busy timer", test_lapm_peer_busy_timer },
		{ "LAPM idle supervision", test_lapm_idle_supervision },
		{ "LAPM acknowledgement progress", test_lapm_acknowledgement_progress },
		{ "bounded LAPM stalled retrain", test_lapm_stalled_retrain },
		{ "LAPM retrain preserves link state",
		  test_lapm_retrain_preserves_link_state },
		{ "retrain preserves V.42 detection", test_retrain_preserves_detection },
		{ "retrain preserves LAPM establishment",
		  test_retrain_preserves_lapm_establishment },
		{ "LAPM timer follows rate changes", test_lapm_timer_follows_rate_changes },
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
