// SPDX-License-Identifier: BSD-3-Clause
#include "ds0/ds0_datapump.h"
#include "hdlc_fcs.h"
#include "modem.h"
#include "modem_state.h"
#include "test.h"
#include <string.h>
#include <sys/ioctl.h>

extern int process_at_command(struct modem *, char *);
struct capture { uint8_t bytes[4096]; size_t count; };
static int sink(void *context, const char *bytes, int count)
{
	struct capture *c = context;
	size_t n = count < 0 ? 0 : (size_t)count;
	if (n > sizeof(c->bytes) - c->count) n = sizeof(c->bytes) - c->count;
	memcpy(c->bytes + c->count, bytes, n); c->count += n; return (int)n;
}
static int start(struct modem *m) { (void)m; return 0; }
static int stop(struct modem *m) { (void)m; return 0; }
static int control(struct modem *m, unsigned cmd, unsigned long value)
{ (void)m; (void)cmd; (void)value; return 0; }
static struct modem *make(struct capture *rx, bool lapm, unsigned rate)
{
	struct modem_driver driver = { "DS0 test", start, stop, control };
	struct modem *m = modem_create(&driver, "DS0 test");
	if (m == NULL) return NULL;
	m->data_output = sink; m->data_output_context = rx;
	m->sregs[SREG_DP] = DP_DS0; m->sregs[SREG_DS0_FRAMING] = lapm;
	/* Explicit DS0 framing overrides the stored raw policy per call. */
	m->sregs[SREG_EC] = 0;
	m->sregs[SREG_ANS_DELAY] = 0;
	if (rate) m->min_rate = m->max_rate = rate;
	return m;
}
static size_t async_frame(const uint8_t *body, size_t length, uint8_t *out)
{
	size_t n = 0; out[n++] = 0x7e;
	uint16_t fcs = hdlc_fcs16(body, length) ^ 0xffffu;
	for (size_t i = 0; i < length + 2; ++i) {
		uint8_t b = i < length ? body[i] : i == length ? fcs & 0xff : fcs >> 8;
		if (b < 0x20 || b == 0x7e || b == 0x7d) { out[n++] = 0x7d; out[n++] = b ^ 0x20; }
		else out[n++] = b;
	}
	out[n++] = 0x7e; return n;
}
static bool tick(struct modem *a, struct modem *b, uint8_t *previous, enum g711_law law)
{
	int16_t rx[MODEM_FRAG], tx[MODEM_FRAG]; uint8_t wire[MODEM_FRAG];
	bool available[MODEM_FRAG], exact = false;
	for (unsigned i = 0; i < MODEM_FRAG; ++i) { rx[i] = g711_decode(law, previous[i]); available[i] = true; }
	modem_process_g711(a, law, previous, rx, available, wire, tx, MODEM_FRAG, &exact);
	for (unsigned i = 0; i < MODEM_FRAG; ++i) {
		if (!exact) wire[i] = g711_encode(law, tx[i]);
		rx[i] = g711_decode(law, wire[i]);
	}
	modem_process_g711(b, law, wire, rx, available, previous, tx, MODEM_FRAG, &exact);
	if (!exact) for (unsigned i = 0; i < MODEM_FRAG; ++i) previous[i] = g711_encode(law, tx[i]);
	CHECK(a->started && b->started, "unexpected disconnect: A=%s B=%s", a->disconnect_reason, b->disconnect_reason);
	return true;
}
static bool pair(bool lapm, unsigned rate, bool automatic, enum g711_law law)
{
	struct capture a_rx = { 0 }, b_rx = { 0 };
	struct modem *a = make(&a_rx, lapm, rate), *b = make(&b_rx, lapm, automatic ? 0 : rate);
	CHECK(a != NULL && b != NULL, "create");
	CHECK(modem_answer(b) == 0 && modem_dial(a) == 0 && modem_report_outgoing_call_connected(a) == 0, "start");
	uint8_t previous[MODEM_FRAG]; memset(previous, 0x7e, sizeof(previous));
	const uint8_t body[] = { 0xff, 3, 0xc0, 0x21, 1, 1, 0, 4, '+', '+', '+', 0x11, 0x13 };
	uint8_t payload[64]; size_t n;
	if (lapm) { n = sizeof(body); memcpy(payload, body, n); }
	else n = async_frame(body, sizeof(body), payload);
	unsigned next_bootstrap = 0;
	for (unsigned i = 0; i < 4000 && (!(a->state == STATE_MODEM_ONLINE || a->state == STATE_COMMAND_ONLINE) || !(b->state == STATE_MODEM_ONLINE || b->state == STATE_COMMAND_ONLINE) || (!lapm && !a_rx.count)); ++i) {
		CHECK(tick(a, b, previous, law), "training tick");
		if (!lapm && (b->state == STATE_MODEM_ONLINE || b->state == STATE_COMMAND_ONLINE) && i >= next_bootstrap) {
			CHECK(modem_data_write(b, payload, n) == (int)n, "PPP bootstrap"); next_bootstrap = i + 3u * MODEM_RATE / MODEM_FRAG;
		}
	}
	CHECK((a->state == STATE_MODEM_ONLINE || a->state == STATE_COMMAND_ONLINE) && (b->state == STATE_MODEM_ONLINE || b->state == STATE_COMMAND_ONLINE), "connection lapm=%d rate=%u auto=%d state=%u/%u", lapm, rate, automatic, a->state, b->state);
	CHECK(a->dp->id == DP_DS0 && b->dp->id == DP_DS0 && a->tx_rate == rate && b->tx_rate == rate && a->rx_rate == rate && b->rx_rate == rate, "selected rates");
	CHECK(a->cfg.ec == lapm && b->cfg.ec == lapm && (lapm || (!a->cfg.comp && !b->cfg.comp)), "framing policy lapm=%d EC=%d/%d comp=%d/%d", lapm,a->cfg.ec,b->cfg.ec,a->cfg.comp,b->cfg.comp);
	CHECK(modem_ec_active_compression(a) == (lapm ? MODEM_EC_COMPRESSION_V44 : MODEM_EC_COMPRESSION_NONE) &&
	      modem_ec_active_compression(b) == (lapm ? MODEM_EC_COMPRESSION_V44 : MODEM_EC_COMPRESSION_NONE),
	      "per-call compression overrides stored raw policy");
	CHECK(modem_data_write(a, payload, n) == (int)n, "send A");
	if (lapm) { CHECK(modem_data_write(b, payload, n) == (int)n, "send B"); }
	for (unsigned i = 0; i < 4000 && (a_rx.count < n || b_rx.count < n); ++i) CHECK(tick(a, b, previous, law), "data tick");
	CHECK(a_rx.count == n && b_rx.count == n && memcmp(a_rx.bytes, payload, n) == 0 && memcmp(b_rx.bytes, payload, n) == 0, "literal bidirectional payload");
	struct dp_link_rates rates = a->dp->op->link_rates(a->dp);
	CHECK(rates.transmit_baud == 8000 && rates.receive_baud == 8000, "codeword clocks");
	CHECK(modem_set_dte_command(a, true) == 0, "command mode");
	CHECK(modem_data_write(b, payload, n) == (int)n, "send in peer command mode");
	for (unsigned i = 0; i < 4000 && a_rx.count < 2 * n; ++i) CHECK(tick(a, b, previous, law), "command receive tick");
	CHECK(a_rx.count == 2 * n && memcmp(a_rx.bytes + n, payload, n) == 0 && (a->modem_info & TIOCM_CD), "command carrier and RX");
	CHECK(modem_set_dte_command(a, false) == 0 && !a->retrain_pending, "resume");
	CHECK(modem_data_write(a, payload, n) == (int)n &&
	      modem_finish_transmit(a) == 1 && modem_data_write(a, payload, n) == 0,
	      "DS0 final message seal");
	for (unsigned i = 0; i < 4000 &&
	     (modem_finish_transmit(a) != 0 || b_rx.count < 2 * n); ++i)
		CHECK(tick(a, b, previous, law), "DS0 completion tick");
	CHECK(modem_finish_transmit(a) == 0 && b_rx.count == 2 * n &&
	      !memcmp(b_rx.bytes + n, payload, n), "DS0 final message completion");
	CHECK(modem_report_remote_call_end(a) == 0 && !a->started && a->dp == NULL && a->disconnect_reason && !a->disconnect_local, "carrier loss");
	modem_delete(a); modem_delete(b); return true;
}

static bool passive_ppp_server(void)
{
	struct capture a_rx = {0}, b_rx = {0};
	struct modem *a = make(&a_rx, false, 0), *b = make(&b_rx, false, 56000);
	CHECK(a && b && modem_answer(b) == 0 && modem_dial(a) == 0 &&
	      modem_report_outgoing_call_connected(a) == 0, "passive start");
	uint8_t previous[MODEM_FRAG]; memset(previous, 0x7e, sizeof(previous));
	const uint8_t body[] = {0xff, 3, 0xc0, 0x21, 1, 7, 0, 4};
	uint8_t payload[32]; size_t n = async_frame(body, sizeof(body), payload);
	unsigned next_send = 0; bool replied = false;
	for (unsigned i = 0; i < 15u * MODEM_RATE / MODEM_FRAG && !a_rx.count; ++i) {
		CHECK(tick(a, b, previous, G711_LAW_PCMU), "passive tick");
		if (a->state == STATE_MODEM_ONLINE && i >= next_send && !b_rx.count) {
			CHECK(modem_data_write(a, payload, n) == (int)n, "client LCP");
			next_send = i + 3u * MODEM_RATE / MODEM_FRAG;
		}
		if (b_rx.count && !replied) {
			CHECK(modem_data_write(b, payload, n) == (int)n, "passive LCP response"); replied = true;
		}
	}
	CHECK(replied && a_rx.count == n && b_rx.count == n && !memcmp(a_rx.bytes,payload,n) &&
	      !memcmp(b_rx.bytes,payload,n), "passive PPP bootstrap preserved");
	CHECK(a->tx_rate == 56000 && a->rx_rate == 56000, "originator automatic fallback");
	modem_delete(a); modem_delete(b); return true;
}
static bool transmit_overflow_disconnect(void)
{
	struct capture a_rx = {0}, b_rx = {0};
	struct modem *a = make(&a_rx, false, 64000), *b = make(&b_rx, false, 64000);
	CHECK(a && b && modem_answer(b) == 0 && modem_dial(a) == 0 &&
	      modem_report_outgoing_call_connected(a) == 0, "overflow start");
	uint8_t previous[MODEM_FRAG]; memset(previous, 0x7e, sizeof(previous));
	CHECK(tick(a, b, previous, G711_LAW_PCMU), "bearer readiness");
	uint8_t body[2049], payload[4110]; memset(body, 0x55, sizeof(body));
	size_t n = async_frame(body, sizeof(body), payload);
	CHECK(modem_data_write(a, payload, n) == (int)n, "oversize accepted into local queue");
	int16_t rx[MODEM_FRAG] = {0}, tx[MODEM_FRAG]; uint8_t wire[MODEM_FRAG]; bool exact;
	for (unsigned i = 0; i < 64 && !a->disconnect_reason; ++i) {
		modem_process_g711(a, G711_LAW_PCMU, previous, rx, NULL, wire, tx, MODEM_FRAG, &exact);
	}
	CHECK(a->disconnect_local && a->disconnect_reason &&
	      strcmp(a->disconnect_reason, "DS0 PPP frame overflow") == 0,
	      "explicit local overflow cause");
	modem_delete(a); modem_delete(b); return true;
}
static bool at_contract(void)
{
	struct capture rx = { 0 }; struct modem *m = make(&rx, false, 0);
	CHECK(m != NULL && m->sregs[SREG_DS0_FRAMING] == 0, "default PPP");
	CHECK(process_at_command(m, "AT+DS0=1+MS=64,0,56000,56000") == 0 && m->sregs[SREG_DS0_FRAMING] == 1 && m->min_rate == 56000, "selectors");
	CHECK(process_at_command(m, "AT+DS0=2") == -1 && m->sregs[SREG_DS0_FRAMING] == 1, "framing bound");
	CHECK(process_at_command(m, "AT+MS=64,0,57000,63000") == -1 && m->min_rate == 56000, "rate domain");
	CHECK(modem_set_sreg(m, SREG_DS0_FRAMING, 2) == -1, "direct register bound");
	CHECK(process_at_command(m, "ATZ") == 0 && m->sregs[SREG_DS0_FRAMING] == 0 && m->sregs[SREG_DP] == DP_V92, "reset");
	modem_delete(m); return true;
}

static bool automatic_startup(bool answerer_auto, bool lapm, unsigned rate, enum g711_law law)
{
	struct capture a_rx = {0}, b_rx = {0};
	struct modem *a = make(&a_rx, lapm, rate), *b = make(&b_rx, lapm, rate);
	CHECK(a && b, "automatic fixture");
	struct modem *automatic = answerer_auto ? b : a;
	struct modem *peer = answerer_auto ? a : b;
	automatic->sregs[SREG_DP] = DP_V92;
	automatic->sregs[SREG_AUTOMODE] = 1;
	/* Stored raw policy must also initialize detected LAPM/compression. */
	automatic->sregs[SREG_EC] = 0;
	automatic->min_rate = MODEM_MIN_RATE; automatic->max_rate = MODEM_MAX_RATE;
	CHECK(modem_answer(b) == 0 && modem_dial(a) == 0 &&
	      modem_report_outgoing_call_connected(a) == 0, "automatic start");
	/* 20-ms media crosses four DSP fragments, including V.8 -> DS0. */
	uint8_t previous[160], wire[160]; int16_t rx[160], tx[160]; bool exact;
	memset(previous, 0x7e, sizeof(previous));
	const uint8_t body[] = {0xff,3,0xc0,0x21,1,3,0,4,0x7f,'+','+','+',0x11,0x13};
	uint8_t payload[64]; size_t n = lapm ? sizeof(body) : async_frame(body,sizeof(body),payload);
	if (lapm) memcpy(payload,body,n);
	bool sent = false;
	for (unsigned tick = 0; tick < 500 &&
	     (automatic->dp == NULL || automatic->dp->id != DP_DS0 ||
	      automatic->state != STATE_MODEM_ONLINE || peer->state != STATE_MODEM_ONLINE ||
	      (!lapm && !(answerer_auto ? b_rx.count : a_rx.count))); ++tick) {
		for (unsigned i=0;i<160;++i) rx[i]=g711_decode(law,previous[i]);
		modem_process_g711(a,law,previous,rx,NULL,wire,tx,160,&exact);
		if (!exact) for(unsigned i=0;i<160;++i) wire[i]=g711_encode(law,tx[i]);
		for(unsigned i=0;i<160;++i) rx[i]=g711_decode(law,wire[i]);
		modem_process_g711(b,law,wire,rx,NULL,previous,tx,160,&exact);
		if (!exact) for(unsigned i=0;i<160;++i) previous[i]=g711_encode(law,tx[i]);
		CHECK(a->started && b->started, "automatic disconnect A=%s B=%s",a->disconnect_reason,b->disconnect_reason);
		if (!lapm && !sent && peer->state == STATE_MODEM_ONLINE) {
			CHECK(modem_data_write(peer,payload,n)==(int)n &&
			      modem_data_write(peer,payload,n)==(int)n,"two proving PPP frames"); sent=true;
		}
	}
	CHECK(automatic->dp && automatic->dp->id == DP_DS0 && automatic->state==STATE_MODEM_ONLINE &&
	      peer->state==STATE_MODEM_ONLINE && automatic->tx_rate==rate && automatic->rx_rate==rate,
	      "autodetect role=%d lapm=%d rate=%u law=%d selected=%d states=%d/%d",
	      answerer_auto,lapm,rate,law,automatic->dp?automatic->dp->id:0,automatic->state,peer->state);
	CHECK(automatic->sregs[SREG_DP]==DP_V92 && automatic->sregs[SREG_EC]==0 && automatic->cfg.ec==lapm,
	      "stored modulation/raw policy retained and actual framing applied");
	CHECK(modem_ec_active_compression(automatic) ==
	      (lapm ? MODEM_EC_COMPRESSION_V44 : MODEM_EC_COMPRESSION_NONE),
	      "detected LAPM initializes the default compression offer");
	if (!lapm) {
		struct capture *received = answerer_auto ? &b_rx : &a_rx;
		CHECK(received->count==2*n && !memcmp(received->bytes,payload,n) &&
		      !memcmp(received->bytes+n,payload,n), "two proving frames preserved");
	}
	CHECK(modem_data_write(automatic,payload,n)==(int)n,"post-handoff send");
	for(unsigned i=0;i<4000 && (answerer_auto?a_rx.count:b_rx.count)<n;++i)
		CHECK(tick(a,b,previous,law),"post-handoff data");
	struct capture *received = answerer_auto ? &a_rx : &b_rx;
	CHECK(received->count==n && !memcmp(received->bytes,payload,n), "post-handoff exact payload");
	modem_delete(a); modem_delete(b); return true;
}
int main(void)
{
	if (!at_contract() || !passive_ppp_server() || !transmit_overflow_disconnect()) return 1;
	for (unsigned role=0;role<2;++role)
		for(unsigned lapm=0;lapm<2;++lapm)
			for(unsigned rate=56000;rate<=64000;rate+=8000)
				for(unsigned law=0;law<2;++law)
					/* A LAPM answerer waits for the caller XID: no frame proof. */
					if((!lapm || role) && !automatic_startup(role,lapm,rate,law)) return 1;
	for (unsigned lapm = 0; lapm <= 1; ++lapm)
		for (unsigned rate = 56000; rate <= 64000; rate += 8000)
			for (unsigned law = 0; law <= 1; ++law)
				if (!pair(lapm, rate, true, law)) return 1;
	puts("DS0 native: PPP/LAPM, 56/64, PCMU/PCMA, automatic selection, command mode and carrier loss passed");
	return 0;
}
