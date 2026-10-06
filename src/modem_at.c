
/*
 *
 *    Copyright (c) 2001, Smart Link Ltd.
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
 *	modem_at.c  --  modem at command processor.
 *
 *	Author: Sasha K (sashak@smlink.com)
 *		Seva	  (seva@smlink.com)
 *
 */

/********************************************************************
 *
 * 	AT commands status. (p) - partiatly
 *
 * 	1. Implemented:
 * 	
 * 	AT : A B C D E H I L M N O P Q S T V X Y Z
 *	AT&: C D E F H P R S F W
 *	AT\: N
 * 	AT%: C E
 * 	AT+: DS DSC MS V34B V90D V90U V91 V92Q V92U FCLASS GCI GMI GMM GMR GCAP
 *      AT#: CID
 *	
 ********************************************************************
 *
 * 	2. Not implemented yet:
 *
 * 	AT+: DS44 DR
 *           FAE FTS FRS FTM FRM FRH FTH
 * 	AT&: A K V Z
 * 	AT\: A B K
 * 	AT*: B
 * 	AT#: BDR RG TL CLS SPK UD
 * 	     VBS VBT VIP VIT VLS VRA VRN 
 * 	     VRX VSD VSP VSR VSS VTD VTM 
 * 	     VTS VGT VTX
 *
 *****************************************************************************/

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include <modem.h>
#include <modem_frontend.h>
#include <modem_debug.h>
#include <pcm_rate.h>

#define AT_DBG(fmt...) // dprintf("at: " fmt)


/* ------------------------------------------------------------ */
/* prototypes from modem.c */
extern const char modem_author[];
extern const char modem_name[];
extern const char modem_version[];
extern const char modem_date[];

extern int  modem_send_to_tty(struct modem *m, const char *buf, int n);
extern int  modem_answer(struct modem *m);
extern int  modem_dial(struct modem *m);
extern int  modem_hook(struct modem *m,unsigned hook_state);
extern int  modem_reset(struct modem *m);
extern void modem_update_speaker(struct modem *m);
extern int  modem_homolog_init(struct modem *m, int id, const char *name);

/* ------------------------------------------------------------ */
/*
 *   local util functions
 */

/* simple toupper + strncmp */
static int toupper_strncmp(const char *s1, const char *s2, int n)
{
	int i;
	for(i = 0 ; i < n ; i++)
		if(toupper(s1[i]) != s2[i])
			return s1[i] - s2[i];
	return 0;
}


/* ------------------------------------------------------------ */

/* Return one for a value, zero for an omitted field, or -1 for invalid input.
 * Rejected or omitted fields leave the cursor and value unchanged. */
static int parse_unsigned(char **p, int *len, unsigned long *value, int base)
{
	char *end;
	const char *digits = *p;
	unsigned long parsed;
	int positive;

	while (isspace((unsigned char)*digits)) digits++;
	positive = *digits == '+';
	if (positive) digits++;
	if (*digits == '-') return -1;
	/* V.250 numbers are decimal. Retain explicit hexadecimal extensions. */
	if (base == 0)
		base = digits[0] == '0' && toupper((unsigned char)digits[1]) == 'X' ? 16 : 10;
	if (base == 16 && digits[0] == '0' &&
	    toupper((unsigned char)digits[1]) == 'X' &&
	    !isxdigit((unsigned char)digits[2]))
		return -1;
	errno = 0;
	parsed = strtoul(*p, &end, base);
	if (end == *p)
		/* With an omitted value, +<letter> begins the next command. */
		return positive && !isalpha((unsigned char)*digits) ? -1 : 0;
	if (errno == ERANGE)
		return -1;
	*value = parsed;
	*len += (int)(end - *p);
	*p = end;
	return 1;
}

/* Basic command parameters are one decimal number, not one digit. */
static int basic_number(char *p, int *len, unsigned int hi)
{
	unsigned long value = 0;
	char *digits = p;

	while (isspace((unsigned char)*digits)) digits++;
	/* +<letter> starts the next command; a signed basic value is invalid. */
	if (*digits == '+' && !isalpha((unsigned char)digits[1]))
		return -1;
	if (parse_unsigned(&p, len, &value, 10) < 0 || value > hi)
		return -1;
	return (int)value;
}

static int set_sreg(struct modem *m, int num, char *p, int hi, int *len)
{
	int value = basic_number(p, len, (unsigned int)hi);

	return value < 0 ? -1 : modem_set_sreg(m, num, value);
}

static int process_plus_DSC(struct modem *m, char *p, int *len)
{
	char response[16];
	unsigned long mask;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			const char capabilities[] = "(0-3)";

			modem_frontend_info(m, capabilities, (int)strlen(capabilities));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &mask, 0) != 1 ||
		    mask > MODEM_COMP_TYPE_ALL || *p == ',')
			return -1;
		return modem_set_sreg(m, SREG_COMP_TYPES, (int)mask);
	case '?':
		*len = 1;
		snprintf(response, sizeof(response), "%d",
			 modem_get_sreg(m, SREG_COMP_TYPES) &
				 MODEM_COMP_TYPE_ALL);
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

static int process_plus_DS(struct modem *m, char *p, int *len)
{
	char response[24];
	unsigned long direction;
	unsigned long mask;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			const char capabilities[] = "(0-3),(0-3)";

			modem_frontend_info(m, capabilities, (int)strlen(capabilities));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &direction, 0) != 1 || direction > 3)
			return -1;
		mask = (unsigned long)modem_get_sreg(m, SREG_COMP_TYPES) &
		       MODEM_COMP_TYPE_ALL;
		if (*p == ',') {
			p++;
			*len += 1;
			if (parse_unsigned(&p, len, &mask, 0) != 1 ||
			    mask > MODEM_COMP_TYPE_ALL || *p == ',')
				return -1;
		}
		if (modem_set_sreg(m, SREG_COMP, (int)direction) < 0 ||
		    modem_set_sreg(m, SREG_COMP_TYPES, (int)mask) < 0)
			return -1;
		return 0;
	case '?':
		*len = 1;
		snprintf(response, sizeof(response), "%d,%d",
			 modem_get_sreg(m, SREG_COMP),
			 modem_get_sreg(m, SREG_COMP_TYPES) &
				 MODEM_COMP_TYPE_ALL);
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

/* ------------------------------------------------------------ */


static int process_A(struct modem *m, char *p, int *len)
{
	int ret;
	AT_DBG("AT A command...\n");
	ret = modem_answer(m);
	if (ret)
		return -1;
	return 1;
}

static int process_B(struct modem *m, char *p, int *len)
{
	AT_DBG("AT B command...\n");
	MODEM_DP(m) = DP_V22;
	MODEM_AUTOMODE(m) = 0;
	m->min_rate = 1200;
	m->max_rate = 1200;
	return 0;
}

static int process_D(struct modem *m, char *p, int *len)
{
	int ret;
	AT_DBG("AT D command...\n");
	switch (toupper(*p)) {
	case 'T':
		modem_set_sreg(m,SREG_TONE_OR_PULSE,1);
		p++; /* Dial modifiers are not part of the SIP destination. */
		break;
	case 'P':
#ifdef NO_PULSE_DIAL
		return -1;
#endif
		modem_set_sreg(m,SREG_TONE_OR_PULSE,0);
		p++;
		break;
	case 'L':
		if(m->dial_string[0] != '\0') {
			modem_frontend_info_header(m);
			modem_send_to_tty(m,"Dialing ",7);
			modem_send_to_tty(m,m->dial_string,
						strlen(m->dial_string));
			modem_send_to_tty(m, "...", 3);
			modem_send_to_tty(m, CRLF_CHARS(m), 2);
		}
		p = 0;
		break;
	default:
		break;
	}
	if (p) {
		size_t length = strlen(p);

		if (length >= sizeof(m->dial_string))
			return -1;
		memcpy(m->dial_string, p, length + 1);
	}
	ret = modem_dial(m);
	if (ret)
		return -1;
	return 1;
}

static int process_H(struct modem *m, char *p, int *len)
{
	int value = basic_number(p, len, 1);

	if (value < 0)
		return -1;
	int result = modem_hook(m, value ? MODEM_HOOK_OFF : MODEM_HOOK_ON);
	return result < 0 ? -1 : result;
}

static int process_I(struct modem *m, char *p, int *len)
{
	const char *s;
	unsigned long selector = 0;
	int i;

	*len = 0;
	if (parse_unsigned(&p, len, &selector, 0) < 0)
		return -1;
	switch (selector) {
	case 1:
	case 2:
		modem_frontend_info_header(m);
		modem_send_to_tty(m,modem_name,strlen(modem_name));
		modem_send_to_tty(m,", ",2);
		modem_send_to_tty(m,modem_version,strlen(modem_version));
		modem_send_to_tty(m,CRLF_CHARS(m),2);
		modem_send_to_tty(m,modem_author,strlen(modem_author));
		modem_send_to_tty(m,CRLF_CHARS(m),2);
		break;
	case 3:
		modem_frontend_info_header(m);
		s = m->dev_name ? m->dev_name : "unknown";
		modem_send_to_tty(m, s, strlen(s));
		modem_send_to_tty(m, CRLF_CHARS(m),2);
		s = m->driver.name ? m->driver.name : "unknown";
		modem_send_to_tty(m, s, strlen(s));
		modem_send_to_tty(m, CRLF_CHARS(m),2);
		break;
	case 4:
		modem_frontend_info_header(m);
		for (i=0;i<24;i++) {
			char str[10];
			sprintf(str,"s%.2d=%.3d ",i,m->sregs[i]);
			modem_send_to_tty(m,str,strlen(str));
			if (!((i+1)%8))
				modem_send_to_tty(m, CRLF_CHARS(m),2);
		}
		modem_send_to_tty(m,CRLF_CHARS(m),2);
		break;
	case 5:
		modem_frontend_info(m, "Stored Profile 0:", 17);
		break;
	case 6:
		modem_frontend_info(m, "Stored Profile 1:", 17);
		break;
	case 7:
		modem_frontend_info_header(m);
		s = m->homolog->name;
		modem_send_to_tty(m,"Country: ",9);
		modem_send_to_tty(m,s,strlen(s));
		modem_send_to_tty(m, CRLF_CHARS(m),2);
		break;
	case 11:
		modem_frontend_call_report(m);
		break;
	case 0:
		modem_frontend_info(m, modem_name, strlen(modem_name));
		break;
	default:
		return -1;
	}
	return 0;
}

static int process_O(struct modem *m, char *p, int *len)
{
	int retrain = basic_number(p, len, 1);

	if (retrain < 0 || modem_online(m, retrain) != 0)
		return -1;
	return 1;
}

static int process_S(struct modem *m, char *p, int *len)
{
	unsigned long number = 0, value = 0;

	*len = 0;
	if (parse_unsigned(&p, len, &number, 0) < 0 || number >= sizeof(m->sregs))
		return -1;
	(*len)++;
	switch (*p++) {
	case '?': {
		int current = modem_get_sreg(m, (unsigned)number);
		char response[16];
		if (current < 0)
			return -1;
		snprintf(response, sizeof(response), "%03d", current);
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	}
	case '=':
		if (parse_unsigned(&p, len, &value, 0) < 0 || value > UCHAR_MAX)
			return -1;
		return modem_set_sreg(m, (unsigned)number, (int)value);
	default:
		return -1;
	}
}

static int process_Z(struct modem *m, char *p, int *len)
{
	return basic_number(p, len, 3) < 0 ? -1 : modem_reset(m);
}


/*
 *    AT+ commands
 */

static int process_plus_GCI(struct modem *m, char *p, int *len)
{
	char response[16];
	unsigned long country;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			(*len)++;
			modem_frontend_info_header(m);
			modem_send_to_tty(m, "+GCI:(", 6);
			for (const struct homolog_set *h = homolog_set; h->name; ++h) {
				snprintf(response, sizeof(response), "%s%x",
				         h == homolog_set ? "" : ",", h->id);
				modem_send_to_tty(m, response, (int)strlen(response));
			}
			modem_send_to_tty(m, ")", 1);
			modem_send_to_tty(m, CRLF_CHARS(m), 2);
		} else {
			/* The retained regional profile table also has 16-bit IDs. */
			if (parse_unsigned(&p, len, &country, 16) != 1 ||
			    country > UINT16_MAX || *p == ',')
				return -1;
			if (modem_homolog_init(m, (int)country, NULL) != 0)
				return -1;
		}
		return 0;
	case '?':
		snprintf(response, sizeof(response), "+GCI:%x", m->homolog->id);
		modem_frontend_info(m, response, (int)strlen(response));
		*len = 1;
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

static int process_plus_MS(struct modem *m, char *p, int *len)
{
	char strval[32];
	enum DP_ID selectable_id;
	unsigned long n;
	unsigned long values[4];
	int is_found=0;
		
	AT_DBG ("AT+MS...\n");
	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			*len += 1;
			modem_frontend_info_header(m);
			modem_send_to_tty(m,"(",1);
			for (size_t index = 0;
			     (selectable_id = modem_dp_selectable_id(index)) != 0; index++) {
				if(is_found++>0) modem_send_to_tty(m,",",1);
				sprintf(strval,"%d",selectable_id);
				modem_send_to_tty(m,strval,strlen(strval));
			}
			modem_send_to_tty(m,"),(0,1),",1+7);
			sprintf(strval,"(%u-%u),",MODEM_MIN_RATE,MODEM_MAX_RATE);
			modem_send_to_tty(m,strval,strlen(strval));
			modem_send_to_tty(m,strval,strlen(strval)-1);
			modem_send_to_tty(m, CRLF_CHARS(m),2);
			break;
		}

		values[0] = MODEM_DP(m);
		values[1] = MODEM_AUTOMODE(m);
		values[2] = m->min_rate;
		values[3] = m->max_rate;
		for (size_t index = 0; index < 4; index++) {
			if (parse_unsigned(&p, len, &values[index], 0) < 0)
				return -1;
			if (*p != ',')
				break;
			if (index == 3)
				return -1;
			(*len)++;
			p++;
		}
		/* Validate in the parsed width before narrowing into modem state. */
		if (values[0] > UINT_MAX || values[1] > 1 ||
		    values[2] > values[3] || values[2] < MODEM_MIN_RATE ||
		    values[3] > MODEM_MAX_RATE ||
		    (values[0] != MODEM_DP(m) &&
		     !modem_dp_is_selectable((enum DP_ID)values[0])))
			return -1;
		if (values[0] == DP_DS0 &&
		    !((values[2] <= 56000 && values[3] >= 56000) ||
		      (values[2] <= 64000 && values[3] >= 64000)))
			return -1;
		MODEM_DP(m) = (unsigned)values[0];
		MODEM_AUTOMODE(m) = (unsigned)values[1];
		m->min_rate = (unsigned)values[2];
		m->max_rate = (unsigned)values[3];
		break;
	case '?':
		n = sprintf(strval,"%d,%d,%u,%u",MODEM_DP(m),MODEM_AUTOMODE(m),
			    m->min_rate,m->max_rate);
		modem_frontend_info(m, strval, n);
		*len = 1;
		break;
	default:
		*len = 0;
		return -1;
	}
	return 0;
}


static int v34_baud_is_valid(unsigned long value)
{
	switch (value) {
	case 0:
	case 2400:
	case 2743:
	case 2800:
	case 3000:
	case 3200:
	case 3429:
		return 1;
	default:
		return 0;
	}
}

static int process_plus_V34B(struct modem *m, char *p, int *len)
{
	char response[16];
	unsigned long value;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			const char capabilities[] =
				"(0,2400,2743,2800,3000,3200,3429)";

			modem_frontend_info(m, capabilities, (int)strlen(capabilities));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &value, 10) != 1 ||
		    !v34_baud_is_valid(value) || *p == ',')
			return -1;
		m->v34_symbol_rate = (unsigned)value;
		return 0;
	case '?':
		*len = 1;
		snprintf(response, sizeof(response), "%u", m->v34_symbol_rate);
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

static int process_plus_choice_param(
	struct modem *m, char *p, int *len, unsigned int sreg)
{
	char response[8];
	unsigned long value;
	unsigned maximum = sreg == SREG_V90_DIGITAL_INDICATION ?
		MODEM_PCM_POLICY_AUTO : 1;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			const char *capabilities = maximum == MODEM_PCM_POLICY_AUTO ? "(0,1,2)" : "(0,1)";

			modem_frontend_info(m, capabilities, (int)strlen(capabilities));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &value, 0) != 1 ||
		    value > maximum || *p == ',')
			return -1;
		return modem_set_sreg(m, sreg, (int)value);
	case '?':
		*len = 1;
		snprintf(response, sizeof(response), "%d",
			 maximum == 1 ? modem_get_sreg_flag(m, sreg) : modem_get_sreg(m, sreg));
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

static int v90_upstream_rate_is_valid(unsigned long value)
{
	return value == 0 ||
	       (value >= MODEM_V90_UPSTREAM_MIN_RATE &&
		value <= MODEM_V90_UPSTREAM_MAX_RATE);
}

static int process_plus_V90U(struct modem *m, char *p, int *len)
{
	char response[96];
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long max_baud;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			snprintf(response, sizeof(response),
				 "(0,%d-%d),(0,%d-%d),"
				 "(0,2400,2743,2800,3000,3200,3429)",
				 MODEM_V90_UPSTREAM_MIN_RATE,
				 MODEM_V90_UPSTREAM_MAX_RATE,
				 MODEM_V90_UPSTREAM_MIN_RATE,
				 MODEM_V90_UPSTREAM_MAX_RATE);
			modem_frontend_info(m, response, (int)strlen(response));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &min_rate, 0) != 1 || *p != ',')
			return -1;
		p++;
		*len += 1;
		if (parse_unsigned(&p, len, &max_rate, 0) != 1)
			return -1;
		max_baud = m->v90_upstream_max_baud;
		if (*p == ',') {
			p++;
			*len += 1;
			if (parse_unsigned(&p, len, &max_baud, 0) != 1 || *p == ',')
				return -1;
		}
		if (!v90_upstream_rate_is_valid(min_rate) ||
		    !v90_upstream_rate_is_valid(max_rate) ||
		    !v34_baud_is_valid(max_baud) ||
		    (min_rate != 0 && max_rate != 0 && min_rate > max_rate))
			return -1;
		m->v90_upstream_min_rate = (unsigned)min_rate;
		m->v90_upstream_max_rate = (unsigned)max_rate;
		m->v90_upstream_max_baud = (unsigned)max_baud;
		return 0;
	case '?':
		*len = 1;
		snprintf(response, sizeof(response), "%u,%u,%u",
			 m->v90_upstream_min_rate, m->v90_upstream_max_rate,
			 m->v90_upstream_max_baud);
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}

static int process_plus_V92U(struct modem *m, char *p, int *len)
{
	char response[24];
	unsigned long enabled;
	unsigned long rate;
	unsigned long lu_ucode = 0;
	unsigned int max_drn = 0;

	switch (*p) {
	case '=':
		p++;
		*len = 1;
		if (*p == '?') {
			const char capabilities[] = "(0,1),(0,24000-48000),(0-127)";

			modem_frontend_info(m, capabilities, (int)strlen(capabilities));
			*len += 1;
			return 0;
		}
		if (parse_unsigned(&p, len, &enabled, 0) != 1 || enabled > 1)
			return -1;
		if (*p == ',') {
			p++;
			*len += 1;
			if (parse_unsigned(&p, len, &rate, 0) != 1 ||
			    enabled == 0 || rate > UINT32_MAX)
				return -1;
			max_drn = pcm_upstream_data_rate_number((uint32_t)rate);
			/* A zero host limit selects automatic rates; wire zero is cleardown. */
			if (rate != 0 && max_drn == 0)
				return -1;
			if (*p == ',') {
				p++;
				*len += 1;
				if (parse_unsigned(&p, len, &lu_ucode, 0) != 1 ||
				    lu_ucode > 127 || *p == ',')
					return -1;
			}
		}
		if (modem_set_sreg(
			    m, SREG_V92_PCM_UPSTREAM, (int)enabled) < 0 ||
		    modem_set_sreg(
			    m, SREG_V92_PCM_UPSTREAM_MAX_DRN, (int)max_drn) < 0)
			return -1;
		m->v92_upstream_lu_ucode = (unsigned)lu_ucode;
		return 0;
	case '?':
		*len = 1;
		max_drn = (unsigned int)modem_get_sreg(
			m, SREG_V92_PCM_UPSTREAM_MAX_DRN);
		if (max_drn == 0 && m->v92_upstream_lu_ucode == 0) {
			snprintf(response, sizeof(response), "%d",
				 modem_get_sreg_flag(m, SREG_V92_PCM_UPSTREAM));
		} else {
			rate = pcm_upstream_bit_rate((uint8_t)max_drn);
			snprintf(response, sizeof(response), "%d,%lu",
				 modem_get_sreg_flag(m, SREG_V92_PCM_UPSTREAM), rate);
		}
		if (m->v92_upstream_lu_ucode != 0) {
			size_t used = strlen(response);

			snprintf(response + used, sizeof(response) - used,
				 ",%u", m->v92_upstream_lu_ucode);
		}
		modem_frontend_info(m, response, (int)strlen(response));
		return 0;
	default:
		*len = 0;
		return -1;
	}
}


/* The data modem retains the class-zero compatibility command. */
static int process_plus_FCLASS(struct modem *m, char *p, int *len)
{
	unsigned long value;

	if (*p == '=') {
		p++;
		*len = 1;
		if (*p != '?')
			return parse_unsigned(&p, len, &value, 0) == 1 &&
			       value == 0 && *p != ',' ? 0 : -1;
		*len += 1;
	} else if (*p == '?') {
		*len = 1;
	} else {
		return -1;
	}
	modem_frontend_info(m, "0", 1);
	return 0;
}

/*
 *    Caller ID
 */

#ifdef MODEM_CONFIG_CID
static int process_CID(struct modem *m,  char *p, int *len)
{
	char ch;
	if(*p == '=' ) {
		p++;
		*len += 2;
		if(*p == '?') {
			modem_frontend_info(m, "0,1,2" , 5);
		}
		else if (isdigit(*p) && *p - '0' < 3) {
			m->cid_requested = *p - '0';
		}
		else
			return -1;
	}
	else if (*p == '?') {
		*len += 1;
		ch = '0' + m->cid_requested;
		modem_frontend_info(m, &ch , 1);
	}
	else
		return -1;
	return 0;
}
#endif


/* ------------------------------------------------------------ */

static int process_plus_G(struct modem *m, char *p, int *len)
{
	static const struct {
		const char *name, *information;
	} actions[] = {
		{ "MI", modem_author }, { "MM", modem_name }, { "MR", modem_version },
		{ "CAP", "+GCAP: +MS, +DS, +FCLASS" },
	};

	if (!toupper_strncmp(p, "CI", 2)) {
		int result = process_plus_GCI(m, p + 2, len);
		*len += 2;
		return result;
	}
	for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
		size_t length = strlen(actions[i].name);
		if (toupper_strncmp(p, actions[i].name, (int)length))
			continue;
		p += length;
		*len = (int)length;
		int test = p[0] == '=' && p[1] == '?';
		if (test) {
			p += 2;
			*len += 2;
		}
		if (*p && *p != ';' && *p != '+')
			return -1;
		if (!test)
			modem_frontend_info(m, actions[i].information,
				(int)strlen(actions[i].information));
		return 0;
	}
	return -1;
}

/*
 *   Main AT processor
 *
 */

int process_at_command(struct modem *m, char *cmd)
{
	char c, *p = cmd;
	int len = 0, ret = -1;

	/* verify 'AT' */
	if (toupper(*p++) != 'A' || toupper(*p++) != 'T')
		return -1;

	while(*p) {
		c = toupper(*p++);
		len = 0;
		ret = -1;
		/* These commands take no numeric parameter. Validate before acting. */
		if (strchr("ABPT", c) != NULL && isdigit((unsigned char)*p))
			return -1;
		switch(c) {

		case ';': /* Extended command separator. */
			ret = 0;
			break;
		case 'A': /* ATA */
			ret = process_A(m,p,&len);
			return ret;
		case 'B': /* ATB */
			ret = process_B(m,p,&len);
			break;
		case 'D': /* ATD */
			ret = process_D(m,p,&len);
			return ret;
		case 'E': /* ATE */
			ret = set_sreg(m,SREG_ECHO,p,1,&len);
			break;
		case 'H': /* ATH */
			ret = process_H(m,p,&len);
			if (ret != 0)
				return ret;
			break;
		case 'I': /* ATI */
			ret = process_I(m,p,&len);
			break;
		case 'L': /* ATL */
			ret = set_sreg(m,SREG_SPEAKER_VOLUME,p,3,&len);
			if(!ret) modem_update_speaker(m);
			break;
		case 'M': /* ATM */
			ret = set_sreg(m,SREG_SPEAKER_CONTROL,p,2,&len);
			if(!ret) modem_update_speaker(m);
			break;
		case 'N': /* ATN */
			ret = set_sreg(m,SREG_AUTOMODE,p,1,&len);
			break;
		case 'O': /* ATO */
			ret = process_O(m,p,&len);
			break;
		case 'P': /* ATP */
#ifdef NO_PULSE_DIAL
			return -1;
#endif
			ret = modem_set_sreg(m,SREG_TONE_OR_PULSE,0);
			break;
		case 'Q': /* ATQ */
			ret = set_sreg(m,SREG_QUIET,p,1,&len);
			break;
		case 'S': /* ATS */
			ret = process_S(m,p,&len);
			break;
		case 'T': /* ATT */
			ret = modem_set_sreg(m,SREG_TONE_OR_PULSE,1);
			break;
		case 'V': /* ATV */
			ret = set_sreg(m,SREG_VERBOSE,p,1,&len);
			break;
		case 'X': /* ATX */
			ret = set_sreg(m,SREG_X_CODE,p,4,&len);
			break;
		case 'Y': /* ATY */
			ret = set_sreg(m,SREG_DEFAULT_SETTING,p,3,&len);
			break;
		case 'Z': /* ATZ */
			ret = process_Z(m,p,&len);
			return ret;

		case '#': /* AT# */
#ifdef MODEM_CONFIG_CID
			if(!toupper_strncmp(p,"CID",3)) {
				p+= 3;
				ret = process_CID(m, p, &len);
			}
			else
#endif
				ret = -1;
			break;
		case '%': /* AT% */
			c = toupper(*p++);
			switch (c) {
			case 'C': /* AT%C */
				ret = set_sreg(m,SREG_COMP,p,3,&len);
				break;
			case 'E': /* AT%E */
				ret = set_sreg(m,SREG_LINE_QUALITY_CONTROL,p,2,&len);
				break;
			default:
				return -1;
			} /* end AT% */
			break;
		case '&':
			c = toupper(*p++);
			switch (c) {
			case 'A': /* AT&A */
				ret = set_sreg(m,SREG_CONNNECT_MSG_FORMAT,p,7,&len);
				break;
			case 'C': /* AT&C */
				ret = set_sreg(m,SREG_CD,p,1,&len);
				break;
			case 'D': /* AT&D */
			case 'R': /* AT&R */
			case 'S': /* AT&S */
				ret = basic_number(p, &len, 9) < 0 ? -1 : 0;
				break;
			case 'E': /* AT&E */
				ret = set_sreg(m,SREG_CONNNECT_MSG_SPEED_SRC,p,1,&len);
				break;
			case 'F': { /* AT&F */
				ret = basic_number(p, &len, 0) < 0 ? -1 : modem_factory_defaults(m);
				break;
			}
			case 'H':  /* AT&H */
				ret = set_sreg(m,SREG_FLOW_CONTROL,p,1,&len);
				break;
			case 'K': /* AT&K */
				ret = set_sreg(m,SREG_COMP,p,3,&len);
				break;
			case 'P': /* AT&P */
				ret = set_sreg(m,SREG_PULSE_RATIO,p,3,&len);
				break;
			case 'V': /* AT&V */
			case 'W': /* AT&W */
				ret = -1;
				break;
			default:
				return -1;
			}
			break;
		case '+':
			c = toupper(*p++);
			switch (c) {
			case 'D': /* AT+D */
				c = toupper(*p++);
				switch (c) {
				case 'S': /* AT+DS, AT+DSC, AT+DS0 */
					if (*p == '0') {
						p++;
						ret = process_plus_choice_param(m, p, &len, SREG_DS0_FRAMING);
					} else if (toupper(*p) == 'C') {
						p++;
						ret = process_plus_DSC(m, p, &len);
					} else {
						ret = process_plus_DS(m, p, &len);
					}
					break;
				default:
					return -1;
				}
				break;
			case 'G': /* AT+G - generic DCE control */
				ret = process_plus_G(m, p, &len);
				break;
			case 'F': /* AT+F */
				if(!toupper_strncmp(p,"CLASS",5)) {
					p+= 5;
					ret = process_plus_FCLASS(m, p, &len);
				}
				break;
			case 'M': /* AT+M */
				c = toupper (*p++);
				if (c == 'S')
					ret = process_plus_MS(m,p,&len);
				break;
			case 'V':
				if (!toupper_strncmp(p, "34B", 3)) {
					p += 3;
					ret = process_plus_V34B(m, p, &len);
					break;
				}
				if (!toupper_strncmp(p, "90D", 3)) {
					p += 3;
					ret = process_plus_choice_param(
						m, p, &len,
						SREG_V90_DIGITAL_INDICATION);
					break;
				}
				if (!toupper_strncmp(p, "90U", 3)) {
					p += 3;
					ret = process_plus_V90U(m, p, &len);
					break;
				}
				if (!toupper_strncmp(p, "91", 2)) {
					p += 2;
					ret = process_plus_choice_param(
						m, p, &len, SREG_V91_ENABLED);
					break;
				}
				if (!toupper_strncmp(p, "92Q", 3)) {
					p += 3;
					ret = process_plus_choice_param(
						m, p, &len,
						SREG_V92_QUICK_CONNECT);
					break;
				}
				if (!toupper_strncmp(p, "92U", 3)) {
					p += 3;
					ret = process_plus_V92U(m, p, &len);
					break;
				}
				return -1;
			default:
				return -1;
			}
			break;
		case '\\':
			c = toupper(*p++);
			switch (c) {
			case 'N': /* AT\N */
				ret = set_sreg(m,SREG_EC,p,5,&len);
				break;
			default:
				return -1;
			}
			break;
		default:
			AT_DBG("unknown command `%c` (%x).\n",c,c);
			return -1;
		}
		if (ret) return ret;
		p += len;
	}
	return 0;
}
