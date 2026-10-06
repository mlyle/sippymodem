
/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    modem.c  --  modem core module.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#include "modem_frontend.h"
#include "modem.h"
#include "modem_state.h"

#include <ctype.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "modem_debug.h"

#define MODEM_DBG(fmt, args...) dprintf("%s: " fmt, m->name, ##args)
#define ESCAPE_CHAR(m) ((m)->sregs[SREG_ESCAPE_CHAR])
#define ESCAPE_TIMEOUT(m) (MODEM_HZ / 2)
#define CR_CHAR(m) ((m)->sregs[SREG_CR_CHAR])
#define LF_CHAR(m) ((m)->sregs[SREG_LF_CHAR])
#define BS_CHAR(m) ((m)->sregs[SREG_BS_CHAR])
#define IS_ECHO(m) ((m)->sregs[SREG_ECHO])
#define IS_QUIET(m) ((m)->sregs[SREG_QUIET])
#define IS_VERBOSE(m) ((m)->sregs[SREG_VERBOSE])
#define IS_STATE_CONNECTING(state) ((state) > STATE_MODEM_IDLE && (state) < STATE_MODEM_ONLINE)

extern int process_at_command(struct modem *modem, char *command);

/* V.250 Table 3: information trailers always use S3/S4. Quiet mode
 * suppresses result codes, not information responses. */
void modem_frontend_info_header(struct modem *m)
{
	if (IS_VERBOSE(m))
		modem_send_to_tty(m, CRLF_CHARS(m), 2);
}

static void info_line(struct modem *m, const char *text, int length)
{
	modem_send_to_tty(m, text, length);
	modem_send_to_tty(m, CRLF_CHARS(m), 2);
}

void modem_frontend_info(struct modem *m, const char *text, int length)
{
	modem_frontend_info_header(m);
	info_line(m, text, length);
}

/* A report owns one header; each line supplies only its trailer. */
static void report_line(struct modem *m, const char *format, ...)
{
	char text[384];
	va_list arguments;
	va_start(arguments, format);
	int length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (length > 0 && (size_t)length < sizeof(text))
		info_line(m, text, length);
}

void modem_frontend_call_report(struct modem *m)
{
	struct modem_call_report snapshot;
	modem_frontend_info_header(m);
	if (!modem_get_call_report(m, &snapshot)) {
		report_line(m, "Last call: none");
		return;
	}
	const struct modem_call_report *report = &snapshot;
	report_line(m, "Last call: %s %s connected=%s",
		report->outgoing ? "outgoing" : "incoming",
		report->active ? "active" : "completed",
		report->connected ? "yes" : "no");
	if (report->caller_number[0])
		report_line(m, "CallerID: %s", report->caller_number);
	report_line(m, "Modulation: %s", report->modulation[0] ? report->modulation : "unknown");
	report_line(m, "Rates: TX=%u bit/s %.2f baud RX=%u bit/s %.2f baud",
		report->rates.transmit_bps, report->rates.transmit_baud,
		report->rates.receive_bps, report->rates.receive_baud);
	report_line(m, "Error control: %s Compression: %s",
		report->connected ? (report->error_control ? "LAPM" : "NONE") : "unknown",
		report->connected ? (report->compression == MODEM_EC_COMPRESSION_V44 ? "V44" :
			report->compression == MODEM_EC_COMPRESSION_V42BIS ? "V42bis" : "NONE") : "unknown");
	report_line(m, "Media time: %" PRIu64 " ms", report->sample_rate ?
		report->media_samples * 1000 / report->sample_rate : 0);
	report_line(m, "DTE bytes: TX-consumed=%" PRIu64 " RX-delivered=%" PRIu64,
		report->tx_dte_bytes, report->rx_dte_bytes);
	report_line(m, "Completed physical retrains: %u", report->retrains);
	const char *result = "HANGUP";
	switch (report->result) {
	case RESULT_NOCARRIER: result = "NO CARRIER"; break;
	case RESULT_BUSY: result = "BUSY"; break;
	case RESULT_NOANSWER: result = "NO ANSWER"; break;
	case RESULT_NODIALTONE: result = "NO DIALTONE"; break;
	case RESULT_ERROR: result = "ERROR"; break;
	default: break;
	}
	report_line(m, "End: %s phase=%s step=%s", report->active ? "ACTIVE" : result,
		report->phase[0] ? report->phase : modem_call_phase(m),
		report->step[0] ? report->step : modem_protocol_step(m));
	report_line(m, "Disconnect: %s reason=%s", report->reason[0] ?
		(report->disconnect_local ? "local" : "remote") : "pending",
		report->reason[0] ? report->reason : "none");
}

static void report_result(struct modem *m, unsigned int code)
{
	static const struct modem_response {
		const char *code;
		const char *text;
	} modem_responses[] = {
		{"0", "OK"},
		{"1", "CONNECT"},
		{"2", "RING"},
		{"3", "NO CARRIER"},
		{"4", "ERROR"},
		{"" , ""},
		{"6", "NO DIALTONE"},
		{"7", "BUSY"},
		{"8", "NO ANSWER"},
	};
	static const char none_str[] = "NONE";
        const char *msg;
	u8 msg_mask;

	if (code >= sizeof(modem_responses) / sizeof(modem_responses[0]))
		return;
        MODEM_DBG("modem report result: %d (%s)\n",code,modem_responses[code].text);
        if (IS_QUIET(m))
                return;
        if (IS_VERBOSE(m)) {
		modem_frontend_info_header(m);
		if(code==RESULT_CONNECT) {
			msg_mask = modem_get_sreg(m,SREG_CONNNECT_MSG_FORMAT);
			if(msg_mask&1) {
				msg = modem_active_modulation_name(m);
				report_line(m, "Modulation: %s", msg ? msg : none_str);
				msg = m->dsp_info.startup == MODEM_STARTUP_QC ? "QC" :
					m->dsp_info.startup == MODEM_STARTUP_V8 ? "V8" :
					m->dsp_info.startup == MODEM_STARTUP_V8_FALLBACK ? "V8-FALLBACK" : NULL;
				if (msg != NULL)
					report_line(m, "Startup: %s", msg);
				if (m->dp != NULL &&
				    (m->dp->id == DP_V90 || m->dp->id == DP_V92)) {
					msg = NULL;
					if (m->dsp_info.upstream_mode == MODEM_UPSTREAM_V34)
						msg = "V34";
					else if (m->dsp_info.upstream_mode == MODEM_UPSTREAM_PCM)
						msg = "PCM";
					if (msg != NULL)
						report_line(m, "Upstream: %s", msg);
				}
			}
			if (msg_mask & 2)
				report_line(m, "Protocol: %s", m->cfg.ec ? "LAPM" : none_str);
			if((msg_mask&3) == 3) {
				switch (modem_ec_active_compression(m)) {
				case MODEM_EC_COMPRESSION_V44:
					msg = "V44";
					break;
				case MODEM_EC_COMPRESSION_V42BIS:
					msg = "V42bis";
					break;
				default:
					msg = none_str;
					break;
				}
				report_line(m, "Compression: %s", msg);
			}
			if ((msg_mask & 4) && m->tx_rate)
				report_line(m, "TxRate: %u", m->tx_rate);
		}
		if ((code == RESULT_RING || code == RESULT_CONNECT) && m->caller_number[0])
			report_line(m, "CallerID: %s", m->caller_number);
                msg = modem_responses[code].text;
		modem_send_to_tty(m,msg,strlen(msg));
		if (code==RESULT_CONNECT && m->mode == MODEM_MODE_DATA
		    && m->rx_rate && modem_get_sreg(m,SREG_X_CODE) != 0) {
			char rate_str[16];
			snprintf(rate_str, sizeof(rate_str), " %u", m->rx_rate);
			modem_send_to_tty(m,rate_str,strlen(rate_str));
		}
        }
        else {
                msg = modem_responses[code].code;
		modem_send_to_tty(m,msg,strlen(msg));
        }
        modem_send_to_tty(m, CRLF_CHARS(m), IS_VERBOSE(m) ? 2 : 1);
}

static void process_input(void *data)
{
	struct modem *m = (struct modem *)data;
	int echo = IS_ECHO(m);
	char lf = LF_CHAR(m), cr = CR_CHAR(m), bs = BS_CHAR(m);
        int ret;
	char ch;
	while(1) {
		if(modem_recv_from_tty(m,&ch,1) <= 0)
			break;
		if (ch == '/' && m->at_count == 1 &&
		    toupper(m->at_line[0]) == 'A') {
			m->at_count = strlen(m->at_line);
			if(echo)
				modem_send_to_tty(m,m->at_line+1,m->at_count-1);
			ch = cr;
		}
		if (echo)
			modem_send_to_tty(m,&ch,1);
		if (ch == cr || ch == lf) {
			int i;
			char *p = m->at_cmd;
			m->at_line[m->at_count] = '\0';
			for(i = 0 ; i < m->at_count ; i++) {
				if( m->at_line[i] == ' ' ||
				    m->at_line[i] == '\t' )
					continue;
				*p++ = m->at_line[i];
			}
			*p = '\0';
			if (strlen(m->at_cmd) < 2 ||
			    toupper(m->at_cmd[0]) != 'A' ||
			    toupper(m->at_cmd[1]) != 'T' ) {
				memset(m->at_line,' ',m->at_count);
				if(echo) {
					modem_send_to_tty(m,&cr,1);
					modem_send_to_tty(m,m->at_line,m->at_count);
					modem_send_to_tty(m,&cr,1);
				}
				m->at_count = 0;
				continue;
			}
			MODEM_DBG("run cmd: %s\n",m->at_cmd);
			ret = process_at_command(m, m->at_cmd);
			if (ret < 0)
				report_result(m,RESULT_ERROR);
			else if (ret==0)
				report_result(m,RESULT_OK);
			m->at_line[m->at_count] = '\0';
			m->at_count = 0;
			echo = IS_ECHO(m);
			cr = CR_CHAR(m), lf = LF_CHAR(m), bs = BS_CHAR(m);
		}
		else if (ch == bs && m->at_count) {
			m->at_count--;
			if(echo) {
				ch = ' ';
				modem_send_to_tty(m,&ch,1);
				modem_send_to_tty(m,&bs,1);
			}
		}
		else if (m->at_count == sizeof(m->at_line) - 1) {
			m->at_count   = 0;
			m->at_line[1] = '\0';
			report_result(m,RESULT_ERROR);
		}
		else {
			m->at_line[m->at_count++] = ch;
		}
	}
}

static int modem_check_escape(struct modem *m, const char *buf, int count)
{
	unsigned long now = get_time();
        int i;
        if(count + m->escape_count > 3 )
		goto noescape_out;
        for( i = 0 ; i < count ; i++) {
                if(buf[i] != ESCAPE_CHAR(m))
			goto noescape_out;
        }

        if(m->escape_count == 0) {
                if(time_before(now, m->last_esc_check + ESCAPE_TIMEOUT(m)))
                        goto noescape_out;
	}
	else if (time_after(now, m->last_esc_check + ESCAPE_TIMEOUT(m)))
		goto noescape_out;

	m->escape_count += count;
	/* One or two held characters also need an expiry: they become line
	 * data when no third escape character arrives. */
	modem_schedule_escape(m);

	m->last_esc_check = now;
	return m->escape_count;

 noescape_out:
	m->escape_count = 0;
        m->last_esc_check = now;
        return 0;
}

int modem_write(struct modem *m, const char *buffer, int count)
{
	if (m == NULL || count < 0 || (count != 0 && buffer == NULL))
		return -1;
	if (m->frontend_report == NULL)
		modem_frontend_init(m);
	const char *buf;
	const char *written = buffer;
	int cnt, ret = 0;
	if(IS_STATE_CONNECTING(m->state)) {
		MODEM_DBG("modem_tty_write: hangup...\n");
		modem_hangup(m);
		return count;
	}

	while(count) {
		cnt = count;
		if(cnt > m->xmit.size - m->xmit.count)
			cnt = m->xmit.size - m->xmit.count;
		if(cnt > m->xmit.size - m->xmit.head)
			cnt = m->xmit.size - m->xmit.head;
		if(cnt <= 0) {
			MODEM_DBG("modem_write: overflow!\n");
			break;
		}
		buf = buffer;
                memcpy(m->xmit.buf + m->xmit.head, buf, cnt);
                m->xmit.count += cnt;
                m->xmit.head = (m->xmit.head + cnt)%m->xmit.size;
		ret += cnt;
		buffer += cnt;
		count -= cnt;
	}
	if (m->state == STATE_MODEM_ONLINE && ret > 0)
		modem_check_escape(m, written, ret);
	if(m->command)
		process_input(m);
	return ret;
}

static int write_pty(void *context, const char *bytes, int count)
{
	struct modem *modem = context;
	int written = write(modem->pty, bytes, (size_t)count);
	return written < 0 ? 0 : written;
}

static void escape_event(struct modem *m)
{
	if (m->state == STATE_MODEM_ONLINE && m->escape_count == 3) {
		if (!m->cfg.ec)
			modem_async_discard_transmit(m);
		/* The confirmed escape is compatibility syntax, not payload. */
		m->xmit.head = m->xmit.tail = m->xmit.count = 0;
		(void)modem_set_dte_command(m, true);
		report_result(m, RESULT_OK);
	}
	m->escape_count = 0;
}

void modem_frontend_init(struct modem *modem)
{
	if (modem == NULL)
		return;
	if (modem->frontend_report != NULL)
		return;
	modem_print_version();
	modem->frontend_report = report_result;
	modem->frontend_escape = escape_event;
	if (modem->control_output == NULL) {
		modem->control_output = write_pty;
		modem->control_output_context = modem;
	}
	if (modem->data_output == NULL) {
		modem->data_output = write_pty;
		modem->data_output_context = modem;
	}
}

void modem_update_termios(struct modem *m, const struct termios *tios)
{
	if (cfgetispeed(tios) == B0 || cfgetospeed(tios) == B0) {
		if (m->state != STATE_MODEM_IDLE)
			modem_hangup(m);
	} else {
		m->modem_info |= TIOCM_DTR;
	}
	m->termios = *tios;
}

void modem_print_version(void)
{
	extern const char modem_name[], modem_version[], modem_date[];
	printf("%s: version %s %s\n", modem_name, modem_version, modem_date);
}
