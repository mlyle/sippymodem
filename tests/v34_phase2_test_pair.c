// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_test_pair.h"

#include <g711.h>

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return g711_decode_pcma(
			g711_encode_pcma(sample));
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

static bool channel_is_valid(const struct test_pair *pair)
{
	return pair != NULL && (unsigned int)pair->codec <= TEST_CODEC_PCMA &&
	       pair->caller_to_answerer.delay_samples != 0 &&
	       pair->caller_to_answerer.delay_samples <=
		       TEST_MAXIMUM_LINE_DELAY &&
	       pair->answerer_to_caller.delay_samples ==
		       pair->caller_to_answerer.delay_samples &&
	       pair->caller_to_answerer.cursor <
		       pair->caller_to_answerer.delay_samples &&
	       pair->answerer_to_caller.cursor <
		       pair->answerer_to_caller.delay_samples;
}

bool test_v34_phase2_channel_receive(
	const struct test_pair *pair, int16_t *caller_receive,
	int16_t *answerer_receive)
{
	if (!channel_is_valid(pair) || caller_receive == NULL ||
	    answerer_receive == NULL)
		return false;
	*caller_receive = delay_line_receive(&pair->answerer_to_caller);
	*answerer_receive = delay_line_receive(&pair->caller_to_answerer);
	return true;
}

bool test_v34_phase2_channel_transmit(
	struct test_pair *pair, int16_t caller_transmit,
	int16_t answerer_transmit)
{
	if (!channel_is_valid(pair))
		return false;
	delay_line_transmit(
		&pair->caller_to_answerer,
		codec_round_trip(caller_transmit, pair->codec));
	delay_line_transmit(
		&pair->answerer_to_caller,
		codec_round_trip(answerer_transmit, pair->codec));
	return true;
}

struct v34_info0 test_v34_phase2_capabilities(
	enum v34_station_role role)
{
	if (role == V34_CALLING_STATION)
		return (struct v34_info0){
			.symbol_rate_2800_supported = true,
			.low_carrier_3000_supported = true,
			.maximum_symbol_rate_difference_steps = 2,
			.transmit_clock_source =
				V34_INFO0_CLOCK_INTERNAL,
		};
	return (struct v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static bool endpoint_init(
	struct test_endpoint *endpoint,
	enum v34_station_role role)
{
	struct v34_info0 local_capabilities =
		test_v34_phase2_capabilities(role);
	struct test_endpoint initialized = {
		.stage = TEST_ENDPOINT_RANGING,
	};

	if (v34_phase2_ranging_init(
		    &initialized.ranging, role, TEST_AMPLITUDE,
		    &local_capabilities) != V34_PHASE2_RANGING_OK)
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
		enum phase2_ranging_event_kind expected_ready =
			endpoint->ranging.info0_exchange.local_role ==
					V34_CALLING_STATION ?
				PHASE2_PROBE_RECEIVE_READY :
				PHASE2_PROBE_TRANSMIT_READY;

		if (v34_phase2_ranging_sample(
			    &endpoint->ranging, received_sample, transmit_sample,
			    &emitted.ranging) != V34_PHASE2_RANGING_OK)
			return false;
		if (emitted.ranging.kind == expected_ready) {
			if (v34_phase2_initial_probe_init(
				    &endpoint->initial_probe, &endpoint->ranging) !=
			    V34_PHASE2_INITIAL_PROBE_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_INITIAL_PROBE;
			emitted.entered_initial_probe = true;
		}
	} else if (endpoint->stage == TEST_ENDPOINT_INITIAL_PROBE) {
		enum v34_phase2_initial_probe_event_kind expected_ready =
			endpoint->initial_probe.ranging.info0_exchange.local_role ==
					V34_CALLING_STATION ?
				V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE :
				V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE;

		if (v34_phase2_initial_probe_sample(
			    &endpoint->initial_probe, received_sample, transmit_sample,
			    &emitted.initial_probe) !=
		    V34_PHASE2_INITIAL_PROBE_OK)
			return false;
		if (endpoint->continue_to_turnaround &&
		    emitted.initial_probe.kind == expected_ready) {
			if (v34_phase2_probe_turnaround_init(
				    &endpoint->turnaround,
				    &endpoint->initial_probe) !=
			    V34_PHASE2_PROBE_TURNAROUND_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_PROBE_TURNAROUND;
			emitted.entered_turnaround = true;
		}
	} else if (endpoint->stage == TEST_ENDPOINT_PROBE_TURNAROUND) {
		enum v34_phase2_probe_turnaround_event_kind expected_ready =
			endpoint->turnaround.initial_probe.ranging.info0_exchange
					.local_role ==
					V34_CALLING_STATION ?
				V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY :
				V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;

		if (v34_phase2_probe_turnaround_sample(
			    &endpoint->turnaround, received_sample, transmit_sample,
			    &emitted.turnaround) !=
		    V34_PHASE2_PROBE_TURNAROUND_OK)
			return false;
		if (endpoint->continue_to_reciprocal_probe &&
		    emitted.turnaround.kind == expected_ready) {
			if (v34_phase2_reciprocal_probe_init(
				    &endpoint->reciprocal_probe,
				    &endpoint->turnaround) !=
			    V34_PHASE2_RECIPROCAL_PROBE_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_RECIPROCAL_PROBE;
			emitted.entered_reciprocal_probe = true;
		}
	} else if (v34_phase2_reciprocal_probe_sample(
			   &endpoint->reciprocal_probe, received_sample, transmit_sample,
			   &emitted.reciprocal_probe) !=
		   V34_PHASE2_RECIPROCAL_PROBE_OK) {
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
		    V34_CALLING_STATION) ||
	    !endpoint_init(
		    &initialized.answerer,
		    V34_ANSWERING_STATION))
		return false;
	*pair = initialized;
	return true;
}

bool test_v34_phase2_pair_sample(
	struct test_pair *pair, struct test_pair_event *event)
{
	struct test_pair_event emitted = { 0 };
	int16_t caller_receive;
	int16_t answerer_receive;

	if (pair == NULL || event == NULL ||
	    !test_v34_phase2_channel_receive(
		    pair, &caller_receive, &answerer_receive) ||
	    !endpoint_sample(
		    &pair->caller, caller_receive,
		    &emitted.caller_transmit, &emitted.caller) ||
	    !endpoint_sample(
		    &pair->answerer, answerer_receive,
		    &emitted.answerer_transmit, &emitted.answerer))
		return false;
	if (!test_v34_phase2_channel_transmit(
		    pair, emitted.caller_transmit,
		    emitted.answerer_transmit))
		return false;
	*event = emitted;
	return true;
}
