// SPDX-License-Identifier: BSD-3-Clause
#include <modem.h>
#include <modem_state.h>
#include <modem_frontend.h>
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#define EXPECT(condition) CHECK((condition), "%s", #condition)

struct capture {
	unsigned char bytes[4096];
	size_t count;
	size_t limit;
};

static int output(void *context, const char *bytes, int count)
{
	struct capture *capture = context;
	size_t accepted = (size_t)count;
	if (accepted > capture->limit - capture->count)
		accepted = capture->limit - capture->count;
	memcpy(capture->bytes + capture->count, bytes, accepted);
	capture->count += accepted;
	return (int)accepted;
}

static int start(struct modem *modem) { (void)modem; return 0; }
static int stop(struct modem *modem) { (void)modem; return 0; }
static int control(struct modem *modem, unsigned cmd, unsigned long arg)
{ (void)modem; (void)cmd; (void)arg; return 0; }

static struct modem *create(struct capture *data, struct capture *commands)
{
	struct modem_driver driver = { "literal DTE", start, stop, control };
	struct modem *modem = modem_create(&driver, "literal");
	if (modem != NULL) {
		modem->data_output = output;
		modem->data_output_context = data;
		modem->control_output = output;
		modem->control_output_context = commands;
	}
	return modem;
}

static bool literal_and_partial(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL && modem->pty == -1 && modem->frontend_report == NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	const unsigned char literal[] = { 0, '+', '+', '+', '\r', 'A', 'T', '\r', 0xff };
	unsigned char received[4096], input[4096];
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == (int)sizeof(literal));
	EXPECT(modem->escape_count == 0 && modem->at_count == 0 && modem->event_scheduled == 0);
	EXPECT(modem->get_chars(modem, (char *)received, sizeof(received)) == (int)sizeof(literal));
	EXPECT(memcmp(received, literal, sizeof(literal)) == 0 && commands.count == 0);
	for (size_t index = 0; index < sizeof(input); ++index)
		input[index] = (unsigned char)index;
	EXPECT(modem_data_write(modem, input, sizeof(input)) == 4096);
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == 0);
	memset(input, 0x55, sizeof(input));
	EXPECT(modem->get_chars(modem, (char *)received, 17) == 17);
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == (int)sizeof(literal));
	EXPECT(modem->get_chars(modem, (char *)received + 17, 4096 - 17) == 4096 - 17);
	for (size_t index = 0; index < sizeof(received); ++index)
		EXPECT(received[index] == (unsigned char)index);
	EXPECT(modem->get_chars(modem, (char *)received, sizeof(received)) == (int)sizeof(literal));
	EXPECT(memcmp(received, literal, sizeof(literal)) == 0);
	modem_delete(modem);
	return true;
}

static bool command_preserves_receive(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	EXPECT(modem_data_write(modem, "x", 1) == 1);
	EXPECT(modem_set_dte_command(modem, true) == -1);
	char scratch;
	EXPECT(modem->get_chars(modem, &scratch, 1) == 1);
	unsigned carrier = modem->modem_info;
	void (*process)(struct modem *, void *, void *, int) = modem->process;
	EXPECT(modem_set_dte_command(modem, true) == 0);
	EXPECT(modem->command && modem->put_chars != NULL && modem->get_chars != NULL);
	EXPECT(modem_data_write(modem, "AT\r", 3) == -1);
	EXPECT(modem->put_chars(modem, "incoming\0+++", 12) == 12);
	EXPECT(data.count == 12 && commands.count == 0 && modem->modem_info == carrier);
	EXPECT(modem_set_dte_command(modem, false) == 0);
	EXPECT(!modem->command && modem->process == process && modem->retrain_pending == 0);
	modem_delete(modem);
	return true;
}

static bool raw_overflow_is_reported(void)
{
	struct capture data = { .limit = 0 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	modem_async_start(modem);
	EXPECT(modem_data_write(modem, "x", 1) == 1);
	unsigned char bits[16];
	EXPECT(modem_async_get_bits(modem, 1, bits, 16) == 16);
	EXPECT(modem_async_put_bits(modem, 1, bits, 16) == 16);
	EXPECT(modem->dte_overflow && modem->disconnect_local);
	EXPECT(strcmp(modem->disconnect_reason, "raw DTE receive overflow") == 0);
	modem_delete(modem);
	return true;
}

static bool at_replies_are_separate(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem_frontend_init(modem);
	EXPECT(modem_write(modem, "AT\r", 3) == 3);
	EXPECT(commands.count != 0 && data.count == 0 && modem->xmit.count == 0);
	modem_delete(modem);
	return true;
}

static bool local_lapm_retry_cause(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem->caller = 1;
	modem->state = STATE_EC_ESTAB;
	modem->rx_rate = modem->tx_rate = 9600;
	modem->cfg.ec = 1;
	modem->cfg.comp = 0;
	modem->cfg.ec_tx_win_size = modem->cfg.ec_rx_win_size = 15;
	modem->cfg.ec_tx_info_size = modem->cfg.ec_rx_info_size = 128;
	EXPECT(modem_ec_init(modem) == 0);
	modem_ec_start(modem);
	EXPECT(modem->packer.hdlc.tx_preamble_complete != NULL);
	modem->packer.hdlc.tx_preamble_complete(modem);
	EXPECT(modem->bit_timer_func != NULL);
	modem->ec.lapm.rtx_count = INT_MAX;
	modem->bit_timer_func(modem);
	EXPECT(modem->disconnect_local && modem->disconnect_reason != NULL);
	EXPECT(strcmp(modem->disconnect_reason, "LAPM XID retry budget exhausted") == 0);
	modem_delete(modem);
	return true;
}

static int report_dp_delete(struct dp *dp) { (void)dp; return 0; }
static char report_progress[96];
static const char *report_dp_progress(const struct dp *dp)
{ (void)dp; return report_progress; }

static bool retained_call_report(void)
{
	struct capture data = { .limit = 1 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands), *other = create(&data, &commands);
	EXPECT(modem != NULL && other != NULL);
	modem_frontend_init(modem);
	EXPECT(modem_write(modem, "ATI11\r", 6) == 6);
	EXPECT(strstr((char *)commands.bytes, "Last call: none\r\n") != NULL);
	EXPECT(modem_answer(modem) == 0);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem->sample_timer_func = NULL;
	const struct dp_operations ops = {
		.delete = report_dp_delete, .progress = report_dp_progress,
	};
	struct dp dp = { .id = DP_V32, .status = DPSTAT_CONNECT,
		.modem = modem, .op = &ops };
	modem->dp = &dp;
	modem->tx_rate = 14400;
	modem->rx_rate = 9600;
	modem_update_status(modem, STATUS_EC_LINK);
	EXPECT(modem->call_report.connected && modem->call_report.rates.transmit_baud == 2400);
	/* A successful rate exchange need not invoke the full-retrain callback. */
	struct modem_call_report active;
	modem->tx_rate = 12000;
	EXPECT(modem_get_call_report(modem, &active) && active.rates.transmit_bps == 12000);
	EXPECT(!modem_get_call_report(NULL, &active) && !modem_get_call_report(modem, NULL));
	EXPECT(modem_data_write(modem, "abc", 3) == 3);
	char consumed[3];
	EXPECT(modem->get_chars(modem, consumed, 3) == 3);
	EXPECT(modem->put_chars(modem, "xy", 2) == 1);
	int16_t receive[160] = { 0 }, transmit[160];
	modem_process(modem, receive, transmit, 160);
	modem_dp_reconnected(modem);
	modem->tx_rate = 14400;
	strcpy(report_progress, "online/receiver");
	modem_note_disconnect(modem, false, "peer ended call");
	EXPECT(modem->call_report.rates.transmit_bps == 14400);
	modem->result_code = RESULT_NOCARRIER;
	EXPECT(modem_stop_now(modem) == 0 && modem_stop_now(modem) == 0);
	strcpy(report_progress, "deleted");
	const struct modem_call_report saved = modem->call_report;
	EXPECT(saved.available && !saved.active && saved.connected && !saved.outgoing);
	EXPECT(saved.tx_dte_bytes == 3 && saved.rx_dte_bytes == 1 && saved.media_samples == 160);
	EXPECT(saved.retrains == 1 && strcmp(saved.step, "online/receiver") == 0);
	EXPECT(!saved.disconnect_local && !other->call_report.available);
	commands.count = 0;
	memset(commands.bytes, 0, sizeof(commands.bytes));
	EXPECT(modem_write(modem, "ATI11\r", 6) == 6);
	EXPECT(strstr((char *)commands.bytes, "Rates: TX=14400 bit/s 2400.00 baud RX=9600 bit/s 2400.00 baud\r\n"));
	EXPECT(strstr((char *)commands.bytes, "Media time: 20 ms\r\n"));
	EXPECT(strstr((char *)commands.bytes, "Disconnect: remote reason=peer ended call\r\n"));
	EXPECT(strstr((char *)commands.bytes, "End: NO CARRIER phase=online step=online/receiver\r\n"));
	EXPECT(modem_write(modem, "ATZ\r", 4) == 4);
	EXPECT(memcmp(&saved, &modem->call_report, sizeof(saved)) == 0);
	EXPECT(modem_answer(modem) == 0 && modem->call_report.active && !modem->call_report.connected);
	EXPECT(!modem->call_report.step[0] && !modem->call_report.reason[0] && !modem->call_report.tx_dte_bytes);
	modem_update_status(modem, STATUS_DP_ERROR);
	EXPECT(modem_stop_now(modem) == 0);
	EXPECT(!modem->call_report.connected && modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.phase, "training") == 0);
	EXPECT(strcmp(modem->call_report.reason, "datapump failed") == 0);
	modem_delete(other);
	modem_delete(modem);
	return true;
}

static bool unanswered_call_report(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL && modem_report_incoming_call(modem) == 0);
	EXPECT(modem->call_report.active && !modem->started);
	EXPECT(modem_report_remote_call_end(modem) == 0);
	EXPECT(!modem->call_report.active && !modem->call_report.connected);
	EXPECT(modem->call_report.result == RESULT_NOCARRIER && !modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.reason, "remote SIP call ended") == 0);
	EXPECT(modem_report_incoming_call(modem) == 0);
	modem_hangup(modem);
	EXPECT(!modem->call_report.active && modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.reason, "local hangup") == 0);
	EXPECT(modem_report_incoming_call(modem) == 0);
	modem_frontend_init(modem);
	EXPECT(modem_write(modem, "ATZ\r", 4) == 4);
	EXPECT(!modem->call_report.active && modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.reason, "AT reset") == 0);
	EXPECT(modem_report_incoming_call(modem) == 0);
	EXPECT(modem_write(modem, "ATH0\r", 5) == 5);
	EXPECT(!modem->call_report.active && modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.reason, "local hangup") == 0);
	modem_delete(modem);
	return true;
}

int main(void)
{
	return literal_and_partial() && command_preserves_receive() &&
		raw_overflow_is_reported() && at_replies_are_separate() && local_lapm_retry_cause() &&
		retained_call_report() && unanswered_call_report() ? 0 : 1;
}
