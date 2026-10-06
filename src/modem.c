
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

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/ioctl.h>

#include <modem.h>
#include "modem_state.h"
#include <modem_param.h>
#include <modem_debug.h>
#include <v8_datapump.h>
#include <datapump_link.h>
#include "call_number.h"


#define XMIT_SIZE 4096

#define MODEM_AUTHOR "Smart Link Ltd."
#define MODEM_NAME "SmartLink Soft Modem"
#define MODEM_VERSION "2.9.11"
#define MODEM_DATE __DATE__" "__TIME__

/* event mask */
#define MDMEVENT_RING_CHECK    0x01
#define MDMEVENT_ESCAPE        0x02

/* debug prints */
#define MODEM_DBG(fmt,arg...) dprintf("%s: " fmt , m->name , ##arg)
#define MODEM_ERR(fmt,arg...) eprintf("%s: " fmt , m->name , ##arg)

/* external symbols */
#ifdef MODEM_CONFIG_RING_DETECTOR
extern void *RD_create(struct modem *m, unsigned rate);
extern void RD_delete(void *obj);
extern int  RD_process(void *obj, void *in, int count);
extern void RD_ring_details(void *obj, long *freq, long *duration);
#endif
#ifdef MODEM_CONFIG_CID
extern void *CID_create(struct modem *m, unsigned rate, unsigned cid_val);
extern void CID_delete(void *cid);
extern int  CID_process(void *cid, void *in, int count);
#endif
#ifdef MODEM_CONFIG_FAX
extern void *FAX_create(struct modem *m, unsigned caller, unsigned srate);
extern int FAX_process(void *obj, void *in, void *out, int count);
extern void FAX_delete(void *obj);
#endif

/* local prototypes */
int modem_answer(struct modem *m);
static void do_modem_change_dp(struct modem *);
static int modem_start(struct modem *);
static int modem_stop (struct modem *);
static int modem_get_chars(struct modem *m, char *buf, int n);
static int modem_put_chars(struct modem *m, const char *buf, int n);
static int modem_put_data(struct modem *m, const char *buf, int n);
static int modem_comp_get_chars(struct modem *m, char *buf, int n);
static int modem_comp_put_chars(struct modem *m, const char *buf, int n);
#ifdef MODEM_CONFIG_CID
static int  modem_cid_start(struct modem *, unsigned timeout);
#endif
#ifdef MODEM_CONFIG_FAX
static int modem_fax_start(struct modem *m);
#endif

/* global config data */


/* data definitions */
const char modem_author[] = MODEM_AUTHOR;
const char modem_name[]   = MODEM_NAME;
const char modem_version[]= MODEM_VERSION;
const char modem_date[]   = MODEM_DATE;


/*
 *    misc macros
 */

/* ring parameter */
#define RING_ON_MIN(m)   MODEM_HZ*3/20 /* 0.15 sec */
#define RING_ON_MAX(m)   MODEM_HZ*5/2  /* 2.5 sec */
#define RING_OFF_MIN(m)  MODEM_HZ*3/2  /* 1.5 sec */
#define RING_OFF_MAX(m)  MODEM_HZ*13/2 /* 6.5 sec */
#define RING_COUNT_MIN(m) 10

#define TOTAL_RINGS_COUNT(m)  ((m)->sregs[SREG_RING_COUNTER])
#define ANSWER_AFTER_RINGS(m) ((m)->sregs[SREG_RINGS_TO_AUTO_ANSWER])

#define ESCAPE_CHAR(m)        ((m)->sregs[SREG_ESCAPE_CHAR])
#define CR_CHAR(m)            ((m)->sregs[SREG_CR_CHAR])
#define LF_CHAR(m)            ((m)->sregs[SREG_LF_CHAR])
#define BS_CHAR(m)            ((m)->sregs[SREG_BS_CHAR])

#define IS_ECHO(m)            ((m)->sregs[SREG_ECHO])
#define IS_QUIET(m)           ((m)->sregs[SREG_QUIET])
#define IS_VERBOSE(m)         ((m)->sregs[SREG_VERBOSE])
#define IS_AUTOMODE(m)        MODEM_AUTOMODE(m)
#define MODEM_EC_ENABLE(m)    ((m)->sregs[SREG_EC])
#define MODEM_EC_DETECTOR(m)  ((m)->sregs[SREG_EC])
#define MODEM_COMP_ENABLE(m)  ((m)->sregs[SREG_COMP]&&MODEM_EC_ENABLE(m))
#define MODEM_COMP_TYPES(m) \
	((m)->sregs[SREG_COMP_TYPES] & MODEM_COMP_TYPE_ALL)
#define ESCAPE_TIMEOUT(m)     (MODEM_HZ/2)
#define ANSWER_DELAY(m)       ((m)->sregs[SREG_ANS_DELAY])
#define SPEAKER_CONTROL(m)    ((m)->sregs[SREG_SPEAKER_CONTROL])
#define SPEAKER_VOLUME(m)     ((m)->sregs[SREG_SPEAKER_VOLUME])

static int modem_skip_ec_detection(const struct modem *m)
{
	/* V.92 9.3.1 requires both roles to bypass ODP/ADP after a joint
	 * V.8 LAPM selection, including conventional-upstream operation.
	 * Keep detection for other modes: V.8 permits legacy peers that still
	 * require it despite advertising LAPM in their menu. */
	if (m->dp != NULL && m->dp->id == DP_DS0)
		return 1;
	return m->dp != NULL && m->dp->id == DP_V92 &&
	       m->dsp_info.v8_lapm;
}


/*
 *    dp operations drivers
 *
 */

static int rate_uses_v32bis(unsigned int rate)
{
	return rate == 7200 || rate == 12000 || rate == 14400;
}

const char *modem_active_modulation_name(const struct modem *m)
{
	if (m == NULL || m->dp == NULL)
		return NULL;
	/* V.32 and V.32bis share one family datapump. The negotiated rate
	 * distinguishes the extension-only operating modes. */
	if (m->dp->id == DP_V32 &&
	    (rate_uses_v32bis(m->rx_rate) || rate_uses_v32bis(m->tx_rate)))
		return "V32bis";
	return modem_dp_name(m->dp->id);
}




/*
 *    state and status handling
 *
 */

static void modem_report_result(struct modem *m, unsigned int code)
{
	if (m->frontend_report != NULL)
		m->frontend_report(m, code);
}

void modem_set_caller_number(struct modem *m, const char *number)
{
	if (m == NULL)
		return;
	char normalized[sizeof(m->caller_number)];
	call_number_copy(normalized, sizeof(normalized), number,
		number != NULL ? strnlen(number, sizeof(normalized)) : 0);
	memcpy(m->caller_number, normalized, sizeof(normalized));
}

static void call_report_begin(struct modem *m, bool outgoing)
{
	m->disconnect_reason = NULL;
	m->disconnect_step = NULL;
	m->disconnect_phase = NULL;
	m->disconnect_local = false;
	m->call_report = (struct modem_call_report){
		.available = true, .active = true, .outgoing = outgoing,
		.sample_rate = m->srate,
	};
	if (outgoing)
		memset(m->caller_number, 0, sizeof(m->caller_number));
	else
		memcpy(m->call_report.caller_number, m->caller_number, sizeof(m->caller_number));
}

static void call_report_refresh(struct modem *m)
{
	struct modem_call_report *report = &m->call_report;
	if (!report->active || !report->connected)
		return;
	const char *name = modem_active_modulation_name(m);
	struct dp_link_rates rates = datapump_get_link_rates(m->dp);
	/* Bit rates are published at link readiness even when a datapump has no
	 * symbol-rate accessor. Preserve the last known values during retrain. */
	if (rates.transmit_bps || m->tx_rate)
		report->rates.transmit_bps = rates.transmit_bps ? rates.transmit_bps : m->tx_rate;
	if (rates.receive_bps || m->rx_rate)
		report->rates.receive_bps = rates.receive_bps ? rates.receive_bps : m->rx_rate;
	if (rates.transmit_baud > 0)
		report->rates.transmit_baud = rates.transmit_baud;
	if (rates.receive_baud > 0)
		report->rates.receive_baud = rates.receive_baud;
	if (name != NULL)
		snprintf(report->modulation, sizeof(report->modulation), "%s", name);
	report->error_control = m->cfg.ec != 0;
	report->compression = modem_ec_active_compression(m);
}

bool modem_get_call_report(struct modem *m, struct modem_call_report *report)
{
	if (m == NULL || report == NULL || !m->call_report.available)
		return false;
	call_report_refresh(m);
	*report = m->call_report;
	return true;
}

void modem_finish_call_report(struct modem *m, bool local, const char *reason, unsigned result)
{
	if (m == NULL || !m->call_report.active)
		return;
	call_report_refresh(m);
	if (!m->call_report.reason[0]) {
		m->call_report.disconnect_local = local;
		snprintf(m->call_report.reason, sizeof(m->call_report.reason), "%s", reason ? reason : "unknown");
		snprintf(m->call_report.phase, sizeof(m->call_report.phase), "%s", modem_call_phase(m));
		snprintf(m->call_report.step, sizeof(m->call_report.step), "%s", modem_protocol_step(m));
	}
	m->call_report.result = result;
	m->call_report.active = false;
	memset(m->caller_number, 0, sizeof(m->caller_number));
}

/* TDB: state lifecycle description */

#define STATE_DISC 0x100

#define STATE_ESTAB           STATE_DP_ESTAB

#define IS_STATE_LINKED(stat) ((stat) > STATE_DP_ESTAB)

#define IS_STATE_IDLE(stat) ((stat) == STATE_MODEM_IDLE)
#define IS_STATE_CONNECTING(stat) ((stat) > STATE_MODEM_IDLE && (stat) < STATE_MODEM_ONLINE)
#define IS_STATE_ONLINE(stat) ((stat) == STATE_MODEM_ONLINE || (stat) == STATE_COMMAND_ONLINE)
#define IS_STATE_DISC(stat) ((stat) == STATE_EC_DISC || (stat) == STATE_DP_DISC)

const char *modem_call_phase(const struct modem *m)
{
	if (m == NULL)
		return "unknown";
	switch (m->state) {
	case STATE_MODEM_IDLE: return "idle";
	case STATE_DP_ESTAB:
		return m->dp != NULL && m->dp->id == DP_CALLPROG ?
			"call-progress" : "training";
	case STATE_EC_ESTAB: return "error-control-negotiation";
	case STATE_MODEM_ONLINE:
	case STATE_COMMAND_ONLINE:
		if (m->retrain_pending)
			return "local-retrain";
		if (m->dp != NULL && m->dp->status == DPSTAT_OK)
			return "retraining";
		return m->state == STATE_MODEM_ONLINE ? "online" : "online-command";
	case STATE_EC_DISC: return "error-control-disconnect";
	case STATE_DP_DISC: return "cleardown";
	default: return "unknown";
	}
}


const char *modem_protocol_step(const struct modem *m)
{
	if (m != NULL && m->dp != NULL && m->dp->op != NULL &&
	    m->dp->op->progress != NULL) {
		const char *step = m->dp->op->progress(m->dp);
		if (step != NULL)
			return step;
	}
	return modem_call_phase(m);
}

void modem_note_disconnect(struct modem *m, bool local, const char *reason)
{
	if (m == NULL || reason == NULL || m->disconnect_reason != NULL)
		return;
	call_report_refresh(m);
	m->disconnect_step = modem_protocol_step(m);
	m->disconnect_phase = modem_call_phase(m);
	m->disconnect_reason = reason;
	m->disconnect_local = local;
	++m->disconnect_sequence;
	if (m->call_report.active) {
		struct modem_call_report *report = &m->call_report;
		report->disconnect_local = local;
		snprintf(report->reason, sizeof(report->reason), "%s", reason);
		snprintf(report->phase, sizeof(report->phase), "%s", m->disconnect_phase);
		snprintf(report->step, sizeof(report->step), "%s", m->disconnect_step);
	}
}

bool modem_ec_can_establish(const struct modem *m)
{
	return m != NULL && m->cfg.ec &&
	       (m->state == STATE_EC_ESTAB || IS_STATE_ONLINE(m->state));
}

static int modem_set_state(struct modem *m, unsigned new_state)
{
        MODEM_DBG("modem set state: %x --> %x...\n",
		  m->state, new_state);
	if(m->state == new_state)
		return 0;
	switch(new_state) {
	case STATE_MODEM_IDLE:
		MODEM_DBG("new state: MODEM_IDLE\n");
		m->modem_info &= ~TIOCM_CD;
		m->xmit.head = m->xmit.tail = m->xmit.count = 0;
		m->command = 1;
		m->get_chars = 0;
		m->put_chars = 0;
		m->get_bits  = 0;
		m->put_bits  = 0;
		break;
	case STATE_DP_ESTAB:
		MODEM_DBG("new state: DP_ESTAB\n");
		m->command = 0;
		m->get_chars = 0;
		m->put_chars = 0;
		m->get_bits  = 0;
		m->put_bits  = 0;
		break;
	case STATE_EC_ESTAB:
		MODEM_DBG("new state: EC_ESTAB\n");
		m->command = 0;
		m->get_chars = 0;
		m->put_chars = 0;
		break;
	case STATE_MODEM_ONLINE:
		MODEM_DBG("new state: MODEM_ONLINE\n");
		m->command = 0;
		m->put_chars = (m->cfg.ec && m->cfg.comp&0x1)
			? modem_comp_put_chars : modem_put_data;
		m->get_chars = (m->cfg.ec && m->cfg.comp&0x2)
			? modem_comp_get_chars : modem_get_chars;
		m->modem_info |= TIOCM_CD;
		break;
	case STATE_COMMAND_ONLINE:
		MODEM_DBG("new state: COMMAND_ONLINE\n");
		m->command = 1;
		/* Literal callers have no AT input sharing the transmit queue. Keep
		 * compression/framing and decoded receive output alive while paused. */
		if (m->frontend_report != NULL)
			m->get_chars = NULL;
		m->put_chars = (m->cfg.ec && (m->cfg.comp & 0x1)) ?
			modem_comp_put_chars : modem_put_data;
		break;
	case STATE_EC_DISC:
		MODEM_DBG("new state: EC_DISC\n");
		m->get_chars = 0;
		m->put_chars = 0;
		break;
	case STATE_DP_DISC:
		MODEM_DBG("new state: DP_DISC\n");
		m->get_chars = 0;
		m->put_chars = 0;
		m->get_bits  = 0;
		m->put_bits  = 0;
		m->bit_timer = 0;
		m->bit_timer_func = NULL;
		m->packer_process = NULL;
		break;
	default:
		MODEM_ERR("invalid state: %d\n", new_state);
		return -1;
	}
        m->state = new_state;
	return 0;
}


static void run_modem_stop(struct modem *m)
{
	modem_stop(m);
}

enum {
	MODEM_STOP_DELAY_SAMPLES = 48,
	/* DISC is a very small frame even at 300 bit/s.  This leaves room for
	 * serialization and a SIP round trip without inheriting LAPM's ordinary
	 * multi-retry data timeout. */
	MODEM_EC_DISCONNECT_TIMEOUT_MS = 1500,
	/* Protocol cleardown is normally well below a second.  Keep a hard owner
	 * deadline so a broken peer or datapump cannot retain the call forever. */
	MODEM_DP_DISCONNECT_TIMEOUT_MS = 3000,
};

static void schedule_modem_timer(
	struct modem *m, void (*function)(struct modem *),
	unsigned int delay_samples)
{
	/* These short delays may cross the unsigned sample counter boundary. */
	m->sample_timer_func = function;
	m->sample_timer = m->count + delay_samples;
}

static unsigned int modem_timeout_samples(
	const struct modem *m, unsigned int milliseconds)
{
	uint64_t sample_rate = m->srate != 0 ? m->srate : MODEM_RATE;
	uint64_t samples = (sample_rate * milliseconds + 999u) / 1000u;

	return samples > UINT_MAX ? UINT_MAX : (unsigned int)samples;
}

static void schedule_modem_stop(struct modem *m)
{
	schedule_modem_timer(m, run_modem_stop, MODEM_STOP_DELAY_SAMPLES);
}

static int run_pending_modem_stop(struct modem *m)
{
	if (m->sample_timer_func != run_modem_stop)
		return 0;
	m->sample_timer = 0;
	m->sample_timer_func = NULL;
	return modem_stop(m);
}

static void start_datapump_disconnect(struct modem *m)
{
	bool pending = false;

	if (m->dp != NULL && m->dp->op != NULL && m->dp->op->hangup != NULL)
		pending = m->dp->op->hangup(m->dp) == 0;
	modem_set_state(m, STATE_DP_DISC);
	if (!pending) {
		schedule_modem_stop(m);
		return;
	}
	schedule_modem_timer(
		m, run_modem_stop,
		modem_timeout_samples(m, MODEM_DP_DISCONNECT_TIMEOUT_MS));
}

static void expire_ec_disconnect(struct modem *m)
{
	if (m->state != STATE_EC_DISC)
		return;
	start_datapump_disconnect(m);
}


static void modem_hup(struct modem *m, unsigned local)
{
	MODEM_DBG("modem_hup...\n");
	if ((!IS_STATE_IDLE(m->state) || m->call_report.active) && local)
		modem_note_disconnect(m, true, "local hangup");
	m->retrain_pending = 0;
	switch(m->state) {
	case STATE_MODEM_IDLE:
		if (m->call_report.active)
			modem_stop(m);
		return;
	case STATE_DP_ESTAB:
		if (m->result_code == 0)
			m->result_code = RESULT_NOCARRIER;
		/* SIP call progress may have no media clock yet. There is no modem
		 * signal to drain, so cancellation must not wait for audio samples. */
		if (m->dp != NULL && m->dp->id == DP_CALLPROG) {
			modem_stop_now(m);
			return;
		}
		if (local)
			start_datapump_disconnect(m);
		else {
			modem_set_state(m, STATE_DP_DISC);
			schedule_modem_stop(m);
		}
		break;
	case STATE_EC_ESTAB:
	case STATE_MODEM_ONLINE:
	case STATE_COMMAND_ONLINE:
		if (local && m->cfg.ec &&
		    m->get_bits == modem_hdlc_get_bits &&
		    m->put_bits == modem_hdlc_put_bits) {
			modem_ec_stop(m);
			modem_set_state(m,STATE_EC_DISC);
			schedule_modem_timer(
				m, expire_ec_disconnect,
				modem_timeout_samples(
					m, MODEM_EC_DISCONNECT_TIMEOUT_MS));
		}
		else {
			if (local)
				start_datapump_disconnect(m);
			else {
				modem_set_state(m, STATE_DP_DISC);
				schedule_modem_stop(m);
			}
		}
		if (m->result_code == 0)
			m->result_code = RESULT_NOCARRIER;
#if ALREADY_DONE
		if(m->modem_info&TIOCM_CD && m->tty &&
		   !m->termios->c_cflag&CLOCAL /* !C_CLOCAL(m->tty) */) {
			MODEM_DBG("tty_hangup...\n");
			tty_hangup(m->tty);
		}
#endif
		m->modem_info &= ~TIOCM_CD;
		break;
	case STATE_EC_DISC:
		if (!local) {
			modem_set_state(m, STATE_DP_DISC);
			schedule_modem_stop(m);
		}
		break;
	case STATE_DP_DISC:
	default:
		return;
	}
}


void modem_update_status(struct modem *m, unsigned status)
{
	/* A late training or link acknowledgement cannot reopen a closing call. */
	if (IS_STATE_DISC(m->state) &&
	    (status == STATUS_DP_LINK || status == STATUS_PACK_LINK ||
	     status == STATUS_EC_LINK))
		return;

	MODEM_DBG("modem_update_status: %d\n", status);
	switch(status) {
	case STATUS_OK:
		break;
	case STATUS_DP_LINK:
		MODEM_DBG("--> DP LINK\n");
		if (!modem_link_supports_v42(m))
			m->cfg.ec = 0;
		modem_set_state(m,STATE_EC_ESTAB);
		/* start packer */
		if ((m->cfg.ec && m->cfg.ec_detector &&
		     !modem_skip_ec_detection(m)) ||
		    (!m->caller && !m->cfg.ec &&
		     !modem_skip_ec_detection(m) && modem_link_supports_v42(m))) {
			modem_detector_start(m);
			m->get_bits = modem_detector_get_bits;
			m->put_bits = modem_detector_put_bits;
		}
		else {  /* skip pack connect phase */
			modem_update_status(m,STATUS_PACK_LINK);
		}
		break;
	case STATUS_PACK_LINK:
		MODEM_DBG("--> PACK LINK\n");
		if(m->cfg.ec) {
			/* start EC */
			modem_hdlc_start(m);
			m->get_bits = modem_hdlc_get_bits;
			m->put_bits = modem_hdlc_put_bits;
			modem_ec_start(m);
		}
		else {
			/* start async, pass EC connect phase */
			modem_async_start(m);
			m->get_bits = modem_async_get_bits;
			m->put_bits = modem_async_put_bits;
			modem_update_status(m,STATUS_EC_LINK);
		}
		break;
	/* case STATUS_CONNECT: */
	case STATUS_EC_LINK:
		MODEM_DBG("--> EC LINK\n");
		modem_set_state(m,STATE_MODEM_ONLINE);
		if (m->call_report.active)
			m->call_report.connected = true;
		call_report_refresh(m);
		modem_report_result(m,RESULT_CONNECT); // fixme
		if(SPEAKER_CONTROL(m) == 1)
			m->driver.ioctl(m,MDMCTL_SPEAKERVOL,0);
		break;
	case STATUS_RETRAIN_COMPLETE:
		if (!m->retrain_pending || !IS_STATE_ONLINE(m->state))
			break;
		m->retrain_pending = 0;
		modem_report_result(m, RESULT_CONNECT);
		break;
	case STATUS_EC_PEER_DISC:
		modem_note_disconnect(m, false, "peer LAPM DISC");
		m->result_code = RESULT_NOCARRIER;
		/* With simultaneous DISC, both ends have queued UA. Keep the
		 * local disconnect deadline: it gives UA time to drain, then
		 * starts our cleardown instead of waiting for the peer forever. */
		if (m->state == STATE_EC_DISC)
			break;
		if (m->state != STATE_DP_DISC) {
			modem_set_state(m, STATE_DP_DISC);
			/* The received DISC has queued UA. Do not clear its framer
			 * or initiate a rate exchange ahead of its closing flag.
			 * Continue media to answer the peer's protocol cleardown;
			 * a missing peer still cannot hold the line indefinitely. */
			m->get_bits = modem_hdlc_get_bits;
			m->put_bits = modem_hdlc_put_bits;
			m->modem_info &= ~TIOCM_CD;
			schedule_modem_timer(m, run_modem_stop,
				modem_timeout_samples(m, MODEM_DP_DISCONNECT_TIMEOUT_MS));
		}
		break;
	case STATUS_EC_RELEASE:
	case STATUS_EC_ERROR:
		modem_note_disconnect(m, status == STATUS_EC_ERROR,
			status == STATUS_EC_ERROR ? "LAPM negotiation or recovery failed" : "LAPM released");
		MODEM_DBG("--> EC UNLINK\n");
		m->result_code = RESULT_NOCARRIER;
		if (m->state == STATE_EC_DISC)
			start_datapump_disconnect(m);
		else
			modem_hup(m, 0);
		break;
	case STATUS_ERROR:
	case STATUS_DP_ERROR:
	case STATUS_NOCARRIER:
	case STATUS_NODIALTONE:
	case STATUS_BUSY:
	case STATUS_NOANSWER:
	default:
		MODEM_DBG("--> FINISH.\n");
		modem_note_disconnect(m, status != STATUS_BUSY,
			status == STATUS_NOANSWER ? "carrier wait expired" :
			status == STATUS_NODIALTONE ? "no dial tone" :
			status == STATUS_BUSY ? "busy" :
			status == STATUS_NOCARRIER ? "carrier lost" :
			status == STATUS_DP_ERROR ? "datapump failed" : "modem processing failed");
		if(status == STATUS_NODIALTONE)
			m->result_code = RESULT_NODIALTONE;
		else if(status == STATUS_BUSY)
			m->result_code = RESULT_BUSY;
		else if(status == STATUS_NOANSWER)
			m->result_code = RESULT_NOANSWER;
		else
			m->result_code = RESULT_NOCARRIER;
		modem_hup(m,IS_STATE_DISC(m->state));
		break;
	}
}



/*
 *    Interrupt handlers and services
 *
 */

static void modem_tick_bit_timer(struct modem *m, int bits)
{
	if (m == NULL || bits <= 0 || !m->bit_timer)
		return;
	m->bit_timer -= bits;
	if (m->bit_timer <= 0) {
		m->bit_timer = 0;
		m->bit_timer_func(m);
	}
}

/* get/put bits */
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	void (*timer_func)(struct modem *m) = m->bit_timer_func;
	int timer = m->bit_timer;
	int bits;

	if(m->get_bits)
		n = m->get_bits(m,nbits,buf,n);
	else
		memset(buf,0xff,n);
	/* process bit timer and packer */
	bits = nbits * n;
	if (m->bit_timer == timer && m->bit_timer_func == timer_func)
		modem_tick_bit_timer(m, bits);
	// FIXME NOW: remove packer_process from modem to EC
	if(m->packer_process)
		m->packer_process(m,bits);
	return n;
}

int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	void (*timer_func)(struct modem *m) = m->bit_timer_func;
	int timer = m->bit_timer;

	if(m->put_bits)
		n = m->put_bits(m,nbits,buf,n);
	if (m->bit_timer == timer && m->bit_timer_func == timer_func)
		modem_tick_bit_timer(m, nbits * n);
	return n;
}


static unsigned dpstat2status(unsigned dpstat)
{
	switch (dpstat) {
	case DPSTAT_OK: return STATUS_OK;
	case DPSTAT_CONNECT: return STATUS_DP_LINK;
	case DPSTAT_ERROR: return STATUS_DP_ERROR;
	case DPSTAT_NODIALTONE: return STATUS_NODIALTONE;
	case DPSTAT_BUSY: return STATUS_BUSY;
	case DPSTAT_NOANSWER: return STATUS_NOANSWER;
	case DPSTAT_CHANGEDP: return STATUS_OK;
	case DPSTAT_DISCONNECT: return STATUS_OK;
	default: return STATUS_ERROR;
	}
}


void modem_dp_connected(struct modem *m)
{
	if (m == NULL)
		return;
	if (IS_STATE_ONLINE(m->state)) {
		if (m->retrain_pending)
			modem_update_status(m, STATUS_RETRAIN_COMPLETE);
		return;
	}
	if (!IS_STATE_LINKED(m->state) && !IS_STATE_DISC(m->state))
		modem_update_status(m, STATUS_DP_LINK);
}

void modem_dp_reconnected(struct modem *m)
{
	if (m == NULL)
		return;
	if (m->call_report.active && m->call_report.connected &&
	    (IS_STATE_ONLINE(m->state) || m->state == STATE_EC_ESTAB)) {
		m->call_report.retrains++;
		call_report_refresh(m);
	}
	if ((IS_STATE_ONLINE(m->state) || m->state == STATE_EC_ESTAB) &&
	    modem_ec_resume_after_retrain(m) != 0) {
		modem_update_status(m, STATUS_EC_ERROR);
		return;
	}
	modem_dp_connected(m);
}


static void modem_dp_process_frame(struct modem *m, enum g711_law law,
				   const u8 *receive_codewords,
				   const bool *received_mask,
				   u8 *transmit_codewords,
				   bool *exact_output,
				   void *in, void *out, int count)
{
	int ret;
	unsigned cnt;
	unsigned int frame_samples = count;
	bool all_exact = receive_codewords != NULL &&
		transmit_codewords != NULL && exact_output != NULL;

	if (exact_output != NULL)
		*exact_output = false;
	//MODEM_DBG("modem_process %d: %d...\n", m->count,count);
	while(count && m->dp) {
		bool fragment_exact = false;
		bool stop_processing = false;

		cnt = count;
		if (cnt > m->frag)
			cnt = m->frag;
		//MODEM_DBG("%d: running dp %s...\n", m->count,dp->name);
		if (receive_codewords != NULL &&
		    m->dp->op->process_g711 != NULL)
			ret = m->dp->op->process_g711(
				m->dp, law, receive_codewords, in, received_mask,
				transmit_codewords, out, cnt, &fragment_exact);
		else {
			ret = m->dp->op->process(m->dp,in,out,cnt);
			fragment_exact = false;
		}
		if (!fragment_exact)
			all_exact = false;
		switch(ret) {
		case DPSTAT_OK:
			break;
		case DPSTAT_CONNECT:
			modem_dp_connected(m);
			break;
		case DPSTAT_CHANGEDP:
			do_modem_change_dp(m);
			break;
		case DPSTAT_DISCONNECT:
			if (m->state == STATE_DP_DISC) {
				m->process = NULL;
				/* m->count still denotes the start of this media block.
				 * Keep the final block alive until its caller sends it. */
				schedule_modem_timer(
					m, run_modem_stop,
					frame_samples + MODEM_STOP_DELAY_SAMPLES);
				stop_processing = true;
			} else {
				modem_update_status(m, STATUS_DP_ERROR);
			}
			break;
		default: /* errors */
			all_exact = false;
			if (m->state == STATE_DP_DISC)
				schedule_modem_stop(m);
			else
				modem_update_status(m,dpstat2status(ret));
			m->process = NULL;
			stop_processing = true;
			break;
		}
		/* Bit timers run inside datapump source/sink callbacks. Defer a
		 * link-quality retrain until the datapump has returned, and never
		 * turn it into an unsolicited terminal CONNECT or reset LAPM. */
		if (ret == DPSTAT_OK && IS_STATE_ONLINE(m->state) && m->dp != NULL &&
		    m->dp->op->retrain != NULL && modem_ec_take_retrain_request(m)) {
			int result = m->dp->op->retrain(m->dp);
			MODEM_DBG("stalled LAPM retrain: %s\n", result == 0 ? "accepted" : "unavailable");
		}

		count -= cnt;
		out += cnt<<MFMT_SHIFT(m->format);
		in  += cnt<<MFMT_SHIFT(m->format);
		if (receive_codewords != NULL)
			receive_codewords += cnt;
		if (received_mask != NULL)
			received_mask += cnt;
		if (transmit_codewords != NULL)
			transmit_codewords += cnt;
		if (stop_processing)
			break;
	}
	if(count) {
		if (all_exact) {
			uint8_t silence = law == G711_LAW_PCMA ? 0xd5 : 0xff;
			/* G.711's smallest positive A-law level is 8, not zero. */
			int16_t linear = law == G711_LAW_PCMA ? 8 : 0;
			memset(transmit_codewords, silence, count);
			for (int index = 0; index < count; ++index)
				((int16_t *)out)[index] = linear;
		} else {
			memset(out,0,count<<MFMT_SHIFT(m->format));
		}
	}
	if (exact_output != NULL)
		*exact_output = all_exact;
}

static void modem_dp_process(struct modem *m,void *in,void *out,int count)
{
	modem_dp_process_frame(
		m, G711_LAW_PCMU, NULL, NULL, NULL, NULL, in, out, count);
}

static bool modem_preserve_pcm_input(const struct modem *m)
{
	if (m == NULL || m->dp == NULL ||
	    (m->dp->id != DP_V90 && m->dp->id != DP_V92))
		return false;

	/* The analogue-side receiver owns the downstream PCM constellation. */
	if (m->dsp_info.pcm_role == MODEM_PCM_ROLE_ANALOG)
		return true;

	/* The digital-side receiver owns V.92 PCM upstream when enabled. */
	return m->dp->id == DP_V92 &&
	       m->dsp_info.pcm_role == MODEM_PCM_ROLE_DIGITAL &&
	       m->sregs[SREG_V92_PCM_UPSTREAM] != 0;
}

static void modem_process_frame(struct modem *m, enum g711_law law,
				const u8 *receive_codewords,
				const bool *received_mask,
				u8 *transmit_codewords,
				bool *exact_output,
				void *in, void *out, int count)
{
	if (exact_output != NULL)
		*exact_output = false;
	if (count > 0 && m->call_report.active)
		m->call_report.media_samples += (unsigned int)count;
	if (count > 0 && !modem_preserve_pcm_input(m) &&
	    dc_remover_process(&m->receive_dc, in, (size_t)count) !=
		    DC_REMOVER_OK) {
		MODEM_ERR("receive DC removal failed.\n");
		memset(out, 0, (size_t)count << MFMT_SHIFT(m->format));
		return;
	}
	if(m->process) {
		if (receive_codewords != NULL && m->process == modem_dp_process)
			modem_dp_process_frame(
				m, law, receive_codewords, received_mask, transmit_codewords,
				exact_output, in, out, count);
		else
			m->process(m,in,out,count);
	}
	else  /* mute output */
		memset(out,0,count<<MFMT_SHIFT(m->format));
	m->count+=count;
	if (m->sample_timer_func != NULL && time_after(m->count, m->sample_timer)) {
		void (*f)(struct modem *) = m->sample_timer_func ;
                m->sample_timer = 0;
		m->sample_timer_func = NULL;
		f(m);
	}
}

void modem_process(struct modem *m,void *in,void *out,int count)
{
	modem_process_frame(
		m, G711_LAW_PCMU, NULL, NULL, NULL, NULL, in, out, count);
}

void modem_process_g711(struct modem *m, enum g711_law law,
			 const u8 *receive_codewords, void *receive_samples,
			 const bool *received_mask,
			 u8 *transmit_codewords, void *transmit_samples,
			 int count, bool *exact_output)
{
	modem_process_frame(
		m, law, receive_codewords, received_mask, transmit_codewords, exact_output,
		receive_samples, transmit_samples, count);
}


/*
 *    event processing
 *
 */


static void schedule_event(struct modem *m, unsigned mask, unsigned long when)
{
	m->new_event |= mask;
	m->event_deadline = get_time() + when;
	m->event_scheduled = 1;
#ifdef DEBUG_TIMERS
	MODEM_DBG("schedule_event: %x | %x : now %lu + %lu = %lu...\n",
		  m->new_event, mask,
		  get_time(), when, m->event_deadline);
#endif
}


void modem_event(struct modem *m)
{
	unsigned event = m->event;
	m->event = 0;
        MODEM_DBG("modem_event: %x...\n", event);

	if(event&MDMEVENT_RING_CHECK) {
		if(m->ring_count == 0) {
			MODEM_DBG("ring cancel...\n");
			TOTAL_RINGS_COUNT(m) = 0;
		}
		else if ( time_before(m->ring_last,m->ring_first+RING_ON_MIN(m)) ||
			  time_after(m->ring_last,m->ring_first+RING_ON_MAX(m)) ||
			  m->ring_count < RING_COUNT_MIN(m)) {
			MODEM_DBG("ring reject: count %d (%lu-%lu)\n",
				  m->ring_count,m->ring_first,m->ring_last);
			TOTAL_RINGS_COUNT(m) = 0;
			m->ring_count = 0;
		}
		else {
			MODEM_DBG("ring valid.\n");
			m->ring_count = 0;
			TOTAL_RINGS_COUNT(m)++;
			if(TOTAL_RINGS_COUNT(m) == 1)
				modem_put_chars(m,CRLF_CHARS(m),2);
			modem_report_result(m,RESULT_RING);
			if (ANSWER_AFTER_RINGS(m) &&
#ifdef MODEM_CONFIG_RING_DETECTOR
			    (!m->started || m->rd_obj) &&
#else
			    !m->started &&
#endif
			    TOTAL_RINGS_COUNT(m) >= ANSWER_AFTER_RINGS(m)) {
				TOTAL_RINGS_COUNT(m) = 0;
				modem_answer(m);
			}
			else {
				schedule_event(m,MDMEVENT_RING_CHECK,
					       RING_OFF_MAX(m) + 1);
			}
		}
	}
	if ((event & MDMEVENT_ESCAPE) && m->frontend_escape != NULL)
		m->frontend_escape(m);
}

int modem_service_event(struct modem *m)
{
	if (m == NULL)
		return -1;
	if (m->event_scheduled &&
	    time_after(get_time(), m->event_deadline)) {
		m->event |= m->new_event;
		m->new_event = 0;
		m->event_scheduled = 0;
	}
	if (m->event == 0)
		return 0;
	modem_event(m);
	return 1;
}

void modem_ring(struct modem *m)
{
	unsigned long now = get_time();
	if (m->state != STATE_MODEM_IDLE)
		return;
	m->ring_count++;
	if(m->ring_count == 1) {
		MODEM_DBG("modem_ring...\n");
		if(time_before(now,m->ring_last + RING_OFF_MIN(m))) {
			MODEM_DBG("bad ring: now %lu, last %lu.\n",
				  now,m->ring_last);
			m->ring_count = 0;
		}
		else {
			m->ring_first = now;
			schedule_event(m,MDMEVENT_RING_CHECK,
				       RING_ON_MAX(m) + 1);
#ifdef MODEM_CONFIG_CID
			if (TOTAL_RINGS_COUNT(m) == 0 && m->cid_requested)
				modem_cid_start(m, RING_ON_MAX(m) + RING_OFF_MIN(m));
#endif
		}
	}
	m->ring_last = now;
}

int modem_report_incoming_call(struct modem *m)
{
	if (m == NULL || m->state != STATE_MODEM_IDLE)
		return -1;
	if (!m->call_report.active)
		call_report_begin(m, false);
	if (TOTAL_RINGS_COUNT(m) < UCHAR_MAX)
		TOTAL_RINGS_COUNT(m)++;
	modem_report_result(m, RESULT_RING);
	if (ANSWER_AFTER_RINGS(m) && !m->started &&
	    TOTAL_RINGS_COUNT(m) >= ANSWER_AFTER_RINGS(m)) {
		TOTAL_RINGS_COUNT(m) = 0;
		return modem_answer(m);
	}
	return 0;
}


void modem_error(struct modem *m)
{
	MODEM_DBG("modem error...\n");
}



/* command mode processing */
/*
 *    TTY procedures
 *
 */


/* get chars */
static int modem_get_chars(struct modem *m, char *buf, int n)
{
	int ret = 0, cnt;
        while(n) {
		int available = m->xmit.count;

		/* Keep a possible escape out of both framing and compression
		 * until its guard expires or later input cancels it. */
		if (m->state == STATE_MODEM_ONLINE)
			available -= (int)m->escape_count;
                cnt = n;
                if (cnt > available)
                        cnt = available;
                if (cnt > m->xmit.size - m->xmit.tail)
                        cnt = m->xmit.size - m->xmit.tail;
                if (cnt <= 0) {
                        break;
                }
                memcpy(buf, m->xmit.buf + m->xmit.tail, cnt);
                ret += cnt;
                n -= cnt;
                buf += cnt;
                m->xmit.count -= cnt;
                m->xmit.tail = (m->xmit.tail+cnt)%m->xmit.size;
        }
	if (m->call_report.active && m->state == STATE_MODEM_ONLINE && !m->command)
		m->call_report.tx_dte_bytes += (unsigned int)ret;
	return ret;
}

/* put chars */
static int modem_put_chars(struct modem *m, const char *buf, int n)
{
	return m->control_output != NULL ?
		m->control_output(m->control_output_context, buf, n) : n;
}

static int modem_put_data(struct modem *m, const char *buf, int n)
{
	int consumed = m->data_output != NULL ?
		m->data_output(m->data_output_context, buf, n) : 0;
	if (consumed < 0 || consumed > n) {
		m->dte_overflow = true;
		return 0;
	}
	if (m->call_report.active)
		m->call_report.rx_dte_bytes += (unsigned int)consumed;
	return consumed;
}

/* compressor get/put chars */
/* fixme: unify interfaces: modem tx -> comp -> ec -> hdlc -->
                            modem tx -> ec -> hdlc         -->
                            modem tx -> async              -->
			    modem tx --> raw output
*/
static int modem_comp_get_chars(struct modem *m, char *buf, int n)
{
	int ret = modem_comp_read_encoder(m, buf, n), cnt;
	char ch;
	if (ret < 0)
		return -1;
	while(ret < n) {
		cnt = modem_get_chars(m,&ch,1);
		if(cnt <= 0)
			break;
		cnt = modem_comp_encode(m,ch,buf+ret,n-ret);
		if(cnt < 0) {
			break;
		}
		ret += cnt;
	}
	if(ret < n) {
		cnt =  modem_comp_flush_encoder(m,buf+ret,n-ret);
		ret += cnt;
	}
	return ret;
}

static int comp_send_output(struct modem *m)
{
	int cnt;
	if(m->comp.count > 0) {
		cnt = modem_put_data(m,m->comp.buf+m->comp.head,m->comp.count);
		m->comp.count -= cnt;
		m->comp.head += cnt;
		if(m->comp.count > 0)
			return 0;
		else
			m->comp.head = 0;
	}
	return 1;
}

static int modem_comp_put_chars(struct modem *m, const char *buf, int n)
{
	int i=0, cnt;
	if(!comp_send_output(m))
		return 0;
	while(i<n) {
		cnt = modem_comp_decode(m,buf[i++],m->comp.buf,sizeof(m->comp.buf));
		if(cnt < 0) {
			MODEM_DBG("decoder error. %d(%d)\n",i,n);
			modem_update_status(m,STATUS_ERROR);
			return -1;
		}
		m->comp.count += cnt;
		if(!comp_send_output(m))
			break;
	}
	/* The buffer still owns any blocked output. Flush after it drains,
	 * including when LAPM retries with no remaining compressed input. */
	if(i==n && m->comp.count == 0) {
		cnt = modem_comp_flush_decoder(m,m->comp.buf,sizeof(m->comp.buf));
		m->comp.count += cnt;
		comp_send_output(m);
	}
	return i;
}




/*
 *    internal procedures
 *
 */

static bool datapump_requires_v8(enum DP_ID id)
{
	/* Phase 1 remains mandatory when +MS fixes the permitted mode. */
	return id == DP_V34 || v8_datapump_uses_pcm(id);
}

static void modem_setup_link_config(struct modem *m, enum DP_ID id);

static void do_modem_change_dp (struct modem *m)
{
	const struct dp_operations *op;
        struct dp *old;

	old = m->dp;

#ifdef MODEM_CONFIG_FAX
	if (m->mode == MODEM_MODE_FAX) {
		modem_fax_start(m);
		m->dp = NULL;
	}
	else {
#endif
		struct dp *dp = NULL;
		int dp_id,id;
		dp_id = m->dp_requested;
		m->dp_requested = 0;

		id = dp_id;
		if(dp_id == 0) {
			dp_id = MODEM_DP(m);
			id = dp_id;
			if (MODEM_AUTOMODE(m) || datapump_requires_v8(dp_id)) {
				enum v8_modulation modulation =
					v8_datapump_modulation(dp_id);

				if (modulation != V8_MODULATION_NONE)
					id = DP_V8;
			}
		}

		MODEM_DBG("%ld: change dp: --> %d...\n", m->count, id);

		dp = v8_datapump_take_legacy(old);
		op = modem_dp_get_registered_ops(id);
		if (dp == NULL && op && op->create)
			dp = op->create(m,dp_id,
					m->caller,m->srate,
					m->frag,op);
		if (!dp) {
			MODEM_ERR("change dp -> %d error.\n", id);
			modem_note_disconnect(m, true, "cannot create selected datapump");
			modem_hup(m,1);
			return;
		}

		if (dp->id == DP_DS0) {
			modem_setup_link_config(m, DP_DS0);
			/* V.8 may have started under a stored raw policy. Rebuild
			 * the per-call LAPM offer and allocate its dictionaries
			 * before replaying the proving frame into the receiver. */
			modem_ec_exit(m);
			if (m->cfg.ec &&
			    ((m->cfg.comp &&
			      (m->cfg.comp_types & MODEM_COMP_TYPE_V42BIS) != 0 &&
			      m->comp.encoder.dict == NULL && modem_comp_init(m)) ||
			     modem_ec_init(m))) {
				dp->op->delete(dp);
				modem_note_disconnect(m, true, "cannot initialize DS0 error control");
				modem_hup(m, 1);
				return;
			}
		}
		m->dp = dp;
		m->process = modem_dp_process;
#ifdef MODEM_CONFIG_FAX
	}
#endif

        if ( old ) {
                old->op->delete(old);
        }
	if (m->dp != NULL && m->dp->status == DPSTAT_CONNECT)
		modem_dp_connected(m);

}


static int modem_set_hook(struct modem *m, unsigned hook_state)
{
        int ret;
        MODEM_DBG("modem set hook: %d --> %d...\n", m->hook, hook_state);
        if ( m->hook == hook_state )
                return 0;
        ret = m->driver.ioctl(m, MDMCTL_HOOKSTATE,hook_state);
        if (!ret)
                m->hook = hook_state;
        return ret;
}

static void modem_setup_link_config(struct modem *m, enum DP_ID id)
{
	m->cfg.ec = MODEM_EC_ENABLE(m);
	m->cfg.ec_detector = MODEM_EC_DETECTOR(m);
	if (id == DP_DS0) {
		m->cfg.ec = m->sregs[SREG_DS0_FRAMING] != 0;
		m->cfg.ec_detector = 0;
	}
	m->cfg.ec_tx_win_size  = LAPM_MAX_WIN_SIZE;
	m->cfg.ec_rx_win_size  = LAPM_MAX_WIN_SIZE;
	m->cfg.ec_tx_info_size = LAPM_MAX_INFO_SIZE;
	m->cfg.ec_rx_info_size = LAPM_MAX_INFO_SIZE;
	m->cfg.comp_types = MODEM_COMP_TYPES(m);
	if(m->cfg.ec && m->sregs[SREG_COMP] && m->cfg.comp_types != 0)
		m->cfg.comp = 0x3; /* bidirectional compression */
	else
		m->cfg.comp = 0;
	m->cfg.comp_dict_size  = COMP_MAX_CODEWORDS;
	m->cfg.comp_max_string = COMP_DEFAULT_STRING;

}

static void modem_setup_config(struct modem *m)
{
	modem_setup_link_config(m, MODEM_DP(m));
	/* setup dsp data */
	/* An exchanged V.8 selection belongs only to this physical call. */
	m->dsp_info.v8_lapm = 0;
	m->dsp_info.startup = MODEM_STARTUP_UNKNOWN;
	m->dsp_info.pcm_role = MODEM_PCM_ROLE_NONE;
	m->dsp_info.upstream_mode = MODEM_UPSTREAM_UNKNOWN;
}


static int do_modem_start(struct modem *m)
{
	int ret;
        ret = m->driver.ioctl(m, MDMCTL_SPEED, m->srate);
        ret = m->driver.ioctl(m, MDMCTL_SETFRAG, m->frag);
        m->count = 0;
        ret = m->driver.start(m);
	m->started = !ret;
	return ret;
}

static int modem_start (struct modem *m)
{
        int ret;

        MODEM_DBG("modem_start..\n");
	if(m->started && modem_stop(m))
		return -1;

	m->result_code = 0;
	m->retrain_pending = 0;
	modem_setup_config(m);
	m->dte_overflow = false;
	if (dc_remover_init(&m->receive_dc) != DC_REMOVER_OK)
		return -1;
        modem_set_state(m, STATE_ESTAB);

	if(SPEAKER_CONTROL(m) == 1)
		m->driver.ioctl(m,MDMCTL_SPEAKERVOL,SPEAKER_VOLUME(m));
        ret = modem_set_hook(m, MODEM_HOOK_OFF);
        if (ret)
                goto error;

	m->xmit.head = m->xmit.tail = m->xmit.count = 0;
	m->command = 0;

	if (m->cfg.ec && m->cfg.comp &&
	    (m->cfg.comp_types & MODEM_COMP_TYPE_V42BIS) != 0 &&
	     (ret = modem_comp_init(m)) )
		goto error;
	if ( m->cfg.ec &&
	     (ret = modem_ec_init(m)))
		goto error;

	/* clear rings and all events */
	TOTAL_RINGS_COUNT(m) = 0;
	m->ring_count = 0;
	m->event = m->new_event = 0;
	m->event_scheduled = 0;

        ret = do_modem_start(m);
	if (!ret)
		return 0;

 error:
	MODEM_ERR("modem start = %d: cannot start device.\n",ret);
	modem_note_disconnect(m, true, "cannot start modem");
	m->result_code = RESULT_NOCARRIER;
	modem_stop(m);
	return ret;
}


static int modem_stop (struct modem *m)
{
        int ret = 0;
        MODEM_DBG("modem_stop..\n");
	if (m->call_report.active) {
		call_report_refresh(m);
		if (m->disconnect_reason == NULL)
			modem_note_disconnect(m, true, "local stop");
		modem_finish_call_report(m, m->disconnect_local, m->disconnect_reason,
			m->result_code ? m->result_code : RESULT_OK);
	}

	m->process = NULL;
	m->retrain_pending = 0;
	m->sample_timer = 0;
	m->sample_timer_func = NULL;
	m->bit_timer = 0;
	m->bit_timer_func = NULL;
	m->packer_process = NULL;

	if(m->started) {
		ret = m->driver.stop(m);
		m->started = ret;
		if ( ret ) {
			MODEM_ERR("modem stop = %d: cannot stop device.\n",ret);
		}
	}
        modem_set_hook(m, MODEM_HOOK_ON);
	if(SPEAKER_CONTROL(m) == 1)
		m->driver.ioctl(m,MDMCTL_SPEAKERVOL,0);
	m->caller = 0;
	m->command = 1;

	// FIXME: If ec,comp were allocated handled inside _exit() - improve?
	modem_ec_exit(m);
	modem_comp_exit(m);

        if (m->dp) {
                struct dp *dp;
                dp = m->dp;
                m->dp = 0;
                if(dp) dp->op->delete(dp);
        }
#ifdef MODEM_CONFIG_RING_DETECTOR
	if(m->rd_obj) {
		RD_delete(m->rd_obj);
		m->rd_obj = NULL;
	}
#endif
#ifdef MODEM_CONFIG_CID
	if(m->cid) {
		CID_delete(m->cid);
		m->cid = NULL;
	}
#endif
#ifdef MODEM_CONFIG_FAX
	if(m->fax_obj) {
		FAX_delete(m->fax_obj);
		m->fax_obj = NULL;
	}
#endif
        modem_set_state(m, STATE_MODEM_IDLE);
	if (m->hangup_command_pending) {
		m->hangup_command_pending = 0;
		modem_report_result(m, RESULT_OK);
	} else if (m->result_code) {
		modem_report_result(m, m->result_code);
	}
	m->result_code = 0;

	m->count = 0;
	return m->started;
}


/*
 *    Caller ID
 *
 */

#ifdef MODEM_CONFIG_CID

static void modem_cid_stop(struct modem *m)
{
	MODEM_DBG("modem_cid_stop...\n");
	m->sample_timer = 0;
	m->sample_timer_func = NULL;
#ifdef MODEM_CONFIG_RING_DETECTOR
	if(!m->rd_obj) {
		m->process = NULL;
		modem_stop(m);
	}
	else {
		CID_delete(m->cid);
		m->cid = NULL;
	}
#else
	m->process = NULL;
	modem_stop(m);
#endif
	/* continue with answer if need */
	if (ANSWER_AFTER_RINGS(m) &&
	    TOTAL_RINGS_COUNT(m) >= ANSWER_AFTER_RINGS(m)) {
		TOTAL_RINGS_COUNT(m) = 0;
		modem_answer(m);
	}
}

static void modem_cid_process(struct modem *m, void *in, void *out, int count)
{
	int status;
	//MODEM_DBG("modem_cid_process: %d...\n", count);
	memset(out,0,count*2);
	status = CID_process(m->cid, in, count);
	if(status) {
		if(status < 0)
			MODEM_DBG("CID failed.\n");
		modem_cid_stop(m);
	}
}

static int modem_cid_start(struct modem *m, unsigned timeout)
{
	MODEM_DBG("modem_cid_start: timeout = %d...\n", timeout);
#ifdef MODEM_CONFIG_RING_DETECTOR
	if(m->started && !m->rd_obj)
#else
	if(m->started)
#endif
		return -1;
	m->cid = CID_create(m, m->srate, m->cid_requested);
	if(!m->cid)
		return -1;
	m->sample_timer = m->count + m->srate*timeout/MODEM_HZ ;
	m->sample_timer_func = modem_cid_stop;
#ifdef MODEM_CONFIG_RING_DETECTOR
	if(m->started && m->rd_obj)
		return 0;
#endif
	m->process = modem_cid_process;
        modem_set_hook(m, MODEM_HOOK_SNOOPING);
	return do_modem_start(m);
}

#endif /* MODEM_CONFIG_CID */


/*
 *    Ring detector (internal)
 *
 */

#ifdef MODEM_CONFIG_RING_DETECTOR
static void modem_ring_detector_process(struct modem *m, void *in, void *out, int count)
{
	int ret;
	memset(out, 0, count*2);
	ret = RD_process(m->rd_obj, in, count);
	if (ret) {
		long freq, duration;
		RD_ring_details(m->rd_obj, &freq, &duration);
		MODEM_DBG("ring details: freq = %ld, duration = %ld\n",
			  freq, duration);
		if(freq == 0) {
			MODEM_DBG("report ring start...\n");
			modem_ring(m);
		}
		else if (freq > 0) {
			MODEM_DBG("report ring end...\n");
			/* ring finishing */
			m->event |= MDMEVENT_RING_CHECK;
			if (m->ring_count <= 1)
				m->ring_count = duration * freq / 1000 ;
			m->ring_last = get_time();
		}
		else
			MODEM_ERR("RD returns %ld freq. (duration %ld)\n",
				  freq, duration);
	}
#ifdef MODEM_CONFIG_CID
	if(m->cid)
		modem_cid_process(m, in, out, count);
#endif
}

int modem_ring_detector_start(struct modem *m)
{
	MODEM_DBG("modem_ring_detector_start...\n");
	if(m->rd_obj) {
		MODEM_ERR("modem_ring_detector_start: rd_obj already exists!\n");
		return -1;
	}
	m->rd_obj = RD_create(m, m->srate);
	m->process = modem_ring_detector_process;
	modem_set_hook(m, MODEM_HOOK_SNOOPING);
	return do_modem_start(m);
}
#endif /* MODEM_CONFIG_RING_DETECTOR */


/*
 *    Fax modem
 *
 */

#ifdef MODEM_CONFIG_FAX

static void modem_fax_process(struct modem *m, void *in, void *out, int count)
{
	int ret;
	//MODEM_DBG("modem_fax_process: %d...\n", count);
	ret = FAX_process(m->fax_obj, in, out, count);
	if(ret) {
		MODEM_DBG("fax_process: %d\n", ret);
		switch(ret) {
		case FAX_STATUS_OK:
			modem_set_state(m, STATE_COMMAND_ONLINE);
			modem_report_result(m, RESULT_OK);
			m->command = 1;
			break;
		case FAX_STATUS_CONNECT:
			modem_set_state(m,STATE_MODEM_ONLINE);
			modem_report_result(m, RESULT_CONNECT);
			m->command = 0;
			break;
		case FAX_STATUS_NOCARRIER:
			modem_set_state(m, STATE_COMMAND_ONLINE);
			modem_report_result(m, RESULT_NOCARRIER);
			m->command = 1;
			break;
		case FAX_STATUS_ERROR:
			modem_update_status(m, STATUS_ERROR);
			break;
		default:
			break;
		}
		if(ret < 0) {
			MODEM_DBG("FAX failed.\n");
			modem_stop(m);
		}
	}
}

static int modem_fax_start(struct modem *m)
{
	MODEM_DBG("modem_fax_start...\n");
	m->fax_obj = FAX_create(m, m->caller, m->srate);
	if(!m->fax_obj)
		return -1;
	m->sample_timer = 0;
	m->sample_timer_func = NULL;
	m->process = modem_fax_process;
	return 0;
}

#endif /* MODEM_CONFIG_FAX */


/*
 *    Commands
 *
 */

int modem_send_to_tty(struct modem *m, const char *buf, int n)
{
	return modem_put_chars(m, buf, n);
}

int modem_recv_from_tty(struct modem *m, char *buf, int n)
{
	return modem_get_chars(m, buf, n);
}

int modem_answer(struct modem *m)
{
        MODEM_DBG("modem answer...\n");
        if ( m->dp ) {
                MODEM_ERR("dp %d is already exists.\n", m->dp->id);
                return -1;
        }
	if(m->started)
		modem_stop(m);
	if (!m->call_report.active)
		call_report_begin(m, false);
	m->dp_requested = 0;
	m->automode_requested = 0; /* MODEM_AUTOMODE(m) */
	m->sample_timer = ANSWER_DELAY(m) ?
		ANSWER_DELAY(m)*m->srate : m->srate/100;
	m->sample_timer_func = do_modem_change_dp;
	return modem_start(m);
}


static int modem_dial_start(struct modem *m)
{
	const struct dp_operations *op;
        struct dp *dp = NULL;
        MODEM_DBG("modem_dial_start...\n");
	if(m->dp) {
		return -1;
	}
	if (m->started)
		modem_stop(m);
	call_report_begin(m, true);
	m->caller = 1;
	op = modem_dp_get_registered_ops(DP_CALLPROG);
	if (op && op->create)
		dp = op->create(m,DP_CALLPROG,
				m->caller,m->srate,
				m->frag,op);
	if (!dp) {
		MODEM_ERR("cannot create CALLPROG.\n");
		modem_note_disconnect(m, true, "cannot create call-progress datapump");
		m->call_report.result = RESULT_ERROR;
		m->call_report.active = false;
		return -1;
	}
        m->dp = dp;
	m->process = modem_dp_process;
	return 0;
}

int modem_dial(struct modem *m)
{
	int ret;
        MODEM_DBG("modem dial: %s...\n", m->dial_string);
	m->dp_requested = 0;
	m->automode_requested = 0;
 	ret = modem_dial_start(m);
	if(ret)
		return -1;
	return modem_start(m);
}

int modem_report_outgoing_call_connected(struct modem *m)
{
	if (m == NULL)
		return -1;
	MODEM_DBG("outgoing call connected...\n");
	if (!m->started || !m->caller || !m->dp ||
	    m->dp->id != DP_CALLPROG)
		return 0;
	do_modem_change_dp(m);
	return 0;
}


int modem_hook(struct modem *m, unsigned hook_state)
{
	MODEM_DBG("modem hook...\n");
	if (IS_STATE_IDLE(m->state) && m->call_report.active && hook_state == MODEM_HOOK_ON) {
		modem_hup(m, 1);
		return 0;
	}
	if (!IS_STATE_IDLE(m->state) && hook_state == MODEM_HOOK_ON) {
		m->hangup_command_pending = 1;
		modem_hup(m, 1);
		/* Keep media and the physical hook owned until the bounded LAPM and
		 * datapump cleardown sequence completes, then acknowledge H with OK
		 * rather than the unsolicited NO CARRIER result (V.250 6.3.6). */
		return 1;
	}
	if (m->hook == hook_state)
		return 0;
        return modem_set_hook(m,hook_state);
}

int modem_online(struct modem *m, int retrain)
{
	char warning[96];
	int warning_length;

	MODEM_DBG("modem online...\n");
	if (m == NULL || (retrain != 0 && retrain != 1) ||
	    m->state != STATE_COMMAND_ONLINE)
                return -1;
	if (retrain != 0) {
		if (m->dp == NULL || m->dp->op == NULL)
			return -1;
		if (m->dp->op->retrain == NULL) {
			warning_length = snprintf(
				warning, sizeof(warning),
				"WARNING: Retrain unavailable for DP %d; "
				"resuming online",
				m->dp->id);
			if (warning_length > 0) {
				if ((size_t)warning_length >= sizeof(warning))
					warning_length = (int)sizeof(warning) - 1;
				(void)modem_put_chars(m, warning, warning_length);
				(void)modem_put_chars(m, CRLF_CHARS(m), 2);
			}
			retrain = 0;
		} else if (m->dp->op->retrain(m->dp) != 0) {
			return -1;
		}
	}
	m->retrain_pending = retrain != 0;
	m->command = 0;
        modem_set_state(m, STATE_MODEM_ONLINE);
	if (retrain == 0)
		modem_report_result(m,RESULT_CONNECT); // fixme
        return 0;
}

void modem_update_speaker(struct modem *m)
{
	//MODEM_DBG("modem update speaker...\n");
	m->driver.ioctl(m,MDMCTL_SPEAKERVOL,
			SPEAKER_CONTROL(m) == 2 ? SPEAKER_VOLUME(m):0);
}

int modem_get_sreg(struct modem *m, unsigned int num)
{
	if (num >= sizeof(m->sregs))
		return -1;
	return m->sregs[num];
}

int modem_set_sreg(struct modem *m, unsigned int num, int val)
{
	if (num >= sizeof(m->sregs) || val < 0 || val > UCHAR_MAX)
		return -1;
	/* V.250 6.2.1..6.2.3: command interface characters are IA5 values. */
	if ((num == SREG_CR_CHAR || num == SREG_LF_CHAR || num == SREG_BS_CHAR) && val > 127)
		return -1;
	if (num == SREG_DS0_FRAMING && val > 1)
		return -1;
	m->sregs[num] = val;
	return 0;
}

/* homolog set initialization */
int modem_homolog_init(struct modem *m, int id, const char *name)
{
	const struct homolog_set *set;
	for(set=homolog_set;set->name;set++) {
		if(set->id == id ||
		   (name && *name && !strcmp(name,set->name))) {
			m->homolog = set;
			modem_set_sreg(m,SREG_DIAL_PAUSE_TIME,
				       set->dial_pause_seconds);
			modem_set_sreg(m,SREG_FLASH_TIMER,
				       set->hook_flash_10ms);
			return 0;
		}
	}
	return -1;
}


int  modem_set_mode(struct modem *m, enum MODEM_MODE mode)
{
	MODEM_DBG("modem set mode: -> %d...\n", mode);
	if (m->mode == mode)
		return 0;
	m->mode = mode;
	return 0;
}


int modem_reset(struct modem *m)
{
	MODEM_DBG("modem reset...\n");
	m->hangup_command_pending = 0;
	/* Reset invalidates the data-link configuration immediately. It cannot
	 * leave an asynchronous disconnect using that configuration behind. */
	if(m->state != STATE_MODEM_IDLE || m->started || m->call_report.active) {
		modem_note_disconnect(m, true, "AT reset");
		modem_stop(m);
	}
	else if(m->hook)
		modem_set_hook(m,MODEM_HOOK_ON);
	modem_set_state(m, STATE_MODEM_IDLE);
	m->command = 1;
	return modem_factory_defaults(m);
}


/*
 *    Init functions
 *
 */

/* set default init values */
int modem_factory_defaults(struct modem *m)
{
	if (m == NULL)
		return -1;
	unsigned char *sregs = m->sregs;
	memset(sregs, 0, sizeof(m->sregs));
	m->min_rate = 300;
	m->max_rate = MODEM_MAX_RATE;
	m->v90_upstream_min_rate = 0;
	m->v90_upstream_max_rate = 0;
	m->v90_upstream_max_baud = 0;
	m->v92_upstream_lu_ucode = 0;
	m->v34_symbol_rate = 0;
        sregs[SREG_ESCAPE_CHAR]                  = '+' ; /* escape char */
        sregs[SREG_CR_CHAR]                      = '\r'; /* cr char */
        sregs[SREG_LF_CHAR]                      = '\n'; /* lf char */
        sregs[SREG_BS_CHAR]                      = '\b'; /* bs char */
        sregs[SREG_DIAL_TONE_WAIT_TIME]          =    2; /* seconds */
        sregs[SREG_WAIT_CARRIER_AFTER_DIAL]      =   60; /* seconds */
        sregs[SREG_DIAL_PAUSE_TIME]              =    2; /* seconds */
        sregs[SREG_CARRIER_DETECT_RESPONSE_TIME] =    6; /* 0.1 sec */
        sregs[SREG_CARRIER_LOSS_DISCONNECT_TIME] =    7; /* 0.1 sec */
        sregs[SREG_DTMF_DURATION]                =  100; /* ms */
        sregs[SREG_ESCAPE_PROMPT_DELAY]          =   50; /* ms */
        sregs[SREG_FLASH_TIMER]                  =   20; /* 10ms */
        sregs[SREG_ECHO]                         =    1; /* yes */
        sregs[SREG_QUIET]                        =    0; /* no  */
        sregs[SREG_VERBOSE]                      =    1; /* yes */
        sregs[SREG_TONE_OR_PULSE]                =    1; /* tone */

        sregs[SREG_X_CODE]                       =    4;

        sregs[SREG_SPEAKER_CONTROL] =  1; /* yes */
        sregs[SREG_SPEAKER_VOLUME]  =  3; /* max */
        sregs[SREG_AUTOMODE]        =  1;
        sregs[SREG_DP]              =  DP_V92;

        sregs[SREG_ANS_DELAY]       =  2; /* seconds */

        sregs[SREG_LINE_QUALITY_CONTROL]   = 0;
        sregs[SREG_CD]                     = 0;
        sregs[SREG_FLOW_CONTROL]           = 0;
        sregs[SREG_CONNNECT_MSG_FORMAT]    = 0;
        sregs[SREG_CONNNECT_MSG_SPEED_SRC] = 0;

	/* new sregs */
	sregs[SREG_EC]   = 1;
	sregs[SREG_COMP] = 0x3;
	sregs[SREG_COMP_TYPES] = MODEM_COMP_TYPE_DEFAULT;
	sregs[SREG_V90_DIGITAL_INDICATION] = MODEM_PCM_POLICY_AUTO;
	sregs[SREG_V91_ENABLED] = 1;
	sregs[SREG_DS0_FRAMING] = 0;
	sregs[SREG_V92_QUICK_CONNECT] = 0;
	sregs[SREG_V92_PCM_UPSTREAM] = 0;
	sregs[SREG_V92_PCM_UPSTREAM_MAX_DRN] = 0;

	if (modem_homolog_init(m, m->homolog != NULL ? m->homolog->id :
	    MODEM_DEFAULT_COUNTRY_CODE, NULL) != 0)
		return -1;
	return modem_set_mode(m, MODEM_MODE_DATA);
}



/* ---------------------------------------------------------------- */

void modem_hangup(struct modem *m)
{
	modem_hup(m,1);
}

int modem_stop_now(struct modem *m)
{
	int result;

	if (m == NULL)
		return -1;
	result = modem_stop(m);
	/* modem_stop has already released every runtime object even when its
	 * driver reports an error. Do not leave freed state marked runnable. */
	m->started = 0;
	return result;
}

int modem_report_remote_call_end(struct modem *m)
{
	if (m == NULL)
		return -1;
	if (!IS_STATE_IDLE(m->state) || m->call_report.active)
		modem_note_disconnect(m, false, "remote SIP call ended");
	if (IS_STATE_IDLE(m->state) && m->call_report.active) {
		modem_finish_call_report(m, false, "remote SIP call ended", RESULT_NOCARRIER);
	}
	TOTAL_RINGS_COUNT(m) = 0;
	m->ring_count = 0;
	modem_hup(m, 0);
	return run_pending_modem_stop(m);
}


int modem_data_write(struct modem *m, const void *input, size_t length)
{
	if (m == NULL || (length != 0 && input == NULL) || length > INT_MAX ||
	    !IS_STATE_ONLINE(m->state) || m->command || m->xmit.buf == NULL ||
	    m->xmit.size == 0 || m->xmit.head >= m->xmit.size || m->xmit.count < 0 ||
	    (unsigned int)m->xmit.count > m->xmit.size)
		return -1;
	const unsigned char *bytes = input;
	size_t count = length;
	if (count > m->xmit.size - (unsigned int)m->xmit.count)
		count = m->xmit.size - (unsigned int)m->xmit.count;
	if (count == 0)
		return 0;
	size_t first = count < m->xmit.size - m->xmit.head ? count : m->xmit.size - m->xmit.head;
	memcpy(m->xmit.buf + m->xmit.head, bytes, first);
	memcpy(m->xmit.buf, bytes + first, count - first);
	m->xmit.head = (m->xmit.head + count) % m->xmit.size;
	m->xmit.count += (int)count;
	return (int)count;
}

int modem_set_dte_command(struct modem *m, bool command)
{
	if (m == NULL || !IS_STATE_ONLINE(m->state) || m->xmit.count != 0)
		return -1;
	return modem_set_state(m, command ? STATE_COMMAND_ONLINE : STATE_MODEM_ONLINE);
}

void modem_schedule_escape(struct modem *m)
{
	schedule_event(m, MDMEVENT_ESCAPE, ESCAPE_TIMEOUT(m));
}

struct modem *modem_create(struct modem_driver *drv, const char *name)
{
	struct modem *m;

	m = malloc(sizeof(*m));
	if (!m)
		return NULL;
	memset(m,0,sizeof(*m));
	if (dc_remover_init(&m->receive_dc) != DC_REMOVER_OK) {
		free(m);
		return NULL;
	}

	m->name = name;
	m->pty = -1;
	m->driver = *drv;
        m->modem_info = 0;
	m->state  = STATE_MODEM_IDLE;
	m->command= 1;
        m->hook   = MODEM_HOOK_ON;
        m->caller = 0;

	m->modem_info |= (TIOCM_DSR|TIOCM_CTS);

	if (modem_factory_defaults(m) != 0) {
		MODEM_ERR("bad default country code `%x'!\n",
			  MODEM_DEFAULT_COUNTRY_CODE);
		free(m);
		return NULL;
	}

	m->ring_last = get_time();

	/* setup config */
	modem_setup_config(m);
	/* packer initializations */
	// fixme: TBD

        /* dp initialization */
        m->format = MODEM_FORMAT;
        m->srate  = MODEM_RATE;
        m->frag   = MODEM_FRAG; /* in samples */
        m->dp = NULL;

	MODEM_DBG("startup modem...\n");
	m->xmit.buf = malloc(XMIT_SIZE);
	if ( !m->xmit.buf ) {
		free(m);
		return NULL;
	}
	m->xmit.size = XMIT_SIZE;
	m->xmit.head = m->xmit.tail = m->xmit.count = 0;
	m->modem_info |= TIOCM_DTR|TIOCM_RTS;
	// TODO: update speed,DTR according to termios

	return m;
}



void modem_delete(struct modem *m)
{
	MODEM_DBG("modem_delete...\n");
	if(m->started || m->dp) {
		MODEM_DBG("shutdown modem...\n");
		if(m->state != STATE_MODEM_IDLE)
			m->result_code = RESULT_NOCARRIER;
		modem_stop(m);		
	}
	/* Link allocations can exist before the device or datapump starts. */
	modem_ec_exit(m);
	modem_comp_exit(m);
	m->xmit.size = 0;
	m->xmit.count = m->xmit.tail = m->xmit.head = 0;
	free(m->xmit.buf);
	m->xmit.buf = 0;

	free(m);
}
