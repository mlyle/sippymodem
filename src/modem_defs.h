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
 *    modem_defs.h - modem common definitions
 *
 *    Author: sashak@smlink.com
 */

#ifndef __MODEM_DEFS_H__
#define __MODEM_DEFS_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else

#include <sys/types.h>

typedef __uint8_t  u8;
typedef __uint16_t u16;
typedef __uint32_t u32;

typedef __int8_t  s8;
typedef __int16_t s16;
typedef __int32_t s32;
#endif /* !__KERNEL__ */

/* hook states */
#define MODEM_HOOK_ON       0
#define MODEM_HOOK_OFF      1

/* modem device ctrl interface */
#define MDMCTL_CAPABILITIES  0x01
#define MDMCTL_HOOKSTATE     0x02
#define MDMCTL_SPEED         0x04
#define MDMCTL_GETFMTS       0x05
#define MDMCTL_SETFMT        0x06
#define MDMCTL_SETFRAG       0x07
#define MDMCTL_SETFRAGMENT   MDMCTL_SETFRAG
#define MDMCTL_SPEAKERVOL    0x08
#define MDMCTL_IODELAY       0x10
#define MDMCTL_CODECTYPE     0x11
#define MDMCTL_PULSEDIAL     0x12
#define MDMCTL_GETSTAT       0x20
#define MDMCTL_START         0x21
#define MDMCTL_STOP          0x22


/* modem device status mask */
#define MDMSTAT_ERROR 0x1
#define MDMSTAT_DATA  0x4


/* codec types - numbers important */
enum codec_types {
	CODEC_UNKNOWN = 0,
	CODEC_AD1821  = 1,
	CODEC_STLC7550= 4,
	CODEC_SILABS  = 9,
	CODEC_SIL3052 = 10,
	CODEC_SIL3054 = 11,
	CODEC_AD1803  = 12,
	CODEC_ALC545A = 13,
};

/* sample formats + extensions */
#define MFMT_SIZE_MASK   0x000f
#define MFMT_SIZE_8      0x0000
#define MFMT_SIZE_16     0x0001
#define MFMT_SIGN_MASK   0x00f0
#define MFMT_SIGNED      0x0010
#define MFMT_UNSIGNED    0x0000
#define MFMT_ORDER_MASK  0x0f00
#define MFMT_ORDER_LE    0x0100
#define MFMT_ORDER_BE    0x0000

#define MFMT_QUERY       0x0

#define MFMT_S8          (MFMT_SIZE_8|MFMT_SIGNED)
#define MFMT_U8          (MFMT_SIZE_8|MFMT_UNSIGNED)
#define MFMT_S16_LE      (MFMT_SIZE_16|MFMT_SIGNED|MFMT_ORDER_LE)
#define MFMT_S16_BE      (MFMT_SIZE_16|MFMT_SIGNED|MFMT_ORDER_BE)
#define MFMT_U16_LE      (MFMT_SIZE_16|MFMT_UNSIGNED|MFMT_ORDER_LE)
#define MFMT_U16_BE      (MFMT_SIZE_16|MFMT_UNSIGNED|MFMT_ORDER_BE)

#define MFMT_IS_16BIT(x)   ((x)&MFMT_SIZE_16)
#define MFMT_IS_LE(x)      ((x)&MFMT_ORDER_LE)
#define MFMT_BYTESSHIFT(x) ((x)&MFMT_SIZE_MASK)
#define MFMT_SHIFT(x)      (MFMT_BYTESSHIFT(x))
#define MFMT_BYTES(x)      (1<<MFMT_BYTESSHFIT(x))


/* s-registers */
enum S_REGISTER {
        SREG_RINGS_TO_AUTO_ANSWER         =  0,
        SREG_RING_COUNTER                 =  1,
        SREG_ESCAPE_CHAR                  =  2,
        SREG_CR_CHAR                      =  3,
        SREG_LF_CHAR                      =  4,
        SREG_BS_CHAR                      =  5,
        SREG_DIAL_TONE_WAIT_TIME          =  6,
        SREG_WAIT_CARRIER_AFTER_DIAL      =  7,
        SREG_DIAL_PAUSE_TIME              =  8,
        SREG_CARRIER_DETECT_RESPONSE_TIME =  9,
        SREG_CARRIER_LOSS_DISCONNECT_TIME = 10,
        SREG_DTMF_DURATION                = 11,
        SREG_ESCAPE_PROMPT_DELAY          = 12,
        SREG_ECHO                         = 13,
        SREG_QUIET                        = 14,
        SREG_VERBOSE                      = 15,
        SREG_TONE_OR_PULSE                = 16,
        SREG_TEST_TIMER                   = 18,
        SREG_INACTIVITY_TIMEOUT           = 19,
        SREG_BREAK_LENGTH                 = 21,
        SREG_ORIGIN_OR_ANSWER             = 22,
        SREG_XOFF_CHAR                    = 23,
        SREG_FLASH_TIMER                  = 24,
        SREG_DELAY_TO_DTR_OFF             = 25,  /* (NA) */
        SREG_RTS_TO_CTS_DELAY             = 26,  /* (NA) */
        SREG_AUTOANSWER_CLEAR_TIMEOUT     = 27,
        SREG_PULSE_RATIO                  = 28,
        SREG_SPEAKER_CONTROL              = 29,
        SREG_SPEAKER_VOLUME               = 30,
        SREG_SPEAKER_LEVEL                = 30,
        SREG_AUTOMODE                     = 31,
        SREG_DP                           = 32,

        SREG_LINE_QUALITY_CONTROL         = 39,
        SREG_X_CODE                       = 56,
        SREG_CD                           = 60,    /* (&CN)    */
        SREG_FLOW_CONTROL                 = 62,    /* (&HN)    */
        SREG_CONNNECT_MSG_FORMAT          = 70,
        SREG_CONNNECT_MSG_SPEED_SRC       = 71,
        SREG_ANS_DELAY                    = 92,
	/* new sgregs */
	SREG_EC                           = 103,
	SREG_COMP                         = 104,
	SREG_COMP_TYPES                   = 105,

        SREG_DEFAULT_SETTING              = 161, // AT used
	SREG_V90_DIGITAL_INDICATION       = 222,
	SREG_V92_QUICK_CONNECT            = 224,
	SREG_V92_PCM_UPSTREAM             = 225,
	SREG_V92_PCM_UPSTREAM_MAX_DRN     = 226,
	SREG_V91_ENABLED                 = 227,
	SREG_DS0_FRAMING                 = 228,
};


/* data pumps identifiers */
enum DP_ID {
        DP_CALLPROG = 2,
        DP_DUMMY = 3,
        DP_AUTOMODE = 4,
        DP_V8 = 8,
	DP_V17 = 17,
	DP_V21 = 21,
        DP_V22 = 22,
        DP_V23 = 23,
        DP_V22BIS = 122,
        DP_V32 = 32,
        DP_V32BIS = 132,
        DP_V34 = 34,
        DP_V34BIS = 134,
        DP_BAUDOT = 45,
        DP_B103 = 103,
        DP_B212 = 212,
        DP_FAX = 100,
        DP_K56 = 56,
        DP_DS0 = 64,
        DP_K56_OR_V90 = 65,
        DP_V8BIS = 108,
        DP_V90 = 90,
        DP_V91 = 91,
        DP_V92 = 92,
        DP_TEST  = 126,
        DP_SINUS = 127,
	DP_MAX = 128,
        MAX_DP = 128
};

/* AT+V90D policy; V.8 selects complementary roles in automatic mode. */
enum modem_pcm_policy {
	MODEM_PCM_POLICY_ANALOG = 0,
	MODEM_PCM_POLICY_DIGITAL = 1,
	MODEM_PCM_POLICY_AUTO = 2,
};

enum modem_pcm_role {
	MODEM_PCM_ROLE_NONE,
	MODEM_PCM_ROLE_ANALOG,
	MODEM_PCM_ROLE_DIGITAL,
};

enum modem_upstream_mode {
	MODEM_UPSTREAM_UNKNOWN,
	MODEM_UPSTREAM_V34,
	MODEM_UPSTREAM_PCM,
};

/* dp status */
#define DPSTAT_OK         0
#define DPSTAT_CONNECT    1
#define DPSTAT_ERROR      4
#define DPSTAT_NODIALTONE 6
#define DPSTAT_BUSY       7
#define DPSTAT_NOANSWER   8
#define DPSTAT_CHANGEDP   10
#define DPSTAT_DISCONNECT 11

enum modem_startup {
	MODEM_STARTUP_UNKNOWN,
	MODEM_STARTUP_V8,
	MODEM_STARTUP_QC,
	MODEM_STARTUP_V8_FALLBACK,
};

/* dsp info structure */
struct dsp_info {
	unsigned connection_type;
	long     clock_deviation;
	/* Both V.8 menus or QC1/QCA1 records selected LAPM. */
	unsigned v8_lapm;
	enum modem_startup startup;
	enum modem_pcm_role pcm_role;
	/* Negotiated PCM-session upstream, not the configured capability. */
	enum modem_upstream_mode upstream_mode;
};


/*
 *    prototypes
 *
 */

struct modem;

extern int modem_get_sreg(struct modem *m, unsigned int num);
extern int modem_set_sreg(struct modem *m, unsigned int num, int val);

/* Read a register as a flag while preserving an access error. */
static inline int modem_get_sreg_flag(struct modem *m, unsigned int num)
{
	int value = modem_get_sreg(m, num);
	return value < 0 ? value : value != 0;
}


#endif /* __MODEM_DEFS_H__ */
