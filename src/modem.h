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
 *    modem.h - modem definitions
 *
 *    Author: sashak@smlink.com
 */

#ifndef __MODEM_H__
#define __MODEM_H__

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#include <termios.h>
#endif

#include <modem_defs.h>
#include <modem_homolog.h>
#include <modem_dp.h>
#include <modem_timer.h>
#include <dc_remover.h>
#include <lapm_v44_session.h>
#include <hdlc_rx.h>

struct v44_character_path;


/* modem status */
#define STATUS_OK         0x0
#define STATUS_NOCARRIER  0x0002 // 3
#define STATUS_ERROR      0x0004 // 4
#define STATUS_NODIALTONE 0x0010 // 6
#define STATUS_BUSY       0x0020 // 7
#define STATUS_NOANSWER   0x0040 // 8
#define STATUS_DP_LINK    0x0100 // 13
#define STATUS_DP_ERROR   0x0200 // 14
#define STATUS_PACK_LINK  0x0400 // 15
#define STATUS_PACK_ERROR 0x0800 // 16
#define STATUS_EC_LINK    0x1000 // 17
#define STATUS_EC_RELEASE 0x2000 // 18
#define STATUS_EC_ERROR   0x4000 // 19
#define STATUS_RETRAIN_COMPLETE 0x8000
#define STATUS_EC_PEER_DISC 0x10000

enum modem_ec_compression {
	MODEM_EC_COMPRESSION_NONE = 0,
	MODEM_EC_COMPRESSION_V42BIS,
	MODEM_EC_COMPRESSION_V44,
};

#define MODEM_COMP_TYPE_NONE   0x00
#define MODEM_COMP_TYPE_V42BIS 0x01
#define MODEM_COMP_TYPE_V44    0x02
#define MODEM_COMP_TYPE_ALL \
	(MODEM_COMP_TYPE_V42BIS | MODEM_COMP_TYPE_V44)
#define MODEM_COMP_TYPE_DEFAULT MODEM_COMP_TYPE_ALL


/*
 *    parameters
 *
 */

#define MODEM_FORMAT MFMT_S16_LE
#define MODEM_RATE   8000
#define MODEM_FRAG   (MODEM_RATE/200)


#define MODEM_MIN_RATE 45
#define MODEM_MAX_RATE 64000
#define MODEM_V90_UPSTREAM_MIN_RATE 4800
#define MODEM_V90_UPSTREAM_MAX_RATE 33600

#define MODEM_DEFAULT_COUNTRY_CODE 0xb5 /* USA */

enum modem_tx_completion {
	MODEM_TX_OPEN, MODEM_TX_FINISHING, MODEM_TX_FINISHED, MODEM_TX_ABORTED,
};

/*
 *    type definitions
 *
 */


/* modem driver struct */
struct modem_driver {
	const char *name;
        int (*start)(struct modem *m);
        int (*stop )(struct modem *m);
	int (*ioctl)(struct modem *m,unsigned cmd, unsigned long arg);
};



/*
 *
 *    compressor definitions
 *
 */

#define COMP_MAX_CODEWORDS 2048  /* dictionary size   */
#define COMP_DEFAULT_STRING 34
#define COMP_MAX_STRING     250 /* V.42bis 5.1 maximum string length */

/* encoder and decoder states */
struct comp_state {
	const char *name;
	enum COMP_MODE {COMP_TRANSPARENT,COMP_COMPRESSED} mode;
	int escape;        /* escape state: decoder only */
	u16 update_at;     /* update dict. at node  */
	u16 next_free;     /* next free dict entry */
	u16 last_matched;  /* last matched dict entry */
	u16 last_added;    /* last added dict entry */
	u16 cw_size;       /* cw size */
	u32 threshold;     /* next cw size threshold, including 65536 */
	u8  escape_char;   /* escape char value */
	int max_string;    /* max string length */
	int dict_size;     /* dictionary size */
	struct dict {
		u8  ch;
		u16 parent;
		u16 child;
		u16 next;
	} *dict;           /* dictionary */
	/* fixme: union may be used? */
	int bit_len;
	u32 bit_data;
	int flushed_len;   /* string length already sent */
	int str_len;       /* string length */
	u8  str_data[COMP_MAX_STRING];
	/* test statistics info : encoder only */
	u16 cmp_last; /* compressed bits per last TEST_SLICE chars */
	u32 cmp_bits; /* total compressed bits */
	u32 raw_bits; /* total non-compressed bits */
	/* temporary */
	int  olen;
	int  ohead;
	char obuf[1024];
};


/* Swap local RX/TX bits with AT or initiator-relative TX/RX bits. */
static inline unsigned modem_comp_reverse_directions(unsigned directions)
{
	return ((directions & 1U) << 1) | ((directions & 2U) >> 1);
}

extern int  modem_comp_init(struct modem *m);
/* C-INIT for a fresh LAPM sequence; preserves the selected limits and storage. */
extern int  modem_comp_reinitialize(struct modem *m);
extern bool modem_comp_parameters_valid(int dict_size, int max_str);
extern int  modem_comp_config(struct modem *m, int dict_size, int max_str);
extern void modem_comp_exit(struct modem *m);

/* Drain queued encoded bytes before submitting another input octet.
 * encode consumes ch on success; pending output or no space is an error. */
extern int  modem_comp_read_encoder(struct modem *m, char *buf, int n);
extern int  modem_comp_encode(struct modem *m, u8 ch, char *buf, int n);
extern int  modem_comp_decode(struct modem *m, u8 ch, char *buf, int n);
extern int  modem_comp_flush_encoder(struct modem *m, char *buf, int n);
extern int  modem_comp_flush_decoder(struct modem *m, char *buf, int n);

/*
 *
 *    HDLC definitions
 *
 */

enum {
	V42_DETECTION_PATTERN_BITS = 36,
	/* V.42 7.2.1 permits 8 to 16 marks after each 10-bit character. */
	V42_DETECTION_MAX_MARK_BITS = 16,
	V42_DETECTION_MAX_PATTERN_BITS = 2 * (10 + V42_DETECTION_MAX_MARK_BITS),
	V42_ORIGINATOR_MATCH_PATTERNS = 2,
	V42_MINIMUM_ANSWER_PATTERNS = 10,
	/* A longer bounded reply gives a damaged pattern room to recover. */
	V42_ANSWER_REPLY_PATTERNS = 16,
	V42_PROTOCOL_PREAMBLE_FLAGS = 16,
};

typedef struct hdlc_frame {
	struct hdlc_frame *next;
	int count;
	int size;
	//u16 addr;
	//u16 ctrl;
	u16 fcs;
	u8 *buf;
} frame_t;



/*
 *    LAPM definitions
 *
 */

#define LAPM_MAX_WIN_SIZE    15  /* max supported winsize  */
#define LAPM_MAX_INFO_SIZE  128  /* max supported infosize */

#define LAPM_INFO_FRAMES    LAPM_MAX_WIN_SIZE + 1 /* number of info frames */
#define LAPM_CTRL_FRAMES      8  /* number of ctrl frames */

/* Retain the HDLC header and payload in one owned byte buffer. */
struct lapm_frame {
	frame_t frame;
	u8 data[3 + LAPM_MAX_INFO_SIZE];
};

enum lapm_reject_state {
	LAPM_REJECT_NONE,
	LAPM_REJECT_PENDING,
	LAPM_REJECT_SENT,
};

/* lapm state */
struct lapm_state {
        /* modem link */
        struct modem *modem;
        struct lapm_v44_session v44_session;
        struct v44_character_path *v44_path;
        int state;
        /* those DATA sub-states may be optimized */
        enum lapm_reject_state reject;
        int busy;
        int peer_busy;
        /* Initial retry count of the post-retrain enquiry; zero when absent. */
        int retrain_poll;
        bool accept_sabme_retry; /* answering DCE, before peer I/S traffic */
        bool stalled_retrain;
        u8 stalled_polls; /* status enquiries without acknowledgement progress */
        u8  cmd_addr;
        u8  rsp_addr;
        u8 vs;
        u8 va;
        u8 vr;
        int rtx_count; /* retransmission counter () */
        unsigned int tx_win_size;
        unsigned int rx_win_size;
        unsigned int tx_info_size;
        unsigned int rx_info_size;
        frame_t *sent_info;
        frame_t *tx_info;
        frame_t *info_list;
        frame_t *tx_ctrl;
        frame_t *ctrl_list;
        /* temporary: internal buffs */
        /* rx buf (used when enter busy state) */
        int rx_count;
        int rx_head;
        u8  rx_buf[LAPM_MAX_INFO_SIZE];
	struct lapm_frame info_buf[LAPM_INFO_FRAMES];
	struct lapm_frame ctrl_buf[LAPM_CTRL_FRAMES];
};


/* Latest accepted attempt, retained after teardown and reset. Diagnostic text
 * is copied while its datapump still exists; this owns no signaling pointers. */
struct modem_call_report {
	bool available, active, connected, outgoing;
	bool error_control, disconnect_local;
	enum modem_ec_compression compression;
	struct dp_link_rates rates;
	uint64_t media_samples, tx_dte_bytes, rx_dte_bytes;
	unsigned sample_rate, retrains;
	char modulation[24], phase[48], step[96], reason[128];
	char caller_number[128];
	unsigned result;
};

/* modem struct */
struct raw_text_probe {
	u8 bytes[128];
	unsigned int length;
	bool confirmed;
	u8 adp_next;
};

struct modem {
	const char *name;
	int pty; /* pty descriptor */
	const char *dev_name, *pty_name; /* and names */
#ifndef _WIN32
	struct termios termios;
#endif
	/* device driver */
	struct modem_driver driver;
	void *dev_data;
	/* Instance-owned byte sinks. Control replies and payload never share the
	 * library's receive queue. Return an exact accepted prefix count. */
	void *control_output_context;
	int (*control_output)(void *, const char *, int);
	void *data_output_context;
	int (*data_output)(void *, const char *, int);
	void (*frontend_report)(struct modem *, unsigned int);
	void (*frontend_escape)(struct modem *);
	bool dte_overflow;
        /* modem states */
        unsigned modem_info;
        unsigned state;
	unsigned started;
	unsigned command;
	unsigned retrain_pending;
	unsigned hangup_command_pending;
	unsigned result_code;
        unsigned hook;
        unsigned caller;
	char caller_number[128]; /* Current incoming identity; cleared at call end. */
	unsigned rx_rate;
	unsigned tx_rate;
	/* Pending frontend escape event. */
	unsigned event;
	unsigned new_event;
	unsigned long event_deadline;
	unsigned event_scheduled;
	/* modem parameters */
	unsigned min_rate;
	unsigned max_rate;
	unsigned v90_upstream_min_rate;
	unsigned v90_upstream_max_rate;
	unsigned v90_upstream_max_baud;
	/* Zero uses the profile's PCM upstream training level. */
	unsigned v92_upstream_lu_ucode;
	/* Media topology, retained across AT resets and call retraining. */
	bool pcm_codec_grid;
	/* Zero selects the V.34 symbol rate automatically. */
	unsigned v34_symbol_rate;
        unsigned char sregs[256];   /* s-registers */
        const struct homolog_set *homolog; /* homologation parameters */
        /* at processor */
        unsigned int at_count;
        char at_line[64];
        char at_cmd[64];
        /* dialer */
        char dial_string[128];
        /* escape counter */
        unsigned escape_count;
	unsigned long last_esc_check;
	/* xmit queue */
	struct xmit_queue {
		int count;
		unsigned int head;
		unsigned int tail;
		unsigned int size;
		unsigned char *buf;
	} xmit;
	/* run time configuration */
	struct modem_config {
		/* ec params */
		u8  ec;
		u8  ec_detector;
		u8  ec_tx_win_size;
		u8  ec_rx_win_size;
		u16 ec_tx_info_size;
		u16 ec_rx_info_size;
		/* compressor params */
		u8  comp;
		u8  comp_types;
		int comp_dict_size;
		int comp_max_string;
	} cfg;
        /* modem packer */
        int (*get_chars)(struct modem *m, char *buf, int n);
        int (*put_chars)(struct modem *m, const char *buf, int n);
	//int tx_window;
	//int rx_window;
	int bit_timer;
	void (*bit_timer_func)(struct modem *m);
	void (*packer_process)(struct modem *m, int bits);
	struct bits_state {
		int bit;
		u32 data;
	} pack,unpack;
	/* Per-call transmit completion; progressed by emitted samples. */
	enum modem_tx_completion tx_completion;
	int tx_finish_tail; /* -1 until the raw encoder/filter bound is latched */
        int (*get_bits)(struct modem *m, int nbits, u8 *buf, int n);
        int (*put_bits)(struct modem *m, int nbits, u8 *buf, int n);
	union packer_union {
		struct async_state {
			unsigned int receive_state;
			unsigned int receive_bit_count;
			u8 receive_data;
			u8 receive_zero_run;
			u8 receive_pending_nuls;
			unsigned int detection_residue_state;
			u8 detection_residue_character;
			u8 detection_residue_pattern[2];
			int transmit_mark_bits;
		} async;
		struct detect_state {
			int tx_count;
			int tx_limit;
			u8 tx_pattern[2];
			int rx_count;
			u8 rx_pattern[2];
			int ec_enable;
			/* Hold a short text prefix until raw mode is unambiguous. */
			struct raw_text_probe text;
		} detector;
		struct hdlc_state {
			struct hdlc_frame *tx_frame;
			struct hdlc_frame *rx_frame;
			struct hdlc_rx rx;
			struct raw_text_probe raw_text;
			struct bits_state raw_bits;
			bool raw_fallback_disabled;
			int tx_ones;
			unsigned int tx_preamble_flags;
			void (*tx_preamble_complete)(struct modem *m);
			/* framer interface */
			struct hdlc_frame *(*get_tx_frame)(void *framer);
			void (*tx_complete)(void *framer, frame_t *fr);
			void (*rx_complete)(void *framer, frame_t *fr);
			void *framer;
			/* temporary: internal bufs */
			struct hdlc_frame _rx_frame;
			u8 _rx_buf[512];
		} hdlc;
	} packer;
	/* error corrector */
	union {
		struct lapm_state lapm;
	} ec;
	/* compressor */
	struct comp_struct {
		// fixme: redo interface
		int head,count;
		bool transmit_dirty;
		char buf[256];
		struct comp_state encoder;
		struct comp_state decoder;
	} comp;
        /* modem data pump interface */
        unsigned srate;
        unsigned format;
        unsigned frag;
        unsigned dp_requested;
	struct dc_remover receive_dc;
	void (*process)(struct modem *m,void *in,void *out,int cnt);
        /* process counter and sample timer */
        unsigned long count;
        unsigned long sample_timer;
	void (*sample_timer_func)(struct modem *);
        /* data pump */
        struct dp *dp;
	/* First call-end cause survives datapump deletion until the next call.
	 * Strings are borrowed process-lifetime literals. */
	const char *disconnect_reason;
	const char *disconnect_step;
	const char *disconnect_phase;
	unsigned int disconnect_sequence;
	bool disconnect_local;
	struct modem_call_report call_report;
	/* dsp info data */
	struct dsp_info dsp_info;
};



/*
 *    prototypes
 *
 */

/* device driver --> modem */
extern struct modem *modem_create(struct modem_driver *drv, const char *dev_name);
extern void modem_delete(struct modem *m);
extern int modem_write(struct modem *m, const char *buf, int n);
extern int modem_data_write(struct modem *m, const void *bytes, size_t length);
/* Seal literal input. 0 complete, 1 pending, -1 unavailable/invalid. */
extern int modem_finish_transmit(struct modem *m);
extern size_t modem_receive_pending(const struct modem *m);
extern int modem_drain_receive(struct modem *m, unsigned budget);
extern int modem_set_dte_command(struct modem *m, bool command);
/* Restore the profile while preserving carrier, buffers and country. */
extern int modem_factory_defaults(struct modem *m);
extern void modem_schedule_escape(struct modem *m);
extern int modem_send_to_tty(struct modem *m, const char *bytes, int count);
extern int modem_recv_from_tty(struct modem *m, char *bytes, int count);
extern void modem_hangup(struct modem *m);
extern int modem_stop_now(struct modem *m);
extern int modem_answer(struct modem *m);
extern int modem_dial(struct modem *m);
extern int modem_online(struct modem *m, int retrain);
/* Publish directional physical rates together, preserving data-link timers. */
extern int modem_set_link_rates(struct modem *m, uint32_t transmit_rate, uint32_t receive_rate);
/* Borrowed static name of the host call phase; no state is changed. */
extern const char *modem_call_phase(const struct modem *m);
extern const char *modem_protocol_step(const struct modem *m);
/* Retain the earliest cause; reason must be a process-lifetime literal. */
extern void modem_note_disconnect(struct modem *m, bool local, const char *reason);
/* Refresh active negotiated facts; completed reports remain immutable. */
extern bool modem_get_call_report(struct modem *m, struct modem_call_report *report);
extern void modem_set_caller_number(struct modem *m, const char *number);
/* Copy a native owner's final cause, including calls still ringing. */
extern void modem_finish_call_report(struct modem *m, bool local, const char *reason, unsigned result);
extern int modem_report_incoming_call(struct modem *m);
extern int modem_report_outgoing_call_connected(struct modem *m);
extern int modem_report_remote_call_end(struct modem *m);
#ifndef _WIN32
extern void modem_update_termios(
	struct modem *m, const struct termios *tios);
#endif
extern void modem_error  (struct modem *m);
extern void modem_event  (struct modem *m);
/* Promote and service one due instance-owned modem event. */
extern int modem_service_event(struct modem *m);
extern void modem_process(struct modem *m,void *in,void *out,int cnt);
extern void modem_process_g711(struct modem *m, enum g711_law law,
			       const u8 *receive_codewords,
			       void *receive_samples, const bool *received_mask,
			       u8 *transmit_codewords,
			       void *transmit_samples, int cnt,
			       bool *exact_output);

/* packers && EC */ // FIXME: improve interface
extern void modem_async_start(struct modem *m);
extern void modem_async_reframe(struct modem *m);
extern void modem_async_discard_transmit(struct modem *m);
/* Preserve pending transmit bits while abandoning a partial receive byte. */
extern void modem_async_retrain(struct modem *m);
extern int  modem_async_get_bits(struct modem *m,int nbits,u8 *buf,int cnt);
extern int  modem_async_put_bits(struct modem *m,int nbits,u8 *buf,int cnt);
extern int modem_link_supports_v42(const struct modem *m);
extern void modem_detector_start(struct modem *m);
extern void modem_detector_reframe(struct modem *m);
extern int  modem_detector_get_bits(struct modem *m,int nbits,u8 *buf,int cnt);
extern int  modem_detector_put_bits(struct modem *m,int nbits,u8 *buf,int cnt);
extern void modem_hdlc_start(struct modem *m);
extern int  modem_hdlc_reframe(struct modem *m);
extern int  modem_hdlc_get_bits(struct modem *m,int nbits,u8 *buf,int cnt);
extern int  modem_hdlc_put_bits(struct modem *m,int nbits,u8 *buf,int cnt);

extern int  modem_ec_init(struct modem *m);
extern void modem_ec_exit(struct modem *m);
extern void modem_ec_start(struct modem *m);
extern void modem_ec_stop(struct modem *m);
/* Borrow the current detection/LAPM exchange label, or NULL outside control. */
extern const char *modem_ec_protocol_step(const struct modem *m);
/* Whether the host still accepts LAPM establishment for this call. */
extern bool modem_ec_can_establish(const struct modem *m);
/* Consume a bounded request; the host invokes retrain outside DSP callbacks. */
extern bool modem_ec_take_retrain_request(struct modem *m);
extern int modem_ec_resume_after_retrain(struct modem *m);
extern bool modem_ec_transmit_idle(const struct modem *m);
extern void modem_ec_update_timer_rate(struct modem *m, uint64_t previous_rate);
extern enum modem_ec_compression
modem_ec_active_compression(const struct modem *m);
/* Returns a process-lifetime literal, or NULL when no datapump is active. */
extern const char *modem_active_modulation_name(const struct modem *m);

/* status & config */
extern void modem_dp_connected(struct modem *m);
extern void modem_dp_reconnected(struct modem *m);
extern void modem_update_status(struct modem *m, unsigned status);

/* modem parameters access macros */
#define CRLF_CHARS(m)     ((char *)((m)->sregs+SREG_CR_CHAR))
#define MODEM_DP(m)       ((m)->sregs[SREG_DP])
#define MODEM_AUTOMODE(m) ((m)->sregs[SREG_AUTOMODE])

#endif /* __MODEM_H__ */
