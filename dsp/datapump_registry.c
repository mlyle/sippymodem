
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

/* Data pump registration. Author: Sasha K (sashak@smlink.com). */

#include <modem_dp.h>
#include "datapumps.h"
#include "baudot_datapump.h"
#include "fsk300_datapump.h"
#include "v8_datapump.h"
#include "v23_datapump.h"
#include "v22_datapump.h"
#include "datapump.h"
#include "v34_datapump.h"
#include "v90_datapump.h"

struct dp_driver {
	enum DP_ID id;
	const char *name;
	bool selectable;
	const struct dp_operations *(*operations)(enum DP_ID id);
};

static const struct dp_driver drivers[] = {
	{ DP_CALLPROG, "CallProg", false, call_progress_datapump_operations },
	{ DP_DUMMY, "Dummy", false, NULL },
	{ DP_AUTOMODE, "Automode", false, NULL },
	{ DP_V8, "V8", false, v8_datapump_operations },
	{ DP_V17, "V17", false, NULL },
	{ DP_V21, "V21", true, fsk300_datapump_operations },
	{ DP_V22, "V22", true, v22_datapump_operations },
	{ DP_V23, "V23", true, v23_datapump_operations },
	{ DP_V22BIS, "V22bis", true, v22_datapump_operations },
	{ DP_V32, "V32", true, v32_datapump_operations },
	{ DP_V32BIS, "V32bis", true, v32_datapump_operations },
	{ DP_V34, "V34", true, v34_datapump_operations },
	{ DP_BAUDOT, "Baudot TTY/TDD", true, baudot_datapump_operations },
	{ DP_B103, "Bell103", true, fsk300_datapump_operations },
	{ DP_B212, "Bell212A", true, v22_datapump_operations },
	{ DP_FAX, "VFax", false, NULL },
	{ DP_K56, "K56Flex", false, NULL },
	{ DP_V8BIS, "V8bis", false, NULL },
	{ DP_V90, "V90", true, v90_datapump_operations },
	{ DP_V92, "V92", true, v90_datapump_operations },
	{ DP_SINUS, "Sinus", false, NULL },
	{ 0 }
};

static const struct dp_driver *get_dp_driver(enum DP_ID id)
{
	for (const struct dp_driver *driver = drivers; driver->id != 0; driver++)
		if (driver->id == id)
			return driver;
	return NULL;
}

const char *modem_dp_name(enum DP_ID id)
{
	const struct dp_driver *driver = get_dp_driver(id);

	return driver != NULL ? driver->name : NULL;
}

bool modem_dp_is_selectable(enum DP_ID id)
{
	const struct dp_driver *driver = get_dp_driver(id);

	return driver != NULL && driver->selectable;
}

enum DP_ID modem_dp_selectable_id(size_t index)
{
	for (const struct dp_driver *driver = drivers; driver->id != 0; driver++) {
		if (!driver->selectable)
			continue;
		if (index == 0)
			return driver->id;
		index--;
	}
	return 0;
}

const struct dp_operations *modem_dp_get_registered_ops(enum DP_ID id)
{
	const struct dp_driver *driver = get_dp_driver(id);

	return driver != NULL && driver->operations != NULL ?
		driver->operations(id) : NULL;
}
