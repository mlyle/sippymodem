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
 *    modem_debug.h - modem debug definitions.
 *
 *    Author: SashaK <sashak@smlink.com>
 *
 *    Change History:
 *
 */

#ifndef __MODEM_DEBUG_H__
#define __MODEM_DEBUG_H__


#ifdef CONFIG_DEBUG_MODEM

/* Configure before starting writers; leave unchanged while they run. */
extern unsigned modem_debug_level;
/* Return body characters written, zero when disabled, or negative on error. */
extern int modem_debug_printf(const char *fmt,...)
     __attribute__ ((format (printf, 1, 2)));

#define eprintf(args...) { modem_debug_printf("err: " args) ; }
#define dprintf(args...) { modem_debug_printf(args) ; }

#else /* not CONFIG_DEBUG_MODEM */

#define eprintf(fmt...)
#define dprintf(fmt...)

#endif /* not CONFIG_DEBUG_MODEM */


#endif /* __MODEM_DEBUG_H__ */

