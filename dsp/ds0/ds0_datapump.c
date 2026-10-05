// SPDX-License-Identifier: BSD-3-Clause
#include "ds0_datapump.h"
#include "ds0_ppp.h"
#include "datapump_link.h"
#include "modem_state.h"
#include <stdlib.h>
#include <string.h>

struct ds0_datapump {
	struct dp dp;
	struct ds0_ppp ppp;
	bool lapm, connected, closing, gap;
	size_t input_pos, input_len;
	uint8_t input[512];
	uint8_t *startup;
	size_t startup_count;
};
static const struct dp_operations operations;

static struct ds0_datapump *owner(const struct dp *dp)
{
	if (dp == NULL || dp->id != DP_DS0 || dp->op != &operations || dp->dp_data == NULL)
		return NULL;
	struct ds0_datapump *pump = dp->dp_data;
	return &pump->dp == dp ? pump : NULL;
}

static struct dp *create(struct modem *m, enum DP_ID id, int caller,
	int rate, int fragment, const struct dp_operations *ops)
{
	bool allow56 = m != NULL && m->min_rate <= 56000 && m->max_rate >= 56000;
	bool allow64 = m != NULL && m->min_rate <= 64000 && m->max_rate >= 64000;
	if (m == NULL || id != DP_DS0 || ops != &operations ||
	    (caller != 0 && caller != 1) || rate != MODEM_RATE ||
	    fragment != MODEM_FRAG || (!allow56 && !allow64))
		return NULL;
	struct ds0_datapump *pump = calloc(1, sizeof(*pump));
	if (pump == NULL) return NULL;
	pump->dp = (struct dp){ .id = id, .modem = m, .op = ops, .dp_data = pump };
	pump->lapm = modem_get_sreg(m, SREG_DS0_FRAMING) != 0;
	struct ds0_config cfg; ds0_config_default(&cfg);
	cfg.rate_policy = allow56 && allow64 ? DS0_RATE_AUTO :
		allow56 ? DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
	/* LAPM owns smaller bounded HDLC frames, without PPP conversion. */
	if (pump->lapm) cfg.rx_frame_limit = sizeof(m->packer.hdlc._rx_buf) - 2;
	if (ds0_ppp_init(&pump->ppp, &cfg) != 0) { free(pump); return NULL; }
	return &pump->dp;
}

static int destroy(struct dp *dp)
{
	struct ds0_datapump *pump = owner(dp);
	if (pump == NULL) return -1;
	free(pump->startup);
	free(pump); return 0;
}
static int hangup(struct dp *dp)
{
	struct ds0_datapump *pump = owner(dp);
	if (pump == NULL) return -1;
	pump->closing = true; return 0;
}
static struct dp_link_rates link_rates(const struct dp *dp)
{
	struct ds0_datapump *pump = owner(dp);
	if (pump == NULL || !pump->connected) return (struct dp_link_rates){ 0 };
	return (struct dp_link_rates){ .transmit_bps = pump->ppp.codec.tx_rate,
		.receive_bps = pump->ppp.codec.selected_rate,
		.transmit_baud = 8000, .receive_baud = 8000 };
}
static const char *progress(const struct dp *dp)
{
	struct ds0_datapump *pump = owner(dp);
	if (pump == NULL) return "DS0/invalid";
	if (pump->closing) return "DS0/cleardown";
	if (!pump->connected) return "DS0/wait-valid-HDLC";
	return pump->lapm ? "DS0/LAPM" : "DS0/PPP";
}

static void connect(struct ds0_datapump *pump)
{
	if (pump->connected) return;
	pump->connected = true;
	pump->dp.status = DPSTAT_CONNECT;
	datapump_set_link_rates(&pump->dp, pump->ppp.codec.tx_rate, pump->ppp.codec.selected_rate);
	modem_dp_connected(pump->dp.modem);
}

static void flush(struct ds0_datapump *pump)
{
	struct ds0_ppp *p = &pump->ppp;
	if (!pump->connected || !p->rx_async_pending_len) return;
	struct modem *m = pump->dp.modem;
	if (m->put_chars == NULL) return;
	size_t left = p->rx_async_pending_len - p->rx_async_pending_pos;
	int accepted = m->put_chars(m, (const char *)p->rx_async_pending + p->rx_async_pending_pos, (int)left);
	if (accepted > 0 && (size_t)accepted <= left) {
		p->rx_async_pending_pos += accepted;
		if (p->rx_async_pending_pos == p->rx_async_pending_len)
			p->rx_async_pending_pos = p->rx_async_pending_len = 0;
	}
}

static void received(void *context, const uint8_t *bytes, size_t count, enum ds0_rate rate)
{
	struct ds0_datapump *pump = context;
	struct modem *m = pump->dp.modem;
	connect(pump);
	if (pump->lapm) {
		/* Reuse the normal LAPM frame receiver. DS0's detector has already
		 * checked FCS and removed stuffing; no second protocol machine. */
		struct hdlc_state *h = &m->packer.hdlc;
		if (h->rx_frame == NULL || h->rx_complete == NULL || count > (size_t)h->rx_frame->size)
			return;
		memcpy(h->rx_frame->buf, bytes, count);
		h->rx_frame->count = (int)count;
		h->rx_complete(h->framer, h->rx_frame);
		h->rx_frame->count = 0;
	} else {
		flush(pump);
		ds0_ppp_receive_frame(&pump->ppp, bytes, count, rate);
		flush(pump);
	}
}

static void transmit_ppp(struct ds0_datapump *pump, uint8_t *out, int count)
{
	struct modem *m = pump->dp.modem;
	size_t produced = 0, consumed = 0;
	/* Bound both conversion and DTE refills even for a stream of invalid
	 * frames. Do not add an idle packet for every 512-byte input chunk. */
	while (produced < (size_t)count && consumed < DS0_MAX_ASYNC_FRAME_OCTETS && !pump->ppp.overflow) {
		if (pump->input_pos == pump->input_len) {
			pump->input_pos = pump->input_len = 0;
			if (pump->connected && m->get_chars != NULL) {
				int n = m->get_chars(m, (char *)pump->input, sizeof(pump->input));
				if (n > 0) pump->input_len = (size_t)n;
			}
		}
		size_t available = pump->input_len - pump->input_pos;
		if (available > DS0_MAX_ASYNC_FRAME_OCTETS - consumed)
			available = DS0_MAX_ASYNC_FRAME_OCTETS - consumed;
		size_t used = 0;
		size_t written = ds0_ppp_encode(&pump->ppp, pump->input + pump->input_pos,
			available, out + produced, (size_t)count - produced, &used);
		pump->input_pos += used; consumed += used; produced += written;
		if (used == 0 && written == 0) break;
	}
	if (produced < (size_t)count)
		ds0_tx_idle(&pump->ppp.codec, out + produced, (size_t)count - produced);
}

static int process_g711(struct dp *dp, enum g711_law law, const uint8_t *input,
	void *rx_samples, const bool *available, uint8_t *output,
	void *tx_samples, int count, bool *exact)
{
	(void)rx_samples;
	struct ds0_datapump *pump = owner(dp);
	if (pump == NULL || input == NULL || output == NULL || tx_samples == NULL ||
	    exact == NULL || count <= 0 || count > MODEM_FRAG ||
	    !g711_law_is_valid(law)) return DPSTAT_ERROR;
	struct modem *m = dp->modem;
	struct ds0 *codec = &pump->ppp.codec;
	uint32_t previous_overflow = codec->selected_rate == DS0_RATE_56000 ?
		codec->counters.rx56.overflow : codec->counters.rx64.overflow;
	bool rate_locked = codec->saw_valid_frame;
	*exact = false;
	/* Raw DS0 has no training handshake: let either PPP endpoint initiate
	 * LCP. LAPM still reports online only after its normal XID exchange. */
	connect(pump);
	if (pump->startup != NULL) {
		/* Bounded qualifying prefix, including the proving frame. Never
		 * reconstruct these codewords from their decoded audio samples. */
		ds0_rx_codec(codec, pump->startup, pump->startup_count, received, pump);
		free(pump->startup);
		pump->startup = NULL;
		pump->startup_count = 0;
	}
	for (int i = 0; i < count && !pump->closing; ++i) {
		bool missing = available != NULL && !available[i];
		if (missing) {
			if (!pump->gap) ds0_rx_gap(codec);
			ds0_advance_samples(codec, 1);
		} else ds0_rx_codec(codec, input + i, 1, received, pump);
		pump->gap = missing;
	}
	uint32_t current_overflow = codec->selected_rate == DS0_RATE_56000 ?
		codec->counters.rx56.overflow : codec->counters.rx64.overflow;
	if (!pump->lapm && rate_locked && current_overflow != previous_overflow)
		pump->ppp.overflow = true;
	if (pump->ppp.overflow) {
		modem_note_disconnect(m, true, "DS0 PPP frame overflow");
		memset(tx_samples, 0, (size_t)count * sizeof(int16_t));
		return DPSTAT_ERROR;
	}
	datapump_set_link_rates(dp, codec->tx_rate, codec->selected_rate);
	if (pump->closing) ds0_tx_idle(codec, output, count);
	else if (pump->lapm) {
		unsigned bits = codec->tx_rate == DS0_RATE_56000 ? 7 : 8;
		if (modem_get_bits(m, bits, output, count) != count) return DPSTAT_ERROR;
		if (bits == 7) for (int i = 0; i < count; ++i) output[i] <<= 1;
	} else { flush(pump); transmit_ppp(pump, output, count); }
	if (pump->ppp.overflow) {
		modem_note_disconnect(m, true, "DS0 PPP frame overflow");
		memset(tx_samples, 0, (size_t)count * sizeof(int16_t));
		return DPSTAT_ERROR;
	}
	int16_t *samples = tx_samples;
	for (int i = 0; i < count; ++i) samples[i] = g711_decode(law, output[i]);
	*exact = true;
	return pump->closing ? DPSTAT_DISCONNECT : DPSTAT_OK;
}
static int process(struct dp *dp, void *input, void *output, int count)
{
	(void)input;
	if (output != NULL && count > 0 && count <= MODEM_FRAG) memset(output, 0, (size_t)count * sizeof(int16_t));
	if (dp != NULL && dp->modem != NULL)
		modem_note_disconnect(dp->modem, true, "DS0 requires exact G.711 codewords");
	return DPSTAT_ERROR;
}
static const struct dp_operations operations = {
	.name = "DS0", .create = create, .delete = destroy, .process = process,
	.process_g711 = process_g711, .hangup = hangup,
	.progress = progress, .link_rates = link_rates,
};
const struct dp_operations *ds0_datapump_operations(enum DP_ID id)
{
	return id == DP_DS0 ? &operations : NULL;
}

struct dp *ds0_datapump_create_handoff(struct modem *m, int caller, const struct ds0_probe *probe)
{
	if (m == NULL || probe == NULL || probe->selected == DS0_RATE_NONE ||
	    (unsigned)probe->selected < m->min_rate || (unsigned)probe->selected > m->max_rate)
		return NULL;
	struct dp *dp = create(m, DP_DS0, caller, MODEM_RATE, MODEM_FRAG, &operations);
	if (dp == NULL) return NULL;
	struct ds0_datapump *pump = owner(dp);
	pump->startup = malloc(DS0_PROBE_HISTORY);
	if (pump->startup == NULL ||
	    (pump->startup_count = ds0_probe_copy(probe, pump->startup, DS0_PROBE_HISTORY)) == 0) {
		destroy(dp); return NULL;
	}
	pump->ppp.codec.cfg.rate_policy = probe->selected == DS0_RATE_56000 ?
		DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
	pump->ppp.codec.tx_rate = pump->ppp.codec.selected_rate = probe->selected;
	return dp;
}
