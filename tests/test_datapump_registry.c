// SPDX-License-Identifier: BSD-3-Clause

#include <modem_dp.h>
#include "test.h"

#include <stdint.h>

static struct dp *unused_create(
	struct modem *modem, enum DP_ID id, int caller, int rate,
	int fragment, struct dp_operations *op)
{
	(void)modem;
	(void)id;
	(void)caller;
	(void)rate;
	(void)fragment;
	(void)op;
	return NULL;
}

static int unused_delete(struct dp *dp)
{
	(void)dp;
	return 0;
}

static int unused_process(struct dp *dp, void *rx, void *tx, int count)
{
	(void)dp;
	(void)rx;
	(void)tx;
	(void)count;
	return DPSTAT_ERROR;
}

static struct dp_operations operations(void)
{
	return (struct dp_operations){
		.create = unused_create,
		.delete = unused_delete,
		.process = unused_process,
	};
}

static bool test_invalid_sets(void)
{
	struct dp_operations op = operations(), invalid;
	struct dp_binding bindings[] = { { DP_V90, &op }, { DP_V92, &op } };

	CHECK(modem_dp_register_set(NULL, 2) != 0 &&
	      modem_dp_register_set(bindings, 0) != 0 &&
	      modem_dp_register_set(bindings, SIZE_MAX) != 0 &&
	      modem_dp_deregister_set(NULL, 2) != 0 &&
	      modem_dp_deregister_set(bindings, 0) != 0 &&
	      modem_dp_deregister_set(bindings, SIZE_MAX) != 0,
	      "invalid set extent was accepted");
	for (unsigned failure = 0; failure < 7; failure++) {
		invalid = operations();
		bindings[1] = (struct dp_binding){ DP_V92, &op };
		switch (failure) {
		case 0: bindings[1].id = (enum DP_ID)-1; break;
		case 1: bindings[1].op = NULL; break;
		case 2: invalid.create = NULL; bindings[1].op = &invalid; break;
		case 3: invalid.delete = NULL; bindings[1].op = &invalid; break;
		case 4: invalid.process = NULL; bindings[1].op = &invalid; break;
		case 5: bindings[1].id = DP_V90; break;
		case 6: invalid.use_count = -1; bindings[1].op = &invalid; break;
		}
		CHECK(modem_dp_register_set(bindings, 2) != 0 &&
		      modem_dp_get_registered_ops(DP_V90) == NULL &&
		      modem_dp_get_registered_ops(DP_V92) == NULL,
		      "invalid late binding changed an earlier alias (case %u)", failure);
	}
	bindings[1] = (struct dp_binding){ DP_V92, &op };
	CHECK(modem_dp_register_set(bindings, 2) == 0,
	      "removal validation fixture did not register");
	for (unsigned failure = 0; failure < 3; failure++) {
		bindings[1] = (struct dp_binding){ DP_V92, &op };
		if (failure == 0)
			bindings[1].id = (enum DP_ID)-1;
		if (failure == 1)
			bindings[1].op = NULL;
		if (failure == 2)
			bindings[1].id = DP_V90;
		CHECK(modem_dp_deregister_set(bindings, 2) != 0 &&
		      modem_dp_get_registered_ops(DP_V90) == &op &&
		      modem_dp_get_registered_ops(DP_V92) == &op,
		      "invalid late removal changed an earlier alias (case %u)", failure);
	}
	bindings[1] = (struct dp_binding){ DP_V92, &op };
	CHECK(modem_dp_deregister_set(bindings, 2) == 0,
	      "removal validation fixture did not release");
	return true;
}

static bool test_foreign_ownership(void)
{
	struct dp_operations op = operations(), foreign = operations();
	const struct dp_binding bindings[] = { { DP_V90, &op }, { DP_V92, &op } };
	const struct dp_binding wrong[] = { { DP_V90, &op }, { DP_V92, &foreign } };

	CHECK(modem_dp_register(DP_V92, &foreign) == 0 &&
	      modem_dp_register_set(bindings, 2) != 0 &&
	      modem_dp_get_registered_ops(DP_V90) == NULL &&
	      modem_dp_get_registered_ops(DP_V92) == &foreign &&
	      modem_dp_deregister(DP_V92, &foreign) == 0,
	      "registration set changed a foreign owner or retained its prefix");
	CHECK(modem_dp_register_set(bindings, 2) == 0,
	      "valid alias set failed to register");
	CHECK(modem_dp_deregister_set(wrong, 2) != 0 &&
	      modem_dp_get_registered_ops(DP_V90) == &op &&
	      modem_dp_get_registered_ops(DP_V92) == &op &&
	      modem_dp_deregister_set(bindings, 2) == 0,
	      "removal set changed a binding before rejecting foreign ownership");
	return true;
}

static bool test_busy_sets(void)
{
	struct dp_operations first = operations(), second = operations();
	const struct dp_binding bindings[] = { { DP_V22, &first }, { DP_V22BIS, &second } };

	second.use_count = 1;
	CHECK(modem_dp_register_set(bindings, 2) != 0 &&
	      modem_dp_get_registered_ops(DP_V22) == NULL,
	      "busy late operation retained an earlier binding");
	second.use_count = 0;
	CHECK(modem_dp_register_set(bindings, 2) == 0,
	      "inactive operations failed to register");
	second.use_count = 1;
	CHECK(modem_dp_deregister_set(bindings, 2) != 0 &&
	      modem_dp_get_registered_ops(DP_V22) == &first &&
	      modem_dp_get_registered_ops(DP_V22BIS) == &second,
	      "busy late operation allowed partial removal");
	second.use_count = 0;
	CHECK(modem_dp_deregister_set(bindings, 2) == 0 &&
	      modem_dp_get_registered_ops(DP_V22) == NULL &&
	      modem_dp_get_registered_ops(DP_V22BIS) == NULL,
	      "inactive alias set did not release all ownership");
	return true;
}

int main(void)
{
	const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "invalid registration sets", test_invalid_sets },
		{ "registration set ownership", test_foreign_ownership },
		{ "busy registration sets", test_busy_sets },
	};
	unsigned failed = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); i++) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		bool passed = tests[i].run();
		fprintf(stderr, passed ? "[       OK ] %s\n" : "[  FAILED  ] %s\n", tests[i].name);
		failed += !passed;
	}
	return failed != 0;
}
