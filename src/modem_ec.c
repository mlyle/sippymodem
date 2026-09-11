
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
	LAPM_TIMER_RESPONSE_MARGIN_MS = 1000,
	LAPM_TIMER_FRAME_OVERHEAD_OCTETS = 7,
	LAPM_TIMER_STUFFED_BITS_PER_OCTET = 10,
	LAPM_TIMER_FALLBACK_BIT_RATE = 57600,
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

enum {
	/* The originator recognizes two ADPs before the answerer's robust reply
	 * is complete.  Keep flags on the wire for the remaining ADPs and then
	 * for the protocol preamble required before the first XID. */
	LAPM_ORIGINATOR_PREAMBLE_FLAGS =
		((V42_ANSWER_REPLY_PATTERNS -
		  V42_ORIGINATOR_MATCH_PATTERNS) *
		 V42_DETECTION_PATTERN_BITS + 7) / 8 +
		V42_PROTOCOL_PREAMBLE_FLAGS,
};

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
#if T403_IMPLEMENTED
#define TIMER_START(l)  { EC_DBG("T403 stop, T401 start.\n"); \
                          (l)->rtx_count = 0; \
			  (l)->modem->bit_timer = lapm_timer_timeout(l); \
                          (l)->modem->bit_timer_func = t401_timeout; }
#define TIMER_STOP(l)   { EC_DBG("T401 stop, T403 start.\n"); \
                          (l)->rtx_count = 0; \
			  (l)->modem->bit_timer = lapm_timer_timeout(l); \
                          (l)->modem->bit_timer_func = t403_timeout; }
#else /* !T403 */
#define TIMER_START(l) T401_START(l)
#define TIMER_STOP(l)  T401_STOP(l)
#endif /* !T403 */

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

static int reinitialize_for_link_establishment(struct lapm_state *l)
{
	if (l->v44_path != NULL &&
	    v44_character_path_reinitialize(l->v44_path) !=
		    V44_CHARACTER_PATH_OK)
		return -1;
	l->rx_head = l->rx_count = 0;
	l->modem->packer_process = NULL;
	reset_lapm_sequence(l);
	return 0;
}

/* le/be value parsing */
extern inline u32 lapm_get_le_val(const u8 *buf, int len)
{
	u32 val = 0;
	while(len--) {
		val <<= 8;
		val |= buf[len];
	}
	return val;
}

extern inline u32 lapm_get_be_val(const u8 *buf, int len)
{
	u32 val = 0;
	while(len--) {
		val <<= 8;
		val |= *buf++;
	}
	return val;
}

/* debug stuffs */
#define EC_ERR(fmt,arg...) eprintf("%s ec err: " fmt , \
                                   l->modem->name , ##arg)

#ifdef EC_DEBUG
#define EC_DBG(fmt,arg...) dprintf("%s ec: " fmt , \
                                   l->modem->name , ##arg)
#define EC_DBG1(fmt...) // EC_DBG(fmt)

static char print_buf[(LAPM_MAX_INFO_SIZE+3)*2];

/* frame debug prints */
static const char *get_frame_name(frame_t *f)
{
	if (!(f->buf[1]&0x1))
		return "INFO";
	switch(f->buf[1]&0xef) {
	case FRAME_S_RR:    return "RR";
	case FRAME_S_RNR:   return "RNR";
	case FRAME_S_REJ:   return "REJ";
	case FRAME_S_SREJ:  return "SREJ";
	case FRAME_U_SABME: return "SABME";
	case FRAME_U_DM:    return "DM";
	case FRAME_U_UI:    return "UI";
	case FRAME_U_DISC:  return "DISC";
	case FRAME_U_UA:    return "UA";
	case FRAME_U_FRMR:  return "FRMR";
	case FRAME_U_XID:   return "XID";
	case FRAME_U_TEST:  return "TEST";
	default:
		break;
	}
	return "invalid frame";
}

static void print_frame(struct lapm_state *l, char *title, int cmd, frame_t *f)
{
	int i;
	i = sprintf(print_buf,"(va/vs/vr %d/%d/%d) %s: %s %s",
		    l->va,l->vs,l->vr,
		    title,get_frame_name(f),cmd?"cmd":"rsp");
	switch (FRAME_TYPE(f)) {
	case FRAME_I:
		EC_DBG("%s[%d] cnt=%d, (ns/nr %d/%d)...\n",print_buf,
			 FRAME_I_PF(f),f->count-3,FRAME_NS(f),FRAME_NR(f));
		break;
	case FRAME_S:
		EC_DBG("%s[%d], (nr %d)...\n",print_buf,
			 FRAME_S_PF(f), FRAME_NR(f));
		break;
	case FRAME_U:
		EC_DBG("%s[%d]...\n",print_buf, FRAME_U_PF(f));
		break;
	default:
		EC_DBG("%s: unknown frame.\n", print_buf);
		break;
	}
}

#define LAPM_PRINT_FRAME(str,cmd,f) print_frame(l,str,cmd,f)

/* XID debug prints */
static int print_xid_params(char *str_buf, const u8 *buf, int len)
{
	u8 pid, plen;
	u32 pval;
	int i = 0;
	while (len > 0) {
		pid  = buf[0];
		plen = buf[1];
		buf += 2;
		len -= 2 + plen;
		if (len < 0)
			break;
		switch (pid) {
		case PI_HDLC_OPTFUNCS:
			i += sprintf(str_buf + i,"HDLC_OPTFUNC = 0x%x, ",
				     lapm_get_le_val(buf,plen));
			break;
		case PI_TX_INFO_MAXSIZE:
			pval = lapm_get_be_val(buf,plen);
			i += sprintf(str_buf + i,"TX_INFOSIZE = %u, ",pval>>3);
			break;
		case PI_RX_INFO_MAXSIZE:
			pval = lapm_get_be_val(buf,plen);
			i += sprintf(str_buf + i,"RX_INFOSIZE = %u, ",pval>>3);
			break;
		case PI_TX_WINDOW_SIZE:
			i += sprintf(str_buf + i,"TX_WINSIZE = %u, ",
				     lapm_get_be_val(buf,plen));
			break;
		case PI_RX_WINDOW_SIZE:
			i += sprintf(str_buf + i,"RX_WINSIZE = %u, ",
				     lapm_get_be_val(buf,plen));
			break;
		default:
			i += sprintf(str_buf + i,"??, ");
			break;
		}
		buf += plen;
	}
	i += sprintf(str_buf+i,"\n");
	return i;
}

static int print_xid_private_params(char *str_buf, const u8 *buf, int len)
{
	u8 pid, plen;
	int i = 0;
	while (len > 0) {
		pid  = buf[0];
		plen = buf[1];
		buf += 2;
		len -= 2 + plen;
		if (len < 0)
			break;
		switch (pid) {
		case PI_PARAMETER_SET_ID:
			i += sprintf(str_buf + i,"PARAM_SETID = 0x%x, ",
				     lapm_get_be_val(buf,plen));
			break;
		case PI_V42BIS_REQUEST:
			i += sprintf(str_buf + i,"V42BIS_REQUEST = 0x%x, ",
				     lapm_get_be_val(buf,plen));
			break;
		case PI_V42BIS_CW_NUMBER:
			i += sprintf(str_buf + i,"V42BIS_CWNUM = %d, ",
				     lapm_get_be_val(buf,plen));
			break;
		case PI_V42BIS_MAX_STRING:
			i += sprintf(str_buf + i,"V42BIS_MAXSTR = %d, ",
				     lapm_get_be_val(buf,plen));
			break;
		default:
			i += sprintf(str_buf + i,"??, ");
			break;
		}
		buf += plen;
	}
	i += sprintf(str_buf+i,"\n");
	return i;
}

static int print_xid(struct lapm_state *l,char *str,frame_t *f, int len)
{
	struct v42_xid_fields fields;
	int i = 0;

	if (len < 0 || v42_xid_decode(
			f->buf + 2, (size_t)len, &fields) !=
			V42_XID_OK) {
		EC_DBG("%s: malformed XID\n", str);
		return -1;
	}
	i = sprintf(print_buf,"XID: FID %02x;\n",
		      V42_XID_GENERAL_FORMAT);
	if (fields.parameter_negotiation != NULL) {
		i += sprintf(print_buf+i,"GI_PARAMS(%zu): ",
			     fields.parameter_negotiation_length);
		i += print_xid_params(
			print_buf+i, fields.parameter_negotiation,
			(int)fields.parameter_negotiation_length);
	}
	if (fields.private_negotiation != NULL) {
		i += sprintf(print_buf+i,"GI_PRIVATE_PARAMS(%zu): ",
			     fields.private_negotiation_length);
		i += print_xid_private_params(
			print_buf+i, fields.private_negotiation,
			(int)fields.private_negotiation_length);
	}
	if (fields.user_data != NULL)
		i += sprintf(print_buf+i,"GI_USER_DATA(%zu)\n",
			     fields.user_data_length);
	EC_DBG("%s : %s\n", str, print_buf);
	return 0;
}

#define LAPM_PRINT_XID(str,f,len) print_xid(l,str,f,len);

#else /* !LAPM_DEBUG */

#define EC_DBG(fmt...)
#define EC_DBG1(fmt...)
#define LAPM_PRINT_FRAME(str,cmd,f)
#define LAPM_PRINT_XID(str,f,len)

#endif /* !LAPM_DEBUG */



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
	l->ctrl_count--; // debug only
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


/* parse xid */
#define SET_PARAM(param,value,default) { \
	if (((value)<(default) && (param)>=(default)) || \
            ((value)>=(default) && (param)<(default))) \
                (param) = (default); \
	else if (((value)<(default) && (param)<(value)) || \
                 ((value)>=(default) && (param)>(value))) \
                (param) = (value); }

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
	XID_PARAMETER_LENGTH = 19,
	XID_PRIVATE_LENGTH = 15,
};

struct lapm_xid_config {
	unsigned int tx_win_size;
	unsigned int rx_win_size;
	unsigned int tx_info_size;
	unsigned int rx_info_size;
	struct modem_config modem;
};

static void parse_xid_params(
	struct lapm_xid_config *config, const u8 *buf, int len)
{
	u32 pval;
	u8 pid, plen;
	while (len > 0) {
		pid  = buf[0];
		plen = buf[1];
		buf += 2;
		len -= 2 + plen;
		if (len < 0)
			break;
		switch (pid) {
		case PI_HDLC_OPTFUNCS:
			break;
		case PI_TX_INFO_MAXSIZE:
			pval = lapm_get_be_val(buf,plen);
			pval >>= 3;
			SET_PARAM(config->tx_info_size,pval,LAPM_DFLT_INFO_SIZE);
			break;
		case PI_RX_INFO_MAXSIZE:
			pval = lapm_get_be_val(buf,plen);
			pval >>= 3;
			SET_PARAM(config->rx_info_size,pval,LAPM_DFLT_INFO_SIZE);
			break;
		case PI_TX_WINDOW_SIZE:
			pval = lapm_get_be_val(buf,plen);
			SET_PARAM(config->tx_win_size,pval,LAPM_DFLT_WIN_SIZE);
			break;
		case PI_RX_WINDOW_SIZE:
			pval = lapm_get_be_val(buf,plen);
			SET_PARAM(config->rx_win_size,pval,LAPM_DFLT_WIN_SIZE);
			break;
		default:
			break;
		}
		buf += plen;
	}
}


static void parse_xid_private_params(
	struct modem_config *cfg, const u8 *buf, int len)
{
	u8 pid, plen;
	while (len > 0) {
		pid  = buf[0];
		plen = buf[1];
		buf += 2;
		len -= 2 + plen;
		if (len < 0)
			break;
		switch (pid) {
		case PI_PARAMETER_SET_ID:
			//pval = lapm_get_be_val(buf,plen);
			break;
		case PI_V42BIS_REQUEST:
			cfg->comp = lapm_get_le_val(buf,plen);
			cfg->comp_types |= MODEM_COMP_TYPE_V42BIS;
			break;
		case PI_V42BIS_CW_NUMBER:
			cfg->comp_dict_size = lapm_get_be_val(buf,plen);
			break;
		case PI_V42BIS_MAX_STRING:
			cfg->comp_max_string = lapm_get_be_val(buf,plen);
			break;
		default:
			break;
		}
		buf += plen;
	}
}

static int decode_xid(
	struct lapm_state *l, frame_t *f, struct lapm_xid_config *config)
{
	struct v42_xid_fields fields;
	size_t length;

	if (f == NULL || f->buf == NULL || f->count < 2 || config == NULL)
		return -1;
	length = (size_t)f->count - 2U;
	LAPM_PRINT_XID("rx_xid",f,(int)length);
	if (v42_xid_decode(
			f->buf + 2, length, &fields) !=
			V42_XID_OK)
		return -1;
	*config = (struct lapm_xid_config) {
		.tx_win_size = l->tx_win_size,
		.rx_win_size = l->rx_win_size,
		.tx_info_size = l->tx_info_size,
		.rx_info_size = l->rx_info_size,
		.modem = DEFAULT_MODEM_EC_CONFIG,
	};
	if (fields.parameter_negotiation != NULL)
		parse_xid_params(
			config, fields.parameter_negotiation,
			(int)fields.parameter_negotiation_length);
	if (fields.private_negotiation != NULL)
		parse_xid_private_params(
			&config->modem, fields.private_negotiation,
			(int)fields.private_negotiation_length);
	config->modem.ec_tx_win_size = config->tx_win_size;
	config->modem.ec_rx_win_size = config->rx_win_size;
	config->modem.ec_tx_info_size = config->tx_info_size;
	config->modem.ec_rx_info_size = config->rx_info_size;
	return 0;
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
	const struct lapm_xid_config *config,
	u8 parameters[XID_PARAMETER_LENGTH],
	u8 private_parameters[XID_PRIVATE_LENGTH],
	struct v42_xid_fields *fields)
{
	static const u8 parameter_template[XID_PARAMETER_LENGTH] = {
		PI_HDLC_OPTFUNCS, 3, 0x8a, 0x89, 0x00,
		PI_TX_INFO_MAXSIZE, 2, 0, 0,
		PI_RX_INFO_MAXSIZE, 2, 0, 0,
		PI_TX_WINDOW_SIZE, 1, 0,
		PI_RX_WINDOW_SIZE, 1, 0,
	};
	static const u8 private_template[XID_PRIVATE_LENGTH] = {
		PI_PARAMETER_SET_ID, 3, 0x56, 0x34, 0x32,
		PI_V42BIS_REQUEST, 1, 0,
		PI_V42BIS_CW_NUMBER, 2, 0, 0,
		PI_V42BIS_MAX_STRING, 1, 0,
	};
	u32 pval;

	memcpy(parameters, parameter_template, sizeof(parameter_template));
	memcpy(private_parameters, private_template, sizeof(private_template));
	*fields = (struct v42_xid_fields) {
		.parameter_negotiation = parameters,
		.parameter_negotiation_length = XID_PARAMETER_LENGTH,
	};
	pval = config->tx_info_size<<3;
	parameters[7] = (pval>>8)&0xff;
	parameters[8] = pval&0xff;
	pval = config->rx_info_size<<3;
	parameters[11] = (pval>>8)&0xff;
	parameters[12] = pval&0xff;
	parameters[15] = config->tx_win_size;
	parameters[18] = config->rx_win_size;
	if (config->modem.comp &&
	    (config->modem.comp_types & MODEM_COMP_TYPE_V42BIS) != 0) {
		private_parameters[7] = config->modem.comp;
		pval = config->modem.comp_dict_size;
		private_parameters[10] = (pval>>8)&0xff;
		private_parameters[11] = pval&0xff;
		private_parameters[14] = config->modem.comp_max_string;
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
		l->ctrl_count++;
		return -1;
	}
	LAPM_PRINT_XID("tx_xid",f,(int)prepared->length);
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
			&config, parameters, private_parameters, &fields);
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

	if (decode_xid(l, f, &config) != 0)
		return -1;
	build_xid_fields(&config, parameters, private_parameters, &fields);
	if (lapm_v44_prepare_response(
			&l->v44_session, f->buf + 2, (size_t)f->count - 2U,
			&fields, &prepared) != LAPM_V44_OK)
		return -1;
	if (v44_offer_has_direction(&prepared.offer) && l->v44_path == NULL) {
		candidate = create_v44_path(l, &prepared.offer);
		if (candidate == NULL)
			return -1;
	}
	if (queue_xid(l, l->rsp_addr, &prepared) != 0) {
		v44_character_path_destroy(candidate);
		return -1;
	}
	if (candidate != NULL)
		l->v44_path = candidate;
	apply_xid_config(l, &config);
	return 0;
}

enum rx_xid_response_result {
	RX_XID_RESPONSE_ACCEPTED = 0,
	RX_XID_RESPONSE_IGNORED = -1,
	RX_XID_RESPONSE_FATAL = -2,
};

static enum rx_xid_response_result rx_xid_response(
	struct lapm_state *l, frame_t *f)
{
	struct lapm_xid_config config;
	struct v44_offer offer;

	if (decode_xid(l, f, &config) != 0 ||
	    lapm_v44_accept_response(
		    &l->v44_session, f->buf + 2,
		    (size_t)f->count - 2U) != LAPM_V44_OK)
		return RX_XID_RESPONSE_IGNORED;
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

	/* T401 is elapsed time stored in duplex bit units. Other users of the
	 * shared timer (notably the detector's mark preamble) count fixed bits. */
	if (m == NULL || m->bit_timer <= 0 || m->bit_timer_func != t401_timeout)
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

#if T403_IMPLEMENTED
static void t403_timeout(struct lapm_state *l)
{
	EC_DBG("t403_timeout...\n");
	if (l->state != LAPM_DATA) {
		EC_ERR("t403: no in data state.\n");
		return;
	}
	if (l->busy)
		TX_RNR(l,l->cmd_addr,1);
	else
		TX_RR(l,l->cmd_addr,1);
	T401_START(l);
	l->rtx_count = 1;
}
#endif


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
		l->info_count--; // debug
		l->tx_count++;   // debug
		LAPM_PRINT_FRAME("tx_info",1,f);
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
	l->tx_count += l->sent_count; // debug
	l->sent_count = 0;            // debug
	return n;
}

/* ack info */
static int ack_info(struct lapm_state *l, u8 nr)
{
	int n = 0;
	unsigned int outstanding = (l->vs - l->va) & 0x7f;
	unsigned int acknowledged = (nr - l->va) & 0x7f;

	/* V.42 8.5.2: V(A) <= N(R) <= V(S), in modulo-128 order.
	 * Validate the entire acknowledgement before releasing any frame. */
	if (acknowledged > outstanding || outstanding > l->tx_win_size) {
		EC_ERR("invalid acknowledgement %u outside window %u..%u\n",
		       nr, l->va, l->vs);
		lapm_disconnect(l);
		return -1;
	}
	while(l->va != nr) {
		if (l->sent_info == l->tx_info) { /* debug only ? */
			EC_ERR("ack info: VERY BAD: empty sent list: va %d, vs %d\n",
				 l->va,l->vs);
			break;
		}
		l->sent_info = l->sent_info->next;
		l->sent_count--; // debug
		l->info_count++; // debug
		l->va = (l->va+1)&0x7f;
		n++;
	}
	/* stop T401 , start T403 */
#if 1
	if(n > 0 && !l->rtx_count) {
#else
	if(n > 0 && !l->rtx_count && !l->peer_busy) {
#endif
		TIMER_STOP(l);
		if ((l->vs-l->va)&0x7f)
			TIMER_START(l); // exp.8.4.8
	}
	return n;
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
	if (l->rx_count && l->modem->put_chars) {
		int ret = l->modem->put_chars(l->modem,
					      (const char*)(l->rx_buf+l->rx_head),
					      l->rx_count);
		if (ret < 0 || ret > l->rx_count) {
			fail_receive_data(l);
			return;
		}
		if (ret > 0) {
			l->rx_head += ret;
			l->rx_count -= ret;
			if (!l->rx_count) {
				l->rx_head = 0;
				/* reset busy state */
				l->busy = 0;
				TX_RR(l,l->cmd_addr,0);
				l->modem->packer_process = NULL;
			}
		}
	}
}

/* info frame process */
static int rx_info(struct lapm_state *l, frame_t *f)
{
	int ret = 0;
	int payload_length;
	bool retrain_progress;
	//LAPM_PRINT_FRAME("rx_info",1,f);
	if (f->count < 3)
		return -1;
	payload_length = f->count - 3;

	/* ack I frames: nr -1 */
	if (ack_info(l,FRAME_NR(f)) < 0)
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
		if (l->reject) /* already sent */
			return -1;
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
	if (ret != payload_length || l->busy) {
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
	//LAPM_PRINT_FRAME("rx super:",1,f);

	/* note 8.4.7:
	   if l->busy each RR,RNR,REJ with p=1 should be replied by RNR with f=1 */
	switch(FRAME_CTRL(f)) {
	case FRAME_S_RR:
		/* clear peer_busy state */
		l->peer_busy = 0;
		if (ack_info(l,FRAME_NR(f)) < 0)
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
		if (ack_info(l,FRAME_NR(f)) < 0)
			return;
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
		if (ack_info(l,FRAME_NR(f)) < 0)
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
	case FRAME_S_SREJ:
		/* TBD, optional */
		EC_ERR("unsupported SREJ command!\n");
		return;
	default:
		EC_ERR("unknown s frame: %02x.\n",FRAME_CTRL(f));
		return;
	}
}

static void rx_super_rsp(struct lapm_state *l, frame_t *f)
{
	bool retrain_response = l->retrain_poll && l->rtx_count &&
		FRAME_S_PF(f);
	//LAPM_PRINT_FRAME("rx_super",0,f);

	if (!l->rtx_count && FRAME_S_PF(f)) {
		EC_ERR("rx_super: unsolisited final response!\n");
		return;
	}
	/* ack I frames <= nr -1 */
	switch(FRAME_CTRL(f)) {
	case FRAME_S_RR:
		l->peer_busy = 0;
		if (ack_info(l,FRAME_NR(f)) < 0)
			return;
		if (l->rtx_count && FRAME_S_PF(f)) {
			reject_info(l);
			TIMER_STOP(l);
		}
		break;
	case FRAME_S_RNR:
		l->peer_busy = 1;
		if (ack_info(l,FRAME_NR(f)) < 0)
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
		if (ack_info(l,FRAME_NR(f)) < 0)
			return;
		if (!l->rtx_count || FRAME_S_PF(f)) {
			reject_info(l);
			TIMER_STOP(l);
		}
		break;
	case FRAME_S_SREJ:
		/* TBD, optional */
		EC_ERR("unsupported SREJ response!\n");
		return;
	default:
		EC_ERR("rx_s_rsp: unknown header %02x.\n", FRAME_CTRL(f));
		return;
	}
	if (retrain_response)
		l->retrain_poll = 0;
}


/* rx unnumbered frame */

static int rx_unnum_cmd(struct lapm_state *l, frame_t *f)
{
	//LAPM_PRINT_FRAME("rx unnum:",1,f);
	switch(FRAME_CTRL(f)&0xef) {
	case FRAME_U_SABME:
	{
		bool already_linked = l->state == LAPM_DATA;

		EC_DBG("sabme received.\n");
		/* discard unacked I frames,
		   reset vs,vr,va, clear exceptions */
		if (reinitialize_for_link_establishment(l) != 0) {
			TX_DM(l,FRAME_U_PF(f));
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return -1;
		}
		if (TX_UA(l,FRAME_U_PF(f)) != 0) {
			l->state = LAPM_IDLE;
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return -1;
		}
		/* going to connected state */
		l->state = LAPM_DATA;
		TIMER_STOP(l);
		if (!already_linked)
			modem_update_status(l->modem, STATUS_EC_LINK);
		break;
	}
	case FRAME_U_UI:
		/* break signal */
		/* pf = 0 */
		/* TBD */
		break;
	case FRAME_U_DISC:
		/* respond UA (or DM) */
		if (l->state == LAPM_IDLE)
			TX_DM(l,1);
		else { /* going to disconnected state,
			  discard unacked I frames, reset all  */
			l->state = LAPM_IDLE;
			reset_lapm_sequence(l);
			T401_STOP(l);
			if (TX_UA(l,FRAME_U_PF(f)) != 0) {
				modem_update_status(l->modem, STATUS_EC_ERROR);
				return -1;
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
	case FRAME_U_TEST:
		/* TBD */
		/* optional */
		break;
	default:
		EC_ERR("rx unnum cmd: unknown frame 0x%02x\n", FRAME_CTRL(f));
		return -1;
	}
	return 0;
}

static int rx_unnum_rsp(struct lapm_state *l, frame_t *f)
{
	//LAPM_PRINT_FRAME("rx unnum",0, f);
	switch(FRAME_CTRL(f)&0xef) {
	case FRAME_U_DM:
		/* notify peer about disconnected state */
		/* TBD */
		switch(l->state) {
		case LAPM_ESTAB:
		case LAPM_DISC:
			if (FRAME_U_PF(f)) {
				l->state = LAPM_IDLE;
				reset_lapm_sequence(l);
				T401_STOP(l);
				// TBD notify CF
				modem_update_status(l->modem,STATUS_EC_RELEASE);
			}
			break;
		case LAPM_DATA:
			if (l->rtx_count || !FRAME_U_PF(f)) {
				l->state = LAPM_IDLE;
				reset_lapm_sequence(l);
				// TBD: notify CF
				modem_update_status(l->modem,STATUS_EC_RELEASE);
			}
			break;
		case LAPM_IDLE:
			if (!FRAME_U_PF(f)) {
				// TBD: notify CF
				// to establish connection
				modem_update_status(l->modem,STATUS_EC_LINK);
			}
			break;
		default:
			break;
		}
		break;
	case FRAME_U_UI:
		/* break signal */
		/* pf = 0 */
		/* TBD */
		break;
	case FRAME_U_UA:
		/* notify us about mode change, responds SABME,DISC */
		switch(l->state) {
		case LAPM_ESTAB:
			if (reinitialize_for_link_establishment(l) != 0) {
				l->state = LAPM_IDLE;
				T401_STOP(l);
				modem_update_status(l->modem, STATUS_EC_ERROR);
				break;
			}
			l->state = LAPM_DATA;
			TIMER_STOP(l);
			modem_update_status(l->modem,STATUS_EC_LINK);
			break;
		case LAPM_DISC:
			if (!FRAME_U_PF(f))
				break;
			l->state = LAPM_IDLE;
			reset_lapm_sequence(l);
			T401_STOP(l);
			modem_update_status(l->modem,STATUS_EC_RELEASE);
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
		/* not recovarable error */
		/* TBD */
		break;
	case FRAME_U_XID:
		if (l->config) {
			enum rx_xid_response_result result =
				rx_xid_response(l, f);

			if (result == RX_XID_RESPONSE_ACCEPTED) {
				l->config = 0;
				T401_STOP(l);
				if (l->state == LAPM_IDLE) {
					lapm_connect(l);
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
	default:
		EC_ERR("rx unnum rsp: unknown frame 0x%02x\n", FRAME_CTRL(f));
		break;
	}
	return 0;
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
	if (!l->modem->bit_timer)
		TIMER_START(l);
	//LAPM_PRINT_FRAME("get_tx_frame",1,f);

	//EC_DBG1("get_tx_frame: sent %d, tx %d, free %d...\n",
	//  	  l->sent_count,l->tx_count,l->info_count);
	return f;
}

static void lapm_tx_complete(void *framer, frame_t *f)
{
	struct lapm_state *l = framer;
	LAPM_PRINT_FRAME("tx",TX_IS_COMMAND(f),f);
	switch(FRAME_TYPE(f)) {
	case FRAME_I:
		l->sent_count++; // debug
		l->tx_count--;   // debug
		//EC_DBG1("tx_info_complete: sent %d, tx %d, free %d.\n",
		//	 l->sent_count,l->tx_count,l->info_count);
		break;
	case FRAME_S:
	case FRAME_U:
		l->ctrl_count++; // debug
		break;
	default:
		EC_ERR("unknown frame type.\n");
		break;
	}
}


static int valid_data_state(struct lapm_state *l)
{
	switch (l->state) {
	case LAPM_ESTAB:
		if (reinitialize_for_link_establishment(l) != 0) {
			l->state = LAPM_IDLE;
			T401_STOP(l);
			modem_update_status(l->modem, STATUS_EC_ERROR);
			return 0;
		}
		l->state = LAPM_DATA;
		modem_update_status(l->modem,STATUS_EC_LINK);
		return 1;
	case LAPM_DATA:
		return 1;
	case LAPM_DISC:
		/* V.42 8.7: ordinary traffic cannot acknowledge a DISC. */
		return 0;
	case LAPM_IDLE:
		return 0;
	default:
		EC_ERR("valid_data_state: unknown state %d.\n", l->state);
		return 0;
	}
	return 0;

}

static void lapm_rx_complete(void *framer, frame_t *f)
{
	struct lapm_state *l = framer;
	LAPM_PRINT_FRAME("rx",RX_IS_COMMAND(f),f);
	switch(FRAME_TYPE(f)) {
	case FRAME_I:
		if(!valid_data_state(l))
			return;
		rx_info(l,f);
		break;
	case FRAME_S:
		if(!valid_data_state(l))
			return;
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
}


/* lapm api functions */

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
	frame_t *queued = l->tx_ctrl;

	/* Discard queued control traffic without reusing the frame that HDLC
	 * may still be serializing. DISC takes the next available wire slot. */
	while (queued != l->ctrl_list) {
		queued = queued->next;
		l->ctrl_count++;
	}
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
	l->sent_count = l->tx_count = 0;
	l->info_count = LAPM_INFO_FRAMES;
	/* discard ctrl frames */
	// TBD ???
}

/* fixme: optimize mem usage */
static int alloc_frames(struct lapm_state *l)
{
#define arrsize(a) (sizeof(a)/sizeof((a)[0]))
	frame_t *f;
	size_t i;
	EC_DBG("alloc_frames: lapm size %d (info %d, ctrl %d).\n",
	       sizeof(*l), sizeof(l->info_buf), sizeof(l->ctrl_buf));
	/* init info list */
	l->info_list = &l->info_buf[0].frame;
	for (i=0;i<arrsize(l->info_buf);i++) {
		f = &l->info_buf[i].frame;
		f->next = &l->info_buf[(i + 1) % arrsize(l->info_buf)].frame;
		f->buf  = l->info_buf[i].ctrl;
		f->size = sizeof(l->info_buf[0]) - sizeof(frame_t);
	}
	l->info_count= arrsize(l->info_buf); // debug
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
	l->ctrl_count= arrsize(l->ctrl_buf); // debug
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
		h->tx_preamble_flags = LAPM_ORIGINATOR_PREAMBLE_FLAGS;
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
				LAPM_ORIGINATOR_PREAMBLE_FLAGS;
		return 0;
	}
	/* A physical interruption loses HDLC framing and leaves the peer's
	 * receive sequence uncertain only while flow control or unacknowledged
	 * information remains.  A clean retained window can continue directly. */
	if (l->retrain_poll) {
		l->peer_busy = 1;
		return 0;
	}
	if (!l->busy && !l->peer_busy && l->va == l->vs)
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
