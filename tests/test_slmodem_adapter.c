// SPDX-License-Identifier: BSD-3-Clause

#include <slmodem_adapter.h>

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
	const uint8_t *codewords;
	const bool *received_mask;
	enum g711_law law;
	unsigned int g711_calls;
	bool exact_output;
	bool stop_modem;
};

static struct modem_process_capture capture;

size_t modem_receive_pending(const struct modem *modem)
{
	return modem && modem->started ? 7 : 0;
}

int modem_drain_receive(struct modem *modem, unsigned budget)
{
	return modem && modem->started && budget == 4 ? 0 : -1;
}

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

void modem_process_g711(
	struct modem *modem, enum g711_law law,
	const uint8_t *receive_codewords, void *receive, const bool *received_mask,
	uint8_t *transmit_codewords, void *transmit,
	int count, bool *exact_output)
{
	capture.g711_calls++;
	capture.law = law;
	capture.codewords = receive_codewords;
	capture.received_mask = received_mask;
	for (int index = 0; index < count; ++index)
		transmit_codewords[index] = (uint8_t)~receive_codewords[index];
	*exact_output = capture.exact_output;
	modem_process(modem, receive, transmit, count);
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
	static const uint8_t codewords[] = { 0x7f, 0xff, 0x55, 0xd5 };
	struct modem modem = configured_modem();
	struct slmodem_adapter adapter;
	const struct modem_core *core;
	int16_t transmit[ARRAY_LENGTH(receive)] = { 0 };
	uint8_t transmit_codewords[ARRAY_LENGTH(receive)] = { 0 };
	bool exact_output = false;

	memset(&capture, 0, sizeof(capture));
	CHECK(MODEM_RATE == MEDIA_SAMPLE_RATE &&
		      slmodem_adapter_init(&adapter, &modem),
	      "adapter did not accept the modem's exact SIP clock");
	core = slmodem_adapter_core(&adapter);
	CHECK(core != NULL && core->context == &adapter,
	      "adapter did not expose its borrowed core");
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      MODEM_CORE_STOPPED &&
		      capture.calls == 0,
	      "inactive modem reached legacy sample processing");
	modem.started = 1;
	CHECK(core->receive_pending(core->context) == 7 &&
	      core->drain_receive(core->context, 4) == MODEM_CORE_OK &&
	      core->drain_receive(core->context, 0) == MODEM_CORE_ERROR &&
	      core->drain_receive(core->context, 5) == MODEM_CORE_ERROR,
	      "adapter lost receive-tail bounds or errors");
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      MODEM_CORE_OK &&
		      capture.calls == 1 &&
		      capture.count == (int)ARRAY_LENGTH(receive) &&
		      capture.receive == receive &&
		      capture.transmit == transmit &&
		      memcmp(transmit, expected, sizeof(expected)) == 0,
	      "adapter changed or truncated the modem frame");
	memset(transmit, 0, sizeof(transmit));
	capture.exact_output = true;
	const bool received_mask[] = { true, false, true, false };
	CHECK(core->process_g711(
		      core->context, G711_LAW_PCMU, codewords,
		      receive, received_mask, transmit_codewords, transmit,
		      ARRAY_LENGTH(receive), &exact_output) == MODEM_CORE_OK &&
		      capture.calls == 2 && capture.g711_calls == 1 &&
		      capture.law == G711_LAW_PCMU &&
		      capture.codewords == codewords && capture.received_mask == received_mask &&
		      exact_output &&
		      memcmp(transmit_codewords,
			     (uint8_t[]){ 0x80, 0x00, 0xaa, 0x2a },
			     sizeof(transmit_codewords)) == 0 &&
		      memcmp(transmit, expected, sizeof(expected)) == 0,
	      "adapter did not preserve both exact G.711 directions");
	capture.stop_modem = true;
	CHECK(core->process(core->context, receive, transmit,
			    ARRAY_LENGTH(receive)) ==
		      MODEM_CORE_STOPPED &&
		      capture.calls == 3,
	      "adapter lost a stop raised during modem processing");
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct modem modem = configured_modem();
	struct slmodem_adapter adapter = {
		.modem = &modem,
	};
	struct slmodem_adapter original = adapter;
	const struct modem_core *core;
	uint8_t codeword = 0xff;
	int16_t sample = 0;
	bool exact_output = true;

	modem.srate++;
	CHECK(!slmodem_adapter_init(&adapter, &modem) &&
		      adapter.modem == original.modem &&
		      adapter.core.context == original.core.context &&
		      adapter.core.process == original.core.process,
	      "invalid modem clock partially changed the adapter");
	modem.srate = MODEM_RATE;
	modem.frag = 0;
	CHECK(!slmodem_adapter_init(&adapter, &modem) &&
		      adapter.modem == original.modem &&
		      adapter.core.context == original.core.context &&
		      adapter.core.process == original.core.process,
	      "invalid modem fragment partially changed the adapter");
	modem.frag = MODEM_FRAG;
	CHECK(!slmodem_adapter_init(NULL, &modem) &&
		      !slmodem_adapter_init(&adapter, NULL) &&
		      slmodem_adapter_core(NULL) == NULL,
	      "adapter accepted incomplete ownership");
	CHECK(slmodem_adapter_init(&adapter, &modem),
	      "valid adapter fixture failed");
	core = slmodem_adapter_core(&adapter);
	modem.started = 1;
	CHECK(core->process(core->context, NULL, &sample, 1) ==
		      MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, NULL, 1) ==
			      MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, &sample, 0) ==
			      MODEM_CORE_ERROR &&
		      core->process(core->context, &sample, &sample,
				    (size_t)INT_MAX + 1) ==
			      MODEM_CORE_ERROR,
	      "adapter accepted an unsafe legacy frame");
	CHECK(core->process_g711(
		      core->context, (enum g711_law)99, &codeword,
		      &sample, NULL, &codeword, &sample, 1,
		      &exact_output) == MODEM_CORE_ERROR &&
		      core->process_g711(
			      core->context, G711_LAW_PCMU, NULL,
			      &sample, NULL, &codeword, &sample, 1,
			      &exact_output) == MODEM_CORE_ERROR &&
		      core->process_g711(
			      core->context, G711_LAW_PCMU, &codeword,
			      &sample, NULL, NULL, &sample, 1,
			      &exact_output) == MODEM_CORE_ERROR &&
		      !exact_output,
	      "adapter accepted an unsafe G.711 frame");
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
