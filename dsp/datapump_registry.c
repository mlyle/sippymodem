
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
#include <stddef.h>

struct dp_driver modem_dp_drivers[] = {
	{ .id = DP_CALLPROG, .name = "CallProg" },
	{ .id = DP_DUMMY, .name = "Dummy" },
	{ .id = DP_AUTOMODE, .name = "Automode" },
	{ .id = DP_V8, .name = "V8" },
	{ .id = DP_V17, .name = "V17" },
	{ .id = DP_V21, .name = "V21", .code = "21" },
	{ .id = DP_V22, .name = "V22", .code = "22" },
	{ .id = DP_V23, .name = "V23", .code = "23" },
	{ .id = DP_V22BIS, .name = "V22bis", .code = "122" },
	{ .id = DP_V32, .name = "V32", .code = "32" },
	{ .id = DP_V32BIS, .name = "V32bis", .code = "132" },
	{ .id = DP_V34, .name = "V34", .code = "34" },
	{ .id = DP_BAUDOT, .name = "Baudot TTY/TDD", .code = "45" },
	{ .id = DP_B103, .name = "Bell103", .code = "103" },
	{ .id = DP_B212, .name = "Bell212A", .code = "212" },
	{ .id = DP_FAX, .name = "VFax" },
	{ .id = DP_K56, .name = "K56Flex", .code = "56" },
	{ .id = DP_V8BIS, .name = "V8bis" },
	{ .id = DP_V90, .name = "V90", .code = "90" },
	{ .id = DP_V92, .name = "V92", .code = "92" },
	{ .id = DP_SINUS, .name = "Sinus" },
	{ 0 }
};

static struct dp_driver *get_dp_driver(enum DP_ID id)
{
	struct dp_driver *p;
	for(p = modem_dp_drivers ; p->id > 0 ; p++)
		if (p->id == id) return p;
	return NULL;
}

const char *modem_dp_name(enum DP_ID id)
{
	struct dp_driver *driver = get_dp_driver(id);

	return driver != NULL ? driver->name : NULL;
}

static int update_bindings(
	const struct dp_binding *bindings, size_t count, bool registering)
{
	if (bindings == NULL || count == 0 ||
	    count >= sizeof(modem_dp_drivers) / sizeof(modem_dp_drivers[0]))
		return -1;
	for (size_t i = 0; i < count; i++) {
		struct dp_driver *driver = get_dp_driver(bindings[i].id);
		struct dp_operations *op = bindings[i].op;

		if (driver == NULL || op == NULL || op->use_count != 0)
			return -1;
		if (registering) {
			if (driver->op != NULL || op->create == NULL ||
			    op->delete == NULL || op->process == NULL)
				return -1;
		} else if (driver->op != op)
			return -1;
		for (size_t previous = 0; previous < i; previous++)
			if (bindings[previous].id == bindings[i].id)
				return -1;
	}
	for (size_t i = 0; i < count; i++)
		get_dp_driver(bindings[i].id)->op = registering ? bindings[i].op : NULL;
	return 0;
}

int modem_dp_register_set(const struct dp_binding *bindings, size_t count)
{
	return update_bindings(bindings, count, true);
}

int modem_dp_deregister_set(const struct dp_binding *bindings, size_t count)
{
	return update_bindings(bindings, count, false);
}

int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	const struct dp_binding binding = { id, op };

	return modem_dp_register_set(&binding, 1);
}

int modem_dp_deregister(enum DP_ID id, struct dp_operations *op)
{
	const struct dp_binding binding = { id, op };

	return modem_dp_deregister_set(&binding, 1);
}

struct dp_operations *modem_dp_get_registered_ops(enum DP_ID id)
{
	struct dp_driver *driver = get_dp_driver(id);

	return driver != NULL ? driver->op : NULL;
}
