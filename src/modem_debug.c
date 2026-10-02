/*****************************************************************************/

/*
 *
 *   Copyright (c) 2002, Smart Link Ltd.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *       1. Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *       2. Redistributions in binary form must reproduce the above
 *          copyright notice, this list of conditions and the following
 *          disclaimer in the documentation and/or other materials provided
 *          with the distribution.
 *       3. Neither the name of the Smart Link Ltd. nor the names of its
 *          contributors may be used to endorse or promote products derived
 *          from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *    modem_debug.c -- modem debug printing.
 *    Author: Sasha K (sashak@smlink.com)
 */

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

#define CONFIG_DEBUG_MODEM 1
#include <modem_debug.h>

unsigned modem_debug_level;

int modem_debug_printf(const char *fmt, ...)
{
	struct timeval now = { 0 };
	va_list args;
	int length;

	if (modem_debug_level == 0)
		return 0;
	flockfile(stderr);
	(void)gettimeofday(&now, NULL);
	va_start(args, fmt);
	length = fprintf(stderr, "<%03ld.%06ld> ",
			 (long)(now.tv_sec % 1000), (long)now.tv_usec);
	if (length >= 0)
		length = vfprintf(stderr, fmt, args);
	va_end(args);
	funlockfile(stderr);
	return length;
}
