// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_slmodem_adapter.h>

#include <modem.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

struct modem_process_capture {
	unsigned int calls;
	int count;
	void *receive;
	void *transmit;
	bool stop_modem;
};

static struct modem_process_capture capture;

void modem_process(struct modem *modem, void *receive, void *transmit,
		   int count)
{
	const int16_t *input = receive;
	int16_t *output = transmit;

	capture.calls++;
	capture.count = count;
	capture.receive = receive;
	capture.transmit = transmit;
	for (int index = 0; index < count; ++index)
		output[index] = (int16_t)-input[index];
	if (capture.stop_modem)
		modem->started = 0;
}

static struct modem configured_modem(void)
{
	struct modem modem;

	memset(&modem, 0, sizeof(modem));
	modem.srate = MODEM_RATE;
	modem.format = MODEM_FORMAT;
	modem.frag = MODEM_FRAG;
	return modem;
}

static bool test_forwards_exact_host_frames(void)
{
	int16_t receive[] = { 1, -2, 300, -400 };
	static const int16_t expected[] = { -1, 2, -300, 400 };
	struct modem modem = configured_modem();
	struct sippymodem_slmodem_adapter adapter;
	const struct sippymodem_modem_core *core;
	int dcr_storage;
	int runtime_storage;
	int16_t transmit[ARRAY_LENGTH(receive)] = { 0 };

	memset(&capture, 0, sizeof(capture));
	CHECK(MODEM_RATE == SIPPYMODEM_MEDIA_SAMPLE_RATE &&
		      sippymodem_slmodem_adapter_init(&adapter, &modem),
	      "adapter did not accept the modem's exact SIP clock");
	core = sippymodem_slmodem_adapter_core(&adapter);
	CHECK(core != NULL && core->context == &adapter,
	      "adapter did not expose its borrowed core");
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      SIPPYMODEM_MODEM_CORE_STOPPED &&
		      capture.calls == 0,
	      "inactive modem reached legacy sample processing");
	modem.started = 1;
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      SIPPYMODEM_MODEM_CORE_ERROR &&
		      capture.calls == 0,
	      "modem ran without its required runtime cleaner");
	modem.dcr = &dcr_storage;
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      SIPPYMODEM_MODEM_CORE_ERROR &&
		      capture.calls == 0,
	      "modem ran without its required data-pump runtime");
	modem.dp_runtime = &runtime_storage;
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      SIPPYMODEM_MODEM_CORE_OK &&
		      capture.calls == 1 &&
		      capture.count == (int)ARRAY_LENGTH(receive) &&
		      capture.receive == receive &&
		      capture.transmit == transmit &&
		      memcmp(transmit, expected, sizeof(expected)) == 0,
	      "adapter changed or truncated the modem frame");
	capture.stop_modem = true;
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      SIPPYMODEM_MODEM_CORE_STOPPED &&
		      capture.calls == 2,
	      "adapter lost a stop raised during modem processing");
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct modem modem = configured_modem();
	struct sippymodem_slmodem_adapter adapter = {
		.modem = &modem,
	};
	struct sippymodem_slmodem_adapter original = adapter;
	const struct sippymodem_modem_core *core;
	int16_t sample = 0;

	modem.srate++;
	CHECK(!sippymodem_slmodem_adapter_init(&adapter, &modem) &&
		      adapter.modem == original.modem &&
		      adapter.core.context == original.core.context &&
		      adapter.core.process == original.core.process,
	      "invalid modem clock partially changed the adapter");
	modem.srate = MODEM_RATE;
	modem.frag = 0;
	CHECK(!sippymodem_slmodem_adapter_init(&adapter, &modem) &&
		      adapter.modem == original.modem &&
		      adapter.core.context == original.core.context &&
		      adapter.core.process == original.core.process,
	      "invalid modem fragment partially changed the adapter");
	modem.frag = MODEM_FRAG;
	CHECK(!sippymodem_slmodem_adapter_init(NULL, &modem) &&
		      !sippymodem_slmodem_adapter_init(&adapter, NULL) &&
		      sippymodem_slmodem_adapter_core(NULL) == NULL,
	      "adapter accepted incomplete ownership");
	CHECK(sippymodem_slmodem_adapter_init(&adapter, &modem),
	      "valid adapter fixture failed");
	core = sippymodem_slmodem_adapter_core(&adapter);
	modem.started = 1;
	modem.dcr = &sample;
	modem.dp_runtime = &sample;
	CHECK(core->process(core->context, NULL, &sample, 1) ==
		      SIPPYMODEM_MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, NULL, 1) ==
			      SIPPYMODEM_MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, &sample, 0) ==
			      SIPPYMODEM_MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, &sample,
				    (size_t)INT_MAX + 1) ==
			      SIPPYMODEM_MODEM_CORE_ERROR,
	      "adapter accepted an unsafe legacy frame");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact host frames", test_forwards_exact_host_frames },
		{ "transactional validation", test_validation_is_transactional },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
