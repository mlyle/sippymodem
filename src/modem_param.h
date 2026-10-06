
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
 *    modem_param.h  --  modem parameters access definitions.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#ifndef __MODEM_PARAM_H__
#define __MODEM_PARAM_H__

/* AT+V90D policy; V.8 selects complementary roles in automatic mode. */
enum modem_pcm_policy {
	MODEM_PCM_POLICY_ANALOG = 0,
	MODEM_PCM_POLICY_DIGITAL = 1,
	MODEM_PCM_POLICY_AUTO = 2,
};

/* param names */
enum MODEM_PARAMETER_NAMES {
	MDMPRM_V90_DIGITAL_INDICATION = 1,
	MDMPRM_V92_QUICK_CONNECT,
	MDMPRM_V92_PCM_UPSTREAM,
	MDMPRM_V91_ENABLED,
};


struct modem;

extern long modem_get_param(struct modem *m, unsigned param);
extern long modem_set_param(struct modem *m, unsigned name, int val);

#endif /* __MODEM_PARAM_H__ */
