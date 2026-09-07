// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_ranging.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_MAXIMUM_LINE_DELAY = 48,
	TEST_SAMPLE_LIMIT = 6000,
	TEST_EDGE_TOLERANCE_SAMPLES = 4,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_delay_line {
	int16_t samples[TEST_MAXIMUM_LINE_DELAY];
	size_t delay_samples;
	size_t cursor;
};

struct ranging_observation {
	size_t answer_tone_start_sample;
	size_t first_answer_reversal_sample;
	size_t caller_reversal_sample;
	size_t second_answer_reversal_sample;
	size_t answer_probe_ready_sample;
	size_t caller_probe_ready_sample;
	uint16_t answer_round_trip_delay_samples;
	uint16_t caller_round_trip_delay_samples;
	bool answer_delay_measured;
	bool caller_probe_ready;
};

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

static struct v34_info0 calling_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_2800_supported = true,
		.low_carrier_3000_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

static struct v34_info0 answering_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static void initialize_observation(struct ranging_observation *observation)
{
	*observation = (struct ranging_observation){
		.answer_tone_start_sample = SIZE_MAX,
		.first_answer_reversal_sample = SIZE_MAX,
		.caller_reversal_sample = SIZE_MAX,
		.second_answer_reversal_sample = SIZE_MAX,
		.answer_probe_ready_sample = SIZE_MAX,
		.caller_probe_ready_sample = SIZE_MAX,
	};
}

static void observe_ranging_events(
	const struct v34_phase2_ranging_event *caller_event,
	const struct v34_phase2_ranging_event *answer_event,
	size_t sample_index,
	struct ranging_observation *observation)
{
	if (answer_event->kind ==
	    PHASE2_ROUND_TRIP_DELAY_MEASURED) {
		observation->answer_delay_measured = true;
		observation->answer_round_trip_delay_samples =
			answer_event->round_trip_delay_samples;
	}
	if (caller_event->kind == PHASE2_PROBE_RECEIVE_READY) {
		observation->caller_probe_ready = true;
		observation->caller_probe_ready_sample = sample_index;
		observation->caller_round_trip_delay_samples =
			caller_event->round_trip_delay_samples;
	}
	if (answer_event->kind == PHASE2_PROBE_TRANSMIT_READY)
		observation->answer_probe_ready_sample = sample_index;
}

static void observe_phase_transition(
	enum phase2_ranging_phase before,
	enum phase2_info0_transmit_phase transmit_before,
	const struct v34_phase2_ranging *after,
	bool caller, size_t sample_index,
	struct ranging_observation *observation)
{
	if (!caller &&
	    transmit_before != PHASE2_INFO0_TONE &&
	    after->info0_exchange.transport.transmit_phase ==
		    PHASE2_INFO0_TONE &&
	    observation->answer_tone_start_sample == SIZE_MAX)
		observation->answer_tone_start_sample = sample_index + 1u;
	if (before == after->control.phase)
		return;
	if (caller &&
	    after->control.phase ==
		    PHASE2_RESPONSE_TONE_TAIL)
		observation->caller_reversal_sample = sample_index;
	if (!caller &&
	    after->control.phase ==
		    PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
		observation->first_answer_reversal_sample = sample_index;
	if (!caller &&
	    after->control.phase ==
		    PHASE2_RESPONSE_TONE_TAIL)
		observation->second_answer_reversal_sample = sample_index;
}

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
}

static bool check_ranging(
	enum test_codec codec, size_t line_delay_samples, int16_t amplitude,
	uint16_t calling_gain_q15, uint16_t answering_gain_q15)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct v34_phase2_ranging answerer;
	struct test_delay_line caller_to_answer = {
		.delay_samples = line_delay_samples,
	};
	struct test_delay_line answer_to_caller = {
		.delay_samples = line_delay_samples,
	};
	struct ranging_observation observation;
	size_t expected_round_trip = line_delay_samples * 2u;

	initialize_observation(&observation);
	CHECK(line_delay_samples != 0 &&
		      line_delay_samples <= TEST_MAXIMUM_LINE_DELAY,
	      "invalid ranging-test line delay");
	CHECK(v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION,
		      amplitude, &calling) ==
		      V34_PHASE2_RANGING_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      amplitude, &answering) ==
		      V34_PHASE2_RANGING_OK,
	      "ranging endpoints did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_ranging_event caller_event;
		struct v34_phase2_ranging_event answer_event;
		enum phase2_ranging_phase caller_before =
			caller.control.phase;
		enum phase2_ranging_phase answer_before =
			answerer.control.phase;
		enum phase2_info0_transmit_phase
			caller_transmit_before =
				caller.info0_exchange.transport.transmit_phase;
		enum phase2_info0_transmit_phase
			answer_transmit_before =
				answerer.info0_exchange.transport.transmit_phase;
		int16_t caller_transmit;
		int16_t answer_transmit;

		CHECK(v34_phase2_ranging_sample(
			      &caller,
			      delay_line_receive(&answer_to_caller),
			      &caller_transmit, &caller_event) ==
			      V34_PHASE2_RANGING_OK &&
		      v34_phase2_ranging_sample(
			      &answerer,
			      delay_line_receive(&caller_to_answer),
			      &answer_transmit, &answer_event) ==
			      V34_PHASE2_RANGING_OK,
		      "ranging failed for codec %d delay %zu at sample %zu",
		      codec, line_delay_samples, index);
		if (caller_before ==
		    PHASE2_RANGING_PROBE_READY)
			CHECK(caller_transmit == 0,
			      "caller emitted audio after its probe boundary");
		if (answer_before ==
		    PHASE2_RANGING_PROBE_READY)
			CHECK(answer_transmit == 0,
			      "answerer emitted placeholder audio at its probe boundary");
		delay_line_transmit(
			&caller_to_answer,
			codec_round_trip((int16_t)((int32_t)caller_transmit *
				calling_gain_q15 / 32768), codec));
		delay_line_transmit(
			&answer_to_caller,
			codec_round_trip((int16_t)((int32_t)answer_transmit *
				answering_gain_q15 / 32768), codec));
		observe_phase_transition(
			caller_before, caller_transmit_before, &caller, true, index,
			&observation);
		observe_phase_transition(
			answer_before, answer_transmit_before, &answerer, false,
			index, &observation);
		observe_ranging_events(
			&caller_event, &answer_event, index, &observation);
		if (caller.control.phase ==
			    PHASE2_RANGING_PROBE_READY &&
		    answerer.control.phase ==
			    PHASE2_RANGING_PROBE_READY &&
		    index > observation.answer_probe_ready_sample + 1u)
			break;
	}
	CHECK(caller.control.phase ==
		      PHASE2_RANGING_PROBE_READY &&
	      answerer.control.phase ==
		      PHASE2_RANGING_PROBE_READY,
	      "ranging did not reach both probe boundaries for codec %d delay %zu",
	      codec, line_delay_samples);
	CHECK(observation.answer_tone_start_sample != SIZE_MAX &&
	      observation.first_answer_reversal_sample ==
		      observation.answer_tone_start_sample +
			      PHASE2_RANGING_INITIAL_TONE_SAMPLES,
	      "answerer did not preserve the initial 50 ms Tone A interval");
	CHECK(absolute_difference(
		      observation.caller_reversal_sample,
		      observation.first_answer_reversal_sample +
			      line_delay_samples +
			      PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "caller response was not 40 ms from the received Tone A edge");
	CHECK(absolute_difference(
		      observation.second_answer_reversal_sample,
		      observation.caller_reversal_sample +
			      line_delay_samples +
			      PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "answerer response was not 40 ms from the received Tone B edge");
	CHECK(observation.answer_probe_ready_sample ==
		      observation.second_answer_reversal_sample +
			      PHASE2_RANGING_TONE_TAIL_SAMPLES - 1u,
	      "answerer did not preserve the 10 ms post-reversal tone");
	CHECK(absolute_difference(
		      observation.caller_probe_ready_sample,
		      observation.answer_probe_ready_sample +
			      line_delay_samples) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "caller probe boundary did not follow the remote tone tail for codec %d delay %zu",
	      codec, line_delay_samples);
	CHECK(absolute_difference(
		      observation.caller_probe_ready_sample -
			      caller.control.probe_reversal_elapsed_samples,
		      observation.second_answer_reversal_sample + line_delay_samples) <=
		      TEST_EDGE_TOLERANCE_SAMPLES &&
	      answerer.control.probe_reversal_elapsed_samples == 0,
	      "probe handoff lost the physical second-reversal epoch");
	CHECK(absolute_difference(
		      observation.answer_round_trip_delay_samples,
		      expected_round_trip) <= TEST_EDGE_TOLERANCE_SAMPLES &&
	      absolute_difference(
		      observation.caller_round_trip_delay_samples,
		      expected_round_trip) <= TEST_EDGE_TOLERANCE_SAMPLES,
	      "round-trip estimate changed for codec %d delay %zu: caller %u answer %u expected %zu",
	      codec, line_delay_samples,
	      observation.caller_round_trip_delay_samples,
	      observation.answer_round_trip_delay_samples,
	      expected_round_trip);
	CHECK(observation.answer_delay_measured &&
	      observation.caller_probe_ready,
	      "ranging event sequence changed for codec %d delay %zu",
	      codec, line_delay_samples);
	return true;
}

static bool test_bidirectional_ranging(void)
{
	static const size_t delays[] = { 1, 13, 47 };

	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index)
			CHECK(check_ranging(codec, delays[index], TEST_AMPLITUDE,
				32768, 32768),
			      "ranging failed for codec %d", codec);
	return true;
}

static bool test_weak_ranging(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (unsigned int weak_caller = 0; weak_caller < 2; ++weak_caller)
			CHECK(check_ranging(codec, 13, 5320,
				weak_caller ? 1600 : 32768,
				weak_caller ? 32768 : 1600),
			      "weak ranging failed for codec %d caller=%u",
			      codec, weak_caller);
	return true;
}

static bool test_early_turnaround_tolerance(void)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging ranging;
	struct v34_phase2_tone_event tone_event = {
		.kind = V34_PHASE2_TONE_PHASE_REVERSED,
		.reversal_delay_samples =
			V34_PHASE2_TONE_WINDOW_SAMPLES - 3u,
	};
	struct phase2_ranging_event event = { 0 };
	int16_t sample = TEST_AMPLITUDE;

	calling.acknowledges_peer_info0 = true;
	CHECK(v34_phase2_ranging_init_retrain(
		      &ranging, V34_ANSWERING_STATION, TEST_AMPLITUDE,
		      &answering, &calling) == V34_PHASE2_RANGING_OK,
	      "turnaround-tolerance fixture did not initialize");
	ranging.control.phase = PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
	ranging.control.local_tone_samples =
		PHASE2_RANGING_INITIAL_TONE_SAMPLES;
	ranging.control.measurement_elapsed_samples =
		PHASE2_RANGING_TURNAROUND_SAMPLES +
		tone_event.reversal_delay_samples -
		PHASE2_RANGING_TURNAROUND_TOLERANCE_SAMPLES;
	CHECK(v34_phase2_ranging_is_valid(&ranging) &&
	      phase2_ranging_finish_sample(
		      &ranging.control,
		      &ranging.info0_exchange.transport, true,
		      PHASE2_INFO0_TONE, &tone_event, &sample, &event) ==
		      PHASE2_RANGING_OK &&
	      ranging.control.phase == PHASE2_RESPONSE_DELAY &&
	      ranging.control.round_trip_delay_valid &&
	      ranging.control.round_trip_delay_samples == 0u &&
	      ranging.control.response_delay_samples ==
		      PHASE2_RANGING_TURNAROUND_SAMPLES -
			      tone_event.reversal_delay_samples &&
	      v34_phase2_ranging_is_valid(&ranging),
	      "earliest normative turnaround was rejected");
	return true;
}

static bool check_info_interrupted_reversal(
	enum test_codec codec, enum v34_station_role local_role)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_info0 peer_info = local_role == V34_CALLING_STATION ?
		answering : calling;
	struct v34_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	enum phase2_signal_role peer_signal =
		local_role == V34_CALLING_STATION ?
			PHASE2_SIGNAL_A : PHASE2_SIGNAL_B;
	size_t edge_sample = SIZE_MAX;
	size_t local_reversal = SIZE_MAX;
	size_t pre_repeat_samples = 0;
	bool repeat_started = false;
	bool peer_reversed = false;

	peer_info.acknowledges_peer_info0 = true;
	CHECK(v34_info0_encode(&peer_info, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      v34_phase2_ranging_init_retrain(
		      &ranging, local_role, TEST_AMPLITUDE,
		      local_role == V34_CALLING_STATION ?
			      &calling : &answering,
		      &peer_info) == V34_PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, peer_signal, TEST_AMPLITUDE, payload,
		      V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      phase2_info0_exchange_start_tone(&peer) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "V.34 interrupted ranging fixture did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v34_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		int16_t peer_line;
		int16_t ignored;

		if (repeat_started && !peer_reversed &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) == V34_PHASE2_TONE_OK,
			      "V.34 peer following tone did not reverse");
			peer_reversed = true;
			edge_sample = index;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V34_PHASE2_RANGING_OK,
		      "V.34 interrupted ranging failed at %zu", index);
		if (phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL) {
			local_reversal = index;
			break;
		}
		if (!repeat_started &&
		    ranging.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			if (++pre_repeat_samples <
			    PHASE2_RANGING_TURNAROUND_SAMPLES)
				continue;
			CHECK(phase2_info0_exchange_restart_info(
				      &peer, payload, V34_INFO0_PAYLOAD_BITS) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "V.34 peer repeated INFO0 did not start");
			repeat_started = true;
		}
	}
	CHECK(repeat_started && peer_reversed && edge_sample != SIZE_MAX &&
	      local_reversal != SIZE_MAX &&
	      absolute_difference(
		      local_reversal,
		      edge_sample + PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "V.34 INFO-interrupted reversal timing failed for %d/%d at %zu/%zu",
	      codec, local_role, edge_sample, local_reversal);
	return true;
}

static bool test_info_interrupted_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			CHECK(check_info_interrupted_reversal(codec, role),
			      "V.34 INFO interruption failed for %d/%d",
			      codec, role);
	return true;
}

static bool check_unanswered_reversal_recovery(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging answerer;
	struct phase2_info0_exchange peer;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	bool repeat_started = false;
	bool recovery_started = false;
	bool recovery_completed = false;
	bool fresh_reversal = false;

	CHECK(v34_info0_encode(&calling, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE, &answering) ==
		      V34_PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_B, TEST_AMPLITUDE, payload,
		      V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "unanswered-reversal recovery did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v34_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before =
			answerer.control.phase;
		int16_t peer_sample;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_sample, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_ranging_sample(
			      &answerer,
			      codec_round_trip(peer_sample, codec),
			      &ignored, &event) == V34_PHASE2_RANGING_OK,
		      "unanswered-reversal recovery failed for %d at %zu",
		      codec, index);
		if (event.exchange_event.recovery_started) {
			recovery_started = true;
			CHECK(phase_before ==
				      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
			      !answerer.recovery_paused &&
			      answerer.control.phase ==
				      PHASE2_RANGING_INFO0_EXCHANGE,
			      "unanswered reversal was retained across INFO0 for %d",
			      codec);
		}
		recovery_completed |= event.exchange_event.recovery_completed;
		if (recovery_completed &&
		    phase_before == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    answerer.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			fresh_reversal = true;
			break;
		}
		if (!repeat_started &&
		    answerer.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    answerer.control.measurement_elapsed_samples >=
			    PHASE2_RANGING_TURNAROUND_SAMPLES &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(phase2_info0_exchange_restart_info(
				      &peer, payload,
				      V34_INFO0_PAYLOAD_BITS) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "peer INFO0 repeat did not start for %d", codec);
			repeat_started = true;
		}
	}
	CHECK(repeat_started && recovery_started && recovery_completed &&
	      fresh_reversal &&
	      answerer.control.local_tone_samples ==
		      PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
	      answerer.control.measurement_elapsed_samples <= 1u,
	      "unanswered reversal did not restart after INFO0 for %d",
	      codec);
	return true;
}

static bool test_unanswered_reversal_recovery(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_unanswered_reversal_recovery(codec),
		      "unanswered-reversal recovery failed for %d", codec);
	return true;
}

static bool check_rejected_edge_before_info(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	size_t stable_rejected_samples = 0;
	size_t reversal_events = 0;
	bool early_edge_started = false;
	bool repeat_started = false;
	bool info_synchronized = false;
	bool causal_edge_accepted = false;

	calling.acknowledges_peer_info0 = true;
	CHECK(v34_info0_encode(&calling, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      v34_phase2_ranging_init_retrain(
		      &ranging, V34_ANSWERING_STATION, TEST_AMPLITUDE,
		      &answering, &calling) == V34_PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_B, TEST_AMPLITUDE, payload,
		      V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      phase2_info0_exchange_start_tone(&peer) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "V.34 rejected-edge fixture did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v34_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		int16_t peer_line;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V34_PHASE2_RANGING_OK,
		      "V.34 rejected-edge ranging failed at %zu for %d",
		      index, codec);
		if (event.exchange_event.tone_event.kind ==
		    V34_PHASE2_TONE_PHASE_REVERSED) {
			reversal_events++;
			if (reversal_events == 1u)
				CHECK(ranging.control.phase ==
					      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
				      ranging.info0_exchange.transport.tone_detector
					      .carrier
					      .rejected_reversal_active,
				      "V.34 early edge was not rejected transactionally");
		}
		if (repeat_started &&
		    ranging.info0_exchange.transport.info_receiver.frame.stage !=
			    V34_PHASE2_INFO_SEARCH_SYNC) {
			info_synchronized = true;
			CHECK(!ranging.info0_exchange.transport.tone_detector
				       .carrier
				       .rejected_reversal_active,
			      "V.34 INFO retained the rejected-edge gate");
		}
		if (phase_before == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    ranging.control.phase == PHASE2_RESPONSE_DELAY) {
			causal_edge_accepted = true;
			break;
		}
		if (reversal_events == 1u && !repeat_started) {
			stable_rejected_samples++;
			CHECK(ranging.control.phase ==
				      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
			      reversal_events == 1u,
			      "V.34 stable rejected phase changed ranging state");
			if (stable_rejected_samples ==
			    2u * V34_PHASE2_TONE_WINDOW_SAMPLES) {
				CHECK(phase2_info0_exchange_restart_info(
					      &peer, payload,
					      V34_INFO0_PAYLOAD_BITS) ==
					      PHASE2_INFO0_EXCHANGE_OK,
				      "V.34 peer INFO repeat did not start");
				repeat_started = true;
			}
		}
		if (!early_edge_started &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "V.34 early peer edge did not start");
			early_edge_started = true;
		}
	}
	CHECK(early_edge_started && repeat_started && info_synchronized &&
	      stable_rejected_samples ==
		      2u * V34_PHASE2_TONE_WINDOW_SAMPLES &&
		      reversal_events == 2u && causal_edge_accepted &&
		      !ranging.info0_exchange.transport.tone_detector
		       .carrier
		       .rejected_reversal_active,
	      "V.34 rejected-edge recovery failed for %d: edges=%zu stable=%zu sync=%d accepted=%d phase=%d",
	      codec, reversal_events, stable_rejected_samples,
	      info_synchronized, causal_edge_accepted, ranging.control.phase);
	return true;
}

static bool test_rejected_edge_before_info(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_rejected_edge_before_info(codec),
		      "V.34 rejected edge failed for %d", codec);
	return true;
}

static bool check_retrain_residual_reversal(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct phase2_info0_exchange peer;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	size_t stable_restored_samples = 0;
	size_t reversal_events = 0;
	bool early_edge_started = false;
	bool early_pair_cancelled = false;
	bool causal_edge_started = false;
	bool causal_edge_accepted = false;

	answering.acknowledges_peer_info0 = true;
	CHECK(v34_info0_encode(&answering, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      v34_phase2_ranging_init_retrain(
		      &caller, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &calling, &answering) == V34_PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_A, TEST_AMPLITUDE, payload,
		      V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      phase2_info0_exchange_start_tone(&peer) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "V.34 retrain reversal fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v34_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = caller.control.phase;
		int16_t peer_line;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_ranging_sample(
			      &caller, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V34_PHASE2_RANGING_OK,
		      "V.34 retrain reversal failed at %zu for %d",
		      index, codec);
		if (event.exchange_event.tone_event.kind ==
		    V34_PHASE2_TONE_PHASE_REVERSED) {
			reversal_events++;
			if (reversal_events == 1u) {
				CHECK(caller.control.phase ==
					      PHASE2_RESPONSE_DELAY,
				      "V.34 first retrain edge was not tentatively accepted");
				CHECK(v34_phase2_tone_transmitter_reverse(
					      &peer.tone_transmitter) ==
					      V34_PHASE2_TONE_OK,
				      "V.34 early peer phase did not return");
			} else if (reversal_events == 2u) {
				CHECK(caller.control.phase ==
					      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
				      !caller.info0_exchange.transport.tone_detector
					       .carrier.rejected_reversal_active,
				      "V.34 early reversal pair did not roll back");
				early_pair_cancelled = true;
			} else if (reversal_events == 3u) {
				CHECK(phase_before ==
					      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
				      caller.control.phase == PHASE2_RESPONSE_DELAY,
				      "V.34 causal retrain edge was not accepted");
				causal_edge_accepted = true;
				break;
			}
		}
		if (early_pair_cancelled && !causal_edge_started) {
			stable_restored_samples++;
			CHECK(caller.control.phase ==
				      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
			      reversal_events == 2u,
			      "V.34 restored retrain phase changed ranging state");
			if (stable_restored_samples ==
			    2u * V34_PHASE2_TONE_WINDOW_SAMPLES) {
				CHECK(v34_phase2_tone_transmitter_reverse(
					      &peer.tone_transmitter) ==
					      V34_PHASE2_TONE_OK,
			      "V.34 causal peer edge did not start");
				causal_edge_started = true;
			}
		}
		if (!early_edge_started &&
		    caller.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "V.34 residual peer edge did not start");
			early_edge_started = true;
		}
	}
	CHECK(early_edge_started && early_pair_cancelled &&
	      causal_edge_started && causal_edge_accepted &&
	      stable_restored_samples ==
		      2u * V34_PHASE2_TONE_WINDOW_SAMPLES &&
	      reversal_events == 3u &&
	      !caller.info0_exchange.transport.tone_detector.carrier
		       .rejected_reversal_active,
	      "V.34 retrain reversal recovery failed for %d: edges=%zu stable=%zu cancelled=%d causal=%d phase=%d",
	      codec, reversal_events, stable_restored_samples,
	      early_pair_cancelled, causal_edge_accepted,
	      caller.control.phase);
	return true;
}

static bool test_retrain_residual_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_retrain_residual_reversal(codec),
		      "V.34 retrain reversal failed for %d", codec);
	return true;
}

static bool test_missing_second_reversal_recovers(void)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct v34_phase2_ranging answerer;
	struct v34_phase2_tone_transmitter peer_tone;
	int16_t caller_line_sample = 0;
	int16_t answer_line_sample = 0;
	bool caller_reversed = false;
	bool remote_tone_reacquired = false;
	bool recovery_boundary_was_silent = false;
	bool caller_tone_resumed = false;

	CHECK(v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &calling) ==
		      V34_PHASE2_RANGING_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE, &answering) ==
		      V34_PHASE2_RANGING_OK,
	      "timeout fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT && !caller_reversed;
	     ++index) {
		struct v34_phase2_ranging_event caller_event;
		struct v34_phase2_ranging_event answer_event;
		int16_t next_caller;
		int16_t next_answer;

		CHECK(v34_phase2_ranging_sample(
			      &caller, answer_line_sample, &next_caller,
			      &caller_event) == V34_PHASE2_RANGING_OK &&
		      v34_phase2_ranging_sample(
			      &answerer, caller_line_sample, &next_answer,
			      &answer_event) == V34_PHASE2_RANGING_OK,
		      "timeout setup failed at sample %zu", index);
		caller_line_sample = next_caller;
		answer_line_sample = next_answer;
		caller_reversed = caller.control.phase ==
			PHASE2_RESPONSE_TONE_TAIL;
	}
	CHECK(caller_reversed, "timeout fixture never reached caller reversal");
	peer_tone = answerer.info0_exchange.transport.tone_transmitter;
	CHECK(v34_phase2_tone_transmitter_reverse(&peer_tone) ==
		      V34_PHASE2_TONE_OK,
	      "opposite-phase recovery tone did not initialize");
	for (size_t index = 0;
	     index < PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES + 4u;
	     ++index) {
		struct v34_phase2_ranging_event event;
		int16_t discarded;
		int16_t transmit;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &discarded) == V34_PHASE2_TONE_OK &&
		      v34_phase2_ranging_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_RANGING_OK,
		      "missing-reversal wait failed at sample %zu", index);
		CHECK(event.kind == PHASE2_RANGING_NO_EVENT,
		      "missing second reversal emitted a ranging event");
	}
	CHECK(caller.control.phase ==
		      PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE &&
	      v34_phase2_ranging_is_valid(&caller),
	      "missing reversal did not enter muted recovery");
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_ACQUIRE_SAMPLES +
			     V34_PHASE2_TONE_WINDOW_SAMPLES;
	     ++index) {
		struct v34_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = caller.control.phase;
		int16_t peer_line;
		int16_t transmit;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &peer_line) == V34_PHASE2_TONE_OK &&
		      v34_phase2_ranging_sample(
			      &caller, peer_line, &transmit, &event) ==
			      V34_PHASE2_RANGING_OK,
		      "remote-tone reacquisition failed at sample %zu", index);
		if (phase_before ==
			    PHASE2_SIGNAL_B_RECOVERY_WAIT_REMOTE_TONE &&
		    caller.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			remote_tone_reacquired =
				event.exchange_event.tone_event.kind ==
				V34_PHASE2_TONE_STARTED;
			recovery_boundary_was_silent = transmit == 0;
		}
		if (remote_tone_reacquired && transmit != 0)
			caller_tone_resumed = true;
	}
	CHECK(remote_tone_reacquired && recovery_boundary_was_silent &&
	      caller_tone_resumed &&
	      caller.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
	      v34_phase2_ranging_is_valid(&caller),
	      "opposite-phase Tone A did not restart Signal B: "
	      "acquired=%d silent=%d tone=%d phase=%d",
	      remote_tone_reacquired, recovery_boundary_was_silent,
	      caller_tone_resumed, caller.control.phase);
	return true;
}

static bool check_info0_recovery(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct v34_phase2_ranging answerer;
	int16_t caller_line_sample = 0;
	int16_t answer_line_sample = 0;
	size_t caller_recovery_started = 0;
	size_t caller_recovery_completed = 0;
	size_t answer_recovery_started = 0;
	size_t answer_recovery_completed = 0;
	bool answer_initial_info_complete = false;

	CHECK(v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &calling) ==
		      V34_PHASE2_RANGING_OK &&
	      v34_phase2_ranging_init(
		      &answerer, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE, &answering) ==
		      V34_PHASE2_RANGING_OK,
	      "INFO0 recovery endpoints did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT * 2u; ++index) {
		struct v34_phase2_ranging_event caller_event;
		struct v34_phase2_ranging_event answer_event;
		enum phase2_info0_transmit_phase answer_before =
			answerer.info0_exchange.transport.transmit_phase;
		int16_t next_caller;
		int16_t next_answer;

		CHECK(v34_phase2_ranging_sample(
			      &caller, answer_line_sample,
			      &next_caller, &caller_event) ==
			      V34_PHASE2_RANGING_OK &&
		      v34_phase2_ranging_sample(
			      &answerer, caller_line_sample,
			      &next_answer, &answer_event) ==
			      V34_PHASE2_RANGING_OK,
		      "INFO0 recovery failed for codec %d at sample %zu",
		      codec, index);
		caller_recovery_started +=
			caller_event.exchange_event.recovery_started;
		caller_recovery_completed +=
			caller_event.exchange_event.recovery_completed;
		answer_recovery_started +=
			answer_event.exchange_event.recovery_started;
		answer_recovery_completed +=
			answer_event.exchange_event.recovery_completed;

		/* Erase only the answerer's first INFO0.  Its following Tone A
		 * must make the caller repeat INFO0c; all recovery traffic then
		 * crosses the same companded line normally. */
		if (!answer_initial_info_complete) {
			next_answer = 0;
			if (answer_before != PHASE2_INFO0_TONE &&
			    answerer.info0_exchange.transport.transmit_phase ==
				    PHASE2_INFO0_TONE)
				answer_initial_info_complete = true;
		}
		caller_line_sample = codec_round_trip(next_caller, codec);
		answer_line_sample = codec_round_trip(next_answer, codec);
		if (caller.control.phase == PHASE2_RANGING_PROBE_READY &&
		    answerer.control.phase == PHASE2_RANGING_PROBE_READY)
			break;
	}
	CHECK(answer_initial_info_complete &&
	      caller_recovery_started == 1 &&
	      caller_recovery_completed == 1 &&
	      answer_recovery_started == 1 &&
	      answer_recovery_completed == 1,
	      "INFO0 recovery lifecycle changed for codec %d: caller %zu/%zu answer %zu/%zu",
	      codec, caller_recovery_started, caller_recovery_completed,
	      answer_recovery_started, answer_recovery_completed);
	CHECK(caller.info0_exchange.peer_info0_received &&
	      caller.info0_exchange.peer_info0.acknowledges_peer_info0 &&
	      answerer.info0_exchange.peer_info0_received &&
	      answerer.info0_exchange.peer_info0.acknowledges_peer_info0,
	      "INFO0 recovery did not establish mutual acknowledgement for codec %d",
	      codec);
	CHECK(caller.control.phase == PHASE2_RANGING_PROBE_READY &&
	      answerer.control.phase == PHASE2_RANGING_PROBE_READY,
	      "INFO0 recovery did not resume ranging for codec %d (phases %d/%d)",
	      codec, caller.control.phase, answerer.control.phase);
	return true;
}

static bool test_info0_recovery(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_info0_recovery(codec),
		      "INFO0 recovery failed for codec %d", codec);
	return true;
}

static bool check_info0_tone_completion(enum test_codec codec)
{
	struct v34_info0 calling = calling_capabilities();
	struct v34_info0 answering = answering_capabilities();
	struct v34_phase2_ranging caller;
	struct phase2_info0_exchange peer;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	unsigned int peer_sequences = 1;
	size_t recovery_started = 0;
	size_t recovery_completed = 0;

	CHECK(v34_info0_encode(&answering, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      v34_phase2_ranging_init(
		      &caller, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &calling) == V34_PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_A, TEST_AMPLITUDE, payload,
		      V34_INFO0_PAYLOAD_BITS, V34_INFO0_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "tone-completion fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v34_phase2_ranging_event caller_event;
		int16_t peer_sample;
		int16_t caller_sample;

		if (peer.transmit_phase == PHASE2_INFO0_TONE &&
		    peer_sequences < 4) {
			CHECK(phase2_info0_exchange_restart_info(
				      &peer, payload,
				      V34_INFO0_PAYLOAD_BITS) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "peer INFO0 repetition %u failed",
			      peer_sequences);
			peer_sequences++;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_sample, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v34_phase2_ranging_sample(
			      &caller, codec_round_trip(peer_sample, codec),
			      &caller_sample, &caller_event) ==
			      V34_PHASE2_RANGING_OK,
		      "tone completion failed for codec %d at sample %zu",
		      codec, index);
		recovery_started +=
			caller_event.exchange_event.recovery_started;
		recovery_completed +=
			caller_event.exchange_event.recovery_completed;
		if (caller.control.phase != PHASE2_RANGING_INFO0_EXCHANGE)
			break;
	}
	CHECK(peer_sequences == 4 && recovery_started == 1 &&
	      recovery_completed == 1 &&
	      caller.info0_exchange.acknowledgements_complete &&
	      caller.info0_exchange.peer_info0_received &&
	      !caller.info0_exchange.peer_info0.acknowledges_peer_info0 &&
	      caller.info0_exchange.completion_tone_seen &&
	      caller.info0_exchange.transport.tone_detector.carrier
		      .tone_present &&
	      caller.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL,
	      "peer tone did not complete INFO0 recovery for codec %d (sequences %u recovery %zu/%zu phase %d)",
	      codec, peer_sequences, recovery_started, recovery_completed,
	      caller.control.phase);
	CHECK(v34_phase2_tone_detector_reset(
		      &caller.info0_exchange.transport.tone_detector) ==
		      V34_PHASE2_TONE_OK &&
	      !caller.info0_exchange.transport.tone_detector.carrier
		       .tone_present &&
	      v34_phase2_ranging_is_valid(&caller),
	      "later tone-detector reuse invalidated completed INFO0 for codec %d",
	      codec);
	return true;
}

static bool test_info0_tone_completion(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_info0_tone_completion(codec),
		      "INFO0 tone completion failed for codec %d", codec);
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 capabilities = calling_capabilities();
	struct v34_phase2_ranging ranging;
	struct v34_phase2_ranging before;
	struct v34_phase2_ranging_event event = {
		.kind = PHASE2_ROUND_TRIP_DELAY_MEASURED,
	};
	int16_t sample = 1234;

	CHECK(v34_phase2_ranging_init(
		      &ranging, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &capabilities) ==
		      V34_PHASE2_RANGING_OK,
	      "validation fixture did not initialize");
	ranging.info0_exchange.transport.info_receiver.modulation.detector_output =
		NAN;
	before = ranging;
	CHECK(v34_phase2_ranging_sample(
		      &ranging, 0, &sample, &event) ==
		      V34_PHASE2_RANGING_INVALID_ARGUMENT &&
	      memcmp(&ranging, &before, sizeof(ranging)) == 0 &&
	      sample == 1234 &&
	      event.kind == PHASE2_ROUND_TRIP_DELAY_MEASURED,
	      "invalid ranging state mutated state or outputs");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional ranging", test_bidirectional_ranging },
		{ "weak bidirectional ranging", test_weak_ranging },
		{ "early turnaround tolerance", test_early_turnaround_tolerance },
		{ "INFO-interrupted reversal", test_info_interrupted_reversal },
		{ "unanswered reversal recovery",
		  test_unanswered_reversal_recovery },
		{ "rejected edge before INFO", test_rejected_edge_before_info },
		{ "retrain residual reversal", test_retrain_residual_reversal },
		{ "INFO0 recovery", test_info0_recovery },
		{ "INFO0 recovery by tone", test_info0_tone_completion },
		{ "missing second reversal recovery",
		  test_missing_second_reversal_recovers },
		{ "validation", test_validation },
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
