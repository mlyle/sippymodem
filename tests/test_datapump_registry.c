// SPDX-License-Identifier: BSD-3-Clause

#include <modem_dp.h>
#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_selectable_modulations(void)
{
	static const enum DP_ID expected[] = {
		DP_V21, DP_V22, DP_V23, DP_V22BIS, DP_V32, DP_V32BIS,
		DP_V34, DP_BAUDOT, DP_B103, DP_B212, DP_V90, DP_V91, DP_V92,
	};

	for (size_t i = 0; i < ARRAY_LENGTH(expected); i++) {
		enum DP_ID id = modem_dp_selectable_id(i);
		const struct dp_operations *op = modem_dp_get_registered_ops(id);

		CHECK(id == expected[i] && modem_dp_is_selectable(id),
		      "AT+MS selection %zu changed or is unavailable", i);
		CHECK(modem_dp_name(id) != NULL && op != NULL &&
		      op->name != NULL && op->create != NULL &&
		      op->delete != NULL && op->process != NULL,
		      "modulation %u has incomplete built-in operations", id);
	}
	CHECK(modem_dp_selectable_id(ARRAY_LENGTH(expected)) == 0 &&
	      modem_dp_selectable_id(SIZE_MAX) == 0,
	      "modulation enumeration did not end at the catalog boundary");
	return true;
}

static bool test_shared_families(void)
{
	static const enum DP_ID aliases[][2] = {
		{ DP_V21, DP_B103 }, { DP_V22, DP_B212 },
		{ DP_V32, DP_V32BIS }, { DP_V90, DP_V92 },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(aliases); i++)
		CHECK(modem_dp_get_registered_ops(aliases[i][0]) ==
		      modem_dp_get_registered_ops(aliases[i][1]),
		      "protocol aliases %u and %u lost their shared family",
		      aliases[i][0], aliases[i][1]);
	CHECK(modem_dp_get_registered_ops(DP_V22) !=
	      modem_dp_get_registered_ops(DP_V22BIS),
	      "V.22bis lost its separate modulation capabilities");
	CHECK(strcmp(modem_dp_name(DP_V90), "V90") == 0 &&
	      strcmp(modem_dp_name(DP_V92), "V92") == 0 &&
	      modem_dp_get_registered_ops(DP_V90)->process_g711 != NULL,
	      "shared PCM operations lost the selected protocol names or exact media");
	return true;
}

static bool test_internal_and_unknown_ids(void)
{
	static const enum DP_ID internal[] = { DP_CALLPROG, DP_V8 };
	static const enum DP_ID unavailable[] = {
		0, (enum DP_ID)-1, (enum DP_ID)INT32_MAX,
		DP_DUMMY, DP_AUTOMODE, DP_V17, DP_FAX, DP_K56, DP_V8BIS, DP_SINUS,
	};

	for (size_t i = 0; i < ARRAY_LENGTH(internal); i++) {
		const struct dp_operations *op = modem_dp_get_registered_ops(internal[i]);

		CHECK(!modem_dp_is_selectable(internal[i]) && op != NULL &&
		      op->create != NULL && op->delete != NULL && op->process != NULL,
		      "internal datapump %u was missing or selectable", internal[i]);
	}
	for (size_t i = 0; i < ARRAY_LENGTH(unavailable); i++)
		CHECK(!modem_dp_is_selectable(unavailable[i]) &&
		      modem_dp_get_registered_ops(unavailable[i]) == NULL,
		      "unavailable datapump %u became callable", unavailable[i]);
	CHECK(modem_dp_name(0) == NULL &&
	      modem_dp_name((enum DP_ID)-1) == NULL &&
	      modem_dp_name((enum DP_ID)INT32_MAX) == NULL,
	      "an unknown datapump inherited another protocol's name");
	return true;
}

int main(void)
{
	const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "selectable modulations", test_selectable_modulations },
		{ "shared families", test_shared_families },
		{ "internal and unknown IDs", test_internal_and_unknown_ids },
	};
	unsigned failures = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); i++) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		bool passed = tests[i].run();

		fprintf(stderr, "[ %s ] %s\n", passed ? "      OK" : " FAILED ", tests[i].name);
		failures += !passed;
	}
	return failures != 0;
}
