// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_phase2_info1_exchange.h>

#include "test.h"

#include <string.h>

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool check_short_exchange(unsigned mode, enum g711_law law, unsigned delay)
{
	struct pcm_info1_selection selection = {
		.startup = { PCM_PROTOCOL_V92, PCM_PHASE2_SHORT },
		.upstream = mode == 2 ? PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34,
		.short_high_carrier = mode == 1,
	};
	struct v34_phase2_tone_transmitter tone;
	struct pcm_phase2_info1_exchange analog, digital;
	int16_t line[257] = { 0 };
	uint8_t expected[V90_INFO1A_PAYLOAD_BYTES];
	uint8_t actual[V90_INFO1A_PAYLOAD_BYTES];
	bool received = false, sent = false;

	if (mode == 2)
		selection.analog.pcm = (struct v92_pcm_info1a){
			.maximum_total_coefficients = 192,
			.maximum_section_coefficients = 128,
			.local_md_length_276_symbol_units = 1,
			.training_ucode = 104,
		};
	else
		selection.analog.conventional = (struct v90_info1a){
			.local_md_length_35_ms_units = 1, .training_ucode = 104,
			.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
			.frequency_offset_1_50_hz_units =
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
		};
	CHECK(v34_phase2_tone_transmitter_init(&tone, V34_ANSWERING_STATION, 12000) ==
		      V34_PHASE2_TONE_OK &&
	      pcm_info1_encode_short(&selection, expected, sizeof(expected)) == PCM_INFO1_OK,
	      "typed short fixtures failed");
	for (unsigned i = 0; i < 137; ++i) {
		int16_t sample;
		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) == V34_PHASE2_TONE_OK,
		      "typed tone did not advance");
	}
	CHECK(pcm_phase2_info1_init_short_analog(&analog, &tone, &selection) ==
		      PHASE2_INFO1_OK &&
	      pcm_phase2_info1_init_short_digital(&digital, &selection.startup,
		      18000) == PHASE2_INFO1_OK,
	      "typed short constructors failed");
	for (unsigned i = 0; i < 10000; ++i) {
		struct pcm_phase2_info1_event ae, de;
		int16_t ao, d_out;
		CHECK(pcm_phase2_info1_sample(&analog, 0, &ao, &ae) == PHASE2_INFO1_OK &&
		      pcm_phase2_info1_sample(&digital, line[i % delay], &d_out, &de) ==
			      PHASE2_INFO1_OK &&
		      d_out == 0 && !analog.digital_record_stored &&
		      !digital.digital_record_stored,
		      "short adapter fabricated a digital record");
		CHECK(!pcm_phase2_info1_retry_pending(&analog) &&
		      !pcm_phase2_info1_retry_pending(&digital),
		      "short adapter permits INFO1d resend");
		if (ae.kind == PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT)
			sent = true;
		if (de.kind == PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			struct pcm_info1_selection decoded = {
				.startup = selection.startup,
				.short_high_carrier = digital.analog.short_high_carrier,
				.analog = digital.analog.record,
				.upstream = digital.analog.kind == PCM_INFO1A_PCM ?
					PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34,
			};
			CHECK(!received && decoded.upstream == selection.upstream &&
			      decoded.short_high_carrier == selection.short_high_carrier &&
			      de.peer_record.analog.short_high_carrier ==
				      selection.short_high_carrier &&
			      de.peer_record.analog.kind == digital.analog.kind &&
			      pcm_info1_encode_short(&decoded, actual, sizeof(actual)) ==
				      PCM_INFO1_OK &&
			      memcmp(actual, expected, sizeof(actual)) == 0,
			      "typed short decision differs from actual INFO1a");
			received = true;
		}
		line[i % delay] = quantize(law, ao);
		if (analog.control.phase == PHASE2_INFO1_COMPLETE &&
		    digital.control.phase == PHASE2_INFO1_COMPLETE)
			break;
	}
	CHECK(sent && received &&
	      analog.control.phase == PHASE2_INFO1_COMPLETE &&
	      digital.control.phase == PHASE2_INFO1_COMPLETE,
	      "typed short exchange did not complete");
	{
		struct pcm_phase2_info1_exchange invalid = digital;

		invalid.digital_record_stored = true;
		CHECK(!pcm_phase2_info1_is_valid(&invalid),
		      "short selection admits a fabricated digital record");
		invalid = digital;
		invalid.protocol = PCM_PROTOCOL_V90;
		CHECK(!pcm_phase2_info1_is_valid(&invalid),
		      "V.90 admits a short-startup selection");
		invalid = digital;
		invalid.info1d_resends = 1;
		CHECK(!pcm_phase2_info1_is_valid(&invalid),
		      "short selection admits an INFO1d resend");
		if (mode == 2) {
			invalid = digital;
			invalid.analog.short_high_carrier = true;
			CHECK(!pcm_phase2_info1_is_valid(&invalid),
			      "PCM upstream admits the conventional carrier bit");
		}
	}
	return true;
}

static bool test_validation(void)
{
	const struct pcm_startup_selection full = { PCM_PROTOCOL_V92, PCM_PHASE2_FULL };
	const struct pcm_startup_selection short_start = {
		PCM_PROTOCOL_V92, PCM_PHASE2_SHORT,
	};
	struct pcm_phase2_info1_exchange exchange = { 0 }, before = exchange;
	struct pcm_phase2_info1_event event;
	int16_t output;

	CHECK(pcm_phase2_info1_init_short_digital(&exchange, &full, 10) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      pcm_phase2_info1_init_short_digital(&exchange, &short_start, 0) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      pcm_phase2_info1_init_short_analog(&exchange, NULL, NULL) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0,
	      "typed constructor accepted full mode or changed rejected output");
	CHECK(pcm_phase2_info1_init_short_digital(&exchange, &short_start, 3) ==
		      PHASE2_INFO1_OK,
	      "typed timeout fixture failed");
	for (unsigned i = 0; i < 3; ++i)
		CHECK(pcm_phase2_info1_sample(&exchange, 0, &output, &event) ==
			      PHASE2_INFO1_OK && output == 0,
		      "typed timeout sample failed");
	CHECK(event.kind == PCM_PHASE2_INFO1_EVENT_FAILED &&
	      event.failure == PHASE2_INFO1_PEER_TIMEOUT &&
	      !exchange.digital_record_stored &&
	      !pcm_phase2_info1_retry_pending(&exchange),
	      "typed timeout created INFO1d recovery");
	before = exchange;
	CHECK(pcm_phase2_info1_sample(&exchange, INT16_MAX, &output, &event) ==
		      PHASE2_INFO1_OK && output == 0 &&
	      event.kind == PCM_PHASE2_INFO1_EVENT_NONE &&
	      memcmp(&before, &exchange, sizeof(exchange)) == 0,
	      "typed short timeout is not stable");
	{
		struct v34_phase2_tone_transmitter tone;
		struct pcm_info1_selection selected = {
			.startup = short_start,
			.upstream = PCM_UPSTREAM_PCM,
			.analog.pcm = {
				.maximum_total_coefficients = 192,
				.maximum_section_coefficients = 128,
				.training_ucode = 104,
			},
		};

		CHECK(v34_phase2_tone_transmitter_init(&tone, V34_CALLING_STATION,
			      12000) == V34_PHASE2_TONE_OK &&
		      pcm_phase2_info1_init_short_analog(&exchange, &tone, &selected) ==
			      PHASE2_INFO1_INVALID_ARGUMENT &&
		      memcmp(&before, &exchange, sizeof(exchange)) == 0,
		      "short analogue constructor accepts Tone B");
		CHECK(v34_phase2_tone_transmitter_init(&tone, V34_ANSWERING_STATION,
			      12000) == V34_PHASE2_TONE_OK,
		      "short constructor validation tone failed");
		selected.short_high_carrier = true;
		CHECK(pcm_phase2_info1_init_short_analog(&exchange, &tone, &selected) ==
			      PHASE2_INFO1_INVALID_ARGUMENT &&
		      memcmp(&before, &exchange, sizeof(exchange)) == 0,
		      "invalid short selection changed output");
		selected.short_high_carrier = false;
		CHECK(pcm_phase2_info1_init_short_analog(&exchange, &tone, &selected) ==
			      PHASE2_INFO1_OK, "short selection could not initialize");
		before = exchange;
		CHECK(pcm_phase2_info1_reply_pcm(&exchange, &selected.analog.pcm) ==
			      PHASE2_INFO1_INVALID_ARGUMENT &&
		      memcmp(&before, &exchange, sizeof(exchange)) == 0,
		      "short exchange accepted a full-startup reply");
	}
	return true;
}

static bool test_semantic_rejection(void)
{
	/* Table-19 shape with a forbidden conventional upstream symbol rate.
	 * The INFO frame generator supplies a correct CRC for these bytes. */
	const uint8_t payload[] = { 0x02, 0x83, 0x94, 0x30, 0x04 };
	const struct pcm_startup_selection startup = {
		PCM_PROTOCOL_V92, PCM_PHASE2_SHORT,
	};
	struct pcm_info1_selection selected;
	CHECK(pcm_info1_select_short(&startup, payload, sizeof(payload),
		      V90_INFO1A_PAYLOAD_BITS, &selected) == PCM_INFO1_INVALID_PAYLOAD,
	      "semantic rejection fixture is valid");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v34_phase2_tone_transmitter tone;
		struct phase2_info1_exchange analog;
		struct pcm_phase2_info1_exchange digital;
		unsigned rejected = 0;
		CHECK(v34_phase2_tone_transmitter_init(&tone, V34_ANSWERING_STATION,
			      12000) == V34_PHASE2_TONE_OK &&
		      phase2_info1_init_short_signal_a(&analog, &tone, payload,
			      V90_INFO1A_PAYLOAD_BITS) == PHASE2_INFO1_OK &&
		      pcm_phase2_info1_init_short_digital(&digital, &startup, 1800) ==
			      PHASE2_INFO1_OK,
		      "semantic rejection wire did not initialize");
		for (unsigned i = 0; i < 1800; ++i) {
			struct phase2_info1_event ae;
			struct pcm_phase2_info1_event de;
			int16_t sample, output;
			CHECK(phase2_info1_sample(&analog, 0, &sample, &ae) == PHASE2_INFO1_OK &&
			      pcm_phase2_info1_sample(&digital, quantize(law, sample),
				      &output, &de) == PHASE2_INFO1_OK &&
			      output == 0 && !digital.analog_record_stored &&
			      !digital.digital_record_stored &&
			      digital.control.peer_deadline_elapsed_samples == i + 1u,
			      "invalid INFO1a renewed its budget or stored a selection");
			if (de.kind == PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED) {
				CHECK(de.frame_event.type == V34_PHASE2_INFO_FRAME_RECEIVED,
				      "semantic fixture failed wire CRC instead");
				rejected++;
			}
		}
		CHECK(rejected == 1 && digital.control.phase == PHASE2_INFO1_FAILED &&
		      digital.control.failure == PHASE2_INFO1_PEER_TIMEOUT,
		      "semantic rejection did not expire at the original budget");
	}
	return true;
}

static bool test_short_exchange(void)
{
	const unsigned delays[] = { 1, 257 };

	for (unsigned mode = 0; mode < 3; ++mode)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned d = 0; d < ARRAY_LENGTH(delays); ++d)
				CHECK(check_short_exchange(mode, law, delays[d]),
				      "mode %u law %u delay %u failed", mode, law, delays[d]);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "typed short INFO1a exchange", test_short_exchange },
		{ "semantic INFO1a rejection", test_semantic_rejection },
		{ "short adapter validation", test_validation },
	};
	unsigned failures = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		if (tests[i].run())
			fprintf(stderr, "[       OK ] %s\n", tests[i].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[i].name);
			++failures;
		}
	}
	return failures != 0;
}
