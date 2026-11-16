// SPDX-License-Identifier: BSD-3-Clause
#define main integration_test_main
#include "test_fsk_integration.c"
#undef main
#include <v8_framing.h>
#include <v8_handoff.h>
#include <pcm_call.h>

/* Observe the actual delivered waveform independently of the PCM owner.
 * A peer's repeated INFO0 can hide loss of its first record if the test only
 * checks eventual CONNECT. Both protocols retain 17-bit INFO0a framing. */
static bool observe_info0, sending_menu, info0_in_menu, accepted_in_menu;
static unsigned handoff_samples, first_info0, accepted_info0;
static struct v34_phase2_info_audio_receiver info0_monitor;

extern bool __real_v8_handoff_retry_init(struct v8_handoff_retry *,
	const struct v8_menu *, const struct v8_menu *);
bool __wrap_v8_handoff_retry_init(struct v8_handoff_retry *retry,
	const struct v8_menu *local, const struct v8_menu *joint)
{
	return __real_v8_handoff_retry_init(retry, local, joint) &&
	       (!observe_info0 || v34_phase2_info_audio_receiver_init(
		       &info0_monitor, V34_CALLING_STATION,
		       V90_INFO0A_PAYLOAD_BITS) == V34_PHASE2_INFO_AUDIO_OK);
}

extern enum v8_handoff_result __real_v8_handoff_retry_sample(
	struct v8_handoff_retry *, int16_t, bool, int16_t *);
enum v8_handoff_result __wrap_v8_handoff_retry_sample(
	struct v8_handoff_retry *retry, int16_t received, bool available,
	int16_t *transmit)
{
	struct v34_phase2_info_event event = { 0 };

	if (observe_info0) {
		++handoff_samples;
		if ((!available && v34_phase2_info_audio_receiver_init(
			     &info0_monitor, V34_CALLING_STATION,
			     V90_INFO0A_PAYLOAD_BITS) != V34_PHASE2_INFO_AUDIO_OK) ||
		    (available && v34_phase2_info_audio_receiver_sample(
			     &info0_monitor, received, &event) != V34_PHASE2_INFO_AUDIO_OK))
			return V8_HANDOFF_INVALID;
	}
	enum v8_handoff_result result = __real_v8_handoff_retry_sample(
		retry, received, available, transmit);

	sending_menu = result == V8_HANDOFF_SEND;
	if (observe_info0 && !first_info0 &&
	    event.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
		first_info0 = handoff_samples;
		info0_in_menu = sending_menu;
	}
	return result;
}

extern enum pcm_call_result __real_pcm_call_octet_owned(struct pcm_call *,
	uint8_t, int16_t, int16_t, bool, uint8_t *, struct pcm_call_observation *);
enum pcm_call_result __wrap_pcm_call_octet_owned(struct pcm_call *call,
	uint8_t octet, int16_t clean, int16_t host, bool available,
	uint8_t *transmit, struct pcm_call_observation *event)
{
	bool stored = call->peer_info0_stored;
	enum pcm_call_result result = __real_pcm_call_octet_owned(
		call, octet, clean, host, available, transmit, event);

	if (observe_info0 && !accepted_info0 && !stored &&
	    call->local_role == V90_DIGITAL_MODEM && call->peer_info0_stored) {
		accepted_info0 = handoff_samples;
		accepted_in_menu = sending_menu;
	}
	return result;
}

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

static bool run_handoff_case(enum media_codec codec, unsigned protocol,
	bool damage, unsigned delay_frames)
{
	struct integration_endpoint ends[2];
	struct media_frame_delay delay = { 0 };
	bool ready = false, saw_retry = false;
	unsigned before = damaged_cj_count;

	observe_info0 = !damage && delay_frames != 0;
	sending_menu = info0_in_menu = accepted_in_menu = false;
	handoff_samples = first_info0 = accepted_info0 = 0;
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
	damage_first_cj = damage;
	CHECK(modem_set_sreg(ends[1].modem, SREG_ANS_DELAY, 0) == 0 &&
	      modem_write(ends[0].modem, "ATDpeer\r", 8) == 8 &&
	      modem_report_outgoing_call_connected(ends[0].modem) == 0 &&
	      modem_answer(ends[1].modem) == 0, "call start");
	for (unsigned frame = 0; frame < 2000; ++frame) {
		CHECK(process_test_frame(&ends[0], &ends[1], codec, NULL, &delay, delay_frames) &&
		      endpoint_drain(&ends[0]) && endpoint_drain(&ends[1]), "training");
		saw_retry |= !strcmp(modem_protocol_step(ends[0].modem), "phase2/retry-V8-CM-CJ");
		if (output_contains(&ends[0], "CONNECT") && output_contains(&ends[1], "CONNECT")) {
			ready = true;
			break;
		}
	}
	CHECK(ready && saw_retry && damaged_cj_count == before + (damage ? 1u : 0u) && !damage_first_cj,
	      "missed CJ recovery failed protocol %u codec %u", protocol, codec);
	if (observe_info0)
		CHECK(first_info0 && info0_in_menu && accepted_in_menu &&
		      first_info0 == accepted_info0,
		      "first peer INFO0 discarded: protocol=%u codec=%u delay=%u first=%u accepted=%u",
		      protocol, codec, delay_frames, first_info0, accepted_info0);
	CHECK(ends[0].modem->dp && ends[1].modem->dp &&
	      ends[0].modem->dp->id == (protocol == 90 ? DP_V90 : DP_V92) &&
	      ends[1].modem->dp->id == (protocol == 90 ? DP_V90 : DP_V92),
	      "handoff left the selected PCM protocol");
	const char *payload = "first duplex bytes after V8 handoff\r\n";
	for (unsigned side = 0; side < 2; ++side) {
		ends[side].output_count = 0;
		CHECK(modem_write(ends[side].modem, payload, strlen(payload)) == (int)strlen(payload),
		      "data write");
	}
	ready = false;
	for (unsigned frame = 0; frame < 500; ++frame) {
		CHECK(process_test_frame(&ends[0], &ends[1], codec, NULL, &delay, delay_frames) &&
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
			CHECK(run_handoff_case(codec, protocol, true, 0), "missed CJ case failed");
	return true;
}

static bool test_info0_during_retry(void)
{
	const unsigned delays[] = { 40, 50 };

	for (enum media_codec codec = MEDIA_CODEC_PCMU; codec <= MEDIA_CODEC_PCMA; ++codec)
		for (unsigned protocol = 90; protocol <= 92; protocol += 2)
			for (unsigned i = 0; i < ARRAY_LENGTH(delays); ++i)
				CHECK(run_handoff_case(codec, protocol, false, delays[i]),
				      "delayed handoff failed");
	return true;
}

int main(void)
{
	bool selected = false;
	if (!run_missed_cj_cases(NULL, &selected) || !selected)
		return 1;
	fprintf(stderr, "[ RUN      ] PCM INFO0 reception during V8 retry\n");
	if (!test_info0_during_retry()) {
		fprintf(stderr, "[  FAILED  ] PCM INFO0 reception during V8 retry\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] PCM INFO0 reception during V8 retry\n");
	return 0;
}
