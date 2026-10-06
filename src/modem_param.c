
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
 *    modem_param.c  --  modem parameters access module.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */


#include <modem.h>
#include <modem_param.h>



/* param access services */
long modem_get_param(struct modem *m, unsigned param_name)
{
	switch (param_name) {
	case MDMPRM_V90_DIGITAL_INDICATION:
		return m->sregs[SREG_V90_DIGITAL_INDICATION];
	case MDMPRM_V91_ENABLED:
		return m->sregs[SREG_V91_ENABLED] != 0;
	case MDMPRM_V92_QUICK_CONNECT:
		return m->sregs[SREG_V92_QUICK_CONNECT] != 0;
	case MDMPRM_V92_PCM_UPSTREAM:
		return m->sregs[SREG_V92_PCM_UPSTREAM] != 0;
	case MDMPRM_V90_UPSTREAM_MIN_RATE:
		return m->v90_upstream_min_rate;
	case MDMPRM_V90_UPSTREAM_MAX_RATE:
		return m->v90_upstream_max_rate;
	case MDMPRM_V90_UPSTREAM_MAX_BAUD:
		return m->v90_upstream_max_baud;
	}
	return -1;
}

long modem_set_param(struct modem *m, unsigned name, int val)
{
	switch(name) {
	case MDMPRM_RX_RATE:
	case MDMPRM_TX_RATE: {
		uint64_t previous_rate = (uint64_t)m->rx_rate + m->tx_rate;

		if (val < 0)
			return -1;
		if (name == MDMPRM_RX_RATE)
			m->rx_rate = (unsigned)val;
		else
			m->tx_rate = (unsigned)val;
		modem_ec_update_timer_rate(m, previous_rate);
		break;
	}
	case MDMPRM_DP_REQUESTED:
		m->dp_requested = val;
		break;
	case MDMPRM_V90_DIGITAL_INDICATION:
		if (val < MODEM_PCM_POLICY_ANALOG || val > MODEM_PCM_POLICY_AUTO)
			return -1;
		m->sregs[SREG_V90_DIGITAL_INDICATION] = val;
		break;
	case MDMPRM_V91_ENABLED:
		if (val < 0 || val > 1)
			return -1;
		m->sregs[SREG_V91_ENABLED] = val;
		break;
	case MDMPRM_V92_QUICK_CONNECT:
		m->sregs[SREG_V92_QUICK_CONNECT] = val != 0;
		break;
	case MDMPRM_V92_PCM_UPSTREAM:
		m->sregs[SREG_V92_PCM_UPSTREAM] = val != 0;
		break;
	case MDMPRM_V90_UPSTREAM_MIN_RATE:
		m->v90_upstream_min_rate = (unsigned)val;
		break;
	case MDMPRM_V90_UPSTREAM_MAX_RATE:
		m->v90_upstream_max_rate = (unsigned)val;
		break;
	case MDMPRM_V90_UPSTREAM_MAX_BAUD:
		m->v90_upstream_max_baud = (unsigned)val;
		break;
	default:
		return -1;
	}
	return 0;
}
