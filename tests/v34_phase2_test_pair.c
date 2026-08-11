// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_test_pair.h"

#include <g711.h>

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	}
	return 0;
}

static int16_t delay_line_receive(const struct test_delay_line *line)
{
	return line->samples[line->cursor];
}

static void delay_line_transmit(
	struct test_delay_line *line, int16_t sample)
{
	line->samples[line->cursor] = sample;
	line->cursor = (line->cursor + 1u) % line->delay_samples;
}

struct sippymodem_v34_info0 test_v34_phase2_capabilities(
	enum sippymodem_v34_station_role role)
{
	if (role == SIPPYMODEM_V34_CALLING_STATION)
		return (struct sippymodem_v34_info0){
			.symbol_rate_2800_supported = true,
			.low_carrier_3000_supported = true,
			.maximum_symbol_rate_difference_steps = 2,
			.transmit_clock_source =
				SIPPYMODEM_V34_INFO0_CLOCK_INTERNAL,
		};
	return (struct sippymodem_v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			SIPPYMODEM_V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static bool endpoint_init(
	struct test_endpoint *endpoint,
	enum sippymodem_v34_station_role role)
{
	struct sippymodem_v34_info0 local_capabilities =
		test_v34_phase2_capabilities(role);
	struct test_endpoint initialized = {
		.stage = TEST_ENDPOINT_RANGING,
	};

	if (sippymodem_v34_phase2_ranging_init(
		    &initialized.ranging, role, TEST_AMPLITUDE,
		    &local_capabilities) != SIPPYMODEM_V34_PHASE2_RANGING_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_sample(
	struct test_endpoint *endpoint, int16_t received_sample,
	int16_t *transmit_sample, struct test_endpoint_event *event)
{
	struct test_endpoint_event emitted = { 0 };

	if (endpoint->stage == TEST_ENDPOINT_RANGING) {
		enum sippymodem_v34_phase2_ranging_event_kind expected_ready =
			endpoint->ranging.info0_exchange.local_role ==
					SIPPYMODEM_V34_CALLING_STATION ?
				SIPPYMODEM_V34_PHASE2_PROBE_RECEIVE_READY :
				SIPPYMODEM_V34_PHASE2_PROBE_TRANSMIT_READY;

		if (sippymodem_v34_phase2_ranging_sample(
			    &endpoint->ranging, received_sample, transmit_sample,
			    &emitted.ranging) != SIPPYMODEM_V34_PHASE2_RANGING_OK)
			return false;
		if (emitted.ranging.kind == expected_ready) {
			if (sippymodem_v34_phase2_initial_probe_init(
				    &endpoint->initial_probe, &endpoint->ranging) !=
			    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_INITIAL_PROBE;
			emitted.entered_initial_probe = true;
		}
	} else if (endpoint->stage == TEST_ENDPOINT_INITIAL_PROBE) {
		enum sippymodem_v34_phase2_initial_probe_event_kind expected_ready =
			endpoint->initial_probe.ranging.info0_exchange.local_role ==
					SIPPYMODEM_V34_CALLING_STATION ?
				SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE :
				SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE;

		if (sippymodem_v34_phase2_initial_probe_sample(
			    &endpoint->initial_probe, received_sample, transmit_sample,
			    &emitted.initial_probe) !=
		    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK)
			return false;
		if (endpoint->continue_to_turnaround &&
		    emitted.initial_probe.kind == expected_ready) {
			if (sippymodem_v34_phase2_probe_turnaround_init(
				    &endpoint->turnaround,
				    &endpoint->initial_probe) !=
			    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_PROBE_TURNAROUND;
			emitted.entered_turnaround = true;
		}
	} else if (endpoint->stage == TEST_ENDPOINT_PROBE_TURNAROUND) {
		enum sippymodem_v34_phase2_probe_turnaround_event_kind expected_ready =
			endpoint->turnaround.initial_probe.ranging.info0_exchange
					.local_role ==
					SIPPYMODEM_V34_CALLING_STATION ?
				SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY :
				SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;

		if (sippymodem_v34_phase2_probe_turnaround_sample(
			    &endpoint->turnaround, received_sample, transmit_sample,
			    &emitted.turnaround) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK)
			return false;
		if (endpoint->continue_to_reciprocal_probe &&
		    emitted.turnaround.kind == expected_ready) {
			if (sippymodem_v34_phase2_reciprocal_probe_init(
				    &endpoint->reciprocal_probe,
				    &endpoint->turnaround) !=
			    SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_RECIPROCAL_PROBE;
			emitted.entered_reciprocal_probe = true;
		}
	} else if (sippymodem_v34_phase2_reciprocal_probe_sample(
			   &endpoint->reciprocal_probe, received_sample, transmit_sample,
			   &emitted.reciprocal_probe) !=
		   SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_OK) {
		return false;
	}
	*event = emitted;
	return true;
}

bool test_v34_phase2_pair_init(
	struct test_pair *pair, enum test_codec codec, size_t delay_samples)
{
	struct test_pair initialized = {
		.caller_to_answerer.delay_samples = delay_samples,
		.answerer_to_caller.delay_samples = delay_samples,
		.codec = codec,
	};

	if (pair == NULL || (unsigned int)codec > TEST_CODEC_PCMA ||
	    delay_samples == 0 ||
	    delay_samples > TEST_MAXIMUM_LINE_DELAY ||
	    !endpoint_init(
		    &initialized.caller,
		    SIPPYMODEM_V34_CALLING_STATION) ||
	    !endpoint_init(
		    &initialized.answerer,
		    SIPPYMODEM_V34_ANSWERING_STATION))
		return false;
	*pair = initialized;
	return true;
}

bool test_v34_phase2_pair_sample(
	struct test_pair *pair, struct test_pair_event *event)
{
	struct test_pair_event emitted = { 0 };

	if (pair == NULL || event == NULL ||
	    !endpoint_sample(
		    &pair->caller,
		    delay_line_receive(&pair->answerer_to_caller),
		    &emitted.caller_transmit, &emitted.caller) ||
	    !endpoint_sample(
		    &pair->answerer,
		    delay_line_receive(&pair->caller_to_answerer),
		    &emitted.answerer_transmit, &emitted.answerer))
		return false;
	delay_line_transmit(
		&pair->caller_to_answerer,
		codec_round_trip(emitted.caller_transmit, pair->codec));
	delay_line_transmit(
		&pair->answerer_to_caller,
		codec_round_trip(emitted.answerer_transmit, pair->codec));
	*event = emitted;
	return true;
}
