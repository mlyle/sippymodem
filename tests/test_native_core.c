// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <modem_param.h>
#include <modem_timer.h>
#include <fsk.h>
#include <single_tone.h>
#include <v21.h>
#include <v42_xid.h>
#include <v44_character_path.h>
#include <datapump.h>
#include <line_startup.h>
#include <v8_datapump.h>
#include <v34_datapump.h>
#include <v90_datapump.h>
#include <v90_profile.h>
#include <pcm_rate.h>

#include "test.h"
#include "lapm_xid_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern int process_at_command(struct modem *modem, char *command);
extern int modem_dial(struct modem *modem);
extern int modem_recv_from_tty(struct modem *modem, char *buffer, int count);

/* Host lifecycle tests substitute call progress without mutating the catalog. */
static const struct dp_operations *call_progress_override;
const struct dp_operations *__real_modem_dp_get_registered_ops(enum DP_ID id);
const struct dp_operations *__wrap_modem_dp_get_registered_ops(enum DP_ID id)
{
	return id == DP_CALLPROG && call_progress_override != NULL ?
		call_progress_override : __real_modem_dp_get_registered_ops(id);
}

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

static bool test_disconnect_cause_survives_call_teardown(void)
{
	struct modem_driver driver = {
		.name = "disconnect cause", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	struct modem *other = modem_create(&driver, driver.name);
	CHECK(m != NULL && other != NULL && modem_answer(m) == 0,
	      "disconnect diagnostic fixture failed");
	modem_note_disconnect(m, true, "protocol rejection");
	modem_update_status(m, STATUS_DP_ERROR);
	CHECK(modem_report_remote_call_end(m) == 0 && modem_stop_now(m) == 0 &&
	      strcmp(m->disconnect_reason, "protocol rejection") == 0 &&
	      strcmp(m->disconnect_step, "training") == 0 && m->disconnect_local &&
	      other->disconnect_reason == NULL,
	      "teardown replaced, discarded, or shared the first failure");
	CHECK(modem_answer(m) == 0 && m->disconnect_reason == NULL &&
	      m->disconnect_step == NULL, "new call inherited the previous failure");
	CHECK(modem_report_remote_call_end(m) == 0 &&
	      strcmp(m->disconnect_reason, "remote SIP call ended") == 0 &&
	      !m->disconnect_local, "peer release became a local failure");
	modem_stop_now(m);
	modem_delete(m);
	modem_delete(other);
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
	int max_fragment, const struct dp_operations *operations)
{
	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations == NULL || retrain_dp.datapump.op != NULL)
		return NULL;
	retrain_dp.datapump = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_CONNECT,
		.op = operations,
	};
	return &retrain_dp.datapump;
}

static int delete_retrain_dp(struct dp *datapump)
{
	if (datapump != &retrain_dp.datapump || datapump->op == NULL)
		return -1;
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
	call_progress_override = &operations;
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
	CHECK(modem_stop_now(modem) == 0 && retrain_dp.datapump.op == NULL,
	      "ATO1 test data pump did not stop");
	modem_delete(modem);
	call_progress_override = NULL;
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

static bool run_ath0_cleardown(
	enum ath0_test_completion completion, enum g711_law law,
	unsigned long initial_count)
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
	call_progress_override = &operations;
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
	modem_update_status(modem, STATUS_PACK_LINK);
	modem->ec.lapm.state = LAPM_DATA_STATE;
	modem->state = TEST_STATE_COMMAND_ONLINE;
	modem->command = 1;
	modem->hook = MODEM_HOOK_OFF;
	modem->count = initial_count;
	if (peer) {
		uint8_t serialized[32];
		response_bytes[0] = modem->ec.lapm.rsp_addr;
		response_bytes[1] = 0x53;
		modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &response);
		CHECK(modem->state == TEST_STATE_DP_DISC && modem->ec.lapm.state == 0 &&
		      modem->get_bits == modem_hdlc_get_bits && modem->put_bits == modem_hdlc_put_bits &&
		      retrain_dp.hangup_calls == 0 && hangup_driver_stop_calls == 0 &&
		      time_before(modem->count, modem->sample_timer) &&
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
		      time_before(modem->count, modem->sample_timer),
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
		      time_before(modem->count, modem->sample_timer),
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
		      modem->hook == MODEM_HOOK_ON && retrain_dp.datapump.op == NULL,
	      "datapump cleardown did not complete ATH0: state=%u stop=%u "
	      "hook=%u active=%u timer=%lu count=%lu process=%p",
	      modem->state, hangup_driver_stop_calls, modem->hook,
	      (unsigned)(retrain_dp.datapump.op != NULL), modem->sample_timer, modem->count,
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
	call_progress_override = NULL;
	return true;
}

static bool test_ath0_drains_link_cleardown(void)
{
	const unsigned long initial_counts[] = {
		0, ULONG_MAX - MODEM_FRAG / 2,
		/* LAPM's 1.5-second disconnect deadline is exactly zero. */
		ULONG_MAX - MODEM_RATE * 3 / 2 + 1,
		/* The datapump's 3-second disconnect deadline is exactly zero. */
		ULONG_MAX - MODEM_RATE * 3 + 1,
	};

	for (size_t i = 0; i < ARRAY_LENGTH(initial_counts); ++i)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (enum ath0_test_completion completion = ATH0_ACKNOWLEDGED;
			     completion <= HANGUP_WITHOUT_COMMAND; ++completion)
				CHECK(run_ath0_cleardown(completion, law, initial_counts[i]),
				      "ATH0 law %u case %u count %lu failed",
				      law, completion, initial_counts[i]);
	return true;
}

struct call_cancel_capture {
	unsigned int stops;
	int stop_result;
};

static int capture_call_cancel(struct modem *modem)
{
	struct call_cancel_capture *capture = modem->dev_data;

	capture->stops++;
	return capture->stop_result;
}

static bool test_stop_cancels_link_detection(void)
{
	struct modem_driver driver = {
		.name = "stopped link detection", .start = unused_start,
		.stop = capture_call_cancel, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned failure = 0; failure < 2; ++failure)
	for (unsigned transmit = 0; transmit < 2; ++transmit) {
		struct call_cancel_capture capture = { .stop_result = failure ? -1 : 0 };
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t bits[32] = { 0 };

		CHECK(m != NULL, "stopped detection allocation failed");
		m->dev_data = &capture;
		m->caller = caller;
		m->started = 1;
		m->state = 4;
		m->cfg.ec = 1;
		m->tx_rate = m->rx_rate = 4800;
		modem_detector_start(m);
		m->get_bits = modem_detector_get_bits;
		m->put_bits = modem_detector_put_bits;
		m->bit_timer = 8;
		CHECK(modem_stop_now(m) == capture.stop_result && capture.stops == 1 &&
		      !m->started && strcmp(modem_call_phase(m), "idle") == 0 &&
		      m->bit_timer == 0 && m->bit_timer_func == NULL,
		      "stop retained detection work: caller %u failure %u", caller, failure);
		CHECK((transmit ? modem_get_bits(m, 8, bits, sizeof(bits)) :
				  modem_put_bits(m, 8, bits, sizeof(bits))) == sizeof(bits) &&
		      strcmp(modem_call_phase(m), "idle") == 0 && m->command &&
		      m->get_bits == NULL && m->put_bits == NULL &&
		      m->bit_timer == 0 && m->bit_timer_func == NULL,
		      "late %s bits revived a stopped call", transmit ? "transmit" : "receive");
		modem_delete(m);
	}
	return true;
}

struct framing_cancel_capture {
	unsigned int hangups;
	unsigned int retries;
	int hangup_result;
};

static int capture_framing_hangup(struct dp *dp)
{
	struct framing_cancel_capture *capture = dp->modem->dev_data;

	capture->hangups++;
	return capture->hangup_result;
}

static void capture_framing_retry(struct modem *m, int bits)
{
	struct framing_cancel_capture *capture = m->dev_data;

	(void)bits;
	capture->retries++;
}

static bool test_hangup_uses_active_framing(void)
{
	enum { DETECTION, LAPM, ASYNC };
	enum { TEST_STATE_DP_ESTAB = 2, TEST_LAPM_DISC = 3 };
	struct modem_driver driver = {
		.name = "framing cancellation", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	const struct dp_operations operations = {
		.name = "framing cancellation", .hangup = capture_framing_hangup,
	};

	for (unsigned mode = DETECTION; mode <= ASYNC; ++mode)
	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned local = 0; local < 2; ++local)
	for (unsigned failure = 0; failure < 2; ++failure)
	for (unsigned pending = 0; pending <= (mode == LAPM && caller); ++pending)
	for (unsigned crossed = 0; crossed <= pending; ++crossed) {
		struct framing_cancel_capture capture = { .hangup_result = failure ? -1 : 0 };
		struct modem *m = modem_create(&driver, driver.name);
		struct dp dp = { .id = DP_V34, .modem = m, .op = &operations,
			.status = DPSTAT_CONNECT };
		uint8_t bits[32] = { 0 };
		uint8_t retained[LAPM_MAX_INFO_SIZE + 3];
		frame_t *active = NULL;
		int active_size = 0, active_count = 0;
		unsigned long deadline;

		CHECK(m != NULL, "framing cancellation allocation failed");
		m->dp = &dp;
		m->dev_data = &capture;
		m->caller = caller;
		m->cfg.ec = mode != ASYNC;
		m->cfg.ec_detector = mode == DETECTION;
		m->cfg.comp = 0;
		m->tx_rate = m->rx_rate = 4800;
		m->state = TEST_STATE_DP_ESTAB;
		m->started = 1;
		m->count = 1234;
		CHECK(modem_ec_init(m) == 0, "framing cancellation LAPM allocation failed");
		modem_dp_connected(m);
		if (mode == ASYNC && !caller)
			modem_update_status(m, STATUS_PACK_LINK);
		CHECK(mode == DETECTION ? m->get_bits == modem_detector_get_bits :
		      mode == LAPM ? m->get_bits == modem_hdlc_get_bits :
		                     m->get_bits == modem_async_get_bits,
		      "framing cancellation did not select mode %u", mode);
		if (pending) {
			struct lapm_state *l = &m->ec.lapm;

			for (unsigned bit = 0; bit < 10000 &&
			     l->v44_session.phase != LAPM_V44_COMMAND_PENDING; ++bit)
				CHECK(modem_get_bits(m, 1, bits, 1) == 1,
				      "cancellation fixture did not transmit its preamble");
			active = m->packer.hdlc.tx_frame;
			CHECK(l->v44_session.phase == LAPM_V44_COMMAND_PENDING &&
			      active != NULL && active->size <= (int)sizeof(retained),
			      "cancellation fixture did not borrow XID");
			active_size = active->size;
			active_count = active->count;
			memcpy(retained, active->buf, (size_t)active_size);
			if (crossed) {
				uint8_t wire[LAPM_MAX_INFO_SIZE + 2] = { l->rsp_addr, 0xaf };
				frame_t command = { .buf = wire,
					.count = 2 + (int)l->v44_session.command_length };

				memcpy(wire + 2, l->v44_session.command, l->v44_session.command_length);
				m->packer.hdlc.rx_complete(l, &command);
				CHECK(l->v44_session.response_length != 0 &&
				      l->v44_session.phase == LAPM_V44_COMMAND_PENDING,
				      "crossed command did not cache an independent response");
			}
		}
		m->bit_timer = 8;
		m->bit_timer_func = bit_timer_expired;
		m->packer_process = capture_framing_retry;
		if (local)
			modem_hangup(m);
		else
			modem_update_status(m, STATUS_NOCARRIER);
		deadline = m->sample_timer;
		if (local && mode == LAPM) {
			CHECK(strcmp(modem_call_phase(m), "error-control-disconnect") == 0 &&
			      capture.hangups == 0 && m->ec.lapm.state == TEST_LAPM_DISC &&
			      m->get_bits == modem_hdlc_get_bits && m->put_bits == modem_hdlc_put_bits &&
			      m->bit_timer > 8 && m->bit_timer_func != bit_timer_expired &&
			      m->packer_process == NULL && deadline - m->count == MODEM_RATE * 3 / 2 &&
			      m->packer.hdlc.tx_preamble_flags == 0 &&
			      m->packer.hdlc.tx_preamble_complete == NULL &&
			      m->ec.lapm.v44_session.phase == LAPM_V44_READY &&
			      m->ec.lapm.v44_session.command_length == 0 &&
			      m->ec.lapm.v44_session.response_length == 0,
			      "active LAPM release lost its frame route or timeout");
			if (active != NULL)
				CHECK(m->packer.hdlc.tx_frame == active && active->size == active_size &&
				      active->count == active_count &&
				      memcmp(active->buf, retained, (size_t)active_size) == 0,
				      "cancelling negotiation overwrote its borrowed XID frame");
			CHECK(modem_get_bits(m, 8, bits, sizeof(bits)) == sizeof(bits) &&
			      m->ec.lapm.tx_ctrl == m->ec.lapm.ctrl_list,
			      "LAPM release could not serialize DISC: caller %u", caller);
		} else {
			unsigned long delay = local && !failure ? MODEM_RATE * 3 : 48;

			CHECK(strcmp(modem_call_phase(m), "cleardown") == 0 &&
			      capture.hangups == local && m->get_bits == NULL && m->put_bits == NULL &&
			      m->bit_timer == 0 && m->bit_timer_func == NULL && m->packer_process == NULL &&
			      deadline - m->count == delay && m->sample_timer_func != NULL,
			      "physical cleardown retained framing work: mode %u caller %u local %u failure %u",
			      mode, caller, local, failure);
			CHECK(mode == LAPM || m->ec.lapm.state == 0,
			      "hangup started LAPM release without HDLC");
			CHECK(modem_get_bits(m, 8, bits, sizeof(bits)) == sizeof(bits) &&
			      modem_put_bits(m, 8, bits, sizeof(bits)) == sizeof(bits) &&
			      m->sample_timer == deadline && m->ring_count == 0 && capture.retries == 0 &&
			      strcmp(modem_call_phase(m), "cleardown") == 0,
			      "late bits ran old work during physical cleardown");
		}
		m->dp = NULL; /* The fixture owns the stack datapump. */
		CHECK(modem_stop_now(m) == 0, "framing cancellation cleanup failed");
		modem_delete(m);
	}
	return true;
}

static bool test_call_progress_cancels_without_media(void)
{
	struct modem_driver driver = {
		.name = "unclocked call cancellation",
		.start = unused_start,
		.stop = capture_call_cancel,
		.ioctl = unused_control,
	};

	for (unsigned int action = 0; action < 4; ++action)
	for (unsigned int failure = 0; failure < 2; ++failure) {
		struct call_cancel_capture capture = { .stop_result = failure ? -1 : 0 };
		struct modem *modem = modem_create(&driver, driver.name);
		int result_pipe[2];
		char result_text[32];
		const char *expected = action == 1 ? "OK\r\n" : "NO CARRIER\r\n";

		CHECK(modem != NULL && pipe(result_pipe) == 0 &&
			      fcntl(result_pipe[0], F_SETFL, O_NONBLOCK) == 0,
		      "call cancellation fixture failed");
		modem->dev_data = &capture;
		modem->pty = result_pipe[1];
		CHECK(process_at_command(modem, "ATE0V1Q0") == 0 &&
			      process_at_command(modem, "ATDpeer") == 1 &&
			      modem->started && modem->dp->id == DP_CALLPROG && modem->count == 0,
		      "call did not enter unclocked SIP progress");
		switch (action) {
		case 0:
			CHECK(modem_write(modem, " ", 1) == 1, "space did not cancel dialing");
			break;
		case 1:
			CHECK(process_at_command(modem, "ATH0") == 1, "ATH0 cancellation failed");
			break;
		case 2:
			modem_hangup(modem);
			break;
		case 3: {
			struct termios termios = modem->termios;
			CHECK(cfsetispeed(&termios, B0) == 0 && cfsetospeed(&termios, B0) == 0,
			      "DTR cancellation fixture failed");
			modem_update_termios(modem, &termios);
			break;
		}
		}
		CHECK(capture.stops == 1 && !modem->started && modem->dp == NULL &&
			      modem->command && strcmp(modem_call_phase(modem), "idle") == 0 &&
			      modem->sample_timer == 0 && modem->sample_timer_func == NULL &&
			      modem->count == 0 && !modem->hangup_command_pending,
		      "cancellation %u/%u waited for audio or retained freed state", action, failure);
		CHECK(read(result_pipe[0], result_text, sizeof(result_text)) == (ssize_t)strlen(expected) &&
			      memcmp(result_text, expected, strlen(expected)) == 0,
		      "cancellation %u/%u did not emit its single result", action, failure);
		CHECK(modem_report_outgoing_call_connected(modem) == 0 &&
			      modem_report_remote_call_end(modem) == 0,
		      "late signaling failed after cancellation");
		modem_hangup(modem);
		CHECK(capture.stops == 1 && modem->dp == NULL && !modem->started &&
			      read(result_pipe[0], result_text, sizeof(result_text)) < 0 && errno == EAGAIN,
		      "late signaling or repeated cancellation reopened the call or duplicated its result");
		CHECK(process_at_command(modem, "ATDpeer") == 1 && modem->started &&
			      modem->dp->id == DP_CALLPROG,
		      "cancelled modem could not start another call");
		modem_hangup(modem);
		CHECK(capture.stops == 2 && !modem->started && modem->dp == NULL,
		      "second call did not release its owner");
		modem->pty = -1;
		close(result_pipe[0]);
		close(result_pipe[1]);
		modem_delete(modem);
	}
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
	int max_fragment, const struct dp_operations *operations)
{
	if (modem == NULL || id != DP_CALLPROG || caller != 1 ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations == NULL || g711_dp.datapump.op != NULL)
		return NULL;
	g711_dp.datapump = (struct dp){
		.id = id,
		.modem = modem,
		.op = operations,
	};
	return &g711_dp.datapump;
}

static int delete_g711_dp(struct dp *datapump)
{
	if (datapump != &g711_dp.datapump || datapump->op == NULL)
		return -1;
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
	call_progress_override = &operations;
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
	CHECK(modem_stop_now(modem) == 0 && g711_dp.datapump.op == NULL,
	      "G.711 test data pump did not stop cleanly");
	modem_delete(modem);
	call_progress_override = NULL;
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
	call_progress_override = &operations;
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
	CHECK(modem_stop_now(modem) == 0 && g711_dp.datapump.op == NULL,
	      "DC front-end data pump did not stop cleanly");
	modem_delete(modem);
	call_progress_override = NULL;
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
	return true;
}

static bool test_v42_detection_deadline(void)
{
	static const unsigned rates[][2] = {
		{ 600, 600 }, { 1200, 1200 }, { 9600, 9600 },
		{ 31200, 28000 }, { 33600, 56000 },
	};
	struct modem_driver driver = {
		.name = "V.42 detection deadline", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t i = 0; i < ARRAY_LENGTH(rates); ++i) {
		for (unsigned caller = 0; caller <= 1; ++caller) {
			struct modem *m = modem_create(&driver, "V.42 detection deadline");
			struct dp dp = { .id = DP_V34, .modem = m };
			unsigned tx_phase = 0, rx_phase = 0, tick;
			unsigned deadline = caller ? 1500 : 750;
			uint32_t transmit_tail = 0;

			CHECK(m != NULL, "detection deadline allocation failed");
			m->dp = &dp;
			m->caller = caller;
			m->tx_rate = rates[i][0];
			m->rx_rate = rates[i][1];
			m->cfg.ec = m->cfg.ec_detector = 1;
			modem_dp_connected(m);
			m->xmit.buf[0] = 'Z';
			m->xmit.count = m->xmit.head = 1;
			CHECK(m->bit_timer == (int)((m->tx_rate + m->rx_rate) * deadline / 1000),
			      "detection timer did not start at its role deadline for case %zu role %u", i, caller);
			for (tick = 0; tick < deadline + 100 && m->get_bits == modem_detector_get_bits; ++tick) {
				tx_phase += m->tx_rate;
				rx_phase += m->rx_rate;
				while (tx_phase >= 1000 && m->get_bits == modem_detector_get_bits) {
					uint8_t bit;

					tx_phase -= 1000;
					CHECK(modem_get_bits(m, 1, &bit, 1) == 1,
					      "detection transmit clock failed");
					transmit_tail = (transmit_tail << 1) | bit;
				}
				while (rx_phase >= 1000 && m->get_bits == modem_detector_get_bits) {
					uint8_t mark = 1;

					rx_phase -= 1000;
					CHECK(modem_put_bits(m, 1, &mark, 1) == 1,
					      "detection receive clock failed");
				}
				CHECK(tick >= deadline - 2 || m->get_bits == modem_detector_get_bits,
				      "T400 expired early for case %zu role %u", i, caller);
			}
			CHECK(tick >= deadline - 1 && tick < deadline + 100 && !m->cfg.ec &&
			      m->get_bits == modem_async_get_bits &&
			      m->put_bits == modem_async_put_bits && !m->bit_timer &&
			      m->xmit.count == 1 && m->pack.bit <= 9,
			      "missing detection reply stalled or lost queued data: case %zu role %u", i, caller);
			CHECK(!caller || (transmit_tail & 0x3ffffu) == (0x89u << 9 | 0x1ffu),
			      "raw fallback truncated the final ODP repetition");
			m->dp = NULL;
			modem_delete(m);
		}
	}
	return true;
}

static bool complete_raw_v14_detection(struct modem *answerer)
{
	uint8_t bits[585];
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
		      v8_datapump_uses_pcm(DP_V91) &&
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
		unsigned caller_policy;
		unsigned answerer_policy;
		enum modem_pcm_role caller_role;
		enum modem_pcm_role answerer_role;
		bool caller_v34_fallback;
		bool caller_lapm;
		bool answerer_lapm;
		bool duplex_automode;
	} selections[] = {
		{ DP_V90, DP_V90, DP_V90,
		  2, 0, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true, false },
		{ DP_V92, DP_V92, DP_V92,
		  0, 2, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true, false },
		{ DP_V92, DP_V92, DP_V92,
		  2, 1, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true, true },
		{ DP_V92, DP_V92, DP_V92,
		  1, 2, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true, true },
		{ DP_V92, DP_V92, DP_V92,
		  2, 2, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true, true },
		{ DP_V92, DP_V34, DP_V34,
		  2, 0, MODEM_PCM_ROLE_NONE, MODEM_PCM_ROLE_NONE,
		  true, true, true, false },

		{ DP_V34, DP_V34, DP_V34,
		  false, false, MODEM_PCM_ROLE_NONE, MODEM_PCM_ROLE_NONE,
		  false, true, true, false },
		{ DP_V90, DP_V90, DP_V90,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true, false },
		{ DP_V90, DP_V90, DP_V90,
		  true, false, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true, false },
		{ DP_V92, DP_V92, DP_V92,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, true, false },
		{ DP_V92, DP_V92, DP_V92,
		  true, false, MODEM_PCM_ROLE_DIGITAL, MODEM_PCM_ROLE_ANALOG,
		  false, true, true, false },
		{ DP_V92, DP_V92, DP_V92,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, false, true, false },
		{ DP_V90, DP_V34, DP_V34,
		  true, false, MODEM_PCM_ROLE_NONE, MODEM_PCM_ROLE_NONE,
		  true, true, true, false },
		{ DP_V90, DP_V90, DP_V90,
		  false, true, MODEM_PCM_ROLE_ANALOG, MODEM_PCM_ROLE_DIGITAL,
		  false, true, false, false },
	};
	struct modem_driver driver = {
		.name = "V.8 handoff test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *caller;
	struct modem *answerer;
	const struct dp_operations *operations;
	struct dp *calling_dp;
	struct dp *answering_dp;
	struct single_tone_detector legacy_carrier;
	int16_t calling_audio[MODEM_FRAG] = { 0 };
	int16_t answering_audio[MODEM_FRAG] = { 0 };
	int calling_status = DPSTAT_OK;
	int answering_status = DPSTAT_OK;

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
		bool duplex = MODEM_AUTOMODE(caller) && selections[selection].duplex_automode;
		enum DP_ID expected = duplex ? DP_V91 : selections[selection].handoff;
		enum modem_pcm_role caller_role = duplex ? MODEM_PCM_ROLE_DIGITAL : selections[selection].caller_role;
		enum modem_pcm_role answerer_role = duplex ? MODEM_PCM_ROLE_DIGITAL : selections[selection].answerer_role;
		CHECK(modem_set_param(caller, MDMPRM_V90_DIGITAL_INDICATION,
			      selections[selection].caller_policy) == 0 &&
		      modem_set_param(answerer, MDMPRM_V90_DIGITAL_INDICATION,
			      selections[selection].answerer_policy) == 0,
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
		CHECK(calling_dp != NULL && answering_dp != NULL,
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
			      caller->dp_requested == expected &&
			      answerer->dp_requested == expected &&
			      caller->dsp_info.pcm_role == caller_role &&
			      answerer->dsp_info.pcm_role == answerer_role &&
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
		if (expected == DP_V91) {
			const struct dp_operations *pcm = modem_dp_get_registered_ops(DP_V91);
			struct modem *ends[] = { caller, answerer };
			struct dp *parents[] = { calling_dp, answering_dp };
			for (unsigned side = 0; side < 2; ++side) {
				ends[side]->dp = parents[side];
				struct dp *child = pcm->create(ends[side], DP_V91, side == 0,
					MODEM_RATE, MODEM_FRAG, pcm);
				uint8_t rx[MODEM_FRAG], tx[MODEM_FRAG];
				int16_t input[MODEM_FRAG] = { 0 }, output[MODEM_FRAG];
				bool exact = false;
				memset(rx, 0xff, sizeof(rx));
				CHECK(child != NULL && pcm->process_g711(child, G711_LAW_PCMU,
					rx, input, NULL, tx, output, MODEM_FRAG, &exact) == DPSTAT_OK && exact,
				      "V.91 did not adopt its completed V.8 handoff");
				for (unsigned i = 0; i < MODEM_FRAG; ++i)
					CHECK(side ? output[i] == 0 : abs(output[i]) > 1000,
					      "V.91 duplicated caller silence or omitted answerer silence");
				CHECK(pcm->delete(child) == 0, "V.91 handoff child leaked");
				ends[side]->dp = NULL;
			}
		}
		CHECK(operations->delete(calling_dp) == 0 &&
			      operations->delete(answering_dp) == 0,
		      "V.8 PCM handshakes did not release their state");
		modem_delete(caller);
		modem_delete(answerer);
	}
	return true;
}

static bool test_automatic_factory_profile(void)
{
	struct modem_driver driver = {
		.name = "automatic factory profile",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, driver.name);

	CHECK(modem != NULL, "factory profile allocation failed");
	for (unsigned reset = 0; reset < 2; ++reset) {
		CHECK(MODEM_DP(modem) == DP_V92 && MODEM_AUTOMODE(modem) == 1 &&
		      modem->min_rate == 300 && modem->max_rate == 64000 &&
		      modem->v90_upstream_min_rate == 0 && modem->v90_upstream_max_rate == 0 &&
		      modem->v34_symbol_rate == 0 && modem->v90_upstream_max_baud == 0 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem_get_param(modem, MDMPRM_V90_DIGITAL_INDICATION) == MODEM_PCM_POLICY_AUTO,
		      "factory profile restricted automatic mode or rate");
		CHECK(process_at_command(modem, "AT+MS=21,0,300,300+V34B=2400+V90U=9600,9600,3000+V92U=1") == 0 &&
		      process_at_command(modem, "ATZ") == 0,
		      "ATZ did not clear fixed family and directional rate settings");
	}
	modem_delete(modem);
	return true;
}

static bool test_automatic_startup_deadline(void)
{
	struct modem_driver driver = {
		.name = "automatic startup deadline",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	const struct dp_operations *ops = modem_dp_get_registered_ops(DP_V8);

	/* The final case admits 600 bit/s: a constant answer mark must still
	 * expire without becoming scrambled training or resetting S7. */
	for (unsigned scenario = 0; scenario < 5; ++scenario) {
		struct modem *modem = modem_create(&driver, driver.name);
		struct single_tone_transmitter tone;
		struct dp *dp;
		unsigned seconds = scenario == 0 ? 7 : scenario == 1 ? 1 : 4;
		unsigned samples = 0;
		int result = DPSTAT_OK;

		CHECK(modem != NULL && ops != NULL, "deadline fixture allocation failed");
		modem_set_sreg(modem, SREG_WAIT_CARRIER_AFTER_DIAL, seconds);
		if (scenario == 2)
			modem->min_rate = 4800;
		if (scenario == 3)
			MODEM_AUTOMODE(modem) = 0;
		CHECK(single_tone_transmitter_init(&tone, 2250, 12000) == SINGLE_TONE_OK,
		      "deadline tone failed");
		dp = ops->create(modem, DP_V92, scenario != 0, MODEM_RATE, MODEM_FRAG, ops);
		CHECK(dp != NULL && v8_datapump_take_legacy(dp) == NULL,
		      "untrained probe was transferable");
		while (result == DPSTAT_OK && samples <= (seconds + 1) * MODEM_RATE) {
			int16_t rx[MODEM_FRAG] = { 0 }, tx[MODEM_FRAG];
			for (unsigned i = 0; scenario >= 2 && i < MODEM_FRAG; ++i)
				CHECK(single_tone_transmitter_sample(&tone, &rx[i]) == SINGLE_TONE_OK,
				      "deadline tone generation failed");
			result = ops->process(dp, rx, tx, MODEM_FRAG);
			samples += MODEM_FRAG;
		}
		CHECK(result == DPSTAT_NOANSWER && samples <= seconds * MODEM_RATE + MODEM_FRAG &&
		      modem->dp_requested == 0 && v8_datapump_take_legacy(dp) == NULL,
		      "startup escaped deadline or rate policy in scenario %u (%u samples, %d)",
		      scenario, samples, result);
		CHECK((scenario != 0 || v8_datapump_answer_tone_complete(dp)) && ops->delete(dp) == 0,
		      "silent answerer did not finish ANSam or release its probe");
		modem_delete(modem);
	}
	return true;
}

static bool test_v8_fallback_limits(void)
{
	struct modem_driver driver = {
		.name = "V.8 fallback limits",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned int restricted = 0; restricted < 3; ++restricted) {
		struct modem *caller = modem_create(&driver, "restricted caller");
		struct modem *answerer = modem_create(&driver, "automatic answerer");
		const struct dp_operations *ops = modem_dp_get_registered_ops(DP_V8);
		int16_t tx[MODEM_FRAG] = { 0 }, rx[MODEM_FRAG] = { 0 };
		int result = DPSTAT_OK;
		struct dp *calling, *answering;

		CHECK(caller != NULL && answerer != NULL && ops != NULL,
		      "fallback limit endpoints failed");
		/* V.21 must not bypass a fixed family or a 1200-bit/s floor.
		 * Conversely, selecting V.21 must not offer the higher V.32 family. */
		MODEM_AUTOMODE(caller) = 0;
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
	return true;
}

static bool test_v8_legacy_v32(void)
{
	struct modem_driver driver = {
		.name = "legacy V.32 selection",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned int scenario = 0; scenario < 8; ++scenario) {
		struct modem *modem = modem_create(&driver, "automatic answerer");
		const struct dp_operations *ops = modem_dp_get_registered_ops(DP_V8);
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
		enum DP_ID requested = scenario == 6 ? DP_V32 :
			scenario == 7 ? DP_V32BIS : DP_V92;
		bool accepted = scenario == 0 || scenario >= 5;
		dp = ops->create(modem, requested, scenario == 3,
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
		CHECK(accepted == (result == DPSTAT_CHANGEDP) &&
		      accepted == v8_datapump_v32_handoff(dp, &carrier) &&
		      modem->dp_requested == (accepted ?
			      (scenario == 6 ? DP_V32 : DP_V32BIS) : 0),
		      "legacy V.32 escaped selection limits in scenario %u", scenario);
		if (accepted)
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
	struct modem_driver driver = {
		.name = "AT dial test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};

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

static bool test_at_compression_rejects_invalid_numbers(void)
{
	static const char *const fields[] = {
		"AT+DSC=%s", "AT+DS=%s,1", "AT+DS=3,%s",
	};
	static const char *const invalid[] = {
		"AT+DSC=1,2", "AT+DSC=-0", "AT+DSC=", "AT+DS=",
		"AT+DS=1,", "AT+DS=1,1,2", "AT+DS=4,1", "AT+DS=1,4",
	};
	struct modem_driver driver = {
		.name = "AT compression numeric validation", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	char number[64], command[128];

	CHECK(m != NULL && process_at_command(m, "AT+DS=3,2") == 0,
	      "compression validation fixture could not be configured");
	for (size_t field = 0; field < ARRAY_LENGTH(fields); ++field) {
		for (unsigned int form = 0; form < 3; ++form) {
			if (form == 2)
				snprintf(number, sizeof(number), "%lu0", ULONG_MAX);
			else
				snprintf(number, sizeof(number), "%s-%lu",
					 form ? " " : "", ULONG_MAX);
			snprintf(command, sizeof(command), fields[field], number);
			CHECK(process_at_command(m, command) == -1 &&
			      modem_get_sreg(m, SREG_COMP) == 3 &&
			      modem_get_sreg(m, SREG_COMP_TYPES) == MODEM_COMP_TYPE_V44,
			      "invalid compression number changed the policy: %s", command);
		}
	}
	for (size_t index = 0; index < ARRAY_LENGTH(invalid); ++index) {
		snprintf(command, sizeof(command), "%s", invalid[index]);
		CHECK(process_at_command(m, command) == -1 &&
		      modem_get_sreg(m, SREG_COMP) == 3 &&
		      modem_get_sreg(m, SREG_COMP_TYPES) == MODEM_COMP_TYPE_V44,
		      "malformed compression setting changed the policy: %s", command);
	}
	CHECK(process_at_command(m, "AT+DSC=0x1+DS=02,0x2") == 0 &&
	      modem_get_sreg(m, SREG_COMP) == 2 &&
	      modem_get_sreg(m, SREG_COMP_TYPES) == MODEM_COMP_TYPE_V44 &&
	      process_at_command(m, "AT+DS=1") == 0 &&
	      modem_get_sreg(m, SREG_COMP) == 1 &&
	      modem_get_sreg(m, SREG_COMP_TYPES) == MODEM_COMP_TYPE_V44,
	      "compression parsing changed numeric bases, chaining or optional mask");
	modem_delete(m);
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
		      modem_get_param(modem, MDMPRM_V90_DIGITAL_INDICATION) == MODEM_PCM_POLICY_AUTO &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 0 &&
		      modem_get_param(modem, MDMPRM_V92_PCM_UPSTREAM) == 0 &&
		      modem->v90_upstream_min_rate == 0 &&
		      modem->v90_upstream_max_rate == 0 &&
		      modem->v90_upstream_max_baud == 0,
	      "PCM profile controls did not have deterministic defaults");
	CHECK(modem->v92_upstream_lu_ucode == 0,
	      "PCM upstream did not default to the profile training level");
	CHECK(process_at_command(modem, "AT+V90D=2") == 0 &&
	      modem_get_param(modem, MDMPRM_V90_DIGITAL_INDICATION) == MODEM_PCM_POLICY_AUTO &&
	      modem_set_param(modem, MDMPRM_V90_DIGITAL_INDICATION, -1) == -1 &&
	      modem_set_param(modem, MDMPRM_V90_DIGITAL_INDICATION, 3) == -1,
	      "automatic PCM role was not retained or validated");
	CHECK(process_at_command(modem, "AT+V90D=1") == 0 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == 1 &&
		      process_at_command(modem, "AT+V90D=3") == -1 &&
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
	CHECK(process_at_command(modem, "AT+V92Q=1") == 0 &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 1 &&
		      process_at_command(modem, "ATZ") == 0 &&
		      modem_get_param(
			      modem, MDMPRM_V90_DIGITAL_INDICATION) == MODEM_PCM_POLICY_AUTO &&
		      modem_get_param(modem, MDMPRM_V92_QUICK_CONNECT) == 0 &&
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

static bool test_at_pcm_rejects_invalid_numbers(void)
{
	static const struct {
		const char *command;
		unsigned long value;
	} fields[] = {
		{ "AT+V90D=%s", 1 }, { "AT+V92Q=%s", 1 },
		{ "AT+V34B=%s", 2400 },
		{ "AT+V90U=%s,31200", 14400 },
		{ "AT+V90U=14400,%s", 28800 },
		{ "AT+V90U=14400,31200,%s", 2400 },
		{ "AT+V92U=%s,28000", 1 },
		{ "AT+V92U=1,%s", 28000 },
		{ "AT+V92U=1,40000,%s", 70 },
	};
	struct modem_driver driver = {
		.name = "AT PCM numeric validation",
		.start = unused_start, .stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	char number[64], command[128];

	CHECK(m != NULL && process_at_command(m,
		"AT+V90D=0+V92Q=0+V34B=3429+V90U=14400,31200,3429"
		"+V92U=1,40000,69") == 0,
	      "PCM validation profile could not be configured");
	for (size_t index = 0; index < ARRAY_LENGTH(fields); ++index) {
		for (unsigned int form = 0; form < 3; ++form) {
			if (form == 2)
				snprintf(number, sizeof(number), "%lu0", ULONG_MAX);
			else
				snprintf(number, sizeof(number), "%s-%lu",
					 form ? " " : "",
					 ULONG_MAX - fields[index].value + 1ul);
			snprintf(command, sizeof(command), fields[index].command, number);
			CHECK(process_at_command(m, command) == -1 &&
			      modem_get_param(m, MDMPRM_V90_DIGITAL_INDICATION) == 0 &&
			      modem_get_param(m, MDMPRM_V92_QUICK_CONNECT) == 0 &&
			      m->v34_symbol_rate == 3429 &&
			      m->v90_upstream_min_rate == 14400 &&
			      m->v90_upstream_max_rate == 31200 &&
			      m->v90_upstream_max_baud == 3429 &&
			      modem_get_param(m, MDMPRM_V92_PCM_UPSTREAM) == 1 &&
			      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 13 &&
			      m->v92_upstream_lu_ucode == 69,
			      "invalid PCM number changed the profile: %s", command);
		}
	}
	CHECK(process_at_command(m, "AT+V34B=2400,2800") == -1 &&
	      m->v34_symbol_rate == 3429,
	      "surplus V.34 symbol-rate field changed the selected cadence");
	CHECK(process_at_command(m, "AT+V90D=-0") == -1 &&
	      process_at_command(m, "AT+V90D=") == -1 &&
	      process_at_command(m, "AT+V90U=,31200") == -1 &&
	      process_at_command(m, "AT+V92U=1,") == -1,
	      "unsigned PCM controls accepted a sign or omitted required value");
	CHECK(process_at_command(m,
		"AT+V34B=02400+V90U=0x3840,0x79e0,0xd65+V90D=01"
		"+V92Q=0x1+V92U=01,0x9c40,0x45") == 0 &&
	      m->v34_symbol_rate == 2400 &&
	      m->v90_upstream_min_rate == 14400 &&
	      m->v90_upstream_max_rate == 31200 &&
	      m->v90_upstream_max_baud == 3429 &&
	      modem_get_param(m, MDMPRM_V90_DIGITAL_INDICATION) == 1 &&
	      modem_get_param(m, MDMPRM_V92_QUICK_CONNECT) == 1 &&
	      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 13 &&
	      m->v92_upstream_lu_ucode == 69,
	      "shared parsing changed numeric bases or concatenated commands");
	modem_delete(m);
	return true;
}

static bool test_at_pcm_upstream_rate_queries(void)
{
	struct modem_driver driver = {
		.name = "AT PCM upstream rates", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	int response_pipe[2];
	char command[64], response[64], expected[32];

	CHECK(m != NULL && pipe(response_pipe) == 0,
	      "PCM rate query fixture could not be created");
	m->pty = response_pipe[1];
	for (uint8_t number = PCM_UPSTREAM_MIN_DATA_RATE_NUMBER;
	     number <= PCM_UPSTREAM_MAX_DATA_RATE_NUMBER; ++number) {
		uint32_t rate = pcm_upstream_bit_rate(number);
		ssize_t length;

		snprintf(command, sizeof(command), "AT+V92U=1,%u", rate);
		CHECK(process_at_command(m, command) == 0 &&
		      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == number &&
		      process_at_command(m, "AT+V92U?") == 0,
		      "PCM upstream rate number %u could not be set and queried", number);
		length = read(response_pipe[0], response, sizeof(response) - 1u);
		CHECK(length > 0, "PCM upstream query produced no response");
		response[length] = '\0';
		snprintf(expected, sizeof(expected), "1,%u\r\n", rate);
		CHECK(strstr(response, expected) != NULL,
		      "PCM upstream query disagreed with rate number %u", number);
		snprintf(command, sizeof(command), "AT+V92U=1,%u", rate + 1u);
		CHECK(process_at_command(m, command) == -1 &&
		      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == number,
		      "off-grid upstream rate changed the retained selection");
	}
	CHECK(process_at_command(m, "AT+V92U=1,4294995296") == -1 &&
	      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 19,
	      "oversized upstream rate wrapped into the shared rate type");
	close(response_pipe[1]);
	close(response_pipe[0]);
	m->pty = -1;
	modem_delete(m);
	return true;
}

static bool test_at_pcm_automatic_upstream_policy(void)
{
	static const struct {
		const char *command;
		const char *response;
		unsigned int maximum_number;
		unsigned int level;
	} cases[] = {
		{ "AT+V92U=1,0", "1\r\n", 0, 0 },
		{ "AT+V92U=1,0,69", "1,0,69\r\n", 0, 69 },
		{ "AT+V92U=1,0,0", "1\r\n", 0, 0 },
		{ "AT+V92U=1,0,127", "1,0,127\r\n", 0, 127 },
		{ "AT+V92U=1,40000,69", "1,40000,69\r\n", 13, 69 },
		{ "AT+V92U=1", "1\r\n", 0, 0 },
	};
	struct modem_driver driver = {
		.name = "AT automatic PCM upstream", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	int response_pipe[2];
	char command[96], response[64];

	CHECK(m != NULL && pipe(response_pipe) == 0,
	      "automatic PCM upstream fixture could not be created");
	m->pty = response_pipe[1];
	CHECK(process_at_command(m, "AT+V92U=?") == 0,
	      "PCM upstream capability query failed");
	ssize_t length = read(response_pipe[0], response, sizeof(response) - 1u);
	CHECK(length > 0, "PCM upstream capabilities produced no response");
	response[length] = '\0';
	CHECK(strcmp(response, "(0,1),(0,24000-48000),(0-127)\r\n") == 0,
	      "PCM upstream capabilities do not advertise automatic selection");
	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		snprintf(command, sizeof(command), "%s", cases[index].command);
		CHECK(process_at_command(m, command) == 0 &&
		      process_at_command(m, "AT+V92U?") == 0,
		      "automatic PCM policy could not be set and queried: %s", command);
		length = read(response_pipe[0], response, sizeof(response) - 1u);
		CHECK(length > 0, "automatic PCM policy query produced no response");
		response[length] = '\0';
		CHECK(strcmp(response, cases[index].response) == 0,
		      "PCM upstream query lost its automatic policy or level");
		response[strcspn(response, "\r\n")] = '\0';
		snprintf(command, sizeof(command), "AT+V92U=%s", response);
		CHECK(process_at_command(m, command) == 0 &&
		      modem_get_param(m, MDMPRM_V92_PCM_UPSTREAM) == 1 &&
		      (unsigned)modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) ==
			      cases[index].maximum_number &&
		      m->v92_upstream_lu_ucode == cases[index].level,
		      "PCM upstream query could not restore the configured policy");
	}
	CHECK(process_at_command(m, "AT+V92U=1,0,69") == 0 &&
	      process_at_command(m, "AT+V92U=1,0,128") == -1 &&
	      process_at_command(m, "AT+V92U=0,0,69") == -1 &&
	      modem_get_sreg(m, SREG_V92_PCM_UPSTREAM_MAX_DRN) == 0 &&
	      m->v92_upstream_lu_ucode == 69,
	      "invalid automatic policy changed the retained training level");
	close(response_pipe[1]);
	close(response_pipe[0]);
	m->pty = -1;
	modem_delete(m);
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

static bool exercise_lapm_dual_compression_retry(bool counteroffer)
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
	if (!counteroffer) {
		caller->cfg.ec_tx_win_size = 13;
		caller->cfg.ec_rx_win_size = 12;
	}
	CHECK(modem_comp_init(caller) == 0 && modem_ec_init(caller) == 0,
	      "compression retry initialization failed");
	modem_ec_start(caller);
	caller->state = 4;
	CHECK(modem_hdlc_get_bits(caller, 8, preamble, sizeof(preamble)) == sizeof(preamble),
	      "compression retry preamble failed");
	outgoing = caller->packer.hdlc.tx_frame;
	caller->packer.hdlc.tx_frame = NULL;
	CHECK(outgoing != NULL, "compression retry initial XID missing");
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	before = caller->ec.lapm.v44_session;
	/* The frozen dual offer also owns the V.42bis-only retry. */
	caller->cfg.comp = 0;
	caller->cfg.comp_types = MODEM_COMP_TYPE_NONE;
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
	if (counteroffer) {
		caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
		outgoing = caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
		      caller->ec.lapm.v44_session.parameters_retried &&
		      v42_xid_decode(outgoing->buf + 2, outgoing->size - 2, &fields) == V42_XID_OK &&
		      fields.parameter_negotiation_length == 19 &&
		      fields.parameter_negotiation[15] == 13 &&
		      fields.parameter_negotiation[18] == 12 &&
		      fields.private_negotiation != NULL && fields.user_data != NULL &&
		      caller->ec.lapm.tx_win_size == 15 && caller->ec.lapm.rx_win_size == 15 &&
		      caller->bit_timer == 14400,
		      "smaller response windows were applied instead of counterproposed");
		caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
		before = caller->ec.lapm.v44_session;
		caller->bit_timer = 1234;
		caller->ec.lapm.rtx_count = 3;
		received[21] = 10;
		received[24] = 11;
		caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
		CHECK(memcmp(&before, &caller->ec.lapm.v44_session, sizeof(before)) == 0 &&
		      caller->bit_timer == 1234 && caller->ec.lapm.rtx_count == 3 &&
		      caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer) == NULL,
		      "a second window counterproposal extended negotiation");
		received[21] = 12;
		received[24] = 13;
	}
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.v44_path == NULL &&
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
	CHECK(caller->ec.lapm.v44_path == NULL &&
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
	modem_ec_exit(caller);
	modem_comp_exit(caller);
	modem_delete(caller);
	return true;
}

static bool test_lapm_dual_compression_retry(void)
{
	return exercise_lapm_dual_compression_retry(false);
}

static bool test_lapm_xid_window_counteroffer(void)
{
	return exercise_lapm_dual_compression_retry(true);
}

static bool test_lapm_xid_parameters(void)
{
	static const uint8_t expected_parameters[] = {
		0x03, 0x03, 0x8a, 0x89, 0x00,
		0x05, 0x02, 0x02, 0x98, /* transmit N401: 83 octets, 664 bits */
		0x06, 0x02, 0x03, 0xb8, /* receive N401: 119 octets, 952 bits */
		0x07, 0x01, 0x07,
		0x08, 0x01, 0x0b,
	};
	uint8_t expected_private[] = {
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

	for (size_t scenario = 0; scenario < 3 * ARRAY_LENGTH(compression_types); ++scenario) {
		size_t index = scenario / 3;
		unsigned int directions = scenario % 3 + 1;
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
		modem->cfg.comp = directions;
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
			/* P0 is relative to the negotiation initiator, while cfg is RX/TX. */
			expected_private[7] = ((directions & 1) << 1) | ((directions & 2) >> 1);
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

static bool start_lapm_compression_fixture(struct modem *m, bool response)
{
	uint8_t preamble[109];
	frame_t *outgoing;

	m->caller = response;
	m->rx_rate = m->tx_rate = 2400;
	CHECK(modem_comp_init(m) == 0 && modem_ec_init(m) == 0,
	      "compression fixture could not initialize");
	modem_ec_start(m);
	m->state = 4; /* Error-control establishment. */
	if (response) {
		CHECK(modem_hdlc_get_bits(m, 8, preamble, sizeof(preamble)) ==
			      (int)sizeof(preamble), "compression fixture preamble failed");
		outgoing = m->packer.hdlc.tx_frame;
		m->packer.hdlc.tx_frame = NULL;
		CHECK(outgoing != NULL, "compression fixture command was missing");
		m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
	}
	return true;
}

static bool test_lapm_v42bis_reinitialization(void)
{
	struct modem_driver driver = {
		.name = "LAPM V.42bis initialization", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const unsigned strings[] = { 6, 250 };
	uint8_t payload[40], decoded[128];
	char encoded[1024];

	for (size_t i = 0; i < sizeof(payload); ++i)
		payload[i] = (uint8_t)(i % 7); /* Includes transparent-mode escapes. */
	for (unsigned response = 0; response < 2; ++response)
	for (size_t limit = 0; limit < ARRAY_LENGTH(strings); ++limit) {
		struct modem *m = modem_create(&driver, driver.name);
		struct modem *peer = modem_create(&driver, driver.name);
		uint8_t wire[] = {
			0, 0xaf, 0x82, 0xf0, 0, 15,
			0, 3, 'V', '4', '2', 1, 1, 3, 2, 2, 2, 0, 3, 1, strings[limit],
		};
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		frame_t *outgoing;
		size_t length = 0;

		CHECK(m != NULL && peer != NULL, "V.42bis initialization allocation failed");
		m->cfg.comp = peer->cfg.comp = 3;
		m->cfg.comp_types = peer->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = peer->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = peer->cfg.comp_max_string = strings[limit];
		CHECK(start_lapm_compression_fixture(m, response) && modem_comp_init(peer) == 0,
		      "V.42bis initialization fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = response ? l->cmd_addr : l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == (response ? 0x7f : 0xaf),
		      "V.42bis initialization did not finish negotiation");
		m->packer.hdlc.tx_complete(l, outgoing);
		/* Discard old codec data before a destructive link initialization. */
		for (unsigned i = 0; i < 1024; ++i)
			CHECK(modem_comp_encode(m, 'A', encoded, sizeof(encoded)) >= 0,
			      "V.42bis history fixture could not encode");
		CHECK(modem_comp_flush_encoder(m, encoded, sizeof(encoded)) >= 0 &&
		      modem_comp_decode(m, 'B', encoded, sizeof(encoded)) >= 0 &&
		      m->comp.encoder.mode == COMPRESSED && m->comp.decoder.str_len == 1,
		      "V.42bis history fixture did not retain stream state");
		m->comp.head = m->comp.count = 1;
		m->comp.buf[1] = 'P';
		struct lapm_v44_session session = l->v44_session;
		wire[1] = response ? 0x73 : 0x7f;
		incoming.count = 2;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->state == 5 && m->cfg.comp == 3 &&
		      m->comp.encoder.next_free == 259 && m->comp.decoder.next_free == 259 &&
		      m->comp.encoder.mode == TRANSPARENT && !m->comp.encoder.raw_bits &&
		      !m->comp.encoder.bit_len && !m->comp.encoder.olen &&
		      !m->comp.decoder.str_len && !m->comp.count && !m->comp.head &&
		      m->comp.encoder.dict_size == 512 &&
		      m->comp.encoder.max_string == (int)strings[limit] &&
		      memcmp(&l->v44_session, &session, sizeof(session)) == 0,
		      "LAPM initialization retained V.42bis history or changed its selection");
		if (!response) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->buf[1] == 0x73,
			      "V.42bis initialization did not acknowledge SABME");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		CHECK(modem_write(m, (const char *)payload, sizeof(payload)) == sizeof(payload),
		      "reinitialized V.42bis link rejected terminal data");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->size > 3 && outgoing->buf[1] == 0 &&
		      m->xmit.count == 0, "reinitialized V.42bis link did not send new data");
		for (int i = 3; i < outgoing->size; ++i) {
			int count = modem_comp_decode(peer, outgoing->buf[i],
				(char *)decoded + length, (int)(sizeof(decoded) - length));
			CHECK(count >= 0, "fresh peer rejected reinitialized V.42bis data");
			length += (size_t)count;
		}
		int count = modem_comp_flush_decoder(peer, (char *)decoded + length,
			(int)(sizeof(decoded) - length));
		CHECK(count >= 0 && length + (size_t)count == sizeof(payload) &&
		      memcmp(decoded, payload, sizeof(payload)) == 0,
		      "reinitialized V.42bis encoder disagreed with a fresh peer");
		m->packer.hdlc.tx_complete(l, outgoing);
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_comp_exit(peer);
		modem_delete(m);
		modem_delete(peer);
	}
	/* C-INIT failure cannot leave a healthy carrier with unusable compression. */
	for (unsigned response = 0; response < 2; ++response) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = {
			0, 0xaf, 0x82, 0xf0, 0, 15,
			0, 3, 'V', '4', '2', 1, 1, 3, 2, 2, 2, 0, 3, 1, 6,
		};
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		frame_t *outgoing;

		CHECK(m != NULL, "missing-codec allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = 6;
		CHECK(start_lapm_compression_fixture(m, response), "missing-codec fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = response ? l->cmd_addr : l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == (response ? 0x7f : 0xaf),
		      "missing-codec fixture did not negotiate");
		m->packer.hdlc.tx_complete(l, outgoing);
		/* Lose the codec after negotiation, immediately before C-INIT. */
		modem_comp_exit(m);
		wire[1] = response ? 0x73 : 0x7f;
		incoming.count = 2;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 0 && !modem_ec_can_establish(m) && !m->bit_timer &&
		      strcmp(modem_call_phase(m), "cleardown") == 0,
		      "failed V.42bis initialization left establishment active");
		modem_ec_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_v42bis_negotiation(void)
{
	static const struct {
		unsigned int local_dict, local_string, peer_dict, peer_string;
	} limits[] = {
		{ 512, 6, 4096, 32 },
		{ 1024, 12, 768, 10 },
		{ 512, 6, 65535, 250 },
		{ 512, 6, 512, 6 },
	};
	struct modem_driver driver = {
		.name = "LAPM V.42bis negotiation", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	/* Both XID directions, every P0 combination, policy, and unequal capacities. */
	for (unsigned int scenario = 0; scenario < 64 * ARRAY_LENGTH(limits); ++scenario) {
		unsigned int capacity = scenario / 64;
		unsigned int peer = scenario & 3;
		unsigned int local = (scenario >> 2) & 3;
		bool supported = (scenario & 16) != 0;
		bool response = (scenario & 32) != 0;
		unsigned int peer_local = response ? ((peer & 1) << 1) | ((peer & 2) >> 1) : peer;
		unsigned int allowed = supported ? local : 0;
		bool invalid_response = response && (peer_local & ~allowed) != 0;
		unsigned int selected = peer_local & allowed;
		unsigned int dict_size = limits[capacity].local_dict;
		unsigned int max_string = limits[capacity].local_string;
		unsigned int peer_dict = limits[capacity].peer_dict;
		unsigned int peer_string = limits[capacity].peer_string;
		uint8_t parameters[] = {
			0, 3, 'V', '4', '2', 1, 1, peer,
			2, 2, peer_dict >> 8, peer_dict, 3, 1, peer_string,
		};
		struct v42_xid_fields fields = {
			.private_negotiation = parameters,
			.private_negotiation_length = sizeof(parameters),
		};
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[64];
		size_t length;
		frame_t incoming = { .buf = wire };
		frame_t *outgoing;
		int timer;

		CHECK(m != NULL, "compression modem allocation failed");
		m->cfg.comp = local;
		m->cfg.comp_types = supported ? MODEM_COMP_TYPE_V42BIS : MODEM_COMP_TYPE_V44;
		m->cfg.comp_dict_size = dict_size;
		m->cfg.comp_max_string = max_string;
		CHECK(start_lapm_compression_fixture(m, response), "compression fixture failed");
		timer = m->bit_timer;
		wire[0] = response ? m->ec.lapm.cmd_addr : m->ec.lapm.rsp_addr;
		wire[1] = 0xaf;
		CHECK(v42_xid_encode(&fields, wire + 2, sizeof(wire) - 2, &length) == V42_XID_OK,
		      "compression XID encoding failed");
		incoming.count = (int)length + 2;
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		if (invalid_response) {
			CHECK(m->bit_timer == timer && m->cfg.comp == local &&
				      m->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING,
			      "compression response %u accepted a direction we did not offer", scenario);
		} else {
			CHECK(m->cfg.comp == selected, "compression case %u selected directions %d, expected %u",
			      scenario, m->cfg.comp, selected);
			if (selected) {
				unsigned int selected_dict = peer_dict < dict_size ? peer_dict : dict_size;
				unsigned int selected_string = peer_string < max_string ? peer_string : max_string;

				CHECK(m->cfg.comp_dict_size == (int)selected_dict &&
					      m->cfg.comp_max_string == (int)selected_string &&
					      m->comp.encoder.dict_size == (int)selected_dict &&
					      m->comp.decoder.dict_size == (int)selected_dict &&
					      m->comp.encoder.max_string == (int)selected_string &&
					      m->comp.decoder.max_string == (int)selected_string,
				      "compression case %u did not select the smaller allocated limits", scenario);
			}
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			CHECK(outgoing != NULL, "compression case %u did not queue its reply", scenario);
			if (!response) {
				CHECK(outgoing->buf[1] == 0xaf &&
					      v42_xid_decode(outgoing->buf + 2, outgoing->size - 2, &fields) == V42_XID_OK,
				      "compression response could not be decoded");
				if (selected) {
					CHECK(fields.private_negotiation_length == sizeof(parameters) &&
						      fields.private_negotiation[7] == selected &&
						      ((fields.private_negotiation[10] << 8) |
						       fields.private_negotiation[11]) == (int)m->cfg.comp_dict_size &&
						      fields.private_negotiation[14] == m->cfg.comp_max_string,
					      "compression reply advertised different directions or limits from its codec");
				} else {
					CHECK(fields.private_negotiation == NULL,
					      "compression reply offered a disabled capability");
				}
			}
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		}
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_v42bis_frozen_offer(void)
{
	static const unsigned limits[][2] = { { 512, 6 }, { 1024, 12 } };
	struct modem_driver driver = {
		.name = "LAPM frozen V.42bis offer", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t limit = 0; limit < ARRAY_LENGTH(limits); ++limit)
	for (unsigned scenario = 0; scenario < 64; ++scenario) {
		unsigned local = scenario & 3;
		unsigned peer = (scenario >> 2) & 3;
		bool supported = (scenario & 16) != 0;
		bool expanded = (scenario & 32) != 0;
		unsigned selected = ((peer & 1) << 1) | ((peer & 2) >> 1);
		bool invalid = (selected & ~(supported ? local : 0)) != 0;
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = {
			0, 0xaf, 0x82, 0xf0, 0, 15,
			0, 3, 'V', '4', '2', 1, 1, peer, 2, 2, 16, 0, 3, 1, 32,
		};
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		frame_t *outgoing;

		CHECK(m != NULL, "frozen compression modem allocation failed");
		m->cfg.comp = local;
		m->cfg.comp_types = supported ? MODEM_COMP_TYPE_V42BIS : MODEM_COMP_TYPE_NONE;
		m->cfg.comp_dict_size = limits[limit][0];
		m->cfg.comp_max_string = limits[limit][1];
		CHECK(start_lapm_compression_fixture(m, true), "frozen compression fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		struct lapm_v44_session before = l->v44_session;
		int timer = m->bit_timer;

		/* Change policy after the command has gone onto the wire. */
		m->cfg.comp = expanded ? 3 : 0;
		m->cfg.comp_types = expanded ? MODEM_COMP_TYPE_ALL : MODEM_COMP_TYPE_NONE;
		m->cfg.comp_dict_size = expanded ? 4096 : 512;
		m->cfg.comp_max_string = expanded ? 250 : 6;
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		if (invalid) {
			CHECK(outgoing == NULL && l->v44_session.phase == LAPM_V44_COMMAND_PENDING &&
			      m->bit_timer == timer &&
			      m->cfg.comp == (expanded ? 3u : 0u) &&
			      memcmp(&l->v44_session, &before, sizeof(before)) == 0,
			      "frozen offer accepted unoffered directions: limit=%zu case=%u", limit, scenario);
			m->bit_timer_func(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == (int)before.command_length + 2 &&
			      memcmp(outgoing->buf + 2, before.command, before.command_length) == 0,
			      "rejected compression response changed the frozen retry");
		} else {
			CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f &&
			      l->v44_session.phase != LAPM_V44_COMMAND_PENDING &&
			      m->cfg.comp == selected && m->state == 4,
			      "frozen compression reply used current policy: limit=%zu case=%u", limit, scenario);
			if (selected) {
				CHECK(modem_ec_active_compression(m) == MODEM_EC_COMPRESSION_V42BIS &&
				      m->cfg.comp_dict_size == (int)limits[limit][0] &&
				      m->cfg.comp_max_string == (int)limits[limit][1] &&
				      m->comp.encoder.dict_size == (int)limits[limit][0] &&
				      m->comp.decoder.dict_size == (int)limits[limit][0] &&
				      m->comp.encoder.max_string == (int)limits[limit][1] &&
				      m->comp.decoder.max_string == (int)limits[limit][1],
				      "frozen compression limits disagree with their codec: limit=%zu case=%u", limit, scenario);
			}
		}
		m->packer.hdlc.tx_complete(l, outgoing);
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_v42bis_configuration_failure(void)
{
	struct modem_driver driver = {
		.name = "LAPM V.42bis configuration failure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned response = 0; response < 2; ++response)
	for (unsigned missing = 0; missing < 2; ++missing) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = {
			0, 0xaf, 0x82, 0xf0, 0, 15,
			0, 3, 'V', '4', '2', 1, 1, 3, 2, 2, 4, 0, 3, 1, 12,
		};
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };

		CHECK(m != NULL, "failed configuration modem allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 1024;
		m->cfg.comp_max_string = 12;
		m->cfg.ec_tx_info_size = m->cfg.ec_rx_info_size = 70;
		m->cfg.ec_tx_win_size = m->cfg.ec_rx_win_size = 2;
		CHECK(start_lapm_compression_fixture(m, response), "failed configuration fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		if (missing)
			modem_comp_exit(m);
		else
			CHECK(modem_comp_config(m, 512, 6) == 0, "smaller codec fixture failed");
		int dict = m->comp.encoder.dict_size;
		int max_string = m->comp.encoder.max_string;
		wire[0] = response ? l->cmd_addr : l->rsp_addr;
		/* A failed response must also be absent from the duplicate cache. */
		for (unsigned attempt = 0; attempt < 2; ++attempt) {
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(!modem_ec_can_establish(m) && !m->bit_timer &&
			      strcmp(modem_call_phase(m), "cleardown") == 0 &&
			      m->packer.hdlc.get_tx_frame(l) == NULL &&
			      l->v44_session.phase == LAPM_V44_READY &&
			      !l->v44_session.command_length && !l->v44_session.response_length &&
			      m->cfg.comp == 3 && m->cfg.comp_dict_size == 1024 && m->cfg.comp_max_string == 12 &&
			      l->tx_info_size == 70 && l->rx_info_size == 70 &&
			      l->tx_win_size == 2 && l->rx_win_size == 2 &&
			      m->comp.encoder.dict_size == dict && m->comp.decoder.dict_size == dict &&
			      m->comp.encoder.max_string == max_string && m->comp.decoder.max_string == max_string,
			      "failed codec setup advertised or applied configuration: response=%u missing=%u", response, missing);
		}
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_v42bis_invalid_parameters(void)
{
	static const struct {
		uint8_t field[16];
		size_t length;
		bool fatal;
		bool ignored;
	} cases[] = {
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 2, 2, 1, 255 }, 12, true, false },
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 2, 2, 0, 0 }, 12, true, false },
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 3, 1, 5 }, 11, true, false },
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 3, 1, 251 }, 11, true, false },
		{ { 0, 3, 'V', '4', '2', 1, 1, 4 }, 8, false, true },
		{ { 0, 3, 'V', '4', '2', 1, 2, 3, 0 }, 9, false, true },
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 2, 3, 1, 2, 0 }, 13, false, true },
		{ { 0, 3, 'V', '4', '2', 1, 1, 3, 3, 2, 0, 6 }, 12, false, true },
		{ { 0, 3, 'X', '4', '2', 1, 1, 3, 2, 2, 1, 255 }, 12, false, false },
		{ { 1, 1, 3, 2, 2, 2, 0 }, 7, false, false },
		{ { 0, 3, 'V', '4', '2', 2, 2, 2, 0, 3, 1, 6 }, 12, false, false },
	};
	struct modem_driver driver = {
		.name = "LAPM invalid V.42bis parameters", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t scenario = 0; scenario < 2 * ARRAY_LENGTH(cases); ++scenario) {
		size_t index = scenario / 2;
		bool response = (scenario & 1) != 0;
		struct modem *m = modem_create(&driver, driver.name);
		struct v42_xid_fields fields = {
			.private_negotiation = cases[index].field,
			.private_negotiation_length = cases[index].length,
		};
		uint8_t wire[64];
		size_t length;
		frame_t incoming = { .buf = wire };
		int timer;

		CHECK(m != NULL, "invalid compression modem allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = 6;
		CHECK(start_lapm_compression_fixture(m, response), "invalid compression fixture failed");
		timer = m->bit_timer;
		wire[0] = response ? m->ec.lapm.cmd_addr : m->ec.lapm.rsp_addr;
		wire[1] = 0xaf;
		CHECK(v42_xid_encode(&fields, wire + 2, sizeof(wire) - 2, &length) == V42_XID_OK,
		      "invalid compression XID envelope could not be encoded");
		incoming.count = (int)length + 2;
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		if (cases[index].fatal) {
			CHECK(!modem_ec_can_establish(m) && m->result_code != 0,
			      "invalid compression limit %zu did not disconnect", scenario);
		} else if (cases[index].ignored) {
			CHECK(m->state == 4 && m->cfg.comp == 3 && m->bit_timer == timer &&
				      (!response || m->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING),
			      "malformed compression parameter %zu changed negotiation", scenario);
		} else {
			CHECK(m->cfg.comp == 0 &&
			      (!response || m->ec.lapm.v44_session.phase != LAPM_V44_COMMAND_PENDING),
			      "unknown private parameter set %zu selected V.42bis", scenario);
		}
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_v42bis_decoder_failure(void)
{
	static const uint8_t payloads[][5] = {
		{ 0, 0, 2, 0, 4 }, /* ECM, impossible STEPUP, trailing data. */
		{ 0, 0, 4, 1 },    /* ECM, unassigned codeword 260 at frame end. */
	};
	static const size_t lengths[] = { 5, 4 };
	struct modem_driver driver = {
		.name = "LAPM V.42bis decoder failure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t scenario = 0; scenario < ARRAY_LENGTH(payloads); ++scenario) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[8] = { 0 };
		frame_t incoming = { .buf = wire, .count = 3 + lengths[scenario] };

		CHECK(m != NULL, "decoder failure modem allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = 6;
		CHECK(start_lapm_compression_fixture(m, false), "decoder failure fixture failed");
		m->ec.lapm.state = 2; /* Established LAPM data state. */
		modem_update_status(m, STATUS_EC_LINK);
		CHECK(m->state == 5 && (m->modem_info & TIOCM_CD) && m->put_chars != NULL,
		      "decoder failure fixture did not go online");
		wire[0] = m->ec.lapm.rsp_addr;
		memcpy(wire + 3, payloads[scenario], lengths[scenario]);
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		CHECK(!modem_ec_can_establish(m) && !(m->modem_info & TIOCM_CD) &&
			      m->result_code == 3 && m->sample_timer != 0 &&
			      m->ec.lapm.vr == 0 && m->ec.lapm.rx_count == 0 && m->packer_process == NULL,
		      "decoder error %zu was acknowledged or retained instead of releasing the call", scenario);
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool fill_lapm_control_queue(struct modem *m)
{
	struct lapm_state *l = &m->ec.lapm;
	uint8_t wire[] = { l->rsp_addr, 5, (uint8_t)((l->va << 1) | 1) };
	frame_t poll = { .buf = wire, .count = sizeof(wire) };

	for (unsigned request = 0; request < LAPM_CTRL_FRAMES &&
	     l->ctrl_list->next != l->tx_ctrl; ++request)
		m->packer.hdlc.rx_complete(l, &poll);
	return l->ctrl_list->next == l->tx_ctrl;
}

static bool test_lapm_v42bis_receive_pressure(void)
{
	static const char *const payloads[] = { "B", "BA", "BAC" };
	struct modem_driver driver = {
		.name = "LAPM V.42bis output pressure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t scenario = 0; scenario < ARRAY_LENGTH(payloads); ++scenario) {
		struct modem *m = modem_create(&driver, driver.name);
		int pipe_fds[2], original_pty;
		char filler[4096] = { 0 }, output[8];
		uint8_t wire[6] = { 0 };
		size_t length = strlen(payloads[scenario]);
		frame_t incoming = { .buf = wire, .count = 3 + length };

		CHECK(m != NULL, "receive-pressure modem allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = 34;
		CHECK(start_lapm_compression_fixture(m, false), "receive-pressure fixture failed");
		m->ec.lapm.state = 2;
		modem_update_status(m, STATUS_EC_LINK);
		CHECK(pipe(pipe_fds) == 0 &&
			      fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == 0 &&
			      fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK) == 0,
		      "receive-pressure pipe setup failed");
		while (write(pipe_fds[1], filler, sizeof(filler)) > 0) {}
		CHECK(errno == EAGAIN, "receive-pressure pipe did not fill");
		original_pty = m->pty;
		m->pty = pipe_fds[1];
		wire[0] = m->ec.lapm.rsp_addr;
		memcpy(wire + 3, payloads[scenario], length);
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		CHECK(m->ec.lapm.busy && m->packer_process != NULL &&
			      m->comp.count == 1 && m->comp.buf[m->comp.head] == 'B' &&
			      m->ec.lapm.vr == 1 && m->ec.lapm.rx_count == (length > 2 ? 1 : 0),
		      "compressed receive case %zu lost its blocked output or retry", scenario);
		m->packer_process(m, 0);
		CHECK(m->ec.lapm.busy && m->comp.count == 1,
		      "compressed receive resumed before output became available");
		CHECK(fill_lapm_control_queue(m), "compressed receive control queue did not fill");
		while (read(pipe_fds[0], filler, sizeof(filler)) > 0) {}
		for (unsigned retry = 0; retry < 3; ++retry) {
			CHECK(m->packer_process != NULL, "compressed receive lost its ready retry");
			m->packer_process(m, 0);
			CHECK(m->ec.lapm.busy && m->ec.lapm.rx_count == 0 &&
			      m->comp.count == 0 && m->comp.decoder.str_len == 0,
			      "compressed receive did not drain once while awaiting a control slot");
		}
		frame_t *outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm);
		CHECK(outgoing != NULL && outgoing->buf[1] == 5,
		      "compressed receive lost its queued busy response");
		m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
		m->packer_process(m, 0);
		CHECK(read(pipe_fds[0], output, sizeof(output)) == (ssize_t)length &&
			      memcmp(output, payloads[scenario], length) == 0 &&
			      !m->ec.lapm.busy && m->ec.lapm.rx_count == 0 &&
			      m->comp.count == 0 && m->comp.decoder.str_len == 0 &&
			      m->packer_process == NULL,
		      "compressed receive case %zu stalled or reordered bytes after pressure cleared", scenario);
		unsigned ready = 0;
		while ((outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm)) != NULL) {
			CHECK(outgoing->size == 3 && outgoing->buf[2] >> 1 == 1 &&
			      (outgoing->buf[1] == 5 || outgoing->buf[1] == 1),
			      "compressed receive queued an invalid status frame");
			ready += outgoing->buf[1] == 1;
			m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
		}
		CHECK(ready == 1 && read(pipe_fds[0], output, sizeof(output)) < 0 && errno == EAGAIN,
		      "compressed receive lost its ready frame or repeated decoded output");
		m->pty = original_pty;
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static int lapm_v44_source(void *context, uint8_t *output, size_t capacity)
{
	return capacity > INT_MAX ? -1 :
		scripted_lapm_source(context, (char *)output, (int)capacity);
}

static int lapm_v44_sink(void *context, const uint8_t *input, size_t length)
{
	return length > INT_MAX ? -1 :
		scripted_lapm_sink(context, (const char *)input, (int)length);
}

static bool test_lapm_v44_receive_pressure(void)
{
	static const uint8_t payload[] = "Accepted input must retain pending output.";
	const struct v44_parameters parameters = {
		.codeword_limit = V44_MIN_CODEWORDS,
		.max_string_length = V44_MIN_STRING_LENGTH,
		.history_size = V44_MIN_HISTORY_SIZE,
	};
	struct modem_driver driver = {
		.name = "LAPM V.44 output pressure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, driver.name);
	uint8_t wire[512];
	size_t wire_length = 0;
	bool idle = false, checked = false;

	CHECK(m != NULL, "V.44 pressure allocation failed");
	m->cfg.comp = 0;
	CHECK(modem_ec_init(m) == 0, "V.44 pressure initialization failed");
	modem_ec_start(m);
	m->ec.lapm.state = 2;
	m->state = 5;
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	lapm_source.data = payload;
	lapm_source.length = sizeof(payload) - 1;
	struct v44_character_path_config config = {
		.codec = { .transmit_enabled = true, .transmit_parameters = parameters },
		.receive_buffer_capacity = 1, .context = m,
		.read = lapm_v44_source, .write = lapm_v44_sink,
	};
	struct v44_character_path *sender = v44_character_path_create(&config);
	CHECK(sender != NULL, "V.44 pressure sender failed");
	for (unsigned attempt = 0; attempt < sizeof(wire) && !idle; ++attempt) {
		size_t length = 0;
		enum v44_character_path_result result = v44_character_path_transmit(
			sender, wire + wire_length, sizeof(wire) - wire_length, &length);
		CHECK((result == V44_CHARACTER_PATH_OK || result == V44_CHARACTER_PATH_IDLE) &&
		      length <= sizeof(wire) - wire_length, "V.44 pressure source failed");
		wire_length += length;
		idle = result == V44_CHARACTER_PATH_IDLE;
	}
	v44_character_path_destroy(sender);
	CHECK(idle && wire_length > 0 && lapm_source.offset == sizeof(payload) - 1,
	      "V.44 pressure wire did not finish");
	config.codec.transmit_enabled = false;
	config.codec.receive_enabled = true;
	config.codec.receive_parameters = parameters;
	m->ec.lapm.v44_path = v44_character_path_create(&config);
	CHECK(m->ec.lapm.v44_path != NULL, "V.44 pressure receiver failed");
	for (size_t index = 0; index < wire_length; ++index) {
		uint8_t frame[] = { m->ec.lapm.rsp_addr, (uint8_t)(m->ec.lapm.vr << 1), 0, wire[index] };
		frame_t incoming = { .buf = frame, .count = sizeof(frame) };
		frame_t *outgoing;

		m->packer.hdlc.rx_complete(&m->ec.lapm, &incoming);
		if (m->ec.lapm.busy) {
			CHECK(!checked && m->ec.lapm.rx_count == 0 && m->packer_process != NULL &&
			      lapm_sink.length == 0, "V.44 lost pending output after the final input byte");
			outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm);
			CHECK(outgoing != NULL && outgoing->buf[1] == 5 &&
			      outgoing->buf[2] >> 1 == m->ec.lapm.vr,
			      "V.44 blocked output did not acknowledge its input with RNR");
			m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
			m->packer_process(m, 0);
			CHECK(m->ec.lapm.busy && m->ec.lapm.rx_count == 0 && lapm_sink.length == 0,
			      "zero-input retry lost blocked V.44 output");
			CHECK(fill_lapm_control_queue(m), "V.44 control queue did not fill");
			lapm_sink.result = 1;
			for (unsigned retry = 0; retry < 3; ++retry) {
				CHECK(m->packer_process != NULL, "V.44 lost its ready retry");
				m->packer_process(m, 0);
				CHECK(m->ec.lapm.busy && m->ec.lapm.rx_count == 0 &&
				      lapm_sink.length == 1 && lapm_sink.data[0] == payload[0],
				      "V.44 did not drain once while awaiting a control slot");
			}
			outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm);
			CHECK(outgoing != NULL && outgoing->buf[1] == 5,
			      "V.44 lost its queued busy response");
			m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
			m->packer_process(m, 0);
			CHECK(!m->ec.lapm.busy && m->ec.lapm.rx_count == 0 && m->packer_process == NULL &&
			      lapm_sink.length == 1 && lapm_sink.data[0] == payload[0],
			      "V.44 output did not drain once and clear busy");
			unsigned ready = 0;
			while ((outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm)) != NULL) {
				CHECK(outgoing->size == 3 && outgoing->buf[2] >> 1 == m->ec.lapm.vr &&
				      (outgoing->buf[1] == 5 || outgoing->buf[1] == 1),
				      "V.44 queued an invalid status frame");
				ready += outgoing->buf[1] == 1;
				m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
			}
			CHECK(ready == 1, "V.44 drained output did not publish exactly one RR");
			checked = true;
		}
		while ((outgoing = m->packer.hdlc.get_tx_frame(&m->ec.lapm)) != NULL)
			m->packer.hdlc.tx_complete(&m->ec.lapm, outgoing);
	}
	CHECK(checked && lapm_sink.length == sizeof(payload) - 1 &&
	      memcmp(lapm_sink.data, payload, sizeof(payload) - 1) == 0 &&
	      v44_character_path_finish_receive(m->ec.lapm.v44_path) == V44_CHARACTER_PATH_OK,
	      "V.44 pressure lost or repeated decoded characters");
	modem_delete(m);
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_v42bis_transmit_pressure(void)
{
	static const int capacities[] = { 1, 3, 128 };
	static const int strings[] = { 34, 250 };
	struct modem_driver driver = {
		.name = "LAPM V.42bis small transmit buffers", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t limit = 0; limit < ARRAY_LENGTH(strings); ++limit)
	for (size_t capacity = 0; capacity < ARRAY_LENGTH(capacities); ++capacity) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t expected[1024];
		char output[2048], wire[128];
		size_t received = 0;

		CHECK(m != NULL, "transmit-pressure modem allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = strings[limit];
		CHECK(start_lapm_compression_fixture(m, false), "transmit-pressure fixture failed");
		m->ec.lapm.state = 2;
		modem_update_status(m, STATUS_EC_LINK);
		for (size_t index = 0; index < sizeof(expected); ++index)
			expected[index] = (uint8_t)(index * 23 + 7);
		CHECK(m->xmit.size > (int)sizeof(expected), "transmit-pressure payload exceeds ring capacity");
		memcpy(m->xmit.buf, expected, sizeof(expected));
		m->xmit.head = m->xmit.count = sizeof(expected);
		m->xmit.tail = 0;
		for (unsigned iteration = 0; iteration < 8192; ++iteration) {
			int count = m->get_chars(m, wire, capacities[capacity]);

			CHECK(count >= 0 && count <= capacities[capacity], "small-buffer encoding failed");
			if (count == 0)
				break;
			for (int byte = 0; byte < count; ++byte) {
				int decoded = modem_comp_decode(m, (uint8_t)wire[byte],
					output + received, sizeof(output) - received);

				CHECK(decoded >= 0, "small-buffer decoding failed");
				received += (size_t)decoded;
			}
		}
		int count = modem_comp_flush_decoder(m, output + received, sizeof(output) - received);
		CHECK(count >= 0, "small-buffer decoder flush failed");
		received += (size_t)count;
		CHECK(received == sizeof(expected) && memcmp(output, expected, sizeof(expected)) == 0 &&
			      m->xmit.count == 0 && m->comp.encoder.olen == 0,
		      "capacity %d with string limit %d consumed input while draining prior output",
		      capacities[capacity], strings[limit]);
		modem_ec_exit(m);
		modem_comp_exit(m);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_xid_directions(void)
{
	static const uint8_t peer_parameters[] = {
		0x03, 0x04, 0x8e, 0x89, 0x00, 0x00, /* request unsupported SREJ */
		0x05, 0x02, 0x02, 0x98, /* peer transmit: 83 octets */
		0x06, 0x02, 0x03, 0xb8, /* peer receive: 119 octets */
		0x07, 0x01, 0x07,
		0x08, 0x01, 0x0b,
	};
	static const uint8_t local_parameters[] = {
		0x03, 0x03, 0x8a, 0x89, 0x00,
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
		uint8_t wire[26] = { 0, 0xaf, 0x82, 0x80, 0, 0 };
		const uint8_t *parameters = response ? local_parameters : peer_parameters;
		const size_t parameter_length = response ?
			sizeof(local_parameters) : sizeof(peer_parameters);
		frame_t incoming = { .buf = wire, .count = 6 + parameter_length };
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
		wire[5] = (uint8_t)parameter_length;
		memcpy(wire + 6, parameters, parameter_length);
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
			CHECK(outgoing->buf[1] == 0x7f &&
			      modem->ec.lapm.v44_session.phase != LAPM_V44_COMMAND_PENDING,
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

static bool test_lapm_xid_numeric_values(void)
{
	static const struct {
		uint8_t bytes[5];
		uint8_t length, parameter;
		bool command_ok, response_ok;
		unsigned int command_value, response_value;
	} cases[] = {
		{ { 2, 0x98 }, 2, 5, true, true, 83, 83 },
		{ { 2, 0 }, 2, 5, true, false, 70, 0 },
		{ { 4, 0 }, 2, 5, true, true, 128, 128 },
		{ { 4, 8 }, 2, 5, true, false, 128, 0 },
		{ { 0xa0 }, 1, 5, true, false, 70, 0 },
		{ { 0, 0, 0, 2, 0x98 }, 5, 5, true, true, 83, 83 },
		{ { 0xff, 0, 0, 2, 0x98 }, 5, 5, false, false, 0, 0 },
		{ { 2, 0x99 }, 2, 5, false, false, 0, 0 },
		{ { 0 }, 0, 5, false, false, 0, 0 },
		{ { 3 }, 1, 7, true, true, 3, 3 },
		{ { 1 }, 1, 7, true, false, 2, 0 },
		{ { 15 }, 1, 7, true, true, 15, 15 },
		{ { 16 }, 1, 7, true, false, 15, 0 },
		{ { 127 }, 1, 7, true, false, 15, 0 },
		{ { 128 }, 1, 7, false, false, 0, 0 },
		{ { 0 }, 1, 7, false, false, 0, 0 },
		{ { 0, 0, 0, 0, 3 }, 5, 7, true, true, 3, 3 },
		{ { 0xff, 0, 0, 0, 3 }, 5, 7, false, false, 0, 0 },
		{ { 0 }, 0, 7, false, false, 0, 0 },
		{ { 0xff, 0, 0, 0, 3 }, 5, 0xff, true, true, 15, 15 },
		{ { 0 }, 0, 0xff, true, true, 15, 15 },
		{ { 0 }, 0, 0, true, true, 15, 15 },
	};
	struct modem_driver driver = {
		.name = "LAPM XID numeric values", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
	for (unsigned direction = 0; direction < 2; ++direction)
	for (unsigned response = 0; response < 2; ++response) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t preamble[109], command[130], parameters[7], wire[32];
		size_t length;
		int command_size = 0, timer;
		bool info = cases[index].parameter == 5;
		bool valid = response ? cases[index].response_ok : cases[index].command_ok;
		bool counteroffer = response && cases[index].parameter == 7 &&
			cases[index].length == 1 && cases[index].bytes[0] == 1;
		unsigned int value = response ? cases[index].response_value : cases[index].command_value;
		struct v42_xid_fields fields = {
			.parameter_negotiation = cases[index].parameter ? parameters : NULL,
			.parameter_negotiation_length = cases[index].parameter ? 2 + cases[index].length : 0,
		};
		frame_t incoming = { .buf = wire };
		frame_t *outgoing;

		CHECK(m != NULL, "numeric XID modem allocation failed");
		m->caller = response;
		m->cfg.comp = 0;
		m->cfg.ec_tx_info_size = m->cfg.ec_rx_info_size = 70;
		m->cfg.ec_tx_win_size = m->cfg.ec_rx_win_size = 2;
		m->rx_rate = m->tx_rate = 2400;
		CHECK(modem_ec_init(m) == 0, "numeric XID fixture failed");
		modem_ec_start(m);
		if (response) {
			CHECK(modem_hdlc_get_bits(m, 8, preamble, sizeof(preamble)) == (int)sizeof(preamble),
			      "numeric XID preamble failed");
			outgoing = m->packer.hdlc.tx_frame;
			m->packer.hdlc.tx_frame = NULL;
			CHECK(outgoing != NULL && outgoing->size <= (int)sizeof(command), "numeric offer missing");
			command_size = outgoing->size;
			memcpy(command, outgoing->buf, command_size);
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
			/* Response validity belongs to that wire offer, even if local policy changes. */
			m->ec.lapm.tx_info_size = m->ec.lapm.rx_info_size = 128;
			m->ec.lapm.tx_win_size = m->ec.lapm.rx_win_size = 15;
		}
		timer = m->bit_timer;
		parameters[0] = cases[index].parameter < 0xff ? cases[index].parameter + direction : 0xff;
		parameters[1] = cases[index].length;
		memcpy(parameters + 2, cases[index].bytes, cases[index].length);
		wire[0] = response ? m->ec.lapm.cmd_addr : m->ec.lapm.rsp_addr;
		wire[1] = 0xaf;
		CHECK(v42_xid_encode(&fields, wire + 2, sizeof(wire) - 2, &length) == V42_XID_OK,
		      "numeric XID encoding failed");
		incoming.count = 2 + length;
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
		if (counteroffer) {
			CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
			      m->ec.lapm.v44_session.parameters_retried &&
			      m->ec.lapm.tx_win_size == 15 && m->ec.lapm.rx_win_size == 15,
			      "smaller window response bypassed a new proposal");
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
			m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			valid = true;
			value = 1;
		}
		if (!valid) {
			CHECK(outgoing == NULL && m->bit_timer == timer &&
				      (m->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING) == (response != 0) &&
				      m->ec.lapm.tx_info_size == (response ? 128u : 70u) &&
				      m->ec.lapm.rx_info_size == (response ? 128u : 70u) &&
				      m->ec.lapm.tx_win_size == (response ? 15u : 2u) &&
				      m->ec.lapm.rx_win_size == (response ? 15u : 2u),
			      "invalid numeric case %zu, direction %u, response %u changed negotiation",
			      index, direction, response);
			if (response) {
				m->bit_timer_func(m);
				outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
				CHECK(outgoing != NULL && outgoing->size == command_size &&
					      memcmp(outgoing->buf, command, command_size) == 0,
				      "invalid numeric reply changed the frozen retry");
			}
		} else {
			CHECK(outgoing != NULL && outgoing->buf[1] == (response ? 0x7f : 0xaf) &&
				      m->ec.lapm.v44_session.phase != LAPM_V44_COMMAND_PENDING &&
				      m->ec.lapm.tx_info_size == (info && direction ? value : 128u) &&
				      m->ec.lapm.rx_info_size == (info && !direction ? value : 128u) &&
				      m->ec.lapm.tx_win_size == (!info && direction ? value : 15u) &&
				      m->ec.lapm.rx_win_size == (!info && !direction ? value : 15u),
			      "valid numeric case %zu, direction %u, response %u selected wrong limits",
			      index, direction, response);
		}
		if (outgoing != NULL)
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_xid_collisions(void)
{
	static const uint8_t peer_parameters[] = {
		3, 4, 0x8a, 0x89, 0, 0,
		5, 2, 2, 0x98, 6, 2, 3, 0xb8, 7, 1, 7, 8, 1, 11,
	};
	static const unsigned compression_types[] = {
		MODEM_COMP_TYPE_NONE, MODEM_COMP_TYPE_V42BIS, MODEM_COMP_TYPE_V44,
	};
	struct modem_driver driver = {
		.name = "LAPM XID collisions", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned int scenario = 0; scenario < 2 * ARRAY_LENGTH(compression_types); ++scenario) {
		unsigned int early_response = scenario / ARRAY_LENGTH(compression_types);
		unsigned int compression = scenario % ARRAY_LENGTH(compression_types);
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_v44_session peer;
		struct lapm_v44_prepared peer_command, peer_response;
		struct v44_offer offer = v44_default_offer();
		struct lapm_v44_config peer_config;
		struct v42_xid_fields fields;
		uint8_t preamble[109], command[130], wire[130], response[130];
		frame_t incoming = { .buf = wire };
		frame_t *outgoing;
		int command_size, response_size;
		int timer;

		CHECK(m != NULL, "collision modem allocation failed");
		m->caller = 1;
		m->rx_rate = m->tx_rate = 2400;
		m->cfg.comp = compression ? 3 : 0;
		m->cfg.comp_types = compression_types[compression];
		m->cfg.ec_tx_info_size = m->cfg.ec_rx_info_size = 70;
		m->cfg.ec_tx_win_size = m->cfg.ec_rx_win_size = 2;
		CHECK((compression != 1 || modem_comp_init(m) == 0) && modem_ec_init(m) == 0,
		      "collision modem initialization failed");
		modem_ec_start(m);
		CHECK(modem_hdlc_get_bits(m, 8, preamble, sizeof(preamble)) ==
			      (int)sizeof(preamble), "collision preamble failed");
		outgoing = m->packer.hdlc.tx_frame;
		m->packer.hdlc.tx_frame = NULL;
		CHECK(outgoing != NULL && outgoing->size <= (int)sizeof(command),
		      "collision command was missing");
		command_size = outgoing->size;
		memcpy(command, outgoing->buf, (size_t)command_size);
		m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		CHECK(v42_xid_decode(command + 2, (size_t)command_size - 2u, &fields) == V42_XID_OK,
		      "collision command did not decode");
		fields.parameter_negotiation = peer_parameters;
		fields.parameter_negotiation_length = sizeof(peer_parameters);
		fields.user_data = NULL;
		fields.user_data_length = 0;
		offer.transmit.enabled = offer.receive.enabled = compression == 2;
		peer_config = (struct lapm_v44_config) {
			.enabled = compression == 2, .native_v44_ready = true, .local_offer = offer,
		};
		CHECK(lapm_v44_session_init(&peer, &peer_config) == LAPM_V44_OK &&
		      lapm_v44_prepare_command(&peer, &fields, &peer_command) == LAPM_V44_OK &&
		      lapm_v44_commit(&peer, &peer_command) == LAPM_V44_OK &&
		      lapm_v44_prepare_response(&peer, command + 2, (size_t)command_size - 2u,
			      &fields, &peer_response) == LAPM_V44_OK &&
		      lapm_v44_commit(&peer, &peer_response) == LAPM_V44_OK,
		      "peer could not prepare independent command and response");
		if (early_response) {
			wire[0] = m->ec.lapm.cmd_addr;
			wire[1] = 0xaf;
			memcpy(wire + 2, peer_response.data, peer_response.length);
			incoming.count = 2 + (int)peer_response.length;
			m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
			      "early XID response did not start establishment");
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		}
		timer = m->bit_timer;
		wire[0] = m->ec.lapm.rsp_addr;
		wire[1] = 0xaf;
		memcpy(wire + 2, peer_command.data, peer_command.length);
		incoming.count = 2 + (int)peer_command.length;
		{
			frame_t *saved_next = m->ec.lapm.ctrl_list->next;
			struct v44_character_path *path = m->ec.lapm.v44_path;

			m->ec.lapm.ctrl_list->next = m->ec.lapm.tx_ctrl;
			m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
			m->ec.lapm.ctrl_list->next = saved_next;
			CHECK(m->ec.lapm.v44_session.response_length == 0 &&
			      m->ec.lapm.v44_path == path && m->bit_timer == timer &&
			      m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer) == NULL,
			      "unqueued collision response changed the transaction or codec");
		}
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf && m->bit_timer == timer,
		      "colliding command did not receive a response with the timer intact");
		response_size = outgoing->size;
		memcpy(response, outgoing->buf, (size_t)response_size);
		m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		CHECK(lapm_v44_accept_response(&peer, response + 2,
			      (size_t)response_size - 2u) == LAPM_V44_OK,
		      "peer rejected the collision response");
		if (!early_response) {
			CHECK(m->ec.lapm.v44_path == NULL &&
			      m->ec.lapm.tx_info_size == 70 && m->ec.lapm.rx_info_size == 70 &&
			      m->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING,
			      "peer command applied configuration before our response");
			m->bit_timer_func(m);
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->size == command_size &&
			      memcmp(outgoing->buf, command, (size_t)command_size) == 0,
			      "colliding command changed the frozen retry");
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		}
		m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
		CHECK(outgoing != NULL && outgoing->size == response_size &&
		      memcmp(outgoing->buf, response, (size_t)response_size) == 0,
		      "duplicate collision command changed its response");
		m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		if (!early_response) {
			wire[0] = m->ec.lapm.cmd_addr;
			memcpy(wire + 2, peer_response.data, peer_response.length);
			incoming.count = 2 + (int)peer_response.length;
			m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
			      "completed collision did not start establishment");
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		}
		CHECK(m->ec.lapm.v44_session.phase != LAPM_V44_COMMAND_PENDING &&
		      m->ec.lapm.tx_info_size == 119 &&
		      m->ec.lapm.rx_info_size == 83 && m->ec.lapm.tx_win_size == 11 &&
		      m->ec.lapm.rx_win_size == 7 &&
		      (m->ec.lapm.v44_path != NULL) == (compression == 2) &&
		      m->cfg.comp == (compression == 1 ? 3u : 0u),
		      "collision did not confirm directional limits and one codec");
		{
			struct v44_character_path *path = m->ec.lapm.v44_path;

			wire[0] = m->ec.lapm.rsp_addr;
			memcpy(wire + 2, peer_command.data, peer_command.length);
			incoming.count = 2 + (int)peer_command.length;
			m->packer.hdlc.rx_complete(m->packer.hdlc.framer, &incoming);
			outgoing = m->packer.hdlc.get_tx_frame(m->packer.hdlc.framer);
			CHECK(outgoing != NULL && outgoing->size == response_size &&
			      memcmp(outgoing->buf, response, (size_t)response_size) == 0 &&
			      m->ec.lapm.v44_path == path &&
			      m->ec.lapm.v44_session.phase != LAPM_V44_COMMAND_PENDING,
			      "late duplicate reset codec ownership or negotiation");
			m->packer.hdlc.tx_complete(m->packer.hdlc.framer, outgoing);
		}
		if (compression == 1)
			modem_comp_exit(m);
		modem_ec_exit(m);
		modem_delete(m);
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
	wire[sizeof(wire) - 1] = 14;
	modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &incoming);
	CHECK(modem->packer.hdlc.get_tx_frame(modem->packer.hdlc.framer) == NULL &&
		      modem->ec.lapm.v44_session.response_length == 0,
	      "conflicting repeated XID values produced a response");
	wire[sizeof(wire) - 1] = 15;
	modem->packer.hdlc.rx_complete(modem->packer.hdlc.framer, &incoming);
	response = modem->packer.hdlc.get_tx_frame(modem->packer.hdlc.framer);
	CHECK(response != NULL && response->buf[1] == 0xaf &&
		      v42_xid_decode(response->buf + 2,
			      (size_t)response->size - 2U, &fields) == V42_XID_OK &&
		      fields.parameter_negotiation_length == 19 &&
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
		0x03, 0x03, 0x8a, 0x89, 0x00,
		0x05, 0x02, 0x04, 0x00,
		0x06, 0x02, 0x04, 0x00,
		0x07, 0x01, 0x0f,
		0x08, 0x01, 0x0f,
	};
	enum {
		XID_CONTROL = 0xaf,
		SABME_CONTROL = 0x7f,
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
	caller->state = answerer->state = 4; /* Both hosts await error control. */
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
		CHECK(caller->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING,
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
	CHECK(caller->ec.lapm.v44_session.phase == LAPM_V44_NEGOTIATED &&
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
	/* Lose the initial UA and advance only the answerer's encoder. The
	 * caller remains in establishment and legitimately retries SABME. */
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	for (size_t index = 0; index < sizeof(discarded); ++index)
		discarded[index] = (uint8_t)(index * 29U + 17U);
	lapm_source.data = discarded;
	lapm_source.length = sizeof(discarded);
	lapm_source.offset = 0;
	answerer->get_chars = scripted_lapm_source;
	outgoing = answerer->packer.hdlc.get_tx_frame(
		answerer->packer.hdlc.framer);
	CHECK(outgoing != NULL && (outgoing->buf[1] & 1U) == 0 &&
	      lapm_source.offset == sizeof(discarded),
	      "lost-UA fixture did not create an unacknowledged V.44 frame");
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	caller->bit_timer_func(caller);
	outgoing = caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->buf[1] == SABME_CONTROL,
	      "lost-UA caller did not retransmit SABME");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	answerer->packer.hdlc.rx_complete(answerer->packer.hdlc.framer, &incoming);
	CHECK(answerer->ec.lapm.state == LAPM_DATA_STATE &&
	      answerer->ec.lapm.sent_info == answerer->ec.lapm.info_list &&
	      answerer->ec.lapm.tx_info == answerer->ec.lapm.info_list,
	      "initial SABME retry retained discarded V.44 information");
	outgoing = answerer->packer.hdlc.get_tx_frame(answerer->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->buf[1] == 0x73,
	      "lost-UA answerer did not repeat its acknowledgement");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	answerer->packer.hdlc.tx_complete(
		answerer->packer.hdlc.framer, outgoing);
	caller->packer.hdlc.rx_complete(
		caller->packer.hdlc.framer, &incoming);
	CHECK(caller->ec.lapm.state == LAPM_DATA_STATE &&
		      answerer->ec.lapm.state == LAPM_DATA_STATE,
	      "LAPM peers did not enter the data state");
	for (size_t index = 0; index < sizeof(recovery); ++index)
		recovery[index] = (uint8_t)('a' + (index / 11U) % 5U);
	lapm_source.data = recovery;
	lapm_source.length = sizeof(recovery);
	lapm_source.offset = 0;
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	lapm_sink.result = 1;
	caller->put_chars = scripted_lapm_sink;
	outgoing = answerer->packer.hdlc.get_tx_frame(answerer->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size > 3 &&
	      lapm_source.offset == sizeof(recovery),
	      "retried LAPM establishment did not produce compressed data");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	answerer->packer.hdlc.tx_complete(answerer->packer.hdlc.framer, outgoing);
	caller->packer.hdlc.rx_complete(caller->packer.hdlc.framer, &incoming);
	CHECK(lapm_sink.length == sizeof(recovery) &&
	      memcmp(lapm_sink.data, recovery, sizeof(recovery)) == 0,
	      "initial SABME retry did not reset the V.44 compression history");
	/* Deliver the acknowledgement before exercising the opposite direction. */
	outgoing = caller->packer.hdlc.get_tx_frame(caller->packer.hdlc.framer);
	CHECK(outgoing != NULL && outgoing->size == 3 && outgoing->buf[1] == 1,
	      "retried LAPM establishment did not acknowledge V.44 data");
	memcpy(received, outgoing->buf, (size_t)outgoing->size);
	incoming.count = outgoing->size;
	caller->packer.hdlc.tx_complete(caller->packer.hdlc.framer, outgoing);
	answerer->packer.hdlc.rx_complete(answerer->packer.hdlc.framer, &incoming);

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

	/* Numbered traffic has completed initial establishment. A new SABME
	 * must release the call instead of silently resetting stream history. */
	received[0] = answerer->ec.lapm.rsp_addr;
	received[1] = SABME_CONTROL;
	incoming.count = 2;
	answerer->packer.hdlc.rx_complete(answerer->packer.hdlc.framer, &incoming);
	CHECK(answerer->ec.lapm.state == 0 && !answerer->bit_timer &&
	      strcmp(modem_call_phase(answerer), "cleardown") == 0,
	      "connected SABME restarted the V.44 stream instead of releasing");
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
		l->v44_session.phase = LAPM_V44_COMMAND_PENDING;
		wire[0] = l->rsp_addr;
		wire[1] = wire[2] = 0;
		incoming.count = sizeof(wire);
		struct lapm_state before = *l;
		unsigned event = m->new_event;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(memcmp(l, &before, sizeof(before)) == 0 &&
		      m->bit_timer == 1234 && m->new_event == event,
		      "early information bypassed XID negotiation");
		l->v44_session.phase = LAPM_V44_READY;
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

struct lapm_host_read {
	int count;
	int capacity;
	unsigned calls;
};

static int counted_lapm_source(struct modem *modem, char *output, int capacity)
{
	struct lapm_host_read *source = modem->dev_data;

	source->capacity = capacity;
	source->calls++;
	/* Even a source with a bad return count writes only the offered buffer. */
	if (source->count > 0)
		memset(output, 'X', (size_t)(source->count < capacity ? source->count : capacity));
	return source->count;
}

static bool test_lapm_transmit_information_limit(void)
{
	struct modem_driver driver = {
		.name = "LAPM host source limit", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	static const unsigned limits[] = { 8, LAPM_MAX_INFO_SIZE, LAPM_MAX_INFO_SIZE + 1u, UINT_MAX };

	for (size_t limit = 0; limit < ARRAY_LENGTH(limits); ++limit) {
		int capacity = limits[limit] < LAPM_MAX_INFO_SIZE ? (int)limits[limit] : LAPM_MAX_INFO_SIZE;
		const int counts[] = { -1, 0, 1, capacity, capacity + 1, INT_MAX };

		for (size_t index = 0; index < ARRAY_LENGTH(counts); ++index) {
			struct lapm_host_read source = { .count = counts[index] };
			struct modem *m = modem_create(&driver, driver.name);

			CHECK(m != NULL, "host source allocation failed");
			m->cfg.comp = 0;
			CHECK(modem_ec_init(m) == 0, "host source initialization failed");
			modem_ec_start(m);
			struct lapm_state *l = &m->ec.lapm;
			l->state = 2;
			l->tx_info_size = limits[limit];
			m->state = 5;
			m->dev_data = &source;
			m->get_chars = counted_lapm_source;
			frame_t *available = l->info_list;
			int saved_size = available->size;
			frame_t *outgoing = m->packer.hdlc.get_tx_frame(l);
			bool invalid = source.count < 0 || source.count > capacity;

			CHECK(source.calls == 1 && source.capacity == capacity,
			      "host buffer exceeded the retained information limit: limit=%u capacity=%d",
			      limits[limit], source.capacity);
			if (invalid || source.count == 0) {
				CHECK(outgoing == NULL && l->info_list == available && l->tx_info == available &&
				      l->vs == 0 && available->size == saved_size &&
				      strcmp(modem_call_phase(m), invalid ? "cleardown" : "online") == 0,
				      "invalid or empty host read queued information: limit=%u count=%d",
				      limits[limit], source.count);
			} else {
				CHECK(outgoing == available && outgoing->size == source.count + 3 &&
				      l->info_list == available->next && l->tx_info == available->next && l->vs == 1,
				      "legal host read did not queue exactly its payload");
				for (int byte = 3; byte < outgoing->size; ++byte)
					CHECK(outgoing->buf[byte] == 'X', "legal host payload changed");
			}
			modem_ec_exit(m);
			modem_delete(m);
		}
	}
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

static bool test_lapm_sabme_readiness(void)
{
	enum { PENDING, PREAMBLE, IDLE, EC_CLOSING, DP_CLOSING, DISABLED };
	struct modem_driver driver = {
		.name = "LAPM SABME readiness", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned poll = 0; poll < 2; ++poll)
	for (unsigned scenario = PENDING; scenario <= DISABLED; ++scenario)
	for (unsigned pressure = 0; pressure < 2; ++pressure) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, 0x6f | poll << 4 };
		uint8_t command[LAPM_MAX_INFO_SIZE + 2];
		int command_size = 0;
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		frame_t *outgoing;

		CHECK(m != NULL, "SABME readiness allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		m->cfg.ec_tx_info_size = 70;
		m->cfg.ec_rx_info_size = 80;
		CHECK(modem_ec_init(m) == 0, "SABME readiness initialization failed");
		/* Exercise an outstanding command in either address orientation. */
		m->caller = scenario <= PREAMBLE ? 1 : 0;
		modem_ec_start(m);
		m->caller = caller;
		m->state = scenario == IDLE ? 1 : scenario == EC_CLOSING ? 7 :
			scenario == DP_CLOSING ? 9 : 4;
		struct lapm_state *l = &m->ec.lapm;
		if (scenario == PENDING) {
			void (*complete)(struct modem *) = m->packer.hdlc.tx_preamble_complete;
			m->packer.hdlc.tx_preamble_complete = NULL;
			m->packer.hdlc.tx_preamble_flags = 0;
			complete(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size <= (int)sizeof(command),
			      "SABME readiness fixture did not send XID");
			command_size = outgoing->size;
			memcpy(command, outgoing->buf, (size_t)command_size);
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		if (scenario == DISABLED)
			m->cfg.ec = 0;
		struct lapm_v44_session session = l->v44_session;
		struct modem_config config = m->cfg;
		unsigned host = m->state;
		void (*timeout)(struct modem *) = m->bit_timer_func;
		void (*preamble)(struct modem *) = m->packer.hdlc.tx_preamble_complete;
		m->bit_timer = 1234;
		l->rtx_count = 2;
		wire[0] = l->rsp_addr;
		for (unsigned repeat = 0; repeat < (pressure ? LAPM_CTRL_FRAMES : 2); ++repeat) {
			frame_t *slot = l->ctrl_list;
			m->packer.hdlc.rx_complete(l, &incoming);
			if (pressure && repeat == LAPM_CTRL_FRAMES - 1)
				break;
			CHECK(l->state == 0 && m->state == host && l->rtx_count == 2 &&
			      (l->v44_session.phase == LAPM_V44_COMMAND_PENDING) == (scenario == PENDING) &&
			      m->bit_timer == 1234 &&
			      m->bit_timer_func == timeout &&
			      m->packer.hdlc.tx_preamble_complete == preamble &&
			      memcmp(&l->v44_session, &session, sizeof(session)) == 0 &&
			      memcmp(&m->cfg, &config, sizeof(config)) == 0 &&
			      l->tx_info_size == 70 && l->rx_info_size == 80,
			      "premature SABME changed establishment: role=%u P=%u scenario=%u",
			      caller, poll, scenario);
			if (pressure)
				continue;
			/* The wire preamble defers serialization, including the DM. */
			outgoing = scenario == PREAMBLE ? slot : m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 2 &&
			      outgoing->buf[0] == l->rsp_addr &&
			      outgoing->buf[1] == (0x0f | poll << 4),
			      "premature SABME did not receive DM with F=P");
			if (scenario != PREAMBLE)
				m->packer.hdlc.tx_complete(l, outgoing);
		}
		if (pressure) {
			CHECK(l->state == 0 && m->bit_timer == 0 &&
			      strcmp(modem_call_phase(m), scenario == IDLE ? "idle" : "cleardown") == 0,
			      "unreplyable SABME left establishment active: role=%u scenario=%u link=%d host=%u phase=%s",
			      caller, scenario, l->state, m->state, modem_call_phase(m));
		} else if (scenario == PENDING) {
			timeout(m);
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == command_size &&
			      memcmp(outgoing->buf, command, (size_t)command_size) == 0 &&
			      l->v44_session.phase == LAPM_V44_COMMAND_PENDING && l->rtx_count == 3,
			      "premature SABME replaced the frozen XID retry");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_default_establishment(void)
{
	struct modem_driver driver = {
		.name = "LAPM default establishment", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned peer_ready = 0; peer_ready < 2; ++peer_ready)
	for (unsigned poll = 0; poll < 2; ++poll) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, peer_ready ? 0x0f : 0x6f | poll << 4, 0, 'B' };
		frame_t incoming = { .buf = wire, .count = 2 };
		frame_t *outgoing;

		CHECK(m != NULL, "default establishment allocation failed");
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS | MODEM_COMP_TYPE_V44;
		m->cfg.ec_tx_info_size = 70;
		m->cfg.ec_rx_info_size = 80;
		m->cfg.ec_tx_win_size = 2;
		m->cfg.ec_rx_win_size = 3;
		CHECK(modem_comp_init(m) == 0 && modem_ec_init(m) == 0,
		      "default establishment codec initialization failed");
		modem_ec_start(m);
		m->state = 4;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = peer_ready ? l->cmd_addr : l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->size == 2 &&
		      outgoing->buf[1] == (peer_ready ? 0x7f : 0x63 | poll << 4),
		      "default establishment did not queue the correct mode frame");
		m->packer.hdlc.tx_complete(l, outgoing);
		if (peer_ready) {
			wire[1] = 0x73;
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		CHECK(l->state == 2 && m->state == 5 && !m->cfg.comp &&
		      l->tx_info_size == 128 && l->rx_info_size == 128 &&
		      l->tx_win_size == 15 && l->rx_win_size == 15 && l->v44_path == NULL,
		      "establishment without XID used unnegotiated compression or limits");
		CHECK(modem_write(m, "A", 1) == 1, "default link rejected terminal input");
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->size == 4 && outgoing->buf[3] == 'A',
		      "default link compressed an unnegotiated terminal byte");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		wire[0] = l->rsp_addr;
		wire[1] = 0;
		wire[2] = 2; /* N(R)=1 acknowledges the transmitted byte. */
		incoming.count = sizeof(wire);
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(lapm_sink.length == 1 && lapm_sink.data[0] == 'B',
		      "default link did not deliver an uncompressed information byte");
		modem_comp_exit(m);
		modem_delete(m);
	}
	memset(&lapm_sink, 0, sizeof(lapm_sink));
	return true;
}

static bool test_lapm_connected_sabme(void)
{
	enum { INITIAL, SHORT_FRAME, WRONG_ADDRESS, INFORMATION, RR_COMMAND,
	       RNR_COMMAND, REJ_COMMAND, RR_RESPONSE, RNR_RESPONSE, REJ_RESPONSE,
	       SEQUENCE_WRAP, RETRAIN };
	struct modem_driver driver = {
		.name = "LAPM connected SABME", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned poll = 0; poll < 2; ++poll)
	for (unsigned scenario = INITIAL; scenario <= RETRAIN; ++scenario) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, 0x7f, 0 };
		frame_t incoming = { .buf = wire, .count = 2 };
		frame_t *outgoing;

		CHECK(m != NULL, "connected SABME allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		CHECK(modem_ec_init(m) == 0, "connected SABME initialization failed");
		modem_hdlc_start(m);
		/* Keep each role's addresses while bypassing XID and its preamble. */
		m->caller = 0;
		modem_ec_start(m);
		m->caller = caller;
		m->state = 4;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = caller ? l->cmd_addr : l->rsp_addr;
		wire[1] = caller ? 0x0f : 0x7f;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == (caller ? 0x7f : 0x73),
		      "connected SABME fixture did not establish the link");
		m->packer.hdlc.tx_complete(l, outgoing);
		if (caller) {
			wire[1] = 0x73;
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		CHECK(l->state == 2 && m->state == 5, "connected SABME fixture did not connect");
		m->state = 6; /* A retry must not announce CONNECT or leave command mode. */
		wire[0] = l->rsp_addr;
		wire[1] = 0;
		incoming.count = 3;
		if (scenario == SHORT_FRAME)
			incoming.count = 2;
		if (scenario == WRONG_ADDRESS)
			wire[0] = 0;
		if (scenario >= RR_COMMAND && scenario <= REJ_RESPONSE) {
			wire[1] = 1 + 4 * ((scenario - RR_COMMAND) % 3);
			wire[0] = scenario <= REJ_COMMAND ? l->rsp_addr : l->cmd_addr;
		}
		if (scenario == RETRAIN)
			wire[1] = 1;
		if (scenario != INITIAL) {
			unsigned frames = scenario == SEQUENCE_WRAP ? 128 : 1;
			for (unsigned n = 0; n < frames; ++n) {
				if (scenario == SEQUENCE_WRAP)
					wire[1] = n << 1;
				m->packer.hdlc.rx_complete(l, &incoming);
				CHECK(l->state == 2, "SABME fixture rejected valid peer traffic");
				while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL)
					m->packer.hdlc.tx_complete(l, outgoing);
			}
		}
		if (scenario == SEQUENCE_WRAP)
			CHECK(!l->vr && !l->vs && !l->va, "SABME fixture did not wrap its sequence");
		if (scenario == RETRAIN) {
			m->get_bits = modem_hdlc_get_bits;
			m->put_bits = modem_hdlc_put_bits;
			CHECK(modem_ec_resume_after_retrain(m) == 0,
			      "SABME fixture could not resume physical retraining");
		}
		wire[0] = l->rsp_addr;
		wire[1] = 0x6f | poll << 4;
		incoming.count = 2;
		bool initial_retry = !caller && scenario <= WRONG_ADDRESS;
		for (unsigned repeat = 0; repeat < (initial_retry ? 3 : 1); ++repeat) {
			m->packer.hdlc.rx_complete(l, &incoming);
			if (!initial_retry) {
				CHECK(l->state == 0 && !m->bit_timer &&
				      strcmp(modem_call_phase(m), "cleardown") == 0 &&
				      l->ctrl_list == l->tx_ctrl,
				      "connected SABME restarted instead of releasing: role=%u P=%u scenario=%u",
				      caller, poll, scenario);
				break;
			}
			CHECK(l->state == 2 && m->state == 6 && !l->vr && !l->vs && !l->va,
			      "initial SABME retry changed host state or retained sequences");
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 2 &&
			      outgoing->buf[0] == l->rsp_addr &&
			      outgoing->buf[1] == (0x63 | poll << 4),
			      "initial SABME retry did not repeat UA with F=P");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		modem_ec_exit(m);
		modem_delete(m);
	}
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
	l->v44_session.phase = LAPM_V44_COMMAND_PENDING;
	m->bit_timer = 1234;
	m->packer.hdlc.rx_complete(l, &incoming);
	CHECK(l->state == 0 && l->v44_session.phase == LAPM_V44_COMMAND_PENDING && m->state == 4 &&
	      m->bit_timer == 1234 &&
	      l->tx_ctrl == l->ctrl_list, "peer-ready DM bypassed pending XID");
	l->v44_session.phase = LAPM_V44_READY;
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

static bool test_lapm_orderly_release(void)
{
	enum { IDLE = 0, DISC = 3, EC_DISC = 7, DP_DISC = 9 };
	struct modem_driver driver = {
		.name = "LAPM orderly release", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned local = 0; local < 2; ++local)
	for (unsigned poll = 0; poll < (local ? 1u : 2u); ++poll)
	for (unsigned pending = 0; pending < 4; ++pending)
	for (unsigned borrowed = 0; borrowed < 2; ++borrowed)
	for (unsigned pressure = 0; pressure < 2; ++pressure) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_host_read source = { .count = 1 };
		uint8_t wire[] = { 0, 0x7f, 0, 'a', 'b', 'c' }, bit;
		uint8_t active_bytes[LAPM_MAX_INFO_SIZE + 3];
		frame_t incoming = { .buf = wire, .count = 2 }, *active = NULL, *reply;
		int active_size = 0, active_count = 0;

		CHECK(m != NULL, "orderly release allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->tx_rate = m->rx_rate = 4800;
		CHECK(modem_ec_init(m) == 0, "orderly release initialization failed");
		struct lapm_state *l = &m->ec.lapm;
		modem_hdlc_start(m);
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		m->caller = 0;
		modem_ec_start(m);
		m->caller = caller;
		m->state = 4;
		wire[0] = l->rsp_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		reply = m->packer.hdlc.get_tx_frame(l);
		CHECK(reply != NULL && reply->buf[1] == 0x73 && l->state == 2 && m->state == 5,
		      "orderly release fixture did not establish LAPM");
		m->packer.hdlc.tx_complete(l, reply);
		m->dev_data = &source;
		m->get_chars = counted_lapm_source;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		if (pending & 1) {
			reply = m->packer.hdlc.get_tx_frame(l);
			CHECK(reply != NULL && !(reply->buf[1] & 1) && l->vs == 1,
			      "orderly release fixture did not retain information");
			m->packer.hdlc.tx_complete(l, reply);
		}
		if (pending & 2) {
			wire[1] = 0;
			incoming.count = sizeof(wire);
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->rx_count == 3 && l->busy && m->packer_process != NULL,
			      "orderly release fixture did not defer receive work");
		}
		/* A polled status reply supplies a real borrowed wire frame. */
		wire[1] = 1;
		wire[2] = 1;
		incoming.count = 3;
		if (borrowed) {
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(modem_hdlc_get_bits(m, 1, &bit, 1) == 1 &&
			      m->packer.hdlc.tx_frame != NULL,
			      "orderly release fixture did not borrow a frame");
			active = m->packer.hdlc.tx_frame;
			active_size = active->size;
			active_count = active->count;
			CHECK(active_size > 0 && active_size <= (int)sizeof(active_bytes),
			      "borrowed release fixture exceeded snapshot capacity");
			memcpy(active_bytes, active->buf, (size_t)active_size);
		}
		if (pressure) {
			/* Fill every available slot without attempting overflow. */
			for (unsigned i = 0; i < LAPM_CTRL_FRAMES - 1; ++i)
				if (l->ctrl_list->next != l->tx_ctrl)
					m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->ctrl_list->next == l->tx_ctrl && l->state == 2,
			      "orderly release fixture did not fill its control queue");
		}
		m->bit_timer = 17;
		if (local) {
			modem_hangup(m);
		} else {
			wire[1] = 0x43 | poll << 4;
			incoming.count = 2;
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		CHECK(l->state == (local ? DISC : IDLE) &&
		      l->v44_session.phase != LAPM_V44_COMMAND_PENDING &&
		      !l->busy && !l->peer_busy && !l->vs && !l->va && !l->vr && !l->rx_count &&
		      !l->rx_head && !l->rtx_count && l->sent_info == l->info_list &&
		      l->tx_info == l->info_list && m->packer_process == NULL &&
		      m->state == (local ? EC_DISC : DP_DISC) &&
		      (local ? m->bit_timer > 17 : m->bit_timer == 0),
		      "release retained stream work: role %u local %u P %u pending %u borrowed %u pressure %u",
		      caller, local, poll, pending, borrowed, pressure);
		if (active != NULL) {
			CHECK(m->packer.hdlc.tx_frame == active && active->size == active_size &&
			      active->count == active_count &&
			      memcmp(active->buf, active_bytes, (size_t)active_size) == 0,
			      "release overwrote HDLC's borrowed frame");
			for (unsigned i = 0; i < (LAPM_MAX_INFO_SIZE + 8) * 10 &&
			     m->packer.hdlc.tx_frame != NULL; ++i)
				CHECK(modem_get_bits(m, 1, &bit, 1) == 1,
				      "release could not finish its borrowed frame");
			CHECK(m->packer.hdlc.tx_frame == NULL,
			      "borrowed release frame did not finish within its bound");
		}
		reply = m->packer.hdlc.get_tx_frame(l);
		CHECK(reply != NULL && reply->size == 2 &&
		      reply->buf[0] == (local ? l->cmd_addr : l->rsp_addr) &&
		      reply->buf[1] == (local ? 0x53 : (0x63 | poll << 4)),
		      "release did not prioritize DISC/UA: role %u local %u P %u pending %u borrowed %u pressure %u",
		      caller, local, poll, pending, borrowed, pressure);
		m->packer.hdlc.tx_complete(l, reply);
		unsigned source_calls = source.calls, sink_calls = lapm_sink.calls;
		int remaining = m->bit_timer;
		unsigned long deadline = m->sample_timer;
		wire[1] = wire[2] = 0;
		incoming.count = sizeof(wire);
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(m->packer.hdlc.get_tx_frame(l) == NULL && !l->rx_count && !l->vr &&
		      m->packer_process == NULL && source.calls == source_calls &&
		      lapm_sink.calls == sink_calls && m->bit_timer == remaining &&
		      m->sample_timer == deadline,
		      "late information resumed or delayed orderly release");
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

static bool test_lapm_recovery_boundaries(void)
{
	enum { XID, SABME, DISC, INFORMATION, IDLE, PEER_BUSY, OWN_BUSY };
	enum { EXHAUSTION, QUEUE_FAILURE, FINAL_RESPONSE, N400 = 5 };
	struct modem_driver driver = {
		.name = "LAPM recovery boundary", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned mode = XID; mode <= OWN_BUSY; ++mode)
	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned outcome = EXHAUSTION; outcome <= FINAL_RESPONSE; ++outcome) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_host_read source = { .count = 1 };
		uint8_t wire[LAPM_MAX_INFO_SIZE + 2] = { 0 };
		frame_t incoming = { .buf = wire, .count = 2 };
		frame_t *outgoing;
		uint8_t expected[LAPM_MAX_INFO_SIZE + 2];
		int expected_size;
		unsigned source_calls;

		CHECK(m != NULL, "recovery boundary allocation failed");
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->caller = caller;
		m->rx_rate = m->tx_rate = 4800;
		m->state = 4;
		m->count = 1234;
		m->dev_data = &source;
		CHECK(modem_ec_init(m) == 0, "recovery boundary initialization failed");
		struct lapm_state *l = &m->ec.lapm;
		/* Preserve each role's addresses while selecting the initial request. */
		m->caller = mode == XID;
		modem_update_status(m, STATUS_PACK_LINK);
		m->caller = caller;
		if (mode == XID) {
			void (*complete)(struct modem *) = m->packer.hdlc.tx_preamble_complete;

			m->packer.hdlc.tx_preamble_flags = 0;
			m->packer.hdlc.tx_preamble_complete = NULL;
			complete(m);
		} else {
			wire[0] = l->cmd_addr;
			wire[1] = 0x0f; /* Peer-ready DM initiates SABME. */
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == (mode == XID ? 0xaf : 0x7f),
		      "recovery boundary did not initiate XID/SABME");
		expected_size = outgoing->size;
		memcpy(expected, outgoing->buf, (size_t)expected_size);
		m->packer.hdlc.tx_complete(l, outgoing);
		if (mode >= DISC) {
			wire[0] = l->cmd_addr;
			wire[1] = 0x73; /* UA F=1 establishes the link. */
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->state == 2 && m->state == 5, "recovery boundary did not connect");
			m->get_chars = NULL;
			if (mode == DISC) {
				m->state = 7;
				modem_ec_stop(m);
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->buf[1] == 0x53,
				      "recovery boundary did not initiate DISC");
				expected_size = outgoing->size;
				memcpy(expected, outgoing->buf, (size_t)expected_size);
				m->packer.hdlc.tx_complete(l, outgoing);
			} else if (mode == INFORMATION) {
				m->get_chars = counted_lapm_source;
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->size == 4,
				      "recovery boundary lacks unacknowledged information");
				m->packer.hdlc.tx_complete(l, outgoing);
			} else if (mode == PEER_BUSY || mode == OWN_BUSY) {
				wire[0] = l->rsp_addr;
				wire[1] = mode == PEER_BUSY ? 5 : 0;
				wire[2] = 0;
				wire[3] = 'R';
				incoming.count = mode == PEER_BUSY ? 3 : 4;
				m->put_chars = NULL;
				m->packer.hdlc.rx_complete(l, &incoming);
				while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL)
					m->packer.hdlc.tx_complete(l, outgoing);
				CHECK(mode == PEER_BUSY ? l->peer_busy : l->busy && l->rx_count == 1,
				      "recovery boundary did not enter receiver busy");
			}
			if (mode >= INFORMATION) {
				expected_size = 3;
				expected[0] = l->cmd_addr;
				expected[1] = mode == OWN_BUSY ? 5 : 1;
				expected[2] = (uint8_t)((l->vr << 1) | 1);
			}
		}
		source_calls = source.calls;
		if (outcome == QUEUE_FAILURE) {
			wire[0] = l->rsp_addr;
			wire[1] = mode >= INFORMATION ? (mode == PEER_BUSY ? 5 : 1) : 0x7f;
			wire[2] = 1;
			incoming.count = mode >= INFORMATION ? 3 : 2;
			for (unsigned request = 0; request < LAPM_CTRL_FRAMES - 1; ++request)
				m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->ctrl_list->next == l->tx_ctrl &&
			      strcmp(modem_call_phase(m), "cleardown") != 0,
			      "recovery boundary did not fill the control queue");
			m->bit_timer_func(m);
		} else {
			for (unsigned expiry = 1; expiry <= N400; ++expiry) {
				m->bit_timer_func(m);
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->size == expected_size &&
				      memcmp(outgoing->buf, expected, (size_t)expected_size) == 0 &&
				      l->rtx_count == (int)expiry && m->bit_timer > 0 && source.calls == source_calls,
				      "recovery changed its request or ended early: mode %u role %u expiry %u",
				      mode, caller, expiry);
				m->packer.hdlc.tx_complete(l, outgoing);
			}
			if (outcome == FINAL_RESPONSE) {
				memcpy(wire, expected, (size_t)expected_size);
				wire[0] = l->cmd_addr;
				wire[1] = mode <= DISC ? (mode == XID ? 0xaf : 0x73) : 1;
				if (mode >= INFORMATION)
					wire[2] = (uint8_t)((l->vs << 1) | 1);
				incoming.count = mode <= DISC ? (mode == XID ? expected_size : 2) : 3;
				m->packer.hdlc.rx_complete(l, &incoming);
				if (mode != DISC) {
					CHECK(l->state == (mode == XID ? 1 : 2) &&
					      l->v44_session.phase != LAPM_V44_COMMAND_PENDING &&
					      !l->rtx_count &&
					      m->bit_timer > 0 && m->get_bits == modem_hdlc_get_bits,
					      "last allowed response failed to recover mode %u role %u", mode, caller);
					modem_ec_exit(m);
					modem_delete(m);
					continue;
				}
			} else {
				m->bit_timer_func(m);
				CHECK(m->packer.hdlc.get_tx_frame(l) == NULL,
				      "recovery exceeded N400 or started another transaction: mode %u role %u",
				      mode, caller);
			}
		}
		CHECK(l->state == 0 && l->v44_session.phase != LAPM_V44_COMMAND_PENDING && !l->rtx_count &&
		      !l->rx_count &&
		      l->sent_info == l->info_list && l->tx_info == l->info_list &&
		      m->bit_timer == 0 && m->bit_timer_func == NULL && m->packer_process == NULL &&
		      m->get_bits == NULL && m->put_bits == NULL && source.calls == source_calls &&
		      strcmp(modem_call_phase(m), "cleardown") == 0 && m->sample_timer - m->count == 48,
		      "failed recovery retained an online link: mode %u role %u outcome %u",
		      mode, caller, outcome);
		CHECK(mode != XID ||
		      (l->v44_session.phase == LAPM_V44_READY &&
		       l->v44_session.command_length == 0 && l->v44_session.response_length == 0),
		      "failed XID recovery retained its negotiation transaction: role %u outcome %u",
		      caller, outcome);
		modem_ec_exit(m);
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

struct lapm_ready_capture {
	uint8_t data[4];
	unsigned length, limit, ready_frames, busy_frames;
	uint8_t command_address, response_address;
	bool invalid_frame;
};

static int capture_ready_characters(struct modem *m, const char *input, int length)
{
	struct lapm_ready_capture *capture = m->dev_data;
	unsigned consumed = (unsigned)length < capture->limit ? (unsigned)length : capture->limit;

	if (length < 0 || consumed > sizeof(capture->data) - capture->length)
		return -1;
	memcpy(capture->data + capture->length, input, consumed);
	capture->length += consumed;
	return (int)consumed;
}

static void capture_ready_frame(void *context, frame_t *frame)
{
	struct lapm_ready_capture *capture = context;

	if (frame->count != 3 || frame->buf[2] >> 1 != 1) {
		capture->invalid_frame = true;
		return;
	}
	if (frame->buf[0] == capture->response_address && frame->buf[1] == 5)
		capture->busy_frames++;
	else if (frame->buf[0] == capture->command_address &&
		 frame->buf[1] == 1 && frame->buf[2] == 2)
		capture->ready_frames++;
	else
		capture->invalid_frame = true;
}

struct lapm_ack_capture {
	uint8_t frame[8][3];
	unsigned count;
	bool invalid;
};

static void capture_lapm_acknowledgment(void *context, frame_t *frame)
{
	struct lapm_ack_capture *capture = context;

	if (frame->count != 3 || capture->count == ARRAY_LENGTH(capture->frame)) {
		capture->invalid = true;
		return;
	}
	memcpy(capture->frame[capture->count++], frame->buf, 3);
}

static bool test_lapm_acknowledgment_publication(void)
{
	struct modem_driver driver = {
		.name = "LAPM acknowledgment publication", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	const uint8_t controls[] = { 1, 5, 9 }; /* RR, RNR, REJ */
	const uint8_t bases[] = { 0, 126 };

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned kind = 0; kind < ARRAY_LENGTH(controls); ++kind)
	for (unsigned poll = 0; poll < 2; ++poll)
	for (unsigned borrowed = 0; borrowed < 2; ++borrowed)
	for (unsigned wrap = 0; wrap < ARRAY_LENGTH(bases); ++wrap) {
		struct modem *m = modem_create(&driver, driver.name);
		struct modem *decoder = modem_create(&driver, driver.name);
		struct lapm_ack_capture capture = { 0 };
		uint8_t wire[] = { 0, 0x0f, 0, 'a' }, bit, retained[3];
		frame_t incoming = { .buf = wire, .count = 2 }, *active = NULL;
		int active_count = 0;

		CHECK(m != NULL && decoder != NULL, "acknowledgment allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 4800;
		m->state = 4;
		CHECK(modem_ec_init(m) == 0, "acknowledgment initialization failed");
		m->caller = 0; /* Preserve addresses while bypassing initial XID. */
		modem_update_status(m, STATUS_PACK_LINK);
		m->caller = caller;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		frame_t *outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
		      "acknowledgment fixture lacks SABME");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[1] = 0x73;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->state == 5, "acknowledgment fixture did not connect");
		m->get_chars = NULL;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = kind == 1 ? 0 : 1;
		l->vr = bases[wrap]; /* Continue a valid receive epoch near wraparound. */
		modem_hdlc_start(decoder);
		decoder->packer.hdlc.framer = &capture;
		decoder->packer.hdlc.rx_complete = capture_lapm_acknowledgment;
		decoder->put_bits = modem_hdlc_put_bits;
		wire[0] = l->rsp_addr;
		wire[1] = (uint8_t)(((bases[wrap] + (kind == 2)) & 127) << 1);
		wire[2] = (uint8_t)poll;
		incoming.count = sizeof(wire);
		m->packer.hdlc.rx_complete(l, &incoming);
		unsigned first_nr = l->vr;
		if (borrowed) {
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1 &&
			      (active = m->packer.hdlc.tx_frame) != NULL,
			      "acknowledgment fixture did not borrow a wire frame");
			memcpy(retained, active->buf, sizeof(retained));
			active_count = active->count;
		}
		if (kind == 1) {
			CHECK(l->busy && m->packer_process != NULL, "RNR fixture did not block");
			lapm_sink.result = 1;
			m->packer_process(m, 0);
			CHECK(!l->busy && lapm_sink.length == 1, "RNR fixture did not clear busy");
		}
		wire[2] = 0;
		for (unsigned frame = kind == 2 ? 0 : 1; frame < 3; ++frame) {
			wire[1] = (uint8_t)(((bases[wrap] + frame) & 127) << 1);
			wire[3] = (uint8_t)('a' + frame);
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		unsigned nr = (bases[wrap] + 3) & 127;
		CHECK(l->vr == nr && !l->busy && !l->reject &&
		      lapm_sink.length == 3 && memcmp(lapm_sink.data, "abc", 3) == 0 &&
		      (!active || (active == m->packer.hdlc.tx_frame &&
		       active->count == active_count && memcmp(active->buf, retained, 3) == 0)),
		      "new input changed a borrowed frame or lost receive progress");
		unsigned expected = kind == 0 ? 3 : 4;
		for (unsigned clock = 0; clock < 2048 && capture.count < expected; ++clock) {
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1,
			      "acknowledgment wire did not progress");
		}
		CHECK(!capture.invalid && capture.count == expected,
		      "acknowledgment wire did not publish all queued replies");
		for (unsigned frame = 0; frame < capture.count; ++frame) {
			uint8_t *header = capture.frame[frame];
			CHECK(header[0] == (kind == 1 && frame == 1 ? l->cmd_addr : l->rsp_addr) &&
			      header[1] == (frame == 0 ? controls[kind] : 1) &&
			      (header[2] & 1) == (frame == 0 ? poll : 0) &&
			      (header[2] >> 1) == (borrowed && frame == 0 ? first_nr : nr),
			      "stale or altered acknowledgment: role %u kind %u P%u borrowed %u wrap %u frame %u N(R)=%u V(R)=%u",
			      caller, kind, poll, borrowed, wrap, frame, header[2] >> 1, nr);
		}
		CHECK(modem_stop_now(m) == 0, "acknowledgment cleanup failed");
		modem_delete(m);
		modem_delete(decoder);
	}
	return true;
}

struct lapm_reject_capture {
	uint8_t frame[16][3];
	unsigned count;
	unsigned rejects;
	bool final_seen;
	bool invalid;
};

static void capture_lapm_reject(void *context, frame_t *frame)
{
	struct lapm_reject_capture *capture = context;

	if (frame->count != 3 || capture->count == ARRAY_LENGTH(capture->frame)) {
		capture->invalid = true;
		return;
	}
	memcpy(capture->frame[capture->count++], frame->buf, 3);
	capture->rejects += frame->buf[1] == 9;
	capture->final_seen |= (frame->buf[2] & 1) != 0;
}

static bool test_lapm_reject_queue_pressure(void)
{
	struct modem_driver driver = {
		.name = "LAPM reject queue pressure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	const uint8_t controls[] = { 0, 1, 5, 9 }; /* I, RR, RNR, REJ polls */
	const uint8_t bases[] = { 0, 126 };

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned borrowed = 0; borrowed < 2; ++borrowed)
	for (unsigned early = 0; early < 2; ++early)
	for (unsigned kind = 0; kind < ARRAY_LENGTH(controls); ++kind)
	for (unsigned wrap = 0; wrap < ARRAY_LENGTH(bases); ++wrap) {
		struct modem *m = modem_create(&driver, driver.name);
		struct modem *decoder = modem_create(&driver, driver.name);
		struct lapm_reject_capture capture = { 0 };
		uint8_t wire[] = { 0, 0x0f, 0, 'a' }, bit, retained[3];
		frame_t incoming = { .buf = wire, .count = 2 }, *active = NULL;
		int active_count = 0;

		CHECK(m != NULL && decoder != NULL, "reject-pressure allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 4800;
		m->state = 4;
		CHECK(modem_ec_init(m) == 0, "reject-pressure initialization failed");
		m->caller = 0; /* Preserve addresses while bypassing initial XID. */
		modem_update_status(m, STATUS_PACK_LINK);
		m->caller = caller;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		frame_t *outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
		      "reject-pressure fixture lacks SABME");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[1] = 0x73;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->state == 5, "reject-pressure fixture did not connect");
		m->get_chars = NULL;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		lapm_sink.result = 1;
		l->vr = bases[wrap];
		modem_hdlc_start(decoder);
		decoder->packer.hdlc.framer = &capture;
		decoder->packer.hdlc.rx_complete = capture_lapm_reject;
		decoder->put_bits = modem_hdlc_put_bits;
		unsigned burst = LAPM_CTRL_FRAMES - 1;
		unsigned accepted = burst + borrowed;
		CHECK(l->rx_win_size >= accepted + 2,
		      "receive burst and sequence gap exceed the accepted window");
		wire[0] = l->rsp_addr;
		wire[2] = 0;
		incoming.count = sizeof(wire);
		for (unsigned frame = 0; frame < accepted; ++frame) {
			if (frame == burst) {
				CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
				      modem_put_bits(decoder, 1, &bit, 1) == 1 &&
				      (active = m->packer.hdlc.tx_frame) != NULL,
				      "reject-pressure fixture did not borrow a frame");
				memcpy(retained, active->buf, 3);
				active_count = active->count;
			}
			wire[1] = (uint8_t)(((bases[wrap] + frame) & 127) << 1);
			wire[3] = (uint8_t)('a' + frame);
			m->packer.hdlc.rx_complete(l, &incoming);
		}
		unsigned nr = (bases[wrap] + accepted) & 127;
		CHECK(l->ctrl_list->next == l->tx_ctrl && l->vr == nr &&
		      lapm_sink.length == accepted, "valid receive burst did not fill the queue");
		wire[1] = (uint8_t)(((nr + 1) & 127) << 1);
		wire[3] = 'x';
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->reject == LAPM_REJECT_PENDING && l->vr == nr &&
		      lapm_sink.length == accepted &&
		      (!active || (active == m->packer.hdlc.tx_frame &&
		       active->count == active_count && memcmp(active->buf, retained, 3) == 0)),
		      "unreported reject lost its state, payload boundary or borrowed frame");
		for (unsigned clock = 0; clock < 2048 &&
		     (early ? capture.count < 2 : capture.rejects == 0); ++clock) {
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1,
			      "reject-pressure wire did not progress");
		}
		CHECK(early ? (capture.count == 2 && capture.rejects == 0 &&
		      l->reject == LAPM_REJECT_PENDING) :
		      (capture.rejects == 1 && l->reject == LAPM_REJECT_SENT),
		      "REJ did not follow the freed control slot without a peer retry");
		wire[1] = kind == 0 ? (uint8_t)(((nr + 1) & 127) << 1) : controls[kind];
		wire[2] = 1;
		incoming.count = kind == 0 ? 4 : 3;
		m->packer.hdlc.rx_complete(l, &incoming);
		for (unsigned clock = 0; clock < 2048 && !capture.final_seen; ++clock) {
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1,
			      "reject-pressure final response did not progress");
		}
		CHECK(!capture.invalid && capture.final_seen && capture.count == accepted + 2 &&
		      capture.rejects == 1 && l->reject == LAPM_REJECT_SENT && l->vr == nr &&
		      lapm_sink.length == accepted,
		      "reject-pressure recovery duplicated REJ, accepted an error or lost the final reply");
		for (unsigned frame = 0; frame < capture.count; ++frame) {
			uint8_t *header = capture.frame[frame];
			unsigned expected_nr = borrowed && frame == 0 ?
				(bases[wrap] + burst) & 127 : nr;
			CHECK(header[0] == l->rsp_addr &&
			      header[1] == (frame == accepted ? 9 : 1) &&
			      (header[2] & 1) == (frame == accepted + 1) &&
			      (header[2] >> 1) == expected_nr,
			      "reject-pressure wire mismatch: role %u borrowed %u early %u kind %u wrap %u frame %u",
			      caller, borrowed, early, kind, wrap, frame);
		}
		/* The missing frame resolves the exception and is delivered once. */
		wire[1] = (uint8_t)(nr << 1);
		wire[2] = 0;
		wire[3] = (uint8_t)('a' + accepted);
		incoming.count = 4;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->reject == LAPM_REJECT_NONE && l->vr == ((nr + 1) & 127) &&
		      lapm_sink.length == accepted + 1 &&
		      memcmp(lapm_sink.data, "abcdefghij", accepted + 1) == 0,
		      "missing information frame did not resume ordered input once");
		CHECK(modem_stop_now(m) == 0, "reject-pressure cleanup failed");
		modem_delete(m);
		modem_delete(decoder);
	}
	return true;
}

static bool test_lapm_receiver_ready_queue_pressure(void)
{
	struct modem_driver driver = {
		.name = "LAPM receiver-ready pressure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned poll = 0; poll < 2; ++poll)
	for (unsigned borrowed = 0; borrowed < 2; ++borrowed)
	for (unsigned partial = 0; partial < 3; ++partial) {
		struct lapm_ready_capture capture = { .limit = partial };
		struct modem *m = modem_create(&driver, driver.name);
		struct modem *decoder = modem_create(&driver, driver.name);
		uint8_t wire[] = { 0, 0x0f, 0, 'a', 'b', 'c' };
		frame_t incoming = { .buf = wire, .count = 2 };
		frame_t *outgoing, *active = NULL;
		uint8_t retained[3], bit;
		void (*retry)(struct modem *, int);
		int active_count = 0;

		CHECK(m != NULL && decoder != NULL, "receiver-ready allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 4800;
		m->state = 4;
		m->dev_data = &capture;
		CHECK(modem_ec_init(m) == 0, "receiver-ready initialization failed");
		m->caller = 0; /* Preserve addresses while bypassing initial XID. */
		modem_update_status(m, STATUS_PACK_LINK);
		m->caller = caller;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
		      "receiver-ready fixture did not initiate SABME");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[1] = 0x73;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->state == 5, "receiver-ready fixture did not connect");
		m->get_chars = NULL;
		m->put_chars = capture_ready_characters;
		capture.command_address = l->cmd_addr;
		capture.response_address = l->rsp_addr;
		modem_hdlc_start(decoder);
		decoder->packer.hdlc.framer = &capture;
		decoder->packer.hdlc.rx_complete = capture_ready_frame;
		decoder->put_bits = modem_hdlc_put_bits;
		wire[0] = l->rsp_addr;
		wire[1] = 0;
		wire[2] = (uint8_t)poll;
		incoming.count = sizeof(wire);
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->busy && l->rx_count == (int)(3 - partial) &&
		      capture.length == partial && m->packer_process != NULL,
		      "receiver-ready fixture did not retain blocked input");
		if (borrowed) {
			capture.limit = 0;
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1 &&
			      (active = m->packer.hdlc.tx_frame) != NULL,
			      "receiver-ready fixture did not borrow a wire frame");
			memcpy(retained, active->buf, sizeof(retained));
			active_count = active->count;
		}
		/* The peer's polls occupy the remaining slots with genuine RNR replies. */
		CHECK(fill_lapm_control_queue(m), "receiver-ready queue did not fill");
		capture.limit = 3;
		retry = m->packer_process;
		for (unsigned attempt = 0; attempt < 3; ++attempt) {
			retry(m, 0);
			CHECK(capture.length == 3 && memcmp(capture.data, "abc", 3) == 0 &&
			      l->busy && !l->rx_head && !l->rx_count && m->packer_process == retry &&
			      (!active || (active->count == active_count && active->size == 3 &&
			       memcmp(active->buf, retained, sizeof(retained)) == 0)),
			      "drained receive data lost its ready announcement or borrowed frame: role %u P%u borrowed %u partial %u",
			      caller, poll, borrowed, partial);
		}
		for (unsigned bits = 0; bits < 2048 && !capture.ready_frames; ++bits) {
			CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
			      modem_put_bits(decoder, 1, &bit, 1) == 1,
			      "receiver-ready wire did not progress");
		}
		CHECK(!capture.invalid_frame && capture.ready_frames == 1 &&
		      capture.busy_frames == LAPM_CTRL_FRAMES - 1 + borrowed &&
		      !l->busy && m->packer_process == NULL && l->state == 2 && m->state == 5 &&
		      l->vr == 1 && capture.length == 3,
		      "receiver-ready retry did not publish one valid RR after old RNR replies");
		wire[1] = 2; /* Next sequenced I frame, no new peer poll. */
		wire[2] = 0;
		wire[3] = 'd';
		incoming.count = 4;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(capture.length == 4 && memcmp(capture.data, "abcd", 4) == 0 &&
		      l->vr == 2 && !l->busy && m->packer_process == NULL,
		      "receiver-ready recovery duplicated bytes or blocked new input");
		CHECK(modem_stop_now(m) == 0, "receiver-ready cleanup failed");
		modem_delete(m);
		modem_delete(decoder);
	}
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
	CHECK(lapm_sink.calls == 3 && modem->ec.lapm.state == 0 &&
		      modem->ec.lapm.vr == 0 &&
		      modem->ec.lapm.rx_count == 0 &&
		      modem->packer_process == NULL,
	      "failed LAPM sink retained a link or invalid output");
	modem_ec_exit(modem);
	modem_delete(modem);
	return true;
}

static bool test_lapm_control_failure(void)
{
	struct modem_driver driver = {
		.name = "LAPM fatal control error", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned pending = 0; pending < 4; ++pending)
	for (unsigned invalid = 0; invalid < 3; ++invalid) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_host_read source = { .count = 1 };
		uint8_t parameters[] = {
			0, 3, 'V', '4', '2', 1, 1, 0, 2, 2, 2, 0, 3, 1, 6,
		};
		struct v42_xid_fields fields = {
			.private_negotiation = parameters,
			.private_negotiation_length = sizeof(parameters),
		};
		uint8_t xid[64], wire[] = { 0, 0x7f, 0, 'p' };
		size_t length;
		frame_t incoming = { .buf = xid }, *outgoing;

		CHECK(m != NULL, "control failure allocation failed");
		m->caller = caller;
		m->cfg.comp = 3;
		m->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		m->cfg.comp_dict_size = 512;
		m->cfg.comp_max_string = 6;
		CHECK(modem_comp_init(m) == 0 && modem_ec_init(m) == 0,
		      "control failure initialization failed");
		struct lapm_state *l = &m->ec.lapm;
		modem_hdlc_start(m);
		m->caller = 0;
		modem_ec_start(m);
		m->caller = caller;
		m->state = 4;
		xid[0] = l->rsp_addr;
		xid[1] = 0xaf;
		CHECK(v42_xid_encode(&fields, xid + 2, sizeof(xid) - 2, &length) == V42_XID_OK,
		      "control failure XID encoding failed");
		incoming.count = (int)length + 2;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
		      l->v44_session.response_length && m->cfg.comp == 0,
		      "control failure fixture did not cache negotiation");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[0] = l->rsp_addr;
		incoming.buf = wire;
		incoming.count = 2;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x73 && l->state == 2 && m->state == 5,
		      "control failure fixture did not establish LAPM");
		m->packer.hdlc.tx_complete(l, outgoing);
		m->dev_data = &source;
		m->get_chars = counted_lapm_source;
		m->put_chars = scripted_lapm_sink;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		if (pending & 1) {
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && !(outgoing->buf[1] & 1) && l->vs == 1,
			      "control failure fixture did not retain information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		if (pending & 2) {
			wire[1] = 0;
			incoming.count = sizeof(wire);
			m->packer.hdlc.rx_complete(l, &incoming);
			CHECK(l->rx_count && l->busy && m->packer_process != NULL,
			      "control failure fixture did not defer input");
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && (outgoing->buf[1] & 0xf) == 5,
			      "control failure fixture did not report receive pressure");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		/* A later XID with an impossible compression limit is fatal even
		 * when a valid command previously populated the response cache. */
		parameters[7] = 3;
		if (invalid < 2) {
			parameters[10] = invalid == 0 ? 0 : 1;
			parameters[11] = invalid == 0 ? 0 : 255;
		} else {
			parameters[14] = 5;
		}
		CHECK(v42_xid_encode(&fields, xid + 2, sizeof(xid) - 2, &length) == V42_XID_OK,
		      "fatal control XID encoding failed");
		incoming.buf = xid;
		incoming.count = (int)length + 2;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 0 && l->v44_session.phase != LAPM_V44_COMMAND_PENDING && !l->vs &&
		      !l->va && !l->vr &&
		      l->sent_info == l->info_list && l->tx_info == l->info_list &&
		      !l->busy && !l->peer_busy && !l->rx_count && !m->bit_timer &&
		      m->packer_process == NULL && strcmp(modem_call_phase(m), "cleardown") == 0,
		      "fatal control retained LAPM: caller %u pending %u limit %u",
		      caller, pending, invalid);
		unsigned source_calls = source.calls, sink_calls = lapm_sink.calls;
		wire[1] = wire[2] = 0;
		incoming.buf = wire;
		incoming.count = sizeof(wire);
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(m->packer.hdlc.get_tx_frame(l) == NULL && !l->vr && !l->rx_count &&
		      m->packer_process == NULL && source.calls == source_calls &&
		      lapm_sink.calls == sink_calls,
		      "late information ran after a fatal control error");
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_information_failure(void)
{
	enum { IDLE = 0, DATA = 2, ONLINE = 5 };
	enum { RECEIVE, DEFERRED, TRANSMIT };
	static const int failures[] = { -1, 2 };
	struct modem_driver driver = {
		.name = "LAPM information failure", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned pending = 0; pending < 2; ++pending)
	for (unsigned scenario = RECEIVE; scenario <= TRANSMIT; ++scenario)
	for (size_t failure = 0; failure < ARRAY_LENGTH(failures); ++failure) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_host_read source = { .count = 1 };
		CHECK(m != NULL, "information failure allocation failed");
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->cfg.ec_tx_info_size = 1;
		m->cfg.ec_tx_win_size = 2;
		CHECK(start_lapm_compression_fixture(m, caller), "information failure fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		CHECK(lapm_v44_session_call_reset(&l->v44_session) == LAPM_V44_OK,
		      "data fixture did not retire its startup transaction");
		l->state = DATA;
		m->state = ONLINE;
		m->get_chars = counted_lapm_source;
		m->put_chars = scripted_lapm_sink;
		m->dev_data = &source;
		memset(&lapm_sink, 0, sizeof(lapm_sink));
		if (pending) {
			if (scenario == TRANSMIT) {
				frame_t *sent = m->packer.hdlc.get_tx_frame(l);
				CHECK(sent != NULL && sent->size == 4, "information failure send fixture failed");
				m->packer.hdlc.tx_complete(l, sent);
			} else {
				uint8_t ready[] = { l->rsp_addr, 1, 0 };
				frame_t command = { .buf = ready, .count = sizeof(ready) };
				m->packer.hdlc.rx_complete(l, &command);
				CHECK(l->tx_info != l->info_list, "information failure queue fixture failed");
			}
		}
		uint8_t wire[] = { l->rsp_addr, 0, 0, 'R' };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		if (scenario == TRANSMIT) {
			source.count = failures[failure];
			CHECK(m->packer.hdlc.get_tx_frame(l) == NULL, "invalid source returned information");
		} else {
			lapm_sink.result = scenario == DEFERRED ? 0 : failures[failure];
			m->packer.hdlc.rx_complete(l, &incoming);
			if (scenario == DEFERRED) {
				CHECK(l->busy && l->rx_count == 1 && m->packer_process != NULL,
				      "information failure deferred fixture failed");
				frame_t *reply = m->packer.hdlc.get_tx_frame(l);
				CHECK(reply != NULL && reply->buf[1] == 5, "deferred fixture RNR missing");
				m->packer.hdlc.tx_complete(l, reply);
				lapm_sink.result = failures[failure];
				m->packer_process(m, 0);
			}
		}
		CHECK(l->state == IDLE && l->va == 0 && l->vs == 0 && l->vr == 0 &&
		      !l->busy && !l->peer_busy && !l->rx_count && !m->bit_timer &&
		      m->packer_process == NULL && strcmp(modem_call_phase(m), "cleardown") == 0 &&
		      m->packer.hdlc.get_tx_frame(l) == NULL,
		      "fatal information error retained a link: role=%u pending=%u scenario=%u failure=%d",
		      caller, pending, scenario, failures[failure]);
		unsigned source_calls = source.calls, sink_calls = lapm_sink.calls;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == IDLE && l->vr == 0 && !l->rx_count && !l->busy &&
		      m->packer_process == NULL && source.calls == source_calls &&
		      lapm_sink.calls == sink_calls && m->packer.hdlc.get_tx_frame(l) == NULL,
		      "late information reactivated a failed link");
		modem_delete(m);
	}
	memset(&lapm_sink, 0, sizeof(lapm_sink));
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
			      l->sent_info == l->info_list && l->tx_info == l->info_list &&
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
		      l->sent_info == retained && l->tx_info == retained->next &&
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
			      l->sent_info == retained && l->tx_info == retained->next &&
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
		m->state = 4;
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
			CHECK(l->v44_session.phase != LAPM_V44_COMMAND_PENDING, "originator idle fixture did not complete XID");
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
		      l->peer_busy && l->rtx_count == 2 && l->stalled_retrain,
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
		m->state = 4;
		m->get_bits = modem_hdlc_get_bits;
		m->put_bits = modem_hdlc_put_bits;
		struct lapm_state *l = &m->ec.lapm;
		/* Agree the small transmit limits before using them for this test. */
		uint8_t request[] = {
			l->rsp_addr, 0xaf, 0x82, 0x80, 0, 6,
			6, 1, 8, 8, 1, 1,
		};
		frame_t xid = { .buf = request, .count = sizeof(request) };
		m->packer.hdlc.rx_complete(l, &xid);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0xaf &&
		      l->tx_info_size == 1 && l->tx_win_size == 1,
		      "progress fixture did not negotiate its small limits");
		m->packer.hdlc.tx_complete(l, outgoing);
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

static bool test_lapm_retrain_recovery_budget(void)
{
	struct modem_driver driver = {
		.name = "LAPM retrain recovery budget", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned information = 0; information < 2; ++information)
	for (unsigned first_retry = !information; first_retry <= 5; ++first_retry)
	for (unsigned full = 0; full < (first_retry ? 2u : 1u); ++full)
	for (unsigned answered = 0; answered < (first_retry == 5 ? 2u : 1u); ++answered) {
		struct modem *m = modem_create(&driver, driver.name);
		struct lapm_host_read source = { .count = 1 };
		uint8_t wire[] = { 0, 0x0f }, bit;
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		frame_t *outgoing;
		unsigned enquiries = first_retry;

		CHECK(m != NULL, "retrain budget allocation failed");
		m->caller = caller;
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		m->rx_rate = m->tx_rate = 4800;
		m->state = 4;
		CHECK(modem_ec_init(m) == 0, "retrain budget initialization failed");
		m->caller = 0;
		modem_update_status(m, STATUS_PACK_LINK);
		m->caller = caller;
		struct lapm_state *l = &m->ec.lapm;
		wire[0] = l->cmd_addr;
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->buf[1] == 0x7f,
		      "retrain budget fixture did not send SABME");
		m->packer.hdlc.tx_complete(l, outgoing);
		wire[1] = 0x73;
		m->packer.hdlc.rx_complete(l, &incoming);
		CHECK(l->state == 2 && m->state == 5, "retrain budget fixture did not connect");
		if (information) {
			m->dev_data = &source;
			m->get_chars = counted_lapm_source;
			outgoing = m->packer.hdlc.get_tx_frame(l);
			CHECK(outgoing != NULL && outgoing->size == 4 && outgoing->buf[1] == 0,
			      "retrain budget fixture did not send information");
			m->packer.hdlc.tx_complete(l, outgoing);
		}
		m->get_chars = NULL;
		for (unsigned retry = 1; retry <= first_retry; ++retry) {
			m->bit_timer_func(m);
			if (retry == first_retry) {
				CHECK(modem_get_bits(m, 1, &bit, 1) == 1 &&
				      m->packer.hdlc.tx_frame != NULL,
				      "retrain budget fixture did not interrupt a wire poll");
			} else {
				outgoing = m->packer.hdlc.get_tx_frame(l);
				CHECK(outgoing != NULL && outgoing->buf[1] == 1 && outgoing->buf[2] == 1,
				      "retrain budget fixture did not send its enquiry");
				m->packer.hdlc.tx_complete(l, outgoing);
			}
		}
		if (full)
			CHECK(fill_lapm_control_queue(m), "retrain budget control queue did not fill");
		frame_t *queued = l->ctrl_list, *next = l->tx_ctrl, *retained = l->sent_info;
		void (*timeout)(struct modem *) = m->bit_timer_func;
		m->bit_timer = 1;
		for (unsigned resume = 0; resume < 3; ++resume) {
			CHECK(modem_ec_resume_after_retrain(m) == 0 &&
			      m->bit_timer == 1 && m->bit_timer_func == timeout &&
			      l->rtx_count == (int)(first_retry ? first_retry : 1) &&
			      l->retrain_poll && l->peer_busy && l->state == 2 && m->state == 5 &&
			      l->sent_info == retained && l->va == 0 && l->vs == information &&
			      source.calls == information && m->packer.hdlc.tx_frame == NULL &&
			      (!first_retry || (l->ctrl_list == queued && l->tx_ctrl == next)),
			      "retrain renewed recovery or lost the retained window: role %u info %u retry %u full %u",
			      caller, information, first_retry, full);
		}
		/* A ready command releases peer flow control without completing our
		 * enquiry. Its retained count must not trigger another physical retrain. */
		uint8_t ready[] = { l->rsp_addr, 1, 0 };
		frame_t command = { .buf = ready, .count = sizeof(ready) };
		m->packer.hdlc.rx_complete(l, &command);
		CHECK(!l->peer_busy && l->retrain_poll && !modem_ec_take_retrain_request(m),
		      "unfinished post-retrain enquiry requested another physical recovery");
		if (answered) {
			uint8_t final[] = { l->cmd_addr, 1, (uint8_t)((information << 1) | 1) };
			frame_t response = { .buf = final, .count = sizeof(final) };
			m->packer.hdlc.rx_complete(l, &response);
			CHECK(l->state == 2 && m->state == 5 && !l->rtx_count && !l->retrain_poll &&
			      !l->peer_busy && l->va == l->vs && m->bit_timer == 96000 &&
			      m->bit_timer_func != timeout && source.calls == information,
			      "retrain rejected the last permitted final response");
			CHECK(modem_stop_now(m) == 0, "retrain budget cleanup failed");
			modem_delete(m);
			continue;
		}
		for (unsigned expiry = 0; l->state == 2 && expiry < 6; ++expiry) {
			while ((outgoing = m->packer.hdlc.get_tx_frame(l)) != NULL) {
				CHECK(outgoing->size == 3 && outgoing->buf[1] == 1 && outgoing->buf[2] <= 1 &&
				      (outgoing->buf[0] == l->rsp_addr || outgoing->buf[2] == 1),
				      "retrain recovery queued an unexpected frame");
				enquiries += outgoing->buf[0] == l->cmd_addr;
				m->packer.hdlc.tx_complete(l, outgoing);
			}
			m->bit_timer_func(m);
		}
		CHECK(enquiries == 5 && l->state == 0 && !m->bit_timer &&
		      m->packer_process == NULL && source.calls == information &&
		      strcmp(modem_call_phase(m), "cleardown") == 0,
		      "retrain extended the five-enquiry recovery budget: role %u info %u retry %u full %u enquiries %u",
		      caller, information, first_retry, full, enquiries);
		modem_delete(m);
	}
	return true;
}

static bool test_lapm_stalled_retrain(void)
{
	enum { ONLINE = 5, LAPM_DATA_STATE = 2, LAPM_IDLE_STATE = 0 };
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
		call_progress_override = &operations;
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
			l->v44_session.phase = excluded == 2 ? LAPM_V44_COMMAND_PENDING : LAPM_V44_READY;
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
		      l->sent_info == retained && l->tx_info == retained->next->next &&
		      l->va == 0 && l->vs == 2,
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
		CHECK(!l->stalled_retrain && !l->rtx_count &&
		      l->sent_info == l->tx_info && l->va == 2,
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
		CHECK(retrain_dp.retrain_calls == 2 && l->state == LAPM_IDLE_STATE &&
		      strcmp(modem_call_phase(m), "cleardown") == 0 && !m->bit_timer &&
		      m->sample_timer - m->count <= 48,
		      "absent peer retained LAPM after retry exhaustion");
		/* Physical cleardown no longer owns an LAPM timeout to force. */
		operations.process = process_retrain_dp;
		for (unsigned tail = 0; tail < 2 && m->dp != NULL; ++tail)
			modem_process(m, receive, transmit, MODEM_FRAG);
		CHECK(m->dp == NULL && strcmp(modem_call_phase(m), "idle") == 0,
		      "exhausted LAPM did not finish physical release");
		CHECK(modem_stop_now(m) == 0 && retrain_dp.datapump.op == NULL, "stall fixture did not stop");
		modem_delete(m);
		call_progress_override = NULL;
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
		      l->vr == sequenced && l->peer_busy &&
		      l->sent_info == &l->info_buf[acknowledged].frame &&
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

static bool test_lapm_supervisory_polls(void)
{
	enum { DATA = 2, ONLINE = 5, RR = 1, RNR = 5, REJ = 9 };
	static const uint8_t controls[] = { RR, RNR, REJ };
	static const uint8_t sequences[] = { 0, 127 };
	static const uint8_t payload = 'X';
	struct modem_driver driver = {
		.name = "LAPM supervisory polls", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned caller = 0; caller < 2; ++caller)
	for (int busy = 0; busy < 2; ++busy)
	for (unsigned recovery = 0; recovery < 2; ++recovery)
	for (unsigned pending = 0; pending < 2; ++pending)
	for (size_t sequence = 0; sequence < ARRAY_LENGTH(sequences); ++sequence)
	for (size_t control = 0; control < ARRAY_LENGTH(controls); ++control) {
		struct modem *m = modem_create(&driver, driver.name);
		frame_t *retained = NULL, *outgoing;
		bool replay = pending && controls[control] == REJ && !recovery;
		unsigned base = sequences[sequence];

		CHECK(m != NULL, "supervisory poll allocation failed");
		m->cfg.ec = 1;
		m->cfg.comp = 0;
		CHECK(start_lapm_compression_fixture(m, caller), "supervisory poll fixture failed");
		struct lapm_state *l = &m->ec.lapm;
		CHECK(lapm_v44_session_call_reset(&l->v44_session) == LAPM_V44_OK,
		      "data fixture did not retire its startup transaction");
		l->state = DATA;
		l->va = l->vs = base;
		l->vr = 17;
		l->busy = busy;
		m->state = ONLINE;
		m->rx_rate = 48000;
		m->tx_rate = 26400;
		m->get_chars = scripted_lapm_source;
		memset(&lapm_source, 0, sizeof(lapm_source));
		lapm_source.data = &payload;
		lapm_source.length = pending;
		if (pending) {
			retained = m->packer.hdlc.get_tx_frame(l);
			CHECK(retained != NULL && retained->size == 4 && retained->buf[3] == payload,
			      "supervisory poll information fixture failed");
			m->packer.hdlc.tx_complete(l, retained);
		}
		l->rtx_count = recovery;
		l->retrain_poll = recovery;
		m->bit_timer = 1234;
		uint8_t wire[] = { l->rsp_addr, controls[control], (uint8_t)((base << 1) | 1) };
		frame_t incoming = { .buf = wire, .count = sizeof(wire) };
		m->packer.hdlc.rx_complete(l, &incoming);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		CHECK(outgoing != NULL && outgoing->size == 3 && outgoing->buf[0] == l->rsp_addr &&
		      outgoing->buf[1] == (busy ? RNR : RR) && outgoing->buf[2] == 35 &&
		      l->busy == busy && l->peer_busy == (controls[control] == RNR) &&
		      l->va == base && l->vs == ((base + (pending && !replay)) & 127) && l->vr == 17,
		      "incorrect poll reply: caller=%u busy=%d recovery=%u pending=%u control=%u base=%u",
		      caller, busy, recovery, pending, controls[control], base);
		CHECK(!recovery || (l->rtx_count == 1 && l->retrain_poll && m->bit_timer == 1234),
		      "peer poll completed or postponed our outstanding enquiry");
		m->packer.hdlc.tx_complete(l, outgoing);
		outgoing = m->packer.hdlc.get_tx_frame(l);
		if (replay) {
			CHECK(outgoing == retained && outgoing->size == 4 &&
			      outgoing->buf[1] == base << 1 && outgoing->buf[3] == payload,
			      "REJ poll did not retransmit the retained information");
			m->packer.hdlc.tx_complete(l, outgoing);
			outgoing = m->packer.hdlc.get_tx_frame(l);
		}
		CHECK(outgoing == NULL && lapm_source.offset == pending,
		      "peer poll duplicated its reply or released information during our enquiry");
		modem_delete(m);
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
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
		      lapm->state == 0 && lapm->v44_session.phase != LAPM_V44_COMMAND_PENDING &&
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
	CHECK(m->bit_timer == 28800 && m->ec.lapm.v44_session.phase == LAPM_V44_COMMAND_PENDING &&
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
	m->get_bits = modem_detector_get_bits;
	timeout = m->bit_timer_func;
	CHECK(m->bit_timer == 7200 &&
	      modem_set_param(m, MDMPRM_TX_RATE, 24000) == 0 &&
	      m->bit_timer == 43200 && m->bit_timer_func == timeout,
	      "rate update altered the detector's remaining T400 interval");
	modem_delete(m);
	return true;
}

static unsigned destroyed_v44_paths;

void __real_v44_character_path_destroy(struct v44_character_path *path);
void __wrap_v44_character_path_destroy(struct v44_character_path *path)
{
	if (path != NULL)
		destroyed_v44_paths++;
	__real_v44_character_path_destroy(path);
}

static bool test_lapm_exit_retires_framing(void)
{
	enum { INITIALIZED, PREAMBLE, QUEUED_REPLY, BORROWED_REPLY, DEFERRED_INPUT };
	struct modem_driver driver = {
		.name = "LAPM framing retirement", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	const struct v44_character_path_config codec = {
		.codec = { .transmit_enabled = true, .transmit_parameters = {
			.codeword_limit = V44_MIN_CODEWORDS,
			.max_string_length = V44_MIN_STRING_LENGTH,
			.history_size = V44_MIN_HISTORY_SIZE,
		} },
		.receive_buffer_capacity = 1,
		.read = lapm_v44_source, .write = lapm_v44_sink,
	};

	for (unsigned phase = INITIALIZED; phase <= DEFERRED_INPUT; ++phase)
	for (unsigned caller = 0; caller < 2; ++caller)
	for (unsigned compressed = 0; compressed < 2; ++compressed) {
		struct modem *m = modem_create(&driver, driver.name);
		uint8_t bits[32], wire[] = { 0, 0x7f, 0, 'x' };
		frame_t incoming = { .buf = wire, .count = 2 };
		unsigned destroyed = destroyed_v44_paths;

		CHECK(m != NULL, "framing retirement allocation failed");
		m->caller = caller;
		m->cfg.comp = 0;
		m->cfg.ec = 1;
		m->state = 4;
		CHECK(modem_ec_init(m) == 0, "framing retirement initialization failed");
		struct lapm_state *l = &m->ec.lapm;
		if (phase == INITIALIZED) {
			/* An initialized corrector does not own the raw packer. */
			modem_async_start(m);
			m->get_bits = modem_async_get_bits;
			m->put_bits = modem_async_put_bits;
			m->pack.bit = 3;
			m->pack.data = 5;
		} else {
			modem_hdlc_start(m);
			m->get_bits = modem_hdlc_get_bits;
			m->put_bits = modem_hdlc_put_bits;
			m->caller = phase == PREAMBLE;
			modem_ec_start(m);
			m->caller = caller;
			if (phase == PREAMBLE) {
				m->packer.hdlc.tx_preamble_flags = 1;
			} else if (phase == DEFERRED_INPUT) {
				l->state = 2;
				m->put_chars = scripted_lapm_sink;
				memset(&lapm_sink, 0, sizeof(lapm_sink));
				wire[0] = l->rsp_addr;
				wire[1] = 0;
				incoming.count = sizeof(wire);
				m->packer.hdlc.rx_complete(l, &incoming);
				CHECK(l->rx_count && m->packer_process != NULL,
				      "deferred input fixture did not queue work");
			} else {
				wire[0] = l->rsp_addr;
				m->packer.hdlc.rx_complete(l, &incoming);
				CHECK(l->tx_ctrl != l->ctrl_list, "reply fixture did not queue a frame");
				if (phase == BORROWED_REPLY) {
					CHECK(modem_hdlc_get_bits(m, 1, bits, 1) == 1 &&
					      m->packer.hdlc.tx_frame != NULL,
					      "reply fixture did not borrow a frame");
				}
			}
		}
		if (compressed) {
			l->v44_path = v44_character_path_create(&codec);
			CHECK(l->v44_path != NULL, "retirement codec allocation failed");
		}
		unsigned state = m->state;
		modem_ec_exit(m);
		modem_ec_exit(m);
		CHECK(l->state == 0 && l->modem == NULL && l->info_list == NULL &&
		      l->ctrl_list == NULL && l->v44_path == NULL && !l->rx_count &&
		      m->packer_process == NULL && destroyed_v44_paths == destroyed + compressed,
		      "exit retained LAPM ownership at phase %u", phase);
		if (phase == INITIALIZED) {
			CHECK(m->get_bits == modem_async_get_bits && m->put_bits == modem_async_put_bits &&
			      m->pack.bit == 3 && m->pack.data == 5,
			      "exit reset an independently owned raw packer");
		} else {
			struct hdlc_state *h = &m->packer.hdlc;
			CHECK(h->framer == NULL && h->get_tx_frame == NULL &&
			      h->tx_complete == NULL && h->rx_complete == NULL &&
			      h->tx_frame == NULL && !h->tx_preamble_flags &&
			      h->tx_preamble_complete == NULL && !m->bit_timer &&
			      m->bit_timer_func == NULL,
			      "exit retained a framing callback at phase %u", phase);
			CHECK(modem_get_bits(m, 8, bits, sizeof(bits)) == sizeof(bits),
			      "retired framing did not generate idle flags");
			for (unsigned i = 0; i < sizeof(bits); ++i)
				CHECK(bits[i] == 0x7e, "retired framing transmitted a stale frame");
			CHECK(modem_put_bits(m, 8, bits, sizeof(bits)) == sizeof(bits) &&
			      l->state == 0 && m->state == state && !m->bit_timer,
			      "late framing input restarted retired LAPM");
		}
		modem_delete(m);
		CHECK(destroyed_v44_paths == destroyed + compressed,
		      "modem destruction repeated codec release");
	}
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
					int state = l->state;
					int retries = l->rtx_count, remaining = resuming->bit_timer;
					void (*timeout)(struct modem *) = resuming->bit_timer_func;

					modem_dp_reconnected(resuming);
					CHECK(l->state == state &&
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
		unsigned destroyed = destroyed_v44_paths;

		CHECK(!peers[side]->started && peers[side]->ec.lapm.v44_path != NULL,
		      "partial modem fixture did not retain negotiated V.44 memory");
		peers[side]->dp = NULL;
		modem_delete(peers[side]);
		CHECK(destroyed_v44_paths == destroyed + 1,
		      "modem destruction retained its V.44 path without a started datapump");
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
	struct modem_driver driver = {
		.name = "V.21 mark acquisition test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct fsk_transmitter answerer;
	const struct dp_operations *operations;
	struct modem *modem;
	struct dp *datapump;
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];

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
	return true;
}

static bool process_owned_datapump(struct dp *dp)
{
	int16_t receive[MODEM_FRAG] = { 0 }, transmit[MODEM_FRAG];
	int status;

	if (dp->op->process_g711 != NULL) {
		uint8_t receive_codewords[MODEM_FRAG], transmit_codewords[MODEM_FRAG];
		bool exact = false;

		memset(receive_codewords, g711_encode_pcmu(0), sizeof(receive_codewords));
		status = dp->op->process_g711(dp, G711_LAW_PCMU,
			receive_codewords, receive, NULL, transmit_codewords, transmit,
			MODEM_FRAG, &exact);
	} else {
		status = dp->op->process(dp, receive, transmit, MODEM_FRAG);
	}
	CHECK(status == DPSTAT_OK || status == DPSTAT_CONNECT,
	      "%s stopped after another modem changed ownership (status %d)",
	      dp->op->name, status);
	return true;
}

static bool test_datapump_factory_instances(void)
{
	static const struct {
		enum DP_ID catalog_id;
		enum DP_ID requested_id;
		unsigned rate;
	} profiles[] = {
		{ DP_CALLPROG, DP_CALLPROG, 300 },
		{ DP_V8, DP_V21, 300 },
		{ DP_V21, DP_V21, 300 },
		{ DP_B103, DP_B103, 300 },
		{ DP_V23, DP_V23, 1200 },
		{ DP_V22, DP_V22, 1200 },
		{ DP_B212, DP_B212, 1200 },
		{ DP_V22BIS, DP_V22BIS, 2400 },
		{ DP_V32, DP_V32, 9600 },
		{ DP_V32BIS, DP_V32BIS, 14400 },
		{ DP_V34, DP_V34, V34_MAX_BIT_RATE },
		{ DP_V90, DP_V90, 28000 },
		{ DP_V92, DP_V92, 28000 },
		{ DP_BAUDOT, DP_BAUDOT, 45 },
	};
	struct modem_driver driver = {
		.name = "factory ownership test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modems[3];
	struct dp *calls[3];

	for (size_t i = 0; i < ARRAY_LENGTH(modems); i++) {
		modems[i] = modem_create(&driver, "factory ownership test");
		CHECK(modems[i] != NULL &&
		      modem_set_param(modems[i], MDMPRM_V92_QUICK_CONNECT, 0) == 0,
		      "independent modem %zu did not initialize", i);
	}
	for (size_t i = 0; i < ARRAY_LENGTH(profiles); i++) {
		const struct dp_operations *op =
			modem_dp_get_registered_ops(profiles[i].catalog_id);
		enum DP_ID id = profiles[i].requested_id;

		CHECK(op != NULL, "factory %u is unavailable", profiles[i].catalog_id);
		for (int caller = profiles[i].catalog_id == DP_CALLPROG; caller <= 1; caller++) {
			for (size_t j = 0; j < ARRAY_LENGTH(modems); j++) {
				modems[j]->min_rate = modems[j]->max_rate = profiles[i].rate;
				modems[j]->dsp_info.pcm_role = j == 1 ?
					MODEM_PCM_ROLE_DIGITAL : MODEM_PCM_ROLE_ANALOG;
				calls[j] = op->create(modems[j], id, caller, MODEM_RATE, MODEM_FRAG, op);
				CHECK(calls[j] != NULL && calls[j]->modem == modems[j] &&
				      calls[j]->op == op,
				      "%s role %d did not own modem %zu", op->name, caller, j);
				for (size_t k = 0; k < j; k++)
					CHECK(calls[j] != calls[k], "%s shared a mutable instance", op->name);
			}
			CHECK(op->delete(calls[0]) == 0,
			      "%s failed to release its first modem", op->name);
			CHECK(process_owned_datapump(calls[1]),
			      "%s deletion stopped another modem", op->name);
			calls[0] = op->create(modems[0], id, caller, MODEM_RATE, MODEM_FRAG, op);
			CHECK(calls[0] != NULL && calls[0] != calls[1] && calls[0] != calls[2],
			      "%s failed to replace its first modem", op->name);
			CHECK(process_owned_datapump(calls[2]),
			      "%s replacement stopped another modem", op->name);
			for (size_t j = ARRAY_LENGTH(calls); j > 0; j--)
				CHECK(op->delete(calls[j - 1]) == 0,
				      "%s failed to release modem %zu", op->name, j - 1);
		}
		/* Callback identity is part of factory ownership, even when every
		 * method in a foreign copy happens to match the built-in family. */
		struct dp_operations foreign = *op;
		int caller = 1;
		struct modem *m = modems[0];

		CHECK(op->create(m, id, caller, MODEM_RATE, MODEM_FRAG, &foreign) == NULL &&
		      op->create(m, id, caller, MODEM_RATE, MODEM_FRAG, NULL) == NULL &&
		      op->create(NULL, id, caller, MODEM_RATE, MODEM_FRAG, op) == NULL &&
		      op->create(m, DP_DUMMY, caller, MODEM_RATE, MODEM_FRAG, op) == NULL &&
		      op->create(m, id, -1, MODEM_RATE, MODEM_FRAG, op) == NULL &&
		      op->create(m, id, 2, MODEM_RATE, MODEM_FRAG, op) == NULL &&
		      op->create(m, id, caller, MODEM_RATE - 1, MODEM_FRAG, op) == NULL &&
		      op->create(m, id, caller, MODEM_RATE, MODEM_FRAG - 1, op) == NULL,
		      "%s accepted foreign operations or invalid factory arguments", op->name);
	}
	for (size_t i = 0; i < ARRAY_LENGTH(modems); i++)
		modem_delete(modems[i]);
	return true;
}

struct factory_thread {
	struct modem *modem;
	atomic_bool *started;
	unsigned failures;
};

static void *run_factory_thread(void *opaque)
{
	struct factory_thread *thread = opaque;
	const struct dp_operations *op = modem_dp_get_registered_ops(DP_V21);

	while (!atomic_load_explicit(thread->started, memory_order_acquire))
		sched_yield();
	for (unsigned i = 0; i < 1000; i++) {
		struct dp *dp = op->create(thread->modem, DP_V21, 1, MODEM_RATE, MODEM_FRAG, op);

		if (dp == NULL) {
			thread->failures++;
			continue;
		}
		if (dp->modem != thread->modem || !process_owned_datapump(dp))
			thread->failures++;
		if (op->delete(dp) != 0)
			thread->failures++;
	}
	return NULL;
}

static bool test_concurrent_datapump_lifetimes(void)
{
	struct modem_driver driver = {
		.name = "concurrent factory test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct factory_thread workers[8] = { 0 };
	pthread_t threads[ARRAY_LENGTH(workers)];
	atomic_bool started = false;
	size_t created = 0;
	bool valid = true;

	for (size_t i = 0; i < ARRAY_LENGTH(workers); i++) {
		workers[i].modem = modem_create(&driver, "concurrent factory test");
		workers[i].started = &started;
		if (workers[i].modem == NULL) {
			valid = false;
			break;
		}
		workers[i].modem->min_rate = workers[i].modem->max_rate = 300;
		if (pthread_create(&threads[i], NULL, run_factory_thread, &workers[i]) != 0) {
			valid = false;
			break;
		}
		created++;
	}
	/* Also release and join an incomplete fixture before reporting failure. */
	atomic_store_explicit(&started, true, memory_order_release);
	for (size_t i = 0; i < created; i++) {
		valid &= pthread_join(threads[i], NULL) == 0;
		valid &= workers[i].failures == 0;
	}
	for (size_t i = 0; i < ARRAY_LENGTH(workers); i++)
		if (workers[i].modem != NULL)
			modem_delete(workers[i].modem);
	CHECK(valid, "independent concurrent datapump lifetimes failed");
	return true;
}

static bool test_call_progress_supports_independent_modems(void)
{
	struct modem modems[2] = { 0 };
	struct dp *calls[2];
	const struct dp_operations *operations;
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];

	operations = modem_dp_get_registered_ops(DP_CALLPROG);
	for (unsigned i = 0; i < 2; ++i)
		calls[i] = operations->create(
			&modems[i], DP_CALLPROG, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(calls[0] != NULL && calls[1] != NULL && calls[0] != calls[1] &&
		      calls[0]->modem == &modems[0] && calls[1]->modem == &modems[1],
	      "one modem's call progress prevented another independent instance");
	CHECK(operations->delete(calls[0]) == 0,
	      "call progress did not release one owned instance");
	memset(transmit, 1, sizeof(transmit));
	CHECK(operations->process(calls[1], receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      samples_are_silent(transmit, MODEM_FRAG),
	      "remaining call progress stopped processing silently");
	calls[0] = operations->create(
		&modems[0], DP_CALLPROG, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(calls[0] != NULL && operations->delete(calls[1]) == 0 &&
		      operations->delete(calls[0]) == 0,
	      "call progress did not support independent replacement and deletion");
	return true;
}

static bool test_at_ms_uses_builtin_catalog(void)
{
	struct modem_driver driver = {
		.name = "modulation selection test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem = modem_create(&driver, "modulation selection test");
	char query[] = "AT+MS=?";
	char response[256];
	int output[2];
	ssize_t count;

	CHECK(modem != NULL && pipe(output) == 0,
	      "modulation selection fixture did not initialize");
	modem->pty = output[1];
	CHECK(process_at_command(modem, query) == 0,
	      "AT+MS did not enumerate the built-in catalog");
	close(output[1]);
	modem->pty = -1;
	count = read(output[0], response, sizeof(response) - 1);
	close(output[0]);
	CHECK(count > 0, "AT+MS returned no modulation choices");
	response[count] = '\0';
	CHECK(strstr(response,
	      "(21,22,23,122,32,132,34,45,103,212,90,91,92),(0,1),") != NULL,
	      "AT+MS choices changed: %s", response);
	char unsupported[] = "AT+MS=56";
	unsigned selected = MODEM_DP(modem);

	CHECK(process_at_command(modem, unsupported) == -1 &&
	      MODEM_DP(modem) == selected,
	      "AT+MS accepted an unavailable modulation or changed the selection");
	modem_delete(modem);
	return true;
}

static bool test_at_ms_rejects_wrapped_values(void)
{
	static const char *invalid[] = {
		"AT+MS=4294967386", "AT+MS=-4294967206",
		"AT+MS=-18446744073709551526", "AT+MS=90, -18446744073709551615",
		"AT+MS=18446744073709551616", "AT+MS=90,4294967297",
		"AT+MS=90,-4294967295", "AT+MS=90,1,4294967596,56000",
		"AT+MS=90,1,300,4295023296", "AT+MS=90,1,-4294966996,56000",
		"AT+MS=90,1,300,-4294911296", "AT+MS=90,1,300,56000,",
	};
	struct modem_driver driver = {
		.name = "modulation input test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, "modulation input test");
	char initial[] = "AT+MS=34,0,2400,33600";
	char omitted[] = "AT+MS=90";
	char sparse[] = "AT+MS=,1,,56000";
	char chained[] = "AT+MS=92+V90D=1";
	char command[96];

	CHECK(m != NULL && process_at_command(m, initial) == 0,
	      "modulation input fixture did not initialize");
	for (size_t i = 0; i < ARRAY_LENGTH(invalid); i++) {
		snprintf(command, sizeof(command), "%s", invalid[i]);
		CHECK(process_at_command(m, command) == -1 &&
		      MODEM_DP(m) == DP_V34 && MODEM_AUTOMODE(m) == 0 &&
		      m->min_rate == 2400 && m->max_rate == 33600,
		      "invalid modulation input altered state: %s", invalid[i]);
	}
	CHECK(process_at_command(m, omitted) == 0 && MODEM_DP(m) == DP_V90 &&
	      MODEM_AUTOMODE(m) == 0 && m->min_rate == 2400 && m->max_rate == 33600,
	      "omitted AT+MS values did not retain the current settings");
	CHECK(process_at_command(m, sparse) == 0 && MODEM_DP(m) == DP_V90 &&
	      MODEM_AUTOMODE(m) == 1 && m->min_rate == 2400 && m->max_rate == 56000,
	      "empty AT+MS fields did not preserve the unrelated settings");
	CHECK(process_at_command(m, chained) == 0 && MODEM_DP(m) == DP_V92 &&
	      modem_get_param(m, MDMPRM_V90_DIGITAL_INDICATION) == 1,
	      "AT+MS did not preserve following PCM configuration commands");
	modem_delete(m);
	return true;
}

static bool test_builtin_datapump_lifecycle(void)
{
	struct modem_driver driver = {
		.name = "call progress test",
		.start = unused_start,
		.stop = unused_stop,
		.ioctl = unused_control,
	};
	struct modem *modem;
	const struct dp_operations *operations;
	const struct dp_operations *v8_operations;
	const struct dp_operations *v21_operations;
	const struct dp_operations *bell103_operations;
	const struct dp_operations *v23_operations;
	const struct dp_operations *v22_operations;
	const struct dp_operations *v22bis_operations;
	const struct dp_operations *bell212_operations;
	const struct dp_operations *v32_operations;
	const struct dp_operations *v32bis_operations;
	const struct dp_operations *v34_operations;
	const struct dp_operations *v90_operations;
	const struct dp_operations *v92_operations;
	const struct dp_operations *baudot_operations;
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
		      operations->hangup == NULL && v8_operations != NULL &&
		      v8_operations != operations &&
		      v8_operations->create != NULL &&
		      v8_operations->delete != NULL &&
		      v8_operations->process != NULL &&
		      v21_operations != NULL && v21_operations->create != NULL &&
		      v21_operations->delete != NULL &&
		      v21_operations->process != NULL &&
		      v21_operations->retrain == NULL &&
		      bell103_operations == v21_operations &&
		      v23_operations != NULL &&
		      v23_operations != v21_operations &&
		      v23_operations->create != NULL &&
		      v23_operations->delete != NULL &&
		      v23_operations->process != NULL &&
		      v23_operations->retrain == NULL &&
		      v22_operations != NULL &&
		      v22_operations != v21_operations &&
		      v22_operations != v23_operations &&
		      v22_operations->create != NULL &&
		      v22_operations->delete != NULL &&
		      v22_operations->process != NULL &&
		      v22_operations->retrain == NULL &&
		      v22bis_operations != NULL &&
		      v22bis_operations != v22_operations &&
		      v22bis_operations->create == v22_operations->create &&
		      v22bis_operations->delete == v22_operations->delete &&
		      v22bis_operations->process == v22_operations->process &&
		      v22bis_operations->retrain != NULL &&
		      bell212_operations == v22_operations &&
		      bell212_operations->retrain == NULL &&
		      v32_operations != NULL &&
		      v32_operations != v21_operations &&
		      v32_operations != v23_operations &&
		      v32_operations != v22_operations &&
		      v32_operations->create != NULL &&
		      v32_operations->delete != NULL &&
		      v32_operations->process != NULL &&
		      v32bis_operations == v32_operations &&
		      v34_operations != NULL &&
		      v34_operations != v21_operations &&
		      v34_operations != v23_operations &&
		      v34_operations != v22_operations &&
		      v34_operations != v32_operations &&
		      v34_operations->create != NULL &&
		      v34_operations->delete != NULL &&
		      v34_operations->process != NULL &&
		      v90_operations != NULL &&
		      v90_operations != v34_operations &&
		      v90_operations->create != NULL &&
		      v90_operations->delete != NULL &&
		      v90_operations->process != NULL &&
		      v90_operations->process_g711 != NULL &&
		      v92_operations == v90_operations &&
		      baudot_operations != NULL &&
		      baudot_operations != v21_operations &&
		      baudot_operations != v23_operations &&
		      baudot_operations != v22_operations &&
		      baudot_operations->create != NULL &&
		      baudot_operations->delete != NULL &&
		      baudot_operations->process != NULL,
	      "source-built modem operations were incomplete");
	modem = modem_create(&driver, "call progress test");
	CHECK(modem != NULL, "call-progress modem could not be created");
	v8 = v8_operations->create(
		modem, DP_V21, 1, MODEM_RATE, MODEM_FRAG, v8_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v8 != NULL && v8->id == DP_V8 &&
		      v8_operations->process(
			      v8, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      v8_operations->delete(v8) == 0,
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
		      v22_operations->delete(v22) == 0,
	      "V.22 600 data pump did not wait silently and release cleanly");
	modem->min_rate = modem->max_rate = 1200;
	v22 = v22_operations->create(
		modem, DP_V22, 1, MODEM_RATE, MODEM_FRAG, v22_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v22 != NULL &&
		      v22_operations->process(
			      v22, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      v22_operations->delete(v22) == 0,
	      "V.22 data pump did not wait silently and release cleanly");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "originating V.22 data pump leaked pre-answer audio");
	bell212 = bell212_operations->create(
		modem, DP_B212, 1, MODEM_RATE, MODEM_FRAG,
		bell212_operations);
	CHECK(bell212 != NULL && bell212->id == DP_B212 &&
		      bell212_operations->delete(bell212) == 0,
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
		      v22bis_operations->delete(v22bis) == 0,
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
			      SINGLE_TONE_OK,
		      "V.32 data pump did not initialize its line startup");
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
	CHECK(v32_operations->delete(v32) == 0,
	      "V.32 data pump did not release after line startup");
	/* The shared DSP must retain the selected protocol's rate set. */
	static const unsigned int bis_rates[] = { 7200, 12000, 14400 };
	for (size_t index = 0; index < ARRAY_LENGTH(bis_rates); ++index) {
		modem->min_rate = modem->max_rate = bis_rates[index];
		CHECK(v32_operations->create(modem, DP_V32, 1, MODEM_RATE,
			      MODEM_FRAG, v32_operations) == NULL,
		      "V.32 accepted the V.32bis-only rate %u", bis_rates[index]);
		v32bis = v32bis_operations->create(modem, DP_V32BIS, 1,
			MODEM_RATE, MODEM_FRAG, v32bis_operations);
		CHECK(v32bis != NULL && v32bis_operations->delete(v32bis) == 0,
		      "V.32bis rejected its rate %u", bis_rates[index]);
	}
	modem->min_rate = modem->max_rate = 14400;
	v32bis = v32bis_operations->create(
		modem, DP_V32BIS, 1, MODEM_RATE, MODEM_FRAG,
		v32bis_operations);
	CHECK(v32bis != NULL && v32bis->id == DP_V32BIS &&
		      v32bis_operations->delete(v32bis) == 0,
	      "V.32bis did not share the V.32 data pump");
	modem->min_rate = V34_MAX_BIT_RATE + 1;
	modem->max_rate = V34_MAX_BIT_RATE + V34_RATE_STEP;
	CHECK(v34_operations->create(
		      modem, DP_V34, 1, MODEM_RATE, MODEM_FRAG,
		      v34_operations) == NULL,
	      "V.34 accepted a range above its implemented rates");
	modem->min_rate = modem->max_rate = V34_MAX_BIT_RATE;
	v34 = v34_operations->create(
		modem, DP_V34, 1, MODEM_RATE, MODEM_FRAG, v34_operations);
	memset(receive, 0, sizeof(receive));
	memset(transmit, 1, sizeof(transmit));
	CHECK(v34 != NULL && v34->id == DP_V34 &&
		      v34_operations->process(
			      v34, receive, transmit, MODEM_FRAG) == DPSTAT_OK &&
		      v34_operations->delete(v34) == 0,
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
		      v90_operations->delete(v90) == 0,
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
		      v92_operations->delete(v92) == 0,
	      "V.92 did not retain the shared PCM call state");
	modem->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
	modem->min_rate = 46;
	modem->max_rate = 49;
	CHECK(baudot_operations->create(
		      modem, DP_BAUDOT, 1, MODEM_RATE, MODEM_FRAG,
		      baudot_operations) == NULL,
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
		      baudot_operations->delete(baudot) == 0,
	      "Baudot data pump did not link and release cleanly");
	for (size_t index = 0; index < ARRAY_LENGTH(transmit); ++index)
		CHECK(transmit[index] == 0,
		      "idle Baudot data pump did not mute its output");
	call_progress = operations->create(
		modem, DP_CALLPROG, 1, MODEM_RATE, MODEM_FRAG, operations);
	CHECK(call_progress != NULL && call_progress->modem == modem &&
		      call_progress->status == DPSTAT_OK,
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
	CHECK(operations->delete(call_progress) == 0,
	      "call progress did not release its owned instance");
	modem_delete(modem);
	return true;
}

static bool test_v42_late_answer_detection(void)
{
	struct modem_driver driver = {
		.name = "late V.42 reply", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};
	struct modem *m = modem_create(&driver, "late V.42 reply");
	struct dp dp = { .id = DP_V22BIS, .modem = m };
	struct modem answerer = { .name = "ADP source" };
	uint8_t bits[73], marks[12];

	CHECK(m != NULL && modem_ec_init(m) == 0,
	      "late detection allocation failed");
	m->dp = &dp;
	m->caller = 1;
	m->tx_rate = m->rx_rate = 2400;
	m->cfg.ec = m->cfg.ec_detector = 1;
	modem_dp_connected(m);
	m->xmit.buf[0] = 'Z';
	m->xmit.count = m->xmit.head = 1;
	memset(marks, 1, sizeof(marks));
	/* A real legacy answerer can reply around 800 ms after carrier.
	 * Advance both physical clocks rather than changing the timer directly. */
	for (unsigned tick = 0; tick < 160; ++tick) {
		CHECK(modem_get_bits(m, 1, bits, 12) == 12 &&
		      modem_put_bits(m, 1, marks, 12) == 12,
		      "late detection clock failed");
	}
	CHECK(m->get_bits == modem_detector_get_bits && m->xmit.count == 1,
	      "originator abandoned detection before the late ADP");
	answerer.packer.detector.tx_count = 1;
	answerer.packer.detector.tx_pattern[0] = 0xa2;
	answerer.packer.detector.tx_pattern[1] = 0xc2;
	CHECK(modem_detector_get_bits(&answerer, 1, bits, sizeof(bits)) == sizeof(bits) &&
	      modem_put_bits(m, 1, bits, sizeof(bits)) == sizeof(bits),
	      "late ADP exchange failed");
	CHECK(m->get_bits == modem_hdlc_get_bits &&
	      m->put_bits == modem_hdlc_put_bits && m->cfg.ec &&
	      m->xmit.count == 1,
	      "late ADP did not select LAPM with queued data intact");
	m->dp = NULL;
	modem_ec_exit(m);
	modem_delete(m);
	return true;
}

static bool test_v42_raw_text_detection(void)
{
	static const char *const lines[] = {
		"\r\nWelcome", "\n\rLogin:", "\r\r", "login:\r", "hello\n",
		"A printable banner with no newline yet",
	};
	struct modem_driver driver = {
		.name = "V.42 text reply", .start = unused_start,
		.stop = unused_stop, .ioctl = unused_control,
	};

	for (unsigned width = 1; width <= 8; width *= 2) {
		for (size_t line = 0; line < ARRAY_LENGTH(lines); ++line) {
			struct modem *m = modem_create(&driver, "V.42 text reply");
			struct dp dp = { .id = DP_V22BIS, .modem = m };
			struct modem source = { .get_chars = scripted_lapm_source };
			uint8_t bits[512], output, received[128];
			int result_pipe[2];
			ssize_t received_length;
			size_t bit_count = strlen(lines[line]) * 10;

			CHECK(m != NULL && pipe(result_pipe) == 0 &&
			      fcntl(result_pipe[0], F_SETFL, O_NONBLOCK) == 0,
			      "text reply allocation failed");
			m->pty = result_pipe[1];
			m->sregs[SREG_QUIET] = 1;
			m->dp = &dp;
			m->caller = 1;
			m->tx_rate = m->rx_rate = 2400;
			m->cfg.ec = m->cfg.ec_detector = 1;
			modem_dp_connected(m);
			lapm_source.data = (const uint8_t *)lines[line];
			lapm_source.length = strlen(lines[line]);
			lapm_source.offset = 0;
			modem_async_start(&source);
			CHECK(modem_async_get_bits(&source, 1, bits, bit_count) == (int)bit_count,
			      "text reply encoding failed");
			for (size_t offset = 0; offset < bit_count;) {
				unsigned count = bit_count - offset < width ?
					(unsigned)(bit_count - offset) : width;
				uint8_t value = 0;

				for (unsigned bit = 0; bit < count; ++bit)
					value = (uint8_t)((value << 1) | bits[offset++]);
				CHECK(modem_put_bits(m, count, &value, 1) == 1 &&
				      modem_get_bits(m, count, &output, 1) == 1,
				      "text reply duplex transfer failed");
			}
			for (unsigned tail = 0; tail < 48 && m->get_bits == modem_detector_get_bits; ++tail)
				CHECK(modem_get_bits(m, 1, &output, 1) == 1,
				      "text reply ODP tail failed");
			received_length = read(result_pipe[0], received, sizeof(received));
			CHECK(m->get_bits == modem_async_get_bits && !m->cfg.ec &&
			      received_length == (ssize_t)strlen(lines[line]) &&
			      memcmp(received, lines[line], strlen(lines[line])) == 0,
			      "text reply lost data or waited for timeout: width %u line %zu length %zd",
			      width, line, received_length);
			close(result_pipe[0]);
			close(result_pipe[1]);
			m->pty = -1;
			m->dp = NULL;
			modem_delete(m);
		}
	}
	memset(&lapm_source, 0, sizeof(lapm_source));
	memset(&lapm_sink, 0, sizeof(lapm_sink));
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
		{ "V.42 detection deadline", test_v42_detection_deadline },
		{ "automatic factory profile", test_automatic_factory_profile },
		{ "automatic startup deadline", test_automatic_startup_deadline },
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
		{ "call progress cancellation without media",
		  test_call_progress_cancels_without_media },
		{ "stop cancels link detection", test_stop_cancels_link_detection },
		{ "hangup follows active framing", test_hangup_uses_active_framing },
		{ "bounded AT dial destinations",
		  test_at_dial_destinations_are_bounded },
		{ "AT compression profile", test_at_compression_profile },
		{ "AT compression numeric validation",
		  test_at_compression_rejects_invalid_numbers },
		{ "AT PCM profile controls", test_at_pcm_profile_controls },
		{ "AT PCM numeric validation", test_at_pcm_rejects_invalid_numbers },
		{ "AT PCM upstream rate queries", test_at_pcm_upstream_rate_queries },
		{ "AT automatic PCM upstream policy", test_at_pcm_automatic_upstream_policy },
		{ "AT V.34 symbol rate", test_at_v34_symbol_rate },
		{ "LAPM XID parameter encoding", test_lapm_xid_parameters },
		{ "LAPM V.42bis negotiation", test_lapm_v42bis_negotiation },
		{ "LAPM frozen V.42bis offer", test_lapm_v42bis_frozen_offer },
		{ "LAPM V.42bis configuration failure", test_lapm_v42bis_configuration_failure },
		{ "LAPM V.42bis reinitialization", test_lapm_v42bis_reinitialization },
		{ "LAPM V.42bis invalid parameters", test_lapm_v42bis_invalid_parameters },
		{ "LAPM V.42bis decoder failure", test_lapm_v42bis_decoder_failure },
		{ "LAPM V.42bis receive output pressure", test_lapm_v42bis_receive_pressure },
		{ "LAPM V.44 receive output pressure", test_lapm_v44_receive_pressure },
		{ "LAPM V.42bis small transmit buffers", test_lapm_v42bis_transmit_pressure },
		{ "LAPM XID negotiation directions", test_lapm_xid_directions },
		{ "LAPM XID numeric values", test_lapm_xid_numeric_values },
		{ "LAPM XID window counterproposal", test_lapm_xid_window_counteroffer },
		{ "LAPM simultaneous XID negotiation", test_lapm_xid_collisions },
		{ "LAPM repeated XID parameters", test_lapm_repeated_xid_parameters },
		{ "bounded LAPM XID envelopes", test_lapm_xid_envelopes },
		{ "bounded LAPM compression retry", test_lapm_dual_compression_retry },
		{ "LAPM directional preamble", test_lapm_directional_preamble },
		{ "LAPM peer-flag preamble", test_lapm_peer_flag_preamble },
		{ "LAPM receive backpressure", test_lapm_receive_backpressure },
		{ "LAPM receiver-ready queue pressure", test_lapm_receiver_ready_queue_pressure },
		{ "LAPM acknowledgment publication", test_lapm_acknowledgment_publication },
		{ "LAPM reject queue pressure", test_lapm_reject_queue_pressure },
		{ "LAPM fatal information failure", test_lapm_information_failure },
		{ "LAPM fatal control failure", test_lapm_control_failure },
		{ "LAPM receive frame boundaries", test_lapm_receive_frame_boundaries },
		{ "LAPM receive information limit", test_lapm_receive_information_limit },
		{ "LAPM transmit information limit", test_lapm_transmit_information_limit },
		{ "LAPM peer-ready DM establishment", test_lapm_peer_ready_dm },
		{ "LAPM SABME establishment readiness", test_lapm_sabme_readiness },
		{ "LAPM defaults without negotiation", test_lapm_default_establishment },
		{ "LAPM connected SABME", test_lapm_connected_sabme },
		{ "LAPM unnumbered response states", test_lapm_unnumbered_responses },
		{ "LAPM disconnected DISC response", test_lapm_disconnected_disc },
		{ "LAPM orderly release discards stream work", test_lapm_orderly_release },
		{ "LAPM recovery retry boundaries", test_lapm_recovery_boundaries },
		{ "LAPM mode command collisions", test_lapm_mode_command_collisions },
		{ "LAPM numbered frame rejection", test_lapm_numbered_frame_rejection },
		{ "LAPM unnumbered frame rejection", test_lapm_unnumbered_frame_rejection },
		{ "LAPM acknowledgement queue consistency", test_lapm_acknowledgement_queue_consistency },
		{ "LAPM acknowledgement window", test_lapm_acknowledgement_window },
		{ "LAPM repeated sequence error poll", test_lapm_repeated_sequence_error_poll },
		{ "LAPM peer-busy acknowledgement", test_lapm_peer_busy_acknowledgement },
		{ "LAPM supervisory command polls", test_lapm_supervisory_polls },
		{ "LAPM peer-busy timer", test_lapm_peer_busy_timer },
		{ "LAPM idle supervision", test_lapm_idle_supervision },
		{ "LAPM retrain recovery budget", test_lapm_retrain_recovery_budget },
		{ "LAPM acknowledgement progress", test_lapm_acknowledgement_progress },
		{ "bounded LAPM stalled retrain", test_lapm_stalled_retrain },
		{ "LAPM retrain preserves link state",
		  test_lapm_retrain_preserves_link_state },
		{ "retrain preserves V.42 detection", test_retrain_preserves_detection },
		{ "retrain preserves LAPM establishment",
		  test_retrain_preserves_lapm_establishment },
		{ "LAPM exit retires framing", test_lapm_exit_retires_framing },
		{ "LAPM timer follows rate changes", test_lapm_timer_follows_rate_changes },
		{ "V.21 brief answer mark",
		  test_v21_originator_answers_brief_mark },
		{ "retained call-end cause", test_disconnect_cause_survives_call_teardown },
		{ "independent datapump factory instances", test_datapump_factory_instances },
		{ "concurrent datapump lifetimes", test_concurrent_datapump_lifetimes },
		{ "AT+MS built-in catalog", test_at_ms_uses_builtin_catalog },
		{ "AT+MS rejects wrapped values", test_at_ms_rejects_wrapped_values },
		{ "source-built data-pump ownership",
		  test_builtin_datapump_lifecycle },
		{ "independent call-progress modems",
		  test_call_progress_supports_independent_modems },
		{ "V.42 late answer detection", test_v42_late_answer_detection },
		{ "V.42 early raw text", test_v42_raw_text_detection },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		call_progress_override = NULL;
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
