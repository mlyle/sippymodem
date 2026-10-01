
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
 *    modem_ec.c  --  modem error corrector (lapm like) implementation.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#include <modem.h>
#include <modem_debug.h>
#include <v42_xid.h>
#include <v44_character_path.h>

#include <limits.h>
#include <stdbool.h>

//#define EC_DEBUG 1

/* LAPM default parameters */
#define LAPM_DFLT_WIN_SIZE   15  /* default win size */
#define LAPM_DFLT_INFO_SIZE 128  /* default info size */

#define LAPM_MAX_RTX          5  /* max num of retransmissions (N400) */

enum {
	LAPM_TIMER_MIN_MS = 3000,
	LAPM_IDLE_TIMER_SECONDS = 10,
	LAPM_TIMER_RESPONSE_MARGIN_MS = 1000,
	LAPM_TIMER_FRAME_OVERHEAD_OCTETS = 7,
	LAPM_TIMER_STUFFED_BITS_PER_OCTET = 10,
	LAPM_TIMER_FALLBACK_BIT_RATE = 57600,
	/* Allow two unanswered status enquiries before physical recovery. */
	LAPM_RETRAIN_POLLS = 2,
};

/* The shared bit timer advances with both line directions. Express T401 in
 * elapsed time, and at slow rates leave room for a maximally stuffed frame
 * before the peer-response margin begins. */
static int lapm_timer_timeout(const struct lapm_state *l)
{
	uint64_t bits_per_second;
	uint64_t tx_rate;
	uint64_t info_octets;
	uint64_t frame_bits;
	uint64_t timeout_ms;
	uint64_t timeout;

	if (l == NULL || l->modem == NULL)
		return LAPM_TIMER_FALLBACK_BIT_RATE * LAPM_TIMER_MIN_MS / 1000;
	bits_per_second = (uint64_t)l->modem->rx_rate + l->modem->tx_rate;
	tx_rate = l->modem->tx_rate;
	info_octets = l->tx_info_size != 0 ? l->tx_info_size :
		LAPM_DFLT_INFO_SIZE;
	frame_bits = (info_octets + LAPM_TIMER_FRAME_OVERHEAD_OCTETS) *
		LAPM_TIMER_STUFFED_BITS_PER_OCTET;
	if (bits_per_second == 0) {
		bits_per_second = LAPM_TIMER_FALLBACK_BIT_RATE;
		tx_rate = LAPM_TIMER_FALLBACK_BIT_RATE / 2;
	} else if (tx_rate == 0) {
		tx_rate = bits_per_second;
	}
	timeout_ms = (frame_bits * 1000 + tx_rate - 1) / tx_rate +
		LAPM_TIMER_RESPONSE_MARGIN_MS;
	if (timeout_ms < LAPM_TIMER_MIN_MS)
		timeout_ms = LAPM_TIMER_MIN_MS;
	timeout = (bits_per_second * timeout_ms + 999) / 1000;
	return timeout > INT_MAX ? INT_MAX : (int)timeout;
}

/* LAPM definitions */

/* frame types */
#define FRAME_I            0x00
#define FRAME_S            0x01
#define FRAME_U            0x03

/* supervisory headers */
#define FRAME_S_RR	   0x01  /* cr */
#define FRAME_S_RNR	   0x05  /* cr */
#define FRAME_S_REJ	   0x09  /* cr */
#define FRAME_S_SREJ	   0x0d  /* cr */
/* unnumbered headers */
#define FRAME_U_SABME	   0x6f  /* c  */
#define FRAME_U_DM	   0x0f  /*  r */
#define FRAME_U_UI	   0x03  /* cr */
#define FRAME_U_DISC	   0x43  /* c  */
#define FRAME_U_UA	   0x63  /*  r */
#define FRAME_U_FRMR	   0x87  /*  r */
#define FRAME_U_XID	   0xaf  /* cr */
#define FRAME_U_TEST	   0xe3  /* c  */

/* useful macros */
#define FRAME_ADDR(f) ((f)->buf[0])
#define FRAME_CTRL(f) ((f)->buf[1])
#define FRAME_TYPE(f) ((f)->buf[1]&0x1 ? ((f)->buf[1]&0x3) : FRAME_I)
#define RX_IS_COMMAND(f) ((f)->buf[0] == l->rsp_addr)
#define TX_IS_COMMAND(f) ((f)->buf[0] == l->cmd_addr)
#define FRAME_I_PF(f) ((f)->buf[2]&0x1)
#define FRAME_S_PF(f) ((f)->buf[2]&0x1)
#define FRAME_U_PF(f) ((f)->buf[1]&0x10)
#define FRAME_NS(f)   ((f)->buf[1]>>1)
#define FRAME_NR(f)   ((f)->buf[2]>>1)

/* Budget the usual answerer reply at the peer's bit rate, including the
 * longest legal inter-character marks. Then add the local 16-flag preamble
 * (V.42 8.10.2). A fixed flag count can outrun ADP on asymmetric PCM calls. */
static unsigned lapm_originator_preamble_flags(const struct modem *m)
{
	uint64_t reply_bits = (V42_ANSWER_REPLY_PATTERNS -
		V42_ORIGINATOR_MATCH_PATTERNS) * V42_DETECTION_MAX_PATTERN_BITS +
		V42_DETECTION_MAX_MARK_BITS;
	uint64_t divisor = 8;
	uint64_t flags;

	/* Recognition ends at the second C character, before its trailing marks.
	 * Before rates are known, retain an equal-rate guard. */
	if (m->tx_rate != 0 && m->rx_rate != 0) {
		reply_bits *= m->tx_rate;
		divisor *= m->rx_rate;
	}
	flags = (reply_bits + divisor - 1) / divisor + V42_PROTOCOL_PREAMBLE_FLAGS;
	return flags > UINT_MAX ? UINT_MAX : (unsigned)flags;
}

/* tx unnumbered */
#define TX_SABME(l) tx_unnum(l,l->cmd_addr,FRAME_U_SABME|0x10,0,0)
#define TX_DISC(l)  tx_unnum(l,l->cmd_addr,FRAME_U_DISC|0x10,0,0)
#define TX_UA(l,pf_mask) tx_unnum(l,l->rsp_addr,FRAME_U_UA|pf_mask,0,0)
#define TX_DM(l,pf_mask) tx_unnum(l,l->rsp_addr,FRAME_U_DM|pf_mask,0,0)
/* tx supervisory */
#define TX_RR(l,addr,pf_mask)  tx_super(l,addr,FRAME_S_RR,pf_mask)
#define TX_RNR(l,addr,pf_mask) tx_super(l,addr,FRAME_S_RNR,pf_mask)
#define TX_REJ(l,addr,pf_mask) tx_super(l,addr,FRAME_S_REJ,pf_mask)

/* timers */
#define T401_START(l) 	{ EC_DBG("T401 start.\n");\
                          (l)->rtx_count = 0; \
			  (l)->modem->bit_timer = lapm_timer_timeout(l); \
			  (l)->modem->bit_timer_func = t401_timeout; }
#define T401_STOP(l) 	{ EC_DBG("T401 stop.\n"); \
                          (l)->modem->bit_timer = 0; (l)->rtx_count = 0; }
#define TIMER_START(l) T401_START(l)
#define TIMER_STOP(l) start_idle_timer(l)


/* xid sub-fields definitions */
/* param negotiation */
#define PI_HDLC_OPTFUNCS	0x03
#define PI_TX_INFO_MAXSIZE	0x05
#define PI_RX_INFO_MAXSIZE	0x06
#define PI_TX_WINDOW_SIZE	0x07
#define PI_RX_WINDOW_SIZE	0x08
/* private param negotiation */
#define PI_PARAMETER_SET_ID	0x00
#define PI_V42BIS_REQUEST	0x01
#define PI_V42BIS_CW_NUMBER	0x02
#define PI_V42BIS_MAX_STRING	0x03
/* user data */
/* fixme: ??? */


/* type definitions */
enum LAPM_STATES { LAPM_IDLE, LAPM_ESTAB, LAPM_DATA, LAPM_DISC };

/* prototypes */
static int lapm_connect(struct lapm_state *l);
static void complete_link_release(struct lapm_state *l);
static void start_idle_timer(struct lapm_state *l);
static void t403_timeout(struct modem *m);
static int lapm_disconnect(struct lapm_state *l);
static void reset_lapm_sequence(struct lapm_state *l);

/*
 * service functions
 *
 */

static bool v44_offer_has_direction(
	const struct v44_offer *offer)
{
	return offer->transmit.enabled || offer->receive.enabled;
}

static int v44_read_characters(
	void *context, uint8_t *output, size_t output_capacity)
{
	struct lapm_state *l = context;

	if (l == NULL || output_capacity > INT_MAX ||
	    (output_capacity != 0 && output == NULL))
		return -1;
	if (l->modem->get_chars == NULL)
		return 0;
	return l->modem->get_chars(
		l->modem, (char *)output, (int)output_capacity);
}

static int v44_write_characters(
	void *context, const uint8_t *input, size_t input_length)
{
	struct lapm_state *l = context;

	if (l == NULL || input_length > INT_MAX ||
	    (input_length != 0 && input == NULL))
		return -1;
	if (l->modem->put_chars == NULL)
		return 0;
	return l->modem->put_chars(
		l->modem, (const char *)input, (int)input_length);
}

static struct v44_character_path *create_v44_path(
	struct lapm_state *l, const struct v44_offer *offer)
{
	struct v44_character_path_config config;

	if (l == NULL || offer == NULL || !v44_offer_has_direction(offer))
		return NULL;
	config = (struct v44_character_path_config) {
		.codec = {
			.transmit_enabled = offer->transmit.enabled,
			.transmit_parameters = offer->transmit.parameters,
			.transmit_adaptation_enabled = offer->transmit.enabled,
			.transmit_adaptation =
				v44_tx_monitor_default_config(),
			.receive_enabled = offer->receive.enabled,
			.receive_parameters = offer->receive.parameters,
		},
		.receive_buffer_capacity = offer->receive.enabled ?
			offer->receive.parameters.max_string_length :
			LAPM_MAX_INFO_SIZE,
		.context = l,
		.read = v44_read_characters,
		.write = v44_write_characters,
	};
	return v44_character_path_create(&config);
}

static int reinitialize_for_link_establishment(struct lapm_state *l);

/* debug stuffs */
#define EC_ERR(fmt,arg...) eprintf("%s ec err: " fmt , \
                                   l->modem->name , ##arg)

#ifdef EC_DEBUG
#define EC_DBG(fmt,arg...) dprintf("%s ec: " fmt , \
                                   l->modem->name , ##arg)
#define EC_DBG1(fmt...) // EC_DBG(fmt)

/* Frames reach this hook only after construction or receive validation. */
static void print_frame(
	struct lapm_state *l, const char *title, bool command,
	const frame_t *f, int length)
{
	EC_DBG("%s %s: addr=%02x control=%02x length=%d (va/vs/vr %d/%d/%d)\n",
	       title, command ? "cmd" : "rsp", FRAME_ADDR(f), FRAME_CTRL(f),
	       length, l->va, l->vs, l->vr);
	if (FRAME_TYPE(f) == FRAME_I && length >= 3) {
		EC_DBG("%s: ns=%d nr=%d pf=%d\n",
		       title, FRAME_NS(f), FRAME_NR(f), FRAME_I_PF(f));
	} else if (FRAME_TYPE(f) == FRAME_S && length >= 3) {
		EC_DBG("%s: nr=%d pf=%d\n", title, FRAME_NR(f), FRAME_S_PF(f));
	}
}

#define LAPM_PRINT_FRAME(str,cmd,f,len) print_frame(l,str,cmd,f,len)

#else /* !EC_DEBUG */

#define EC_DBG(fmt...)
#define EC_DBG1(fmt...)
#define LAPM_PRINT_FRAME(str,cmd,f,len)

#endif /* !EC_DEBUG */



/*
 * service functions
 *
 */

/* get frame from ctrl list */
static inline frame_t *get_ctrl_frame(struct lapm_state *l)
{
	frame_t *f;
	if ( l->ctrl_list->next == l->tx_ctrl ) {
		return 0;
	}
	f = l->ctrl_list;
	l->ctrl_list = l->ctrl_list->next;
	return f;
}



/* U transmission */
static int tx_unnum(struct lapm_state *l, u8 addr, u8 ctrl, u8 *info, int len)
{
	frame_t *f = get_ctrl_frame(l);
	if (!f) {
		EC_ERR("tx_unnum: cannot alloc frame.\n");
		return -1;
	}
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	if (info && len)
		memcpy(f->buf+2,info,len);
	f->size = 2 + len;
	return 0;
}

/* S transmission */
static int tx_super(struct lapm_state *l, u8 addr, u8 ctrl, u8 pf_mask)
{
	frame_t *f = get_ctrl_frame(l);
	if (!f) {
		EC_ERR("tx_super: cannot alloc frame.\n");
		return -1;
	}
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	f->buf[2] = (l->vr << 1)|pf_mask;
	f->size = 3;
	return 0;
}


/* V.42 9.2: select within the interval between the request and the default. */
static bool select_xid_parameter(
	unsigned int proposed, unsigned int received, unsigned int default_value,
	unsigned int capacity, bool command, unsigned int *selected)
{
	unsigned int requested = command ? received : proposed;
	unsigned int value = command ? proposed : received;
	unsigned int minimum = requested < default_value ? requested : default_value;
	unsigned int maximum = requested > default_value ? requested : default_value;

	if (command) {
		if (value < minimum)
			value = minimum;
		if (value > maximum)
			value = maximum;
	}
	if (value == 0 || value > capacity || value < minimum || value > maximum)
		return false;
	*selected = value;
	return true;
}

#define DEFAULT_MODEM_EC_CONFIG { \
	.ec = 1, \
	.ec_detector = 1, \
	.ec_tx_win_size = 15, \
	.ec_rx_win_size = 15, \
	.ec_tx_info_size = 128, \
	.ec_rx_info_size = 128, \
	.comp = 0, \
	.comp_types = MODEM_COMP_TYPE_NONE, \
	.comp_dict_size = 512, \
	.comp_max_string = 6, \
}

enum {
	XID_PARAMETER_LENGTH = 20,
	XID_PRIVATE_LENGTH = 15,
};

struct lapm_xid_config {
	unsigned int tx_win_size;
	unsigned int rx_win_size;
	unsigned int tx_info_size;
	unsigned int rx_info_size;
	struct modem_config modem;
};

enum xid_decode_result {
	XID_VALID = 0,
	XID_MALFORMED = -1,
	XID_INVALID_COMPRESSION = -2,
};

/* The XID envelope has already validated each parameter's extent.
 * Decode in the sending DCE's directions; missing values mean defaults. */
static bool parse_xid_params(
	struct lapm_xid_config *config, const u8 *buf, int len)
{
	unsigned int seen = 0;

	config->tx_info_size = config->rx_info_size = LAPM_DFLT_INFO_SIZE;
	config->tx_win_size = config->rx_win_size = LAPM_DFLT_WIN_SIZE;
	while (len > 0) {
		unsigned int pval = 0, *value, mask;
		u8 pid = buf[0], plen = buf[1];
		bool info = pid == PI_TX_INFO_MAXSIZE || pid == PI_RX_INFO_MAXSIZE;

		buf += 2;
		len -= 2 + plen;
		switch (pid) {
		case PI_TX_INFO_MAXSIZE:
			value = &config->tx_info_size;
			break;
		case PI_RX_INFO_MAXSIZE:
			value = &config->rx_info_size;
			break;
		case PI_TX_WINDOW_SIZE:
			value = &config->tx_win_size;
			break;
		case PI_RX_WINDOW_SIZE:
			value = &config->rx_win_size;
			break;
		default:
			buf += plen;
			continue;
		}
		if (plen == 0)
			return false;
		for (unsigned int index = 0; index < plen; ++index) {
			if (pval > (UINT_MAX >> 8))
				return false;
			pval = (pval << 8) | buf[index];
		}
		if (info) {
			/* N401 is an octet count, encoded in bits (Table 11a). */
			if ((pval & 7U) != 0)
				return false;
			pval >>= 3;
		} else if (pval == 0 || pval > 127) { /* V.42 8.2.3.2.2 */
			return false;
		}
		mask = 1U << (pid - PI_TX_INFO_MAXSIZE);
		if ((seen & mask) != 0 && *value != pval)
			return false;
		*value = pval;
		seen |= mask;
		buf += plen;
	}
	return true;
}


static unsigned int reverse_compression_directions(unsigned int directions)
{
	return ((directions & 1U) << 1) | ((directions & 2U) >> 1);
}

static enum xid_decode_result parse_xid_private_params(
	struct modem_config *cfg, const struct modem_config *local,
	bool command, const u8 *buf, int len)
{
	static const u8 identifier[] = { PI_PARAMETER_SET_ID, 3, 'V', '4', '2' };
	unsigned int allowed = (local->comp_types & MODEM_COMP_TYPE_V42BIS) ? local->comp : 0;
	u8 pid, plen;

	/* Table 11b: the parameter-set identifier comes first. Unknown sets
	 * do not give the remaining parameter identifiers V.42bis semantics. */
	if (len < (int)sizeof(identifier) || memcmp(buf, identifier, sizeof(identifier)) != 0)
		return XID_VALID;
	buf += sizeof(identifier);
	len -= sizeof(identifier);
	while (len > 0) {
		pid  = buf[0];
		plen = buf[1];
		buf += 2;
		len -= 2 + plen;
		switch (pid) {
		case PI_V42BIS_REQUEST:
			if (plen != 1 || buf[0] > 3)
				return XID_MALFORMED;
			/* cfg bit 1 is local RX, bit 2 local TX. P0 directions are
			 * relative to the negotiation initiator in both frame types. */
			cfg->comp = command ? buf[0] : reverse_compression_directions(buf[0]);
			if (!command && (cfg->comp & ~allowed) != 0)
				return XID_MALFORMED;
			cfg->comp &= allowed;
			cfg->comp_types |= MODEM_COMP_TYPE_V42BIS;
			break;
		case PI_V42BIS_CW_NUMBER:
			if (plen != 2)
				return XID_MALFORMED;
			cfg->comp_dict_size = ((unsigned int)buf[0] << 8) | buf[1];
			break;
		case PI_V42BIS_MAX_STRING:
			if (plen != 1)
				return XID_MALFORMED;
			cfg->comp_max_string = buf[0];
			break;
		default:
			break;
		}
		if (!modem_comp_parameters_valid(cfg->comp_dict_size, cfg->comp_max_string))
			return XID_INVALID_COMPRESSION;
		buf += plen;
	}
	/* V.42bis 5.1: both DCEs use the lower P1/P2 proposals. Select before
	 * advertising a response so it agrees with the allocated codec limits. */
	if (cfg->comp_dict_size > local->comp_dict_size)
		cfg->comp_dict_size = local->comp_dict_size;
	if (cfg->comp_max_string > local->comp_max_string)
		cfg->comp_max_string = local->comp_max_string;
	return XID_VALID;
}

static enum xid_decode_result decode_xid(
	struct lapm_state *l, frame_t *f, struct lapm_xid_config *config)
{
	struct v42_xid_fields fields;
	struct lapm_xid_config peer;
	bool command;
	size_t length;

	if (f == NULL || f->buf == NULL || f->count < 2 || config == NULL)
		return XID_MALFORMED;
	length = (size_t)f->count - 2U;
	if (v42_xid_decode(
			f->buf + 2, length, &fields) !=
			V42_XID_OK)
		return XID_MALFORMED;
	*config = (struct lapm_xid_config) {
		.tx_win_size = l->tx_win_size,
		.rx_win_size = l->rx_win_size,
		.tx_info_size = l->tx_info_size,
		.rx_info_size = l->rx_info_size,
		.modem = DEFAULT_MODEM_EC_CONFIG,
	};
	if (!parse_xid_params(&peer, fields.parameter_negotiation,
		(int)fields.parameter_negotiation_length))
		return XID_MALFORMED;
	command = RX_IS_COMMAND(f);
	if (!command) {
		struct v42_xid_fields offered;

		/* Our cached command owns the proposal and retry interval.
		 * A response confirms its values; it is not another request. */
		if (v42_xid_decode(l->v44_session.command, l->v44_session.command_length,
			&offered) != V42_XID_OK ||
		    !parse_xid_params(config, offered.parameter_negotiation,
			(int)offered.parameter_negotiation_length))
			return XID_MALFORMED;
	}
	/* Table 11a directions are relative to the sending DCE. */
	if (!select_xid_parameter(config->tx_info_size, peer.rx_info_size,
		LAPM_DFLT_INFO_SIZE, LAPM_MAX_INFO_SIZE, command, &config->tx_info_size) ||
	    !select_xid_parameter(config->rx_info_size, peer.tx_info_size,
		LAPM_DFLT_INFO_SIZE, LAPM_MAX_INFO_SIZE, command, &config->rx_info_size) ||
	    !select_xid_parameter(config->tx_win_size, peer.rx_win_size,
		LAPM_DFLT_WIN_SIZE, LAPM_MAX_WIN_SIZE, command, &config->tx_win_size) ||
	    !select_xid_parameter(config->rx_win_size, peer.tx_win_size,
		LAPM_DFLT_WIN_SIZE, LAPM_MAX_WIN_SIZE, command, &config->rx_win_size))
		return XID_MALFORMED;
	if (fields.private_negotiation != NULL) {
		enum xid_decode_result result = parse_xid_private_params(
			&config->modem, &l->modem->cfg, RX_IS_COMMAND(f), fields.private_negotiation,
			(int)fields.private_negotiation_length);

		if (result != XID_VALID) {
			if (result == XID_INVALID_COMPRESSION)
				EC_ERR("invalid V.42bis compression limits\n");
			return result;
		}
	}
	config->modem.ec_tx_win_size = config->tx_win_size;
	config->modem.ec_rx_win_size = config->rx_win_size;
	config->modem.ec_tx_info_size = config->tx_info_size;
	config->modem.ec_rx_info_size = config->rx_info_size;
	return XID_VALID;
}

static void apply_xid_config(
	struct lapm_state *l, const struct lapm_xid_config *config)
{
	struct modem_config modem = config->modem;

	l->tx_win_size = config->tx_win_size;
	l->rx_win_size = config->rx_win_size;
	l->tx_info_size = config->tx_info_size;
	l->rx_info_size = config->rx_info_size;
	if (l->v44_path != NULL)
		modem.comp = 0;
	modem_update_config(l->modem, &modem);
}

static struct lapm_xid_config current_xid_config(
	const struct lapm_state *l)
{
	const struct lapm_xid_config config = {
		.tx_win_size = l->tx_win_size,
		.rx_win_size = l->rx_win_size,
		.tx_info_size = l->tx_info_size,
		.rx_info_size = l->rx_info_size,
		.modem = l->modem->cfg,
	};

	return config;
}

static void build_xid_fields(
	const struct lapm_xid_config *config, bool initiator,
	u8 parameters[XID_PARAMETER_LENGTH],
	u8 private_parameters[XID_PRIVATE_LENGTH],
	struct v42_xid_fields *fields)
{
	const unsigned int tx_bits = config->tx_info_size * 8U;
	const unsigned int rx_bits = config->rx_info_size * 8U;
	const u8 parameter_values[XID_PARAMETER_LENGTH] = {
		/* V.42 Table 11a: a four-octet mask with mandatory bits set. */
		PI_HDLC_OPTFUNCS, 4, 0x8a, 0x89, 0x00, 0x00,
		PI_TX_INFO_MAXSIZE, 2, (u8)(tx_bits >> 8), (u8)tx_bits,
		PI_RX_INFO_MAXSIZE, 2, (u8)(rx_bits >> 8), (u8)rx_bits,
		PI_TX_WINDOW_SIZE, 1, config->tx_win_size,
		PI_RX_WINDOW_SIZE, 1, config->rx_win_size,
	};
	const u8 private_values[XID_PRIVATE_LENGTH] = {
		PI_PARAMETER_SET_ID, 3, 0x56, 0x34, 0x32,
		PI_V42BIS_REQUEST, 1, initiator ?
			reverse_compression_directions(config->modem.comp) : config->modem.comp,
		PI_V42BIS_CW_NUMBER, 2,
		(u8)(config->modem.comp_dict_size >> 8),
		(u8)config->modem.comp_dict_size,
		PI_V42BIS_MAX_STRING, 1, config->modem.comp_max_string,
	};

	memcpy(parameters, parameter_values, sizeof(parameter_values));
	memcpy(private_parameters, private_values, sizeof(private_values));
	*fields = (struct v42_xid_fields) {
		.parameter_negotiation = parameters,
		.parameter_negotiation_length = XID_PARAMETER_LENGTH,
	};
	if (config->modem.comp &&
	    (config->modem.comp_types & MODEM_COMP_TYPE_V42BIS) != 0) {
		fields->private_negotiation = private_parameters;
		fields->private_negotiation_length = XID_PRIVATE_LENGTH;
	}
}

static int queue_xid(
	struct lapm_state *l, u8 addr,
	const struct lapm_v44_prepared *prepared)
{
	frame_t *f;

	if (prepared == NULL || prepared->length > LAPM_MAX_INFO_SIZE)
		return -1;
	f = get_ctrl_frame(l);
	if (!f) {
		EC_ERR("queue_xid: cannot alloc frame.\n");
		return -1;
	}
	f->buf[0] = addr;
	f->buf[1] = FRAME_U_XID;
	memcpy(f->buf + 2, prepared->data, prepared->length);
	f->size = 2 + prepared->length;
	if (lapm_v44_commit(&l->v44_session, prepared) !=
	    LAPM_V44_OK) {
		l->ctrl_list = f;
		return -1;
	}
	return 0;
}

static int tx_xid_command(struct lapm_state *l)
{
	struct lapm_v44_prepared prepared;
	enum lapm_v44_result result;

	if (l->v44_session.phase == LAPM_V44_COMMAND_PENDING)
		result = lapm_v44_prepare_retransmission(
			&l->v44_session, &prepared);
	else {
		struct lapm_xid_config config = current_xid_config(l);
		u8 parameters[XID_PARAMETER_LENGTH];
		u8 private_parameters[XID_PRIVATE_LENGTH];
		struct v42_xid_fields fields;

		build_xid_fields(
			&config, true, parameters, private_parameters, &fields);
		result = lapm_v44_prepare_command(
			&l->v44_session, &fields, &prepared);
	}
	if (result != LAPM_V44_OK)
		return -1;
	return queue_xid(l, l->cmd_addr, &prepared);
}

static int rx_xid_command(struct lapm_state *l, frame_t *f)
{
	struct lapm_xid_config config;
	struct lapm_v44_prepared prepared;
	struct v44_character_path *candidate = NULL;
	u8 parameters[XID_PARAMETER_LENGTH];
	u8 private_parameters[XID_PRIVATE_LENGTH];
	struct v42_xid_fields fields;
	bool duplicate = l->v44_session.response_length != 0;
	bool pending = l->v44_session.phase == LAPM_V44_COMMAND_PENDING;
	enum xid_decode_result decoded;

	if (!duplicate && l->state == LAPM_DATA)
		return -1;
	decoded = decode_xid(l, f, &config);
	if (decoded == XID_INVALID_COMPRESSION) {
		l->config = 0;
		T401_STOP(l);
		modem_update_status(l->modem, STATUS_EC_ERROR);
		return -1;
	}
	if (decoded != XID_VALID)
		return -1;
	build_xid_fields(&config, false, parameters, private_parameters, &fields);
	if (lapm_v44_prepare_response(
			&l->v44_session, f->buf + 2, (size_t)f->count - 2U,
			&fields, &prepared) != LAPM_V44_OK)
		return -1;
	if (!duplicate && !pending && v44_offer_has_direction(&prepared.offer)) {
		candidate = create_v44_path(l, &prepared.offer);
		if (candidate == NULL)
			return -1;
	}
	if (queue_xid(l, l->rsp_addr, &prepared) != 0) {
		v44_character_path_destroy(candidate);
		return -1;
	}
	/* V.42 8.10.2: a peer command does not confirm our own command.
	 * Keep its timer, frozen offer, and codec setup until our response arrives.
	 * A cached reply must not reapply old parameters or reset codec histories. */
	if (duplicate || pending)
		return 0;
	v44_character_path_destroy(l->v44_path);
	l->v44_path = candidate;
	apply_xid_config(l, &config);
	return 0;
}

enum rx_xid_response_result {
	RX_XID_RESPONSE_ACCEPTED = 0,
	RX_XID_RESPONSE_IGNORED = -1,
	RX_XID_RESPONSE_FATAL = -2,
};

static void t401_timeout(struct modem *m);

static enum rx_xid_response_result rx_xid_response(
	struct lapm_state *l, frame_t *f)
{
	struct lapm_xid_config config;
	struct v44_offer offer;
	enum xid_decode_result decoded = decode_xid(l, f, &config);

	if (decoded == XID_INVALID_COMPRESSION)
		return RX_XID_RESPONSE_FATAL;
	if (decoded != XID_VALID)
		return RX_XID_RESPONSE_IGNORED;
	if (lapm_v44_accept_response(
		    &l->v44_session, f->buf + 2,
		    (size_t)f->count - 2U) != LAPM_V44_OK) {
		struct lapm_v44_prepared retry;

		/* V.44 7.3 permits only one compression selection in a response.
		 * Obtain an unambiguous reply with one V.42bis-only command. Do not
		 * apply the rejected parameters or extend retries under queue pressure. */
		if (l->modem->cfg.comp &&
		    (l->modem->cfg.comp_types & MODEM_COMP_TYPE_V42BIS) != 0 &&
		    lapm_v44_prepare_retry_without_v44(&l->v44_session,
			f->buf + 2, (size_t)f->count - 2U, &retry) == LAPM_V44_OK &&
		    queue_xid(l, l->cmd_addr, &retry) == 0)
			T401_START(l);
		return RX_XID_RESPONSE_IGNORED;
	}
	if (l->v44_session.phase == LAPM_V44_NEGOTIATED) {
		if (lapm_v44_get_negotiated_offer(
			    &l->v44_session, &offer) != LAPM_V44_OK)
			return RX_XID_RESPONSE_FATAL;
		if (l->v44_path != NULL)
			return RX_XID_RESPONSE_FATAL;
		l->v44_path = create_v44_path(l, &offer);
		if (l->v44_path == NULL)
			return RX_XID_RESPONSE_FATAL;
	}
	apply_xid_config(l, &config);
	return RX_XID_RESPONSE_ACCEPTED;
}


/* timeout procedures */

static void t401_timeout(struct modem *m)
{
	struct lapm_state *l = &m->ec.lapm;
	EC_DBG("t401_timeout: %d...\n",l->rtx_count);
	/* A final RR can complete every enquiry while acknowledging no data.
	 * Keep a separate local progress budget across those recovery episodes;
	 * the ordinary N400 counter still governs unanswered polls. */
	if (l->state == LAPM_DATA && !l->config) {
		if (l->busy || l->peer_busy)
			l->stalled_polls = 0;
		else if (l->sent_info != l->info_list &&
			 ++l->stalled_polls > LAPM_MAX_RTX) {
			EC_DBG("no information progress after status enquiries: disconnect\n");
			lapm_disconnect(l);
			return;
		}
	}
	if (l->rtx_count > LAPM_MAX_RTX ) {
		EC_DBG("t401: max retransmission count was reached.\n");
		l->rtx_count = 0;
		if (l->config) {
			l->config = 0;
			l->state = LAPM_IDLE;
			lapm_v44_session_call_reset(
				&l->v44_session);
			modem_update_status(l->modem, STATUS_EC_RELEASE);
			return;
		}
		// TBD
		switch(l->state) {
		case LAPM_ESTAB:
		case LAPM_DISC:
			l->state = LAPM_IDLE;
			modem_update_status(l->modem,STATUS_EC_RELEASE);
			break;
		case LAPM_DATA:
			// TBD: disconnect
			EC_DBG("t401: max data rtx was reached: disconnect\n");
			lapm_disconnect(l);
			break;
		}
		return ;
	}
	l->rtx_count++;
	if (l->config) {
		tx_xid_command(l);
	}
	else {
		switch(l->state) {
		case LAPM_ESTAB:
			TX_SABME(l);
			break;
		case LAPM_DISC:
			TX_DISC(l);
			break;
		case LAPM_DATA:
			if (l->busy)
				TX_RNR(l,l->cmd_addr,1);
			else
				TX_RR(l,l->cmd_addr,1);
			break;
		}
	}
	m->bit_timer      = lapm_timer_timeout(l);
	m->bit_timer_func = t401_timeout;
}

void modem_ec_update_timer_rate(struct modem *m, uint64_t previous_rate)
{
	uint64_t rate, remaining;

	/* LAPM elapsed-time timers use duplex bit units. Other timer owners,
	 * including the detector's mark preamble, count fixed bits. */
	if (m == NULL || m->bit_timer <= 0 ||
	    (m->bit_timer_func != t401_timeout &&
	     m->bit_timer_func != t403_timeout))
		return;
	rate = (uint64_t)m->rx_rate + m->tx_rate;
	if (rate == 0)
		rate = LAPM_TIMER_FALLBACK_BIT_RATE;
	if (previous_rate == 0)
		previous_rate = LAPM_TIMER_FALLBACK_BIT_RATE;
	/* Round up so a positive residual cannot become a disabled timer, and
	 * a publication of either directional rate cannot expire it early. */
	remaining = ((uint64_t)m->bit_timer * rate + previous_rate - 1) /
		previous_rate;
	m->bit_timer = remaining > INT_MAX ? INT_MAX : (int)remaining;
}

/* V.42 8.12: supervise idle links with an ordinary status enquiry, then
 * use the existing T401/N400 recovery procedure. */
static void t403_timeout(struct modem *m)
{
	struct lapm_state *l = &m->ec.lapm;

	if (l->state == LAPM_DATA && !l->config && !l->rtx_count)
		t401_timeout(m);
}

static void start_idle_timer(struct lapm_state *l)
{
	uint64_t rate = (uint64_t)l->modem->rx_rate + l->modem->tx_rate;
	uint64_t timeout;

	T401_STOP(l);
	if (l->state != LAPM_DATA || l->config)
		return;
	if (l->sent_info == l->info_list) {
		l->stalled_polls = 0;
		l->stalled_retrain = false;
	}
	if (rate == 0)
		rate = LAPM_TIMER_FALLBACK_BIT_RATE;
	timeout = rate * LAPM_IDLE_TIMER_SECONDS;
	l->modem->bit_timer = timeout > INT_MAX ? INT_MAX : (int)timeout;
	l->modem->bit_timer_func = t403_timeout;
}



/* transmit info */
static int read_info_payload(
	struct lapm_state *l, uint8_t *output, size_t output_capacity)
{
	if (l->v44_path != NULL) {
		size_t output_length = 0;
		enum v44_character_path_result result =
			v44_character_path_transmit(
				l->v44_path, output, output_capacity,
				&output_length);

		if (output_length > output_capacity || output_length > INT_MAX)
			return -1;
		if (result == V44_CHARACTER_PATH_IDLE)
			return output_length == 0 ? 0 : -1;
		if (result != V44_CHARACTER_PATH_OK)
			return -1;
		return (int)output_length;
	}
	if (l->modem->get_chars == NULL || output_capacity > INT_MAX)
		return 0;
	return l->modem->get_chars(
		l->modem, (char *)output, (int)output_capacity);
}

static int tx_info(struct lapm_state *l)
{
	frame_t *f;
	int n = 0;
	if (l->peer_busy ||
	    ((l->vs - l->va)&0x7f) >= l->tx_win_size) {
		return 0;
	}
	if (l->tx_info == l->info_list) { /* empty tx */
		if (l->info_list->next == l->tx_info ||
		    l->info_list->next == l->sent_info) {
			EC_ERR("tx_info: cannot alloc frame.\n");
			return 0;
		}
		f = l->info_list;
		n = read_info_payload(l, f->buf + 3, l->tx_info_size);
		if (n < 0) { /* error */
			EC_ERR("tx_info: get chars error.\n");
			modem_update_status(l->modem,STATUS_EC_ERROR);
			return 0;
		}
		if ( n == 0) { /* no data */
			return 0;
		}
			
		f->size = n + 3;
		l->info_list = l->info_list->next;
	}

	/* start T401 */
	//if (!l->timer)
	//	TIMER_START(l);
	return 1;
}

/* reject all noacked frames (>va) */
static int reject_info(struct lapm_state *l)
{
	u8 n = (l->vs-l->va)&0x7f;
	if (l->state != LAPM_DATA) /* ignore retransmission */
		return 0;
	EC_DBG("(va/vs/vr %d/%d/%d) reject info: reject %d frames...\n",
		 l->va,l->vs,l->vr,n);
	l->vs = l->va;
	l->tx_info = l->sent_info;
	return n;
}

/* ack info */
static bool ack_info(struct lapm_state *l, u8 nr)
{
	int n = 0;
	/* The receive boundary has validated N(R) before any state change. */
	while(l->va != nr) {
		if (l->sent_info == l->tx_info) {
			EC_ERR("acknowledgement exceeds retained frames: va=%u vs=%u nr=%u\n",
			       l->va, l->vs, nr);
			complete_link_release(l);
			return false;
		}
		l->sent_info = l->sent_info->next;
		l->va = (l->va+1)&0x7f;
		n++;
	}
	if (n > 0) {
		l->stalled_polls = 0;
		l->stalled_retrain = false;
	}
	/* V.42 8.4.3.2: an I acknowledgement must not stop or restart
	 * T401 while our status poll or the peer's busy condition persists. */
	if (n > 0 && !l->rtx_count && !l->peer_busy) {
		TIMER_STOP(l);
		if ((l->vs-l->va)&0x7f)
			TIMER_START(l); // exp.8.4.8
	}
	return true;
}


/* push rest data (when busy) */
static void fail_receive_data(struct lapm_state *l)
{
	l->rx_head = l->rx_count = 0;
	l->busy = 0;
	l->modem->packer_process = NULL;
	modem_update_status(l->modem, STATUS_EC_ERROR);
}

static void push_rest_data(struct modem *m, int bits)
{
	struct lapm_state *l = &m->ec.lapm;
	(void)bits;
	if (l->state != LAPM_DATA)
		return;
	if (l->v44_path != NULL) {
		size_t consumed = 0;
		enum v44_character_path_result result =
			v44_character_path_receive(
				l->v44_path,
				l->rx_count == 0 ? NULL : l->rx_buf + l->rx_head,
				(size_t)l->rx_count, &consumed);

		if (consumed > (size_t)l->rx_count) {
			fail_receive_data(l);
			return;
		}
		l->rx_head += (int)consumed;
		l->rx_count -= (int)consumed;
		if (result == V44_CHARACTER_PATH_NEED_OUTPUT)
			return;
		if (result != V44_CHARACTER_PATH_OK || l->rx_count != 0) {
			fail_receive_data(l);
			return;
		}
		l->rx_head = 0;
		l->busy = 0;
		TX_RR(l,l->cmd_addr,0);
		l->modem->packer_process = NULL;
		return;
	}
	/* A compression sink can own output after accepting the last input
	 * octet. Retry even with no remaining LAPM payload to drain that output. */
	if (l->modem->put_chars) {
		int ret = l->modem->put_chars(l->modem,
					      (const char*)(l->rx_buf+l->rx_head),
					      l->rx_count);
		if (ret < 0 || ret > l->rx_count) {
			fail_receive_data(l);
			return;
		}
		l->rx_head += ret;
		l->rx_count -= ret;
		if (!l->rx_count && l->modem->comp.count == 0) {
			l->rx_head = 0;
			l->busy = 0;
			TX_RR(l,l->cmd_addr,0);
			l->modem->packer_process = NULL;
		}
	}
}

/* info frame process */
static int rx_info(struct lapm_state *l, frame_t *f)
{
	int ret = 0;
	int payload_length;
	bool retrain_progress;
	payload_length = f->count - 3;

	/* ack I frames: nr -1 */
	if (!ack_info(l,FRAME_NR(f)))
		return -1;
	/* busy */
	if (l->busy) {
		if(FRAME_I_PF(f)) /* 8.4.7 */
			TX_RNR(l,l->rsp_addr,1);
		return 0;
	}
	/* NS sequence error */
	if (FRAME_NS(f)!= l->vr) {
		/* TBD */ /* is info may be send */
		EC_DBG("seq.error: ns %d, vr %d\n",FRAME_NS(f),l->vr);
		// reject should be sent
		if (l->reject) {
			/* V.42 8.5.1: even a repeated sequence error carries a
			 * valid poll. Reply without repeating REJ or accepting
			 * its payload; own-receiver busy is handled above. */
			if (FRAME_I_PF(f))
				TX_RR(l, l->rsp_addr, 1);
			return -1;
		}
		EC_DBG("tx reject...\n");
		TX_REJ(l,l->rsp_addr,FRAME_I_PF(f));
		/* TX_REJ(l,l->cmd_addr,1); */
		l->reject = 1;
		// fixme: why to start timer
		//if (!l->timer)
		//	TIMER_START(l);
		return -1;
	}
	l->reject = 0;
	retrain_progress = l->retrain_poll && l->rtx_count;
	if (retrain_progress) {
		/* Some peers resume with sequenced information instead of the
		 * solicited supervisory response.  Its N(R) has synchronized the
		 * retained transmit window, and a valid N(S) proves the new HDLC
		 * epoch is usable. */
		reject_info(l);
		TIMER_STOP(l);
		l->peer_busy = 0;
		l->retrain_poll = 0;
	}
	/* recv data */

	if (l->v44_path != NULL) {
		size_t consumed = 0;
		enum v44_character_path_result result =
			v44_character_path_receive(
				l->v44_path, f->buf + 3, (size_t)payload_length,
				&consumed);

		if (consumed > (size_t)payload_length ||
		    (result != V44_CHARACTER_PATH_OK &&
		     result != V44_CHARACTER_PATH_NEED_OUTPUT) ||
		    (result == V44_CHARACTER_PATH_OK &&
		     consumed != (size_t)payload_length)) {
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return -1;
		}
		ret = (int)consumed;
		if (result == V44_CHARACTER_PATH_NEED_OUTPUT)
			l->busy = 1;
	} else if(l->modem->put_chars)
		ret = l->modem->put_chars(
			l->modem, (const char *)(f->buf + 3), payload_length);
	if (ret < 0 || ret > payload_length) {
		modem_update_status(l->modem, STATUS_EC_ERROR);
		return -1;
	}
	if (ret != payload_length || l->busy || l->modem->comp.count != 0) {
		/* save rest data, enter busy state */
		l->rx_head = 0;
		l->rx_count = payload_length - ret;
		memcpy(l->rx_buf,f->buf+3+ret,l->rx_count);
		// TBD
		l->busy = 1;
		// try to push rest data later
		l->modem->packer_process = push_rest_data;
	}
	/* increment vr */
	l->vr = (l->vr+1)&0x7f;
	/* response I,RR,RNR */
	if (l->busy)
		TX_RNR(l,FRAME_ADDR(f),FRAME_I_PF(f));
	else if ( l->peer_busy || FRAME_I_PF(f) ||
		  !tx_info(l) )
		TX_RR(l,FRAME_ADDR(f),FRAME_I_PF(f));
	return 0;
}

/* rx supervisory frame */

static void rx_super_cmd(struct lapm_state *l, frame_t *f)
{

	/* note 8.4.7:
	   if l->busy each RR,RNR,REJ with p=1 should be replied by RNR with f=1 */
	switch(FRAME_CTRL(f)) {
	case FRAME_S_RR:
		/* clear peer_busy state */
		l->peer_busy = 0;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		/* p=1 may be used for status checking */
		if (FRAME_S_PF(f) || !tx_info(l)) {
			if (l->busy)
				TX_RNR(l,FRAME_ADDR(f),FRAME_S_PF(f));
			else
				TX_RR(l,FRAME_ADDR(f),FRAME_S_PF(f));
		}
		break;
	case FRAME_S_RNR:
		/* going to peer_busy state */
		l->peer_busy = 1;
		l->stalled_polls = 0;
		l->stalled_retrain = false;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		/* V.42 8.4.6: enquire even with no outstanding information,
		 * but do not postpone an already transmitted status poll. */
		if (!l->rtx_count)
			TIMER_START(l);
		/* if p=1 may be used for status checking ?? */
		if (FRAME_S_PF(f)) {
			if (l->busy)
				TX_RNR(l,l->rsp_addr,FRAME_S_PF(f));
			else
				TX_RR(l,l->rsp_addr,FRAME_S_PF(f));
		}
		break;
	case FRAME_S_REJ:
		/* clear peer_busy state */
		l->peer_busy = 0;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		if (!l->rtx_count) {
			TIMER_STOP(l);
			reject_info(l);
		}
		/* p=1 may be used for status checking */
		if (FRAME_S_PF(f) || !tx_info(l)) {
			if (l->busy)
				TX_RNR(l,FRAME_ADDR(f),FRAME_S_PF(f));
			else
				TX_RR(l,FRAME_ADDR(f),FRAME_S_PF(f));
		}
		break;
	}
}

static void rx_super_rsp(struct lapm_state *l, frame_t *f)
{
	bool retrain_response = l->retrain_poll && l->rtx_count &&
		FRAME_S_PF(f);

	if (!l->rtx_count && FRAME_S_PF(f)) {
		EC_ERR("rx_super: unsolisited final response!\n");
		return;
	}
	/* ack I frames <= nr -1 */
	switch(FRAME_CTRL(f)) {
	case FRAME_S_RR:
		l->peer_busy = 0;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		if (l->rtx_count && FRAME_S_PF(f)) {
			reject_info(l);
			TIMER_STOP(l);
		}
		break;
	case FRAME_S_RNR:
		l->peer_busy = 1;
		l->stalled_polls = 0;
		l->stalled_retrain = false;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		if (l->rtx_count && FRAME_S_PF(f)) {
			reject_info(l);
			TIMER_STOP(l); // fixme: need stop even on RNR ??
		}
		if (!l->rtx_count)
			TIMER_START(l);
		break;
	case FRAME_S_REJ:
		l->peer_busy = 0;
		if (!ack_info(l,FRAME_NR(f)))
			return;
		if (!l->rtx_count || FRAME_S_PF(f)) {
			reject_info(l);
			TIMER_STOP(l);
		}
		break;
	}
	if (retrain_response)
		l->retrain_poll = 0;
}


/* rx unnumbered frame */

static bool lapm_can_establish(const struct lapm_state *l)
{
	return !l->config && modem_ec_can_establish(l->modem) &&
	       l->modem->packer.hdlc.tx_preamble_complete == NULL;
}

static void rx_unnum_cmd(struct lapm_state *l, frame_t *f)
{
	unsigned control = FRAME_CTRL(f) & 0xef;

	/* V.42 8.9: answer colliding mode commands without replacing the
	 * local transaction. Its UA/DM F=1 or retry deadline still owns release
	 * or establishment; a peer command cannot restart that deadline. */
	if ((control == FRAME_U_SABME || control == FRAME_U_DISC) &&
	    (l->state == LAPM_ESTAB || l->state == LAPM_DISC)) {
		bool identical = (control == FRAME_U_SABME) == (l->state == LAPM_ESTAB);

		if ((identical ? TX_UA(l, FRAME_U_PF(f)) :
		     TX_DM(l, FRAME_U_PF(f))) != 0)
			complete_link_release(l);
		return;
	}
	switch(control) {
	case FRAME_U_SABME:
	{
		bool already_linked = l->state == LAPM_DATA;

		EC_DBG("sabme received.\n");
		/* V.42 8.4.9.1: connected SABME terminates the call except
		 * for the answerer's initial establishment retry after a lost UA. */
		if (already_linked && !l->accept_sabme_retry) {
			complete_link_release(l);
			return;
		}
		/* V.42 7.2.2/8.3.2.1: the control function must be ready,
		 * with negotiation complete, before accepting establishment. */
		if (!lapm_can_establish(l)) {
			if (TX_DM(l, FRAME_U_PF(f)) != 0)
				complete_link_release(l);
			return;
		}
		/* discard unacked I frames,
		   reset vs,vr,va, clear exceptions */
		if (reinitialize_for_link_establishment(l) != 0) {
			TX_DM(l,FRAME_U_PF(f));
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return;
		}
		if (TX_UA(l,FRAME_U_PF(f)) != 0) {
			l->state = LAPM_IDLE;
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return;
		}
		/* going to connected state */
		l->state = LAPM_DATA;
		TIMER_STOP(l);
		if (!already_linked)
			modem_update_status(l->modem, STATUS_EC_LINK);
		break;
	}
	case FRAME_U_DISC:
		/* respond UA (or DM) */
		if (l->state == LAPM_IDLE)
			TX_DM(l,FRAME_U_PF(f));
		else { /* going to disconnected state,
			  discard unacked I frames, reset all  */
			l->state = LAPM_IDLE;
			reset_lapm_sequence(l);
			T401_STOP(l);
			if (TX_UA(l,FRAME_U_PF(f)) != 0) {
				modem_update_status(l->modem, STATUS_EC_ERROR);
				return;
			}
			/* UA is queued, not yet serialized. Keep HDLC and the modem
			 * line alive for that response and the peer's cleardown. */
			modem_update_status(l->modem, STATUS_EC_PEER_DISC);
		}
		break;
	case FRAME_U_XID:
		/* exchange general id info */
		rx_xid_command(l,f);
		break;
	}
}

static bool complete_link_establishment(struct lapm_state *l)
{
	if (reinitialize_for_link_establishment(l) != 0) {
		l->state = LAPM_IDLE;
		T401_STOP(l);
		modem_update_status(l->modem, STATUS_EC_ERROR);
		return false;
	}
	l->state = LAPM_DATA;
	TIMER_STOP(l);
	modem_update_status(l->modem, STATUS_EC_LINK);
	return true;
}

static void complete_link_release(struct lapm_state *l)
{
	l->state = LAPM_IDLE;
	reset_lapm_sequence(l);
	l->rx_head = l->rx_count = 0;
	l->modem->packer_process = NULL;
	T401_STOP(l);
	modem_update_status(l->modem, STATUS_EC_RELEASE);
}

static void rx_unnum_rsp(struct lapm_state *l, frame_t *f)
{
	switch(FRAME_CTRL(f)&0xef) {
	case FRAME_U_DM:
		if (l->state == LAPM_IDLE) {
			/* V.42 8.8: establish with SABME before indicating a link. */
			if (!FRAME_U_PF(f) && lapm_can_establish(l) &&
			    lapm_connect(l) != 0)
				modem_update_status(l->modem, STATUS_EC_ERROR);
		} else if ((l->state == LAPM_DATA && (l->rtx_count || !FRAME_U_PF(f))) ||
			   ((l->state == LAPM_ESTAB || l->state == LAPM_DISC) && FRAME_U_PF(f))) {
			complete_link_release(l);
		}
		break;
	case FRAME_U_UA:
		/* notify us about mode change, responds SABME,DISC */
		if (!FRAME_U_PF(f))
			break; /* Both locally initiated mode commands set P=1. */
		switch(l->state) {
		case LAPM_ESTAB:
			if (!l->config)
				complete_link_establishment(l);
			break;
		case LAPM_DISC:
			complete_link_release(l);
			break;
		default:
			// TBD
			/* handle unsolicited UA */
			break;
		}
		/* clear all exceptions, busy states (self and peer) */
		/* reset vars */
		break;
	case FRAME_U_FRMR:
		/* V.42 8.5.6: retransmission cannot repair a rejected frame. */
		if (l->state == LAPM_DATA)
			complete_link_release(l);
		break;
	case FRAME_U_XID:
		if (l->config) {
			enum rx_xid_response_result result =
				rx_xid_response(l, f);

			if (result == RX_XID_RESPONSE_ACCEPTED) {
				l->config = 0;
				TIMER_STOP(l);
				if (l->state == LAPM_IDLE) {
					if (lapm_connect(l) != 0)
						modem_update_status(l->modem, STATUS_EC_ERROR);
				}
				else if (l->state == LAPM_DATA) {
					l->busy = 0;
					TX_RR(l,l->cmd_addr,0);
				}
			}
			else if (result == RX_XID_RESPONSE_FATAL) {
				l->config = 0;
				T401_STOP(l);
				modem_update_status(l->modem, STATUS_EC_ERROR);
			}
		}
		break;
	}
}


/* lapm framer interface functions */

static frame_t *lapm_get_tx_frame(void *framer)
{
	struct lapm_state *l = framer;
	frame_t *f;
	/* get ctrl frame */
	if ( l->tx_ctrl != l->ctrl_list ) {
		f = l->tx_ctrl;
		l->tx_ctrl = l->tx_ctrl->next;
		return f;
	}
	/* get info frame */
	/* V.42 8.4.6/8.4.8: clearing peer busy with an ordinary RR/REJ must
	 * not release information while our recovery enquiry is outstanding. */
	if (l->peer_busy || l->rtx_count || l->config || l->state != LAPM_DATA)
		return 0;
	if ( l->tx_info == l->info_list && !tx_info(l) )
		return 0;
	f = l->tx_info;
	l->tx_info = l->tx_info->next;
	
	f->buf[0] = l->cmd_addr;
	f->buf[1] = l->vs << 1;
	f->buf[2] = l->vr << 1;
	l->vs = (l->vs + 1)&0x7f;
	if (!l->modem->bit_timer || l->modem->bit_timer_func == t403_timeout)
		TIMER_START(l);

	return f;
}

static void lapm_tx_complete(void *framer, frame_t *f)
{
#ifdef EC_DEBUG
	struct lapm_state *l = framer;
	LAPM_PRINT_FRAME("tx", TX_IS_COMMAND(f), f, f->size);
#else
	(void)framer;
	(void)f;
#endif
}

static bool valid_numbered_frame(
	const struct lapm_state *l, const frame_t *f, unsigned type)
{
	unsigned base = l->state == LAPM_ESTAB ? 0 : l->va;
	unsigned outstanding = l->state == LAPM_ESTAB ? 0 : (l->vs - l->va) & 0x7f;
	unsigned acknowledged = (FRAME_NR(f) - base) & 0x7f;

	/* V.42 8.2.4.12/8.5.2: validate the modulo-128 window before
	 * acknowledging information or completing a missing-UA exchange. */
	if (acknowledged > outstanding || outstanding > l->tx_win_size)
		return false;
	if (type == FRAME_I)
		return RX_IS_COMMAND(f) &&
		       (unsigned)(f->count - 3) <= l->rx_info_size &&
		       (size_t)(f->count - 3) <= sizeof(l->rx_buf);
	/* The negotiated receiver implements RR, RNR and REJ. Supervisory
	 * frames carry exactly two control octets and no information field. */
	return f->count == 3 &&
	       (FRAME_CTRL(f) == FRAME_S_RR || FRAME_CTRL(f) == FRAME_S_RNR ||
		FRAME_CTRL(f) == FRAME_S_REJ);
}

static bool valid_unnumbered_frame(const struct lapm_state *l, const frame_t *f)
{
	/* V.42 Table 8/8.2.4: fixed U headers permit no information field;
	 * FRMR carries five octets. Validate direction before dispatch, too. */
	switch (FRAME_CTRL(f) & 0xef) {
	case FRAME_U_SABME:
	case FRAME_U_DISC:
		return RX_IS_COMMAND(f) && f->count == 2;
	case FRAME_U_UA:
	case FRAME_U_DM:
		return !RX_IS_COMMAND(f) && f->count == 2;
	case FRAME_U_FRMR:
		return !RX_IS_COMMAND(f) && f->count == 7;
	case FRAME_U_TEST:
		if (!RX_IS_COMMAND(f))
			return false;
		/* Optional TEST and UI are currently ignored after validation. */
		/* fall through */
	case FRAME_U_UI:
	case FRAME_U_XID:
		return !FRAME_U_PF(f);
	default:
		return false;
	}
}

static void lapm_rx_complete(void *framer, frame_t *f)
{
	struct lapm_state *l = framer;
	unsigned type;

	/* V.42 8.1.3: invalid length or DLCI frames have no protocol effect.
	 * Check before logging or reading sequence fields from the HDLC buffer. */
	if (f == NULL || f->buf == NULL || f->count < 2 ||
	    (FRAME_ADDR(f) != l->cmd_addr && FRAME_ADDR(f) != l->rsp_addr))
		return;
	type = FRAME_TYPE(f);
	if (type != FRAME_U && (f->count < 3 ||
	    (l->state != LAPM_DATA && l->state != LAPM_ESTAB)))
		return;
	/* V.42 8.5.5/8.4.9.2: reject malformed frames before any link
	 * indication, acknowledgement, configuration or payload delivery. */
	if (type == FRAME_U ? !valid_unnumbered_frame(l, f) :
	    !valid_numbered_frame(l, f, type)) {
		EC_ERR("rejected frame: control=%02x length=%d\n",
		       FRAME_CTRL(f), f->count);
		if (l->state == LAPM_DATA)
			complete_link_release(l);
		return;
	}
	/* V.42 (2002) 8.3.2.1 permits valid I/S traffic after a lost UA.
	 * Validate it before completing SABME, and never bypass pending XID. */
	if (type != FRAME_U && l->state == LAPM_ESTAB &&
	    (l->config || !complete_link_establishment(l)))
		return;
	/* Valid numbered traffic proves the peer has left establishment. This
	 * history must survive sequence wrap and physical retraining. */
	if (type != FRAME_U)
		l->accept_sabme_retry = false;
	LAPM_PRINT_FRAME("rx",RX_IS_COMMAND(f),f,f->count);
	switch(type) {
	case FRAME_I:
		rx_info(l,f);
		break;
	case FRAME_S:
		if (RX_IS_COMMAND(f))
			rx_super_cmd(l,f);
		else
			rx_super_rsp(l,f);
		break;
	case FRAME_U:
		if (RX_IS_COMMAND(f))
			rx_unnum_cmd(l,f);
		else
			rx_unnum_rsp(l,f);
		break;
	default:
		EC_ERR("unknown frame type.\n");
		break;
	}
	/* Valid I/S traffic restarts idle supervision, without postponing an
	 * outstanding acknowledgement, enquiry, or peer-busy timer. */
	if (type != FRAME_U &&
	    l->state == LAPM_DATA && !l->config &&
	    (!l->modem->bit_timer || l->modem->bit_timer_func == t403_timeout))
		start_idle_timer(l);
}


/* lapm api functions */

static int reinitialize_for_link_establishment(struct lapm_state *l)
{
	if (l->v44_session.phase == LAPM_V44_READY) {
		/* V.42 7.2.2: omitting XID selects the defaults, including P0=0. */
		struct lapm_xid_config config = {
			.tx_win_size = LAPM_DFLT_WIN_SIZE,
			.rx_win_size = LAPM_DFLT_WIN_SIZE,
			.tx_info_size = LAPM_DFLT_INFO_SIZE,
			.rx_info_size = LAPM_DFLT_INFO_SIZE,
			.modem = DEFAULT_MODEM_EC_CONFIG,
		};
		apply_xid_config(l, &config);
	}
	if (l->v44_path != NULL &&
	    v44_character_path_reinitialize(l->v44_path) != V44_CHARACTER_PATH_OK)
		return -1;
	/* V.42bis 5.6: a new LAPM sequence initializes both compression histories. */
	if (l->modem->cfg.comp && modem_comp_reinitialize(l->modem) != 0)
		return -1;
	l->rx_head = l->rx_count = 0;
	l->modem->packer_process = NULL;
	reset_lapm_sequence(l);
	l->accept_sabme_retry = !l->modem->caller;
	return 0;
}

static int lapm_connect(struct lapm_state *l)
{
	if (l->state != LAPM_IDLE)
		return -1;
	if (reinitialize_for_link_establishment(l) != 0)
		return -1;

	/* connect */
	l->state = LAPM_ESTAB;
	if (TX_SABME(l) != 0) {
		l->state = LAPM_IDLE;
		return -1;
	}
	/* start t401 (and not t403) */
	T401_START(l);
	return 0;
}

/* negotiate params */
static int lapm_config(struct lapm_state *l)
{
	if (tx_xid_command(l) != 0)
		return -1;
	l->config = 1;
	if (l->state == LAPM_DATA) {
		l->busy = 1;
		TX_RNR(l,l->cmd_addr,1);
	}
	T401_START(l);
	return 0;
}

static int lapm_disconnect(struct lapm_state *l)
{
	/* Discard queued control traffic without reusing the frame that HDLC
	 * may still be serializing. DISC takes the next available wire slot. */
	l->ctrl_list = l->tx_ctrl;
	l->config = 0;
	l->rtx_count = 0;
	l->state = LAPM_DISC;
	TX_DISC(l);
	/* start T401 (and not T403) */
	T401_START(l);
	return 0;
}


static void reset_lapm_sequence(struct lapm_state *l)
{
	l->busy = 0;
	l->peer_busy = 0;
	l->retrain_poll = 0;
	l->accept_sabme_retry = false;
	l->stalled_retrain = false;
	l->reject = 0;
	l->vs = 0;
	l->va = 0;
	l->vr = 0;
	l->rtx_count = 0;
	/* discard pended data */
	// l->rx_head = l->rx_count = 0;
	/* discard info frames */
	l->sent_info = l->info_list;
	l->tx_info   = l->info_list;
	l->stalled_polls = 0;
	/* discard ctrl frames */
	// TBD ???
}

/* fixme: optimize mem usage */
static int alloc_frames(struct lapm_state *l)
{
#define arrsize(a) (sizeof(a)/sizeof((a)[0]))
	frame_t *f;
	size_t i;
	EC_DBG("alloc_frames: lapm size %zu (info %zu, ctrl %zu).\n",
	       sizeof(*l), sizeof(l->info_buf), sizeof(l->ctrl_buf));
	/* init info list */
	l->info_list = &l->info_buf[0].frame;
	for (i=0;i<arrsize(l->info_buf);i++) {
		f = &l->info_buf[i].frame;
		f->next = &l->info_buf[(i + 1) % arrsize(l->info_buf)].frame;
		f->buf  = l->info_buf[i].ctrl;
		f->size = sizeof(l->info_buf[0]) - sizeof(frame_t);
	}
	l->tx_info   = l->info_list;
	l->sent_info = l->info_list;
	/* init  ctrl list */
	l->ctrl_list = &l->ctrl_buf[0].frame;
	for (i=0;i<arrsize(l->ctrl_buf);i++) {
		f = &l->ctrl_buf[i].frame;
		f->next = &l->ctrl_buf[(i + 1) % arrsize(l->ctrl_buf)].frame;
		f->buf  = l->ctrl_buf[i].ctrl;
		f->size = sizeof(l->ctrl_buf[0]) - sizeof(frame_t);
	}
	l->tx_ctrl   = l->ctrl_list;
	return 0;
}


static int lapm_init(struct lapm_state *l, struct modem *m)
{
	struct lapm_v44_config v44_config;
	struct v44_offer local_offer;

	memset(l,0,sizeof(*l));
	l->modem = m;
	l->state      = LAPM_IDLE;
	l->busy       = 0;
	l->peer_busy  = 0;
	/* set originator/answer addresses */
	l->cmd_addr = m->caller ? 0x3 : 0x1;
	l->rsp_addr = m->caller ? 0x1 : 0x3;

	l->tx_win_size  = m->cfg.ec_tx_win_size;
	l->rx_win_size  = m->cfg.ec_rx_win_size;
	l->tx_info_size = m->cfg.ec_tx_info_size;
	l->rx_info_size = m->cfg.ec_rx_info_size;
	local_offer = v44_default_offer();
	local_offer.transmit.enabled =
		(m->cfg.comp_types & MODEM_COMP_TYPE_V44) != 0 &&
		(m->cfg.comp & 0x2U) != 0;
	local_offer.receive.enabled =
		(m->cfg.comp_types & MODEM_COMP_TYPE_V44) != 0 &&
		(m->cfg.comp & 0x1U) != 0;
	v44_config = (struct lapm_v44_config) {
		.enabled = v44_offer_has_direction(&local_offer),
		.native_v44_ready = true,
		.local_offer = local_offer,
	};
	if (lapm_v44_session_init(
			&l->v44_session, &v44_config) !=
			LAPM_V44_OK)
		return -1;

	alloc_frames(l);
	reset_lapm_sequence(l);
	return 0;
}

static int lapm_exit(struct lapm_state *l)
{
	if (l->modem != NULL)
		l->modem->packer_process = NULL;
	l->rx_head = l->rx_count = 0;
	v44_character_path_destroy(l->v44_path);
	l->v44_path = NULL;
	reset_lapm_sequence(l);
	l->info_list = 0;
	l->ctrl_list = 0;
	return 0;
}



/* modem ec interface func */
static void modem_ec_negotiate(struct modem *m)
{
	m->packer.hdlc.tx_complete  = lapm_tx_complete;
	m->packer.hdlc.get_tx_frame = lapm_get_tx_frame;
	if (lapm_config(&m->ec.lapm) != 0)
		modem_update_status(m, STATUS_EC_ERROR);
}

void modem_ec_start(struct modem *m)
{
	struct hdlc_state *h = &m->packer.hdlc;
	struct lapm_state *l = &m->ec.lapm;
	EC_DBG("modem_ec_start...\n");
	h->framer = l;
	h->tx_complete  = NULL;
	h->rx_complete  = lapm_rx_complete;
	h->get_tx_frame = NULL;
	if(m->caller) {
		/* V.42 requires at least 16 transmitted flags before the
		 * originator's first XID. Count wire flags rather than callbacks,
		 * and cover an answerer that finishes its required ADP reply first. */
		h->tx_preamble_flags = lapm_originator_preamble_flags(m);
		h->tx_preamble_complete = modem_ec_negotiate;
	}
	else {
		h->tx_complete  = lapm_tx_complete;
		h->get_tx_frame = lapm_get_tx_frame;
	}
	// fixme: improve BUSY becoming, remove packer_process()
	m->packer_process = NULL;
}

void modem_ec_stop(struct modem *m)
{
	struct hdlc_state *h = &m->packer.hdlc;
	struct lapm_state *l = &m->ec.lapm;
	EC_DBG("modem_ec_stop...\n");
	m->bit_timer = 0;
	h->tx_preamble_flags = 0;
	h->tx_preamble_complete = NULL;
	m->packer_process = NULL;
	lapm_disconnect(l);
}

bool modem_ec_take_retrain_request(struct modem *m)
{
	struct lapm_state *l;

	if (m == NULL || !m->cfg.ec || m->get_bits != modem_hdlc_get_bits ||
	    m->put_bits != modem_hdlc_put_bits)
		return false;
	l = &m->ec.lapm;
	if (l->state != LAPM_DATA || l->config || l->busy || l->peer_busy ||
	    m->bit_timer <= 0 || m->bit_timer_func != t401_timeout ||
	    (l->rtx_count < LAPM_RETRAIN_POLLS &&
	     l->stalled_polls < LAPM_RETRAIN_POLLS) ||
	    l->stalled_retrain)
		return false;
	/* A PCM frame-phase slip can produce legal symbols but no usable HDLC
	 * frames, even with no local information outstanding. Try physical
	 * recovery once before LAPM exhausts N400. Keep
	 * the queues and T401 progress, even if the datapump rejects the request.
	 * Acknowledgement progress or a recovered idle link rearms the allowance;
	 * starting or resuming retraining does not. Peer flow control is not a
	 * channel fault. */
	l->stalled_retrain = true;
	return true;
}

int modem_ec_resume_after_retrain(struct modem *m)
{
	struct lapm_state *l;

	if (m == NULL)
		return -1;
	if (m->get_bits == modem_detector_get_bits &&
	    m->put_bits == modem_detector_put_bits) {
		modem_detector_reframe(m);
		return 0;
	}
	if (!m->cfg.ec || m->get_bits != modem_hdlc_get_bits ||
	    m->put_bits != modem_hdlc_put_bits)
		return 0;
	l = &m->ec.lapm;
	if (l->modem != m || l->state < LAPM_IDLE || l->state > LAPM_DATA ||
	    l->ctrl_list == NULL ||
	    l->tx_ctrl == NULL)
		return -1;
	if (modem_hdlc_reframe(m) != 0)
		return -1;
	/* V.42 7.10.2/7.11 also suspend and resume protocol establishment.
	 * Keep XID/SABME, compression selection and T401 progress; an unfinished
	 * wire frame is recovered by the existing retransmission procedure. */
	if (l->state != LAPM_DATA) {
		if (m->packer.hdlc.tx_preamble_complete != NULL)
			m->packer.hdlc.tx_preamble_flags =
				lapm_originator_preamble_flags(m);
		return 0;
	}
	/* A physical interruption loses HDLC framing and leaves the peer's
	 * receive sequence uncertain only while flow control or unacknowledged
	 * information or a status enquiry remains. A clean retained window can
	 * continue directly. */
	if (l->retrain_poll) {
		l->peer_busy = 1;
		return 0;
	}
	if (!l->busy && !l->peer_busy && l->va == l->vs && !l->rtx_count)
		return 0;
	if (l->ctrl_list->next == l->tx_ctrl)
		return -1;
	l->peer_busy = 1;
	if ((l->busy ? TX_RNR(l, l->cmd_addr, 1) :
		      TX_RR(l, l->cmd_addr, 1)) != 0)
		return -1;
	T401_START(l);
	/* The first poll is already queued; its F=1 response is solicited. */
	l->rtx_count = 1;
	l->retrain_poll = 1;
	return 0;
}

int modem_ec_init(struct modem *m)
{
	struct lapm_state *l = &m->ec.lapm;
	return lapm_init(l,m);
}

void modem_ec_exit(struct modem *m)
{
	struct lapm_state *l = &m->ec.lapm;
	lapm_exit(l);
}

enum modem_ec_compression
modem_ec_active_compression(const struct modem *m)
{
	if (m == NULL || !m->cfg.ec)
		return MODEM_EC_COMPRESSION_NONE;
	if (m->ec.lapm.v44_path != NULL)
		return MODEM_EC_COMPRESSION_V44;
	return m->cfg.comp &&
	       (m->cfg.comp_types & MODEM_COMP_TYPE_V42BIS) != 0 ?
		       MODEM_EC_COMPRESSION_V42BIS :
		       MODEM_EC_COMPRESSION_NONE;
}
