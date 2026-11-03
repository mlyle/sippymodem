// SPDX-License-Identifier: BSD-3-Clause
#define main integration_test_main
#include "test_fsk_integration.c"
#undef main
#include <v8_framing.h>

/* Lose one of the first CJ's zero octets without altering later retries. */
static bool damage_first_cj;
static unsigned damaged_cj_count;
extern enum v8_framing_result __real_v8_cj_bit(size_t index, uint8_t *bit);
enum v8_framing_result __wrap_v8_cj_bit(size_t index, uint8_t *bit)
{
	enum v8_framing_result result = __real_v8_cj_bit(index, bit);
	if (result == V8_FRAMING_OK && damage_first_cj && index == V8_FRAMED_OCTET_BITS + 1u) {
		*bit = 1;
		damage_first_cj = false;
		++damaged_cj_count;
	}
	return result;
}

static bool run_missed_cj_case(enum media_codec codec, unsigned protocol)
{
	struct integration_endpoint ends[2];
	bool ready = false, saw_retry = false;
	unsigned before = damaged_cj_count;
	for (unsigned side = 0; side < 2; ++side) {
		CHECK(endpoint_init(&ends[side], "missed CJ") && endpoint_prepare_media(&ends[side], codec),
		      "endpoint init");
		const char *commands[] = { "ATE0\\N4\r", "AT+DS=0,0\r", "AT+V92Q=0\r",
			"AT+V92U=1,36000\r", side ? "AT+V90D=0\r" : "AT+V90D=1\r",
			protocol == 90 ? "AT+MS=90,0,25000,56000\r" : "AT+MS=92,0,25000,56000\r" };
		for (unsigned i = 0; i < ARRAY_LENGTH(commands); ++i)
			CHECK(modem_write(ends[side].modem, commands[i], strlen(commands[i])) ==
			      (int)strlen(commands[i]) && endpoint_drain(&ends[side]) &&
			      !output_contains(&ends[side], "ERROR"), "setup side %u command %u", side, i);
		ends[side].output_count = 0;
	}
	damage_first_cj = true;
	CHECK(modem_set_sreg(ends[1].modem, SREG_ANS_DELAY, 0) == 0 &&
	      modem_write(ends[0].modem, "ATDpeer\r", 8) == 8 &&
	      modem_report_outgoing_call_connected(ends[0].modem) == 0 &&
	      modem_answer(ends[1].modem) == 0, "call start");
	for (unsigned frame = 0; frame < 2000; ++frame) {
		CHECK(process_sip_frame(&ends[0], &ends[1], codec, NULL) &&
		      endpoint_drain(&ends[0]) && endpoint_drain(&ends[1]), "training");
		saw_retry |= !strcmp(modem_protocol_step(ends[0].modem), "phase2/retry-V8-CM-CJ");
		if (output_contains(&ends[0], "CONNECT") && output_contains(&ends[1], "CONNECT")) {
			ready = true;
			break;
		}
	}
	CHECK(ready && saw_retry && damaged_cj_count == before + 1u && !damage_first_cj,
	      "missed CJ recovery failed protocol %u codec %u", protocol, codec);
	const char *payload = "first duplex bytes after missed CJ\r\n";
	for (unsigned side = 0; side < 2; ++side) {
		ends[side].output_count = 0;
		CHECK(modem_write(ends[side].modem, payload, strlen(payload)) == (int)strlen(payload),
		      "data write");
	}
	ready = false;
	for (unsigned frame = 0; frame < 500; ++frame) {
		CHECK(process_sip_frame(&ends[0], &ends[1], codec, NULL) &&
		      endpoint_drain(&ends[0]) && endpoint_drain(&ends[1]), "data");
		if (output_contains(&ends[0], payload) && output_contains(&ends[1], payload)) {
			ready = true;
			break;
		}
	}
	CHECK(ready, "first duplex data missing");
	endpoint_destroy(&ends[1]); endpoint_destroy(&ends[0]);
	return true;
}

static bool run_missed_cj_cases(const char *filter, bool *selected)
{
	const char *name = "V8 missed CJ recovery";
	if (filter != NULL && filter[0] && strstr(name, filter) == NULL) return true;
	*selected = true;
	for (enum media_codec codec = MEDIA_CODEC_PCMU; codec <= MEDIA_CODEC_PCMA; ++codec)
		for (unsigned protocol = 90; protocol <= 92; protocol += 2)
			CHECK(run_missed_cj_case(codec, protocol), "missed CJ case failed");
	return true;
}


int main(void)
{
	bool selected = false;
	return run_missed_cj_cases(NULL, &selected) && selected ? 0 : 1;
}
