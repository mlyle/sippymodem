// SPDX-License-Identifier: BSD-3-Clause
#include <modem.h>
#include <modem_state.h>
#include <modem_param.h>
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

static bool at_response_formats(void)
{
	/* Literal V.250 Table 3 bytes, including custom S3/S4 and changes made
	 * within the command line. Q1 suppresses only the final result. */
	static const struct {
		unsigned echo, verbose, quiet, cr, lf;
		const char *input, *expected;
	} cases[] = {
		{0,1,0,13,10, "AT\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMI=?\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMI\r", "\r\nSmart Link Ltd.\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMM\r", "\r\nSmartLink Soft Modem\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMR\r", "\r\n2.9.11\r\n\r\nOK\r\n"},
		{0,0,0,13,10, "AT+GCAP\r", "+GCAP: +MS, +DS, +FCLASS\r\n0\r"},
		{0,1,1,13,10, "AT+GCAP\r", "\r\n+GCAP: +MS, +DS, +FCLASS\r\n"},
		{0,1,0,13,10, "AT+GMI=?1\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMI=?E0\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GME=?\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMM=?\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMR=?\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GCAP=?\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GCAP\r", "\r\n+GCAP: +MS, +DS, +FCLASS\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMI?\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMM=1\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMR=\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMIX\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GCAP?\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "AT+GMI=?;+GMM=?;+GMR=?;+GCAP=?\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GMI=?+GMM=?+GMR=?\r", "\r\nOK\r\n"},
		{0,0,0,13,10, "AT+GMI=?\r", "0\r"},
		{0,1,1,13,10, "AT+GMI=?\r", ""},
		{0,1,0,13,10, "at+gmi=?;e0\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+V91=?;+GCI?\r", "\r\n(0,1)\r\n\r\n+GCI:b5\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GCI?;+GMI=?\r", "\r\n+GCI:b5\r\n\r\nOK\r\n"},
		{1,1,0,13,10, "AT\r", "AT\r\r\nOK\r\n"},
		{0,0,0,13,10, "AT\r", "0\r"},
		{1,0,0,13,10, "AT\r", "AT\r0\r"},
		{0,1,1,13,10, "AT\r", ""},
		{1,1,1,13,10, "AT\r", "AT\r"},
		{0,1,0,13,10, "ATS0?\r", "\r\n000\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS0=1S0?\r", "\r\n001\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS0=255S0?\r", "\r\n255\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS3?\r", "\r\n013\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS2=043S2?\r", "\r\n043\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS009=008S9?\r", "\r\n008\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "ATS0=+FCLASS?\r", "\r\n0\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT&F0X1\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "AT+V34B=0X1\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "ATI0011\r", "\r\nLast call: none\r\n\r\nOK\r\n"},
		{0,0,0,13,10, "ATI4294967296\r", "4\r"},
		{0,1,0,13,10, "ATE001\r", "\r\nOK\r\n"},
		{0,1,0,13,10, "ATE002\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATZ4\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATE0+MA\r", "\r\nERROR\r\n"},
		{0,0,0,13,10, "ATE0+M\r", "4\r"},
		{0,1,0,13,10, "ATS0=256\r", "\r\nERROR\r\n"},
		{0,0,0,13,10, "ATS0=4294967296\r", "4\r"},
		{0,1,0,13,10, "ATS4294967296?\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATS3=128\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATS4=128\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATS5=128\r", "\r\nERROR\r\n"},
		{0,1,0,13,10, "ATS3=59\rAT;", ";\nOK;\n;\nOK;\n"},
		{0,1,0,13,10, "ATS4=58\r", "\r:OK\r:"},
		{0,1,0,13,10, "ATS5=127\rATQ9\1770\r", "\r\nOK\r\n\r\nOK\r\n"},
		{0,0,0,13,10, "ATS0?\r", "000\r\n0\r"},
		{0,1,1,13,10, "ATS0?\r", "\r\n000\r\n"},
		{0,0,1,13,10, "ATS0?\r", "000\r\n"},
		{0,1,0,59,58, "AT;", ";:OK;:"},
		{0,0,0,59,58, "AT;", "0;"},
		{0,1,0,59,58, "ATS0?;", ";:000;:;:OK;:"},
		{0,0,0,59,58, "ATS0?;", "000;:0;"},
		{0,1,0,13,10, "ATV0S0?\r", "000\r\n0\r"},
		{0,0,0,13,10, "ATV1S0?\r", "\r\n000\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+V91?+V91=?\r", "\r\n1\r\n\r\n(0,1)\r\n\r\nOK\r\n"},
		{0,0,0,13,10, "AT+V91?+V91=?\r", "1\r\n(0,1)\r\n0\r"},
		{0,1,0,13,10, "AT+V91=2\r", "\r\nERROR\r\n"},
		{0,0,0,13,10, "AT+V91=2\r", "4\r"},
		{0,1,0,13,10, "AT+V34B=?\r", "\r\n(0,2400,2743,2800,3000,3200,3429)\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+FCLASS?+DS0?\r", "\r\n0\r\n\r\n0\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+FCLASS=?\r", "\r\n0\r\n\r\nOK\r\n"},
		{0,1,0,13,10, "AT+GCI=00+GCI?\r", "\r\n+GCI:0\r\n\r\nOK\r\n"},
		{0,0,0,13,10, "AT+GCI=00+GCI?\r", "+GCI:0\r\n0\r"},
		{0,1,0,13,10, "AT+MS=21,0,300,300+MS?\r", "\r\n21,0,300,300\r\n\r\nOK\r\n"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
		struct modem *modem = create(&data, &commands);
		EXPECT(modem != NULL);
		modem_frontend_init(modem);
		modem_set_sreg(modem, SREG_ECHO, cases[i].echo);
		modem_set_sreg(modem, SREG_VERBOSE, cases[i].verbose);
		modem_set_sreg(modem, SREG_QUIET, cases[i].quiet);
		modem_set_sreg(modem, SREG_CR_CHAR, cases[i].cr);
		modem_set_sreg(modem, SREG_LF_CHAR, cases[i].lf);
		size_t count = strlen(cases[i].input), expected = strlen(cases[i].expected);
		EXPECT(modem_write(modem, cases[i].input, (int)count) == (int)count);
		CHECK(commands.count == expected && !memcmp(commands.bytes, cases[i].expected, expected),
		      "AT response format case %zu", i);
		EXPECT(data.count == 0 && modem->xmit.count == 0);
		modem_delete(modem);
	}
	const char *streamed[] = { "AT+MS=?\r", "AT+GCI=?\r" };
	for (size_t i = 0; i < ARRAY_LENGTH(streamed); ++i) {
		struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
		struct modem *modem = create(&data, &commands);
		EXPECT(modem != NULL);
		modem_frontend_init(modem);
		modem_set_sreg(modem, SREG_ECHO, 0);
		size_t count = strlen(streamed[i]);
		EXPECT(modem_write(modem, streamed[i], (int)count) == (int)count);
		EXPECT(commands.count > 10 && !memcmp(commands.bytes, "\r\n", 2));
		char *body = (char *)commands.bytes + 2;
		char *trailer = body + strcspn(body, "\r\n");
		/* A streamed query has one uninterrupted body and one final result. */
		EXPECT(trailer > body && strcmp(trailer, "\r\n\r\nOK\r\n") == 0);
		EXPECT(strstr(body, ",)") == NULL);
		EXPECT(data.count == 0);
		modem_delete(modem);
	}
	/* Ring headers are frontend syntax; a library line emits no AT bytes. */
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL && modem_report_incoming_call(modem) == 0);
	EXPECT(data.count == 0 && commands.count == 0);
	modem_frontend_init(modem);
	EXPECT(modem_report_incoming_call(modem) == 0);
	EXPECT(commands.count == 8 && !memcmp(commands.bytes, "\r\nRING\r\n", 8));
	modem_delete(modem);
	return true;
}

static bool connection_report_layout(void)
{
	static const struct {
		unsigned verbose, quiet, mask, cr, lf;
		const char *expected;
	} cases[] = {
		{1,0,7,13,10, "\r\nModulation: V34\r\nStartup: V8\r\nProtocol: LAPM\r\nCompression: V42bis\r\nTxRate: 31200\r\nCONNECT 31200\r\n"},
		{1,0,7,59,58, ";:Modulation: V34;:Startup: V8;:Protocol: LAPM;:Compression: V42bis;:TxRate: 31200;:CONNECT 31200;:"},
		{1,0,0,13,10, "\r\nCONNECT 31200\r\n"},
		{1,0,1,13,10, "\r\nModulation: V34\r\nStartup: V8\r\nCONNECT 31200\r\n"},
		{1,0,4,13,10, "\r\nTxRate: 31200\r\nCONNECT 31200\r\n"},
		{0,0,7,13,10, "1\r"},
		{1,1,7,13,10, ""},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
		struct modem *modem = create(&data, &commands);
		struct dp dp = { .id = DP_V34, .modem = modem };
		EXPECT(modem != NULL);
		modem_frontend_init(modem);
		modem->dp = &dp;
		modem->tx_rate = modem->rx_rate = 31200;
		modem->cfg.ec = modem->cfg.comp = 1;
		modem->cfg.comp_types = MODEM_COMP_TYPE_V42BIS;
		modem->dsp_info.startup = MODEM_STARTUP_V8;
		modem->sregs[SREG_VERBOSE] = cases[i].verbose;
		modem->sregs[SREG_QUIET] = cases[i].quiet;
		modem->sregs[SREG_CONNNECT_MSG_FORMAT] = cases[i].mask;
		modem->sregs[SREG_CR_CHAR] = cases[i].cr;
		modem->sregs[SREG_LF_CHAR] = cases[i].lf;
		modem->frontend_report(modem, RESULT_CONNECT);
		CHECK(commands.count == strlen(cases[i].expected) &&
		      memcmp(commands.bytes, cases[i].expected, commands.count) == 0 &&
		      data.count == 0, "CONNECT report layout case %zu", i);
		modem->dp = NULL;
		modem_delete(modem);
	}
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
	EXPECT(strstr((char *)commands.bytes, "Modulation: V32bis\r\nRates: TX=14400 bit/s 2400.00 baud RX=9600 bit/s 2400.00 baud\r\n"));
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

static bool caller_identity_results(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands), *other = create(&data, &commands);
	EXPECT(modem != NULL && other != NULL);
	modem_frontend_init(modem);
	modem_set_caller_number(modem, "+1(202)555-0123");
	modem_set_caller_number(other, "1.202.555.0124");
	EXPECT(modem_report_incoming_call(modem) == 0);
	EXPECT(strstr((char *)commands.bytes, "CallerID: +12025550123\r\nRING\r\n"));
	EXPECT(strcmp(other->caller_number, "12025550124") == 0);
	EXPECT(modem_answer(modem) == 0 && strcmp(modem->caller_number, "+12025550123") == 0);
	commands.count = 0;
	memset(commands.bytes, 0, sizeof(commands.bytes));
	modem->cfg.ec = modem->cfg.comp = 0;
	modem->rx_rate = modem->tx_rate = 9600;
	modem_update_status(modem, STATUS_EC_LINK);
	EXPECT(strstr((char *)commands.bytes, "CallerID: +12025550123\r\nCONNECT 9600\r\n"));
	modem_note_disconnect(modem, false, "peer ended call");
	EXPECT(modem_stop_now(modem) == 0 && !modem->caller_number[0]);
	EXPECT(strcmp(modem->call_report.caller_number, "+12025550123") == 0);
	commands.count = 0;
	memset(commands.bytes, 0, sizeof(commands.bytes));
	EXPECT(modem_write(modem, "ATI11\r", 6) == 6);
	EXPECT(strstr((char *)commands.bytes, "CallerID: +12025550123\r\n"));
	char oversized[129];
	memset(oversized, '1', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	char maximum[128];
	memset(maximum, '1', sizeof(maximum) - 1);
	maximum[sizeof(maximum) - 1] = '\0';
	modem_set_caller_number(modem, maximum);
	EXPECT(strlen(modem->caller_number) == sizeof(maximum) - 1);
	const char *invalid[] = { NULL, "", "+", "().-", "1+2", "anonymous", "123\r\nRING", "123\x1b[2J", oversized };
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
		modem_set_caller_number(modem, "1234");
		modem_set_caller_number(modem, invalid[index]);
		EXPECT(!modem->caller_number[0]);
	}
	EXPECT(modem_report_incoming_call(modem) == 0 && !modem->call_report.caller_number[0]);
	modem_finish_call_report(modem, true, "native unanswered cancellation", RESULT_NOCARRIER);
	EXPECT(!modem->call_report.active && modem->call_report.disconnect_local);
	EXPECT(strcmp(modem->call_report.reason, "native unanswered cancellation") == 0);
	modem_set_caller_number(modem, "1234");
	EXPECT(modem_report_incoming_call(modem) == 0);
	modem->sregs[SREG_VERBOSE] = 0;
	commands.count = 0;
	memset(commands.bytes, 0, sizeof(commands.bytes));
	EXPECT(modem_report_incoming_call(modem) == 0);
	EXPECT(strcmp((char *)commands.bytes, "2\r") == 0);
	modem_hangup(modem);
	modem_delete(other);
	modem_delete(modem);
	return true;
}

int main(void)
{
	return literal_and_partial() && command_preserves_receive() &&
		raw_overflow_is_reported() && at_replies_are_separate() && at_response_formats() && connection_report_layout() && local_lapm_retry_cause() &&
		retained_call_report() && unanswered_call_report() && caller_identity_results() ? 0 : 1;
}
