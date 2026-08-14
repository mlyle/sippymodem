// SPDX-License-Identifier: BSD-3-Clause

#include <datapump_timing.h>

#include "test.h"

int modem_get_sreg(struct modem *modem, unsigned int number)
{
	if (modem == NULL || number >= ARRAY_LENGTH(modem->sregs))
		return -1;
	return modem->sregs[number];
}

static bool test_default_and_configured_wait(void)
{
	struct modem modem = { 0 };

	CHECK(datapump_carrier_wait_ticks(&modem) ==
		      UINT64_C(60) * MODEM_RATE,
	      "zero S7 did not select the bounded default carrier wait");
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 1;
	CHECK(datapump_carrier_wait_ticks(&modem) == MODEM_RATE,
	      "one-second carrier wait was not converted to media ticks");
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = UINT8_MAX;
	CHECK(datapump_carrier_wait_ticks(&modem) ==
		      UINT64_C(255) * MODEM_RATE,
	      "full-width S7 carrier wait was truncated");
	return true;
}

static bool test_invalid_modem(void)
{
	CHECK(datapump_carrier_wait_ticks(NULL) == 0,
	      "missing modem produced a carrier-wait deadline");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "default and configured wait", test_default_and_configured_wait },
		{ "invalid modem", test_invalid_modem },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
