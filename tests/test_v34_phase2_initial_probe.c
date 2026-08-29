// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_initial_probe.h>
#include <v34_phase2_probe_turnaround.h>

#include "v34_phase2_test_pair.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SAMPLE_LIMIT = 12000,
	TEST_STABLE_TONE_SAMPLES = 160,
	TEST_EARLY_TONE_START_L2_SAMPLES =
		V34_PHASE2_TONE_WINDOW_SAMPLES * 3,
	TEST_EARLY_TONE_LIMIT_L2_SAMPLES =
		V34_PHASE2_PROBE_L1_SAMPLES / 2,
};

struct turn_observation {
	size_t caller_ready;
	size_t answerer_ready;
	size_t caller_first_probe_sample;
	size_t answerer_first_probe_sample;
	size_t answerer_l2_started;
	size_t caller_measurement_ready;
	size_t caller_probe_complete;
	size_t answerer_probe_complete;
	size_t caller_tone_pair_active;
	uint32_t answerer_l2_samples;
	uint64_t caller_tone_energy;
	uint64_t answerer_tone_energy;
	bool caller_acquired;
	bool caller_l2_started;
};

struct turnaround_observation {
	size_t caller_entered;
	size_t answerer_entered;
	size_t answerer_first_a;
	size_t answerer_first_reversed_a;
	size_t caller_a_reversal_event;
	size_t caller_estimated_a_reversal;
	size_t caller_first_reversed_b;
	size_t caller_ready;
	size_t answerer_b_reversal_event;
	size_t answerer_estimated_b_reversal;
	size_t answerer_ready;
	uint16_t answerer_b_reversal_delay;
	uint16_t answerer_initial_a_samples;
	uint16_t answerer_reversed_a_samples;
	uint16_t caller_reversed_b_samples;
};

static bool completed_ranging_pair(
	struct v34_phase2_ranging *caller,
	struct v34_phase2_ranging *answerer);

static int64_t sample_magnitude(int16_t sample)
{
	return sample < 0 ? -(int64_t)sample : sample;
}

static void initialize_observation(struct turn_observation *observation)
{
	*observation = (struct turn_observation){
		.caller_ready = SIZE_MAX,
		.answerer_ready = SIZE_MAX,
		.caller_first_probe_sample = SIZE_MAX,
		.answerer_first_probe_sample = SIZE_MAX,
		.answerer_l2_started = SIZE_MAX,
		.caller_measurement_ready = SIZE_MAX,
		.caller_probe_complete = SIZE_MAX,
		.answerer_probe_complete = SIZE_MAX,
		.caller_tone_pair_active = SIZE_MAX,
	};
}

static void observe_turn(
	const struct test_pair *pair, const struct test_pair_event *event,
	bool caller_was_probing, bool answerer_was_probing,
	size_t sample_index, struct turn_observation *observation)
{
	if (event->caller.entered_initial_probe)
		observation->caller_ready = sample_index;
	if (event->answerer.entered_initial_probe)
		observation->answerer_ready = sample_index;
	if (caller_was_probing &&
	    observation->caller_first_probe_sample == SIZE_MAX)
		observation->caller_first_probe_sample = sample_index;
	if (answerer_was_probing &&
	    observation->answerer_first_probe_sample == SIZE_MAX)
		observation->answerer_first_probe_sample = sample_index;
	if (event->caller.initial_probe.receive_event.kind ==
	    V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED)
		observation->caller_acquired = true;
	if (event->caller.initial_probe.receive_event.kind ==
	    V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED)
		observation->caller_l2_started = true;
	if (event->caller.initial_probe.receive_event.kind ==
	    V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY)
		observation->caller_measurement_ready = sample_index;
	if (event->answerer.initial_probe.transmit_event.kind ==
	    V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED)
		observation->answerer_l2_started = sample_index;
	if (event->caller.initial_probe.kind ==
	    PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE)
		observation->caller_probe_complete = sample_index;
	if (event->answerer.initial_probe.kind ==
	    PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
		observation->answerer_probe_complete = sample_index;
		observation->answerer_l2_samples =
			event->answerer.initial_probe.transmit_event.l2_samples;
	}
	if (event->caller.initial_probe.kind ==
	    PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE)
		observation->caller_tone_pair_active = sample_index;
	if (pair->caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
	    pair->caller.initial_probe.control.phase !=
		    PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING)
		observation->caller_tone_energy +=
			(uint64_t)sample_magnitude(event->caller_transmit);
	if (pair->answerer.stage == TEST_ENDPOINT_INITIAL_PROBE &&
	    pair->answerer.initial_probe.control.phase ==
		    PHASE2_INITIAL_PROBE_SIGNAL_A_TONE)
		observation->answerer_tone_energy +=
			(uint64_t)sample_magnitude(event->answerer_transmit);
}

static double measurement_power(
	const struct v34_phase2_probe_spectrum_measurement *spectrum)
{
	double power = 0.0;

	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index)
		power += spectrum->harmonics[index].mean_square;
	return power;
}

static bool check_initial_probe_turn(
	enum test_codec codec, size_t delay_samples)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct test_pair pair;
	struct turn_observation observation;
	size_t stable_samples = 0;

	CHECK(test_v34_phase2_pair_init(&pair, codec, delay_samples),
	      "probe-turn endpoints did not initialize");
	initialize_observation(&observation);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;
		bool caller_was_probing =
			pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE;
		bool answerer_was_probing =
			pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE;

		CHECK(test_v34_phase2_pair_sample(&pair, &event),
		      "probe turn failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);
		observe_turn(
			&pair, &event, caller_was_probing, answerer_was_probing,
			index, &observation);
		if (pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.caller.initial_probe.control.phase ==
			    PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE &&
		    pair.answerer.initial_probe.control.phase ==
			    PHASE2_INITIAL_PROBE_SIGNAL_A_TONE) {
			if (++stable_samples == TEST_STABLE_TONE_SAMPLES)
				break;
		} else {
			stable_samples = 0;
		}
	}
	CHECK(stable_samples == TEST_STABLE_TONE_SAMPLES &&
	      v34_phase2_initial_probe_is_valid(
		      &pair.caller.initial_probe) &&
	      v34_phase2_initial_probe_is_valid(
		      &pair.answerer.initial_probe),
	      "probe turn did not establish the tone pair for codec %d delay %zu",
	      codec, delay_samples);
	CHECK(observation.caller_ready != SIZE_MAX &&
	      observation.answerer_ready != SIZE_MAX &&
	      observation.caller_first_probe_sample ==
		      observation.caller_ready + 1u &&
	      observation.answerer_first_probe_sample ==
		      observation.answerer_ready + 1u,
	      "ranging did not hand off the following sample");
	CHECK(observation.answerer_l2_started ==
		      observation.answerer_first_probe_sample +
			      V34_PHASE2_PROBE_L1_SAMPLES - 1u,
	      "answerer changed the exact 160 ms L1 boundary");
	CHECK(observation.caller_acquired && observation.caller_l2_started &&
	      observation.caller_measurement_ready != SIZE_MAX &&
	      observation.caller_probe_complete ==
		      observation.caller_measurement_ready + 1u,
	      "caller did not complete the measured probe causally");
	CHECK(observation.answerer_probe_complete >
		      observation.caller_probe_complete + delay_samples &&
	      observation.caller_tone_pair_active >
		      observation.answerer_probe_complete + delay_samples &&
	      observation.answerer_l2_samples >=
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      observation.answerer_l2_samples <
		      (uint32_t)
			      V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		      pair.answerer.initial_probe.ranging.control
			      .round_trip_delay_samples,
	      "tone response did not end answerer L2 within its bound");
	CHECK(v34_phase2_initial_probe_measure(
		      &pair.caller.initial_probe, &measurement) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples ==
		      V34_PHASE2_PROBE_L1_SAMPLES,
	      "caller did not retain exact probe measurement windows");
	{
		double l1_power = measurement_power(&measurement.l1);
		double l2_power = measurement_power(&measurement.l2);

		CHECK(l2_power > 0.0 && l1_power / l2_power > 3.9 &&
		      l1_power / l2_power < 4.1,
		      "probe power ratio changed for codec %d: %.6f", codec,
		      l1_power / l2_power);
	}
	CHECK(observation.caller_tone_energy != 0 &&
	      observation.answerer_tone_energy != 0,
	      "established tones remained silent");
	return true;
}

static bool test_bidirectional_turn(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
	} cases[] = {
		{ TEST_CODEC_LINEAR, 1 },
		{ TEST_CODEC_PCMU, 13 },
		{ TEST_CODEC_PCMA, 47 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_initial_probe_turn(
			      cases[index].codec, cases[index].delay_samples),
		      "initial probe turn failed for case %zu", index);
	return true;
}

static bool test_answerer_accepts_early_tone_b(void)
{
	struct v34_phase2_ranging caller_ranging;
	struct v34_phase2_ranging answerer_ranging;
	struct v34_phase2_initial_probe answerer;
	struct v34_phase2_initial_probe_event event = { 0 };
	struct v34_phase2_tone_transmitter peer_tone;
	int16_t prior_local_sample = 0;
	bool peer_active = false;

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging) &&
	      v34_phase2_initial_probe_init(
		      &answerer, &answerer_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &peer_tone, V34_CALLING_STATION, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "early Tone B fixture did not initialize");

	for (size_t samples = 0; samples < TEST_SAMPLE_LIMIT; ++samples) {
		const struct v34_phase2_probe_sequence *sequence =
			&answerer.control.path.transmit;
		const bool l2_active =
			sequence->phase ==
			V34_PHASE2_PROBE_SEQUENCE_PHASE_L2;
		const uint32_t l2_samples = l2_active ?
			sequence->samples_emitted -
				V34_PHASE2_PROBE_L1_SAMPLES :
			0;
		int32_t received = prior_local_sample / 2;
		int16_t transmit;

		if (l2_active &&
		    l2_samples >= TEST_EARLY_TONE_START_L2_SAMPLES) {
			int16_t peer_sample;

			CHECK(v34_phase2_tone_transmitter_sample(
				      &peer_tone, &peer_sample) ==
				      V34_PHASE2_TONE_OK,
			      "early Tone B generation failed");
			received += peer_sample;
			peer_active = true;
		}
		CHECK(v34_phase2_initial_probe_sample(
			      &answerer, (int16_t)received, &transmit, &event) ==
			      V34_PHASE2_INITIAL_PROBE_OK &&
		      (peer_active || event.kind !=
				      PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE),
		      "answerer mistook its probe echo for Tone B");
		prior_local_sample = transmit;
		if (event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE)
			break;
	}
	CHECK(peer_active && event.kind ==
		      PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE &&
	      event.transmit_event.l2_samples <
		      TEST_EARLY_TONE_LIMIT_L2_SAMPLES &&
	      answerer.control.phase == PHASE2_INITIAL_PROBE_SIGNAL_A_TONE,
	      "answerer did not accept early Tone B during L2");
	return true;
}

static bool test_answerer_receives_concurrent_probe(void)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_ranging caller_ranging;
	struct v34_phase2_ranging answerer_ranging;
	struct v34_phase2_initial_probe answerer;
	struct v34_phase2_probe_sequence peer_probe;
	struct v34_phase2_initial_probe_event event;
	uint64_t local_probe_magnitude = 0;
	int16_t receive_gain = 0;
	bool completed = false;

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging) &&
	      v34_phase2_initial_probe_init(
		      &answerer, &answerer_ranging, true) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      v34_phase2_probe_sequence_init(
		      &peer_probe, TEST_AMPLITUDE,
		      answerer_ranging.control.round_trip_delay_samples) ==
		      V34_PHASE2_PROBE_SEQUENCE_OK,
	      "concurrent-probe fixture did not initialize");
	for (size_t sample = 0; sample < TEST_SAMPLE_LIMIT; ++sample) {
		struct v34_phase2_probe_sequence_event peer_event;
		int16_t peer_transmit;
		int16_t local_transmit;

		CHECK(v34_phase2_probe_sequence_sample(
			      &peer_probe, false, &peer_transmit,
			      &peer_event) == V34_PHASE2_PROBE_SEQUENCE_OK &&
		      v34_phase2_initial_probe_sample(
			      &answerer, peer_transmit, &local_transmit,
			      &event) == V34_PHASE2_INITIAL_PROBE_OK,
		      "concurrent probe stopped at sample %zu", sample);
		local_probe_magnitude +=
			(uint64_t)sample_magnitude(local_transmit);
		if (event.receive_event.kind ==
		    V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED) {
			completed = true;
			break;
		}
	}
	CHECK(completed && local_probe_magnitude != 0 &&
	      answerer.concurrent_receive.phase ==
		      V34_PHASE2_PROBE_ACQUISITION_COMPLETE &&
	      v34_phase2_initial_probe_measure(
		      &answerer, &measurement) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      v34_phase2_initial_probe_receive_gain(
		      &answerer, &receive_gain) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      receive_gain > 0,
	      "answerer did not retain the probe received while transmitting");
	return true;
}

static void initialize_turnaround_observation(
	struct turnaround_observation *observation)
{
	*observation = (struct turnaround_observation){
		.caller_entered = SIZE_MAX,
		.answerer_entered = SIZE_MAX,
		.answerer_first_a = SIZE_MAX,
		.answerer_first_reversed_a = SIZE_MAX,
		.caller_a_reversal_event = SIZE_MAX,
		.caller_estimated_a_reversal = SIZE_MAX,
		.caller_first_reversed_b = SIZE_MAX,
		.caller_ready = SIZE_MAX,
		.answerer_b_reversal_event = SIZE_MAX,
		.answerer_estimated_b_reversal = SIZE_MAX,
		.answerer_ready = SIZE_MAX,
	};
}

static bool samples_are_close(size_t first, size_t second, size_t tolerance)
{
	return first >= second ? first - second <= tolerance :
				 second - first <= tolerance;
}

static bool check_probe_turnaround(
	enum test_codec codec, size_t delay_samples)
{
	struct v34_phase2_probe_turnaround caller_terminal;
	struct v34_phase2_probe_turnaround answerer_terminal;
	struct turnaround_observation observation;
	struct test_pair pair;

	CHECK(test_v34_phase2_pair_init(&pair, codec, delay_samples),
	      "turnaround endpoints did not initialize");
	pair.caller.continue_to_turnaround = true;
	pair.answerer.continue_to_turnaround = true;
	initialize_turnaround_observation(&observation);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;
		bool caller_turning =
			pair.caller.stage == TEST_ENDPOINT_PROBE_TURNAROUND;
		bool answerer_turning =
			pair.answerer.stage == TEST_ENDPOINT_PROBE_TURNAROUND;
		enum phase2_probe_turnaround_phase caller_phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL;
		enum phase2_probe_turnaround_phase answerer_phase =
			PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE;
		uint16_t caller_remaining = 0;

		if (caller_turning) {
			caller_phase = pair.caller.turnaround.control.phase;
			caller_remaining =
				pair.caller.turnaround.control.phase_samples_remaining;
		}
		if (answerer_turning)
			answerer_phase = pair.answerer.turnaround.control.phase;
		CHECK(test_v34_phase2_pair_sample(&pair, &event),
		      "turnaround failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);

		if (event.caller.entered_turnaround)
			observation.caller_entered = index;
		if (event.answerer.entered_turnaround)
			observation.answerer_entered = index;
		if (answerer_turning &&
		    answerer_phase ==
			    PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE) {
			if (observation.answerer_first_a == SIZE_MAX)
				observation.answerer_first_a = index;
			observation.answerer_initial_a_samples++;
		} else if (answerer_turning &&
			   answerer_phase ==
				   PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL) {
			if (observation.answerer_first_reversed_a == SIZE_MAX)
				observation.answerer_first_reversed_a = index;
			observation.answerer_reversed_a_samples++;
		}
		if (caller_turning &&
		    (caller_phase ==
			     PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL ||
		     (caller_phase ==
			      PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY &&
		      caller_remaining == 1))) {
			if (observation.caller_first_reversed_b == SIZE_MAX)
				observation.caller_first_reversed_b = index;
			observation.caller_reversed_b_samples++;
		}
		if (event.caller.turnaround.tone_event.kind ==
		    V34_PHASE2_TONE_PHASE_REVERSED) {
			uint16_t detector_delay =
				event.caller.turnaround.tone_event
					.reversal_delay_samples;

			CHECK(index >= detector_delay,
			      "caller reversal estimate preceded the test epoch");
			observation.caller_a_reversal_event = index;
			observation.caller_estimated_a_reversal =
				index - detector_delay;
		}
		if (event.answerer.turnaround.tone_event.kind ==
		    V34_PHASE2_TONE_PHASE_REVERSED) {
			uint16_t detector_delay =
				event.answerer.turnaround.tone_event
					.reversal_delay_samples;

			CHECK(index >= detector_delay,
			      "answerer reversal estimate preceded the test epoch");
			observation.answerer_b_reversal_event = index;
			observation.answerer_b_reversal_delay = detector_delay;
			observation.answerer_estimated_b_reversal =
				index - detector_delay;
		}
		if (event.caller.turnaround.kind ==
		    PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY)
			observation.caller_ready = index;
		if (event.answerer.turnaround.kind ==
		    PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY)
			observation.answerer_ready = index;
		if (pair.caller.stage == TEST_ENDPOINT_PROBE_TURNAROUND &&
		    pair.answerer.stage == TEST_ENDPOINT_PROBE_TURNAROUND &&
		    pair.caller.turnaround.control.phase ==
			    PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY &&
		    pair.answerer.turnaround.control.phase ==
			    PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY)
			break;
	}

	CHECK(observation.caller_entered != SIZE_MAX &&
	      observation.answerer_entered != SIZE_MAX &&
	      observation.answerer_first_a == observation.answerer_entered + 1u &&
	      observation.answerer_initial_a_samples ==
		      PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
	      observation.answerer_first_reversed_a ==
		      observation.answerer_first_a +
			      PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
	      observation.answerer_reversed_a_samples ==
		      PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES,
	      "answerer changed the exact Tone A intervals");
	CHECK(observation.caller_a_reversal_event != SIZE_MAX &&
	      samples_are_close(
		      observation.caller_estimated_a_reversal,
		      observation.answerer_first_reversed_a + delay_samples, 4) &&
	      observation.caller_first_reversed_b ==
		      observation.caller_estimated_a_reversal +
			      PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES,
	      "caller did not compensate the Tone A reversal deadline");
	CHECK(observation.caller_reversed_b_samples ==
		      PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
	      observation.caller_ready ==
		      observation.caller_first_reversed_b +
			      PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
			      1u,
	      "caller changed the exact reversed Tone B tail");
	CHECK(observation.answerer_b_reversal_event != SIZE_MAX &&
	      samples_are_close(
		      observation.answerer_estimated_b_reversal,
		      observation.caller_first_reversed_b + delay_samples, 4),
	      "answerer did not estimate the returning Tone B reversal");
	{
		const size_t handoff_delay =
			PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
			PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES;
		size_t expected_ready =
			observation.answerer_b_reversal_delay >=
					handoff_delay ?
				observation.answerer_b_reversal_event :
				observation.answerer_estimated_b_reversal +
					handoff_delay -
					1u;

		CHECK(observation.answerer_ready == expected_ready,
		      "answerer did not hand off the first reciprocal-probe sample");
	}
	CHECK(v34_phase2_probe_turnaround_is_valid(
		      &pair.caller.turnaround) &&
	      v34_phase2_probe_turnaround_is_valid(
		      &pair.answerer.turnaround),
	      "completed turnaround state is invalid");
	caller_terminal = pair.caller.turnaround;
	answerer_terminal = pair.answerer.turnaround;
	for (size_t index = 0; index < 8; ++index) {
		struct test_pair_event event;

		CHECK(test_v34_phase2_pair_sample(&pair, &event) &&
		      event.caller_transmit == 0 &&
		      event.answerer_transmit == 0 &&
		      event.caller.turnaround.kind ==
			      PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
		      event.answerer.turnaround.kind ==
			      PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
		      memcmp(&pair.caller.turnaround, &caller_terminal,
			     sizeof(caller_terminal)) == 0 &&
		      memcmp(&pair.answerer.turnaround, &answerer_terminal,
			     sizeof(answerer_terminal)) == 0,
		      "ready turnaround was not silent and stable");
	}
	return true;
}

static bool test_probe_turnaround(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
	} cases[] = {
		{ TEST_CODEC_LINEAR, 1 },
		{ TEST_CODEC_PCMU, 13 },
		{ TEST_CODEC_PCMA, 47 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_probe_turnaround(
			      cases[index].codec, cases[index].delay_samples),
		      "probe turnaround failed for case %zu", index);
	return true;
}

static bool completed_ranging_pair(
	struct v34_phase2_ranging *caller,
	struct v34_phase2_ranging *answerer)
{
	struct test_pair pair;

	if (!test_v34_phase2_pair_init(&pair, TEST_CODEC_LINEAR, 1))
		return false;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;

		if (!test_v34_phase2_pair_sample(&pair, &event))
			return false;
		if (pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE) {
			*caller = pair.caller.ranging;
			*answerer = pair.answerer.ranging;
			return true;
		}
	}
	return false;
}

static bool completed_initial_probe_states(
	struct v34_phase2_initial_probe *caller,
	struct v34_phase2_initial_probe *answerer)
{
	struct test_pair pair;
	bool caller_ready = false;
	bool answerer_ready = false;

	if (!test_v34_phase2_pair_init(&pair, TEST_CODEC_LINEAR, 1))
		return false;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;

		if (!test_v34_phase2_pair_sample(&pair, &event))
			return false;
		if (!answerer_ready &&
		    event.answerer.initial_probe.kind ==
			    PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
			*answerer = pair.answerer.initial_probe;
			answerer_ready = true;
		}
		if (!caller_ready &&
		    event.caller.initial_probe.kind ==
			    PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE) {
			*caller = pair.caller.initial_probe;
			caller_ready = true;
		}
		if (caller_ready && answerer_ready)
			return true;
	}
	return false;
}

static bool test_concurrent_probe_crosses_turnaround(void)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_initial_probe caller_probe;
	struct v34_phase2_initial_probe answerer_probe;
	struct v34_phase2_probe_turnaround turnaround;
	struct v34_phase2_probe_sequence peer_probe;
	struct v34_phase2_probe_acquisition_event concurrent_event;
	struct v34_phase2_probe_turnaround_event turnaround_event;
	int16_t receive_gain = 0;
	bool partial = false;
	bool complete = false;

	CHECK(completed_initial_probe_states(&caller_probe, &answerer_probe) &&
	      v34_phase2_probe_acquisition_init(
		      &answerer_probe.concurrent_receive) ==
		      V34_PHASE2_PROBE_ACQUISITION_OK,
	      "turnaround-crossing probe fixture did not initialize");
	answerer_probe.concurrent_receive_enabled = true;
	CHECK(v34_phase2_initial_probe_is_valid(&answerer_probe) &&
	      v34_phase2_probe_sequence_init(
		      &peer_probe, TEST_AMPLITUDE,
		      answerer_probe.ranging.control
			      .round_trip_delay_samples) ==
		      V34_PHASE2_PROBE_SEQUENCE_OK,
	      "turnaround-crossing probe owners were invalid");
	for (size_t sample = 0; sample < TEST_SAMPLE_LIMIT; ++sample) {
		struct v34_phase2_probe_sequence_event peer_event;
		int16_t peer_transmit;

		CHECK(v34_phase2_probe_sequence_sample(
			      &peer_probe, false, &peer_transmit,
			      &peer_event) == V34_PHASE2_PROBE_SEQUENCE_OK &&
		      v34_phase2_initial_probe_sample_concurrent(
			      &answerer_probe, peer_transmit,
			      &concurrent_event) ==
			      V34_PHASE2_INITIAL_PROBE_OK,
		      "concurrent probe prefix stopped at sample %zu", sample);
		if (answerer_probe.concurrent_receive.phase ==
			    V34_PHASE2_PROBE_ACQUISITION_RECEIVING &&
		    answerer_probe.concurrent_receive.receiver.l2_spectrum
				    .samples_observed >=
			    V34_PHASE2_PROBE_L1_SAMPLES / 4u) {
			partial = true;
			break;
		}
	}
	CHECK(partial &&
	      answerer_probe.concurrent_receive.receiver.l1_spectrum
			      .samples_observed ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      answerer_probe.concurrent_receive.phase !=
		      V34_PHASE2_PROBE_ACQUISITION_COMPLETE &&
	      v34_phase2_probe_turnaround_init(
		      &turnaround, &answerer_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK,
	      "probe did not retain its partial L2 at turnaround");
	for (size_t sample = 0; sample < TEST_SAMPLE_LIMIT; ++sample) {
		struct v34_phase2_probe_sequence_event peer_event;
		int16_t peer_transmit;
		int16_t local_transmit;

		CHECK(v34_phase2_probe_sequence_sample(
			      &peer_probe, false, &peer_transmit,
			      &peer_event) == V34_PHASE2_PROBE_SEQUENCE_OK &&
		      v34_phase2_probe_turnaround_sample(
			      &turnaround, peer_transmit, &local_transmit,
			      &turnaround_event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "concurrent probe turnaround stopped at sample %zu",
		      sample);
		if (turnaround.initial_probe.concurrent_receive.phase ==
		    V34_PHASE2_PROBE_ACQUISITION_COMPLETE) {
			complete = true;
			break;
		}
	}
	CHECK(complete &&
	      v34_phase2_initial_probe_measure(
		      &turnaround.initial_probe, &measurement) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      v34_phase2_initial_probe_receive_gain(
		      &turnaround.initial_probe, &receive_gain) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      receive_gain > 0,
	      "turnaround did not finish and retain the overlapping probe");
	return true;
}

static bool test_answerer_reversal_recovery(void)
{
	struct v34_phase2_initial_probe caller_probe;
	struct v34_phase2_initial_probe answerer_probe;
	struct v34_phase2_probe_turnaround answerer;
	struct v34_phase2_probe_turnaround_event event = { 0 };
	struct v34_phase2_tone_transmitter peer_tone;
	int16_t peer_sample;
	int16_t transmit;
	size_t samples;
	bool ready = false;

	CHECK(completed_initial_probe_states(&caller_probe, &answerer_probe) &&
	      /* The independently generated peer starts a new causal tone phase. */
	      v34_phase2_tone_detector_reset(
		      &answerer_probe.ranging.info0_exchange.transport
			       .tone_detector) == V34_PHASE2_TONE_OK &&
	      v34_phase2_probe_turnaround_init(
		      &answerer, &answerer_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK &&
	      v34_phase2_tone_transmitter_init(
		      &peer_tone, V34_CALLING_STATION, 7000) ==
		      V34_PHASE2_TONE_OK,
	      "answerer reversal-recovery fixture did not initialize");
	for (samples = 0;
	     samples < PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			       PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES +
			       PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES;
	     ++samples)
		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &peer_sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_probe_turnaround_sample(
			      &answerer, peer_sample, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "answerer reversal recovery failed at sample %zu",
		      samples);
	CHECK(event.kind == PHASE2_PROBE_TURNAROUND_EVENT_RECOVERY_STARTED &&
	      answerer.control.phase ==
		      PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE &&
	      answerer.control.phase_samples_remaining ==
		      PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
	      answerer.control.wait_samples == 0 &&
	      answerer.control.failure ==
		      PHASE2_PROBE_TURNAROUND_NO_FAILURE &&
	      transmit == 0,
	      "persistent Tone B did not restart the exact answerer turn");

	for (samples = 0; samples < TEST_SAMPLE_LIMIT; ++samples) {
		if (samples ==
		    PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			    PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES)
			CHECK(v34_phase2_tone_transmitter_reverse(&peer_tone) ==
				      V34_PHASE2_TONE_OK,
			      "peer Tone B reversal failed");
		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &peer_sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_probe_turnaround_sample(
			      &answerer, peer_sample, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "retried answerer turn failed at sample %zu", samples);
		if (event.kind ==
		    PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY) {
			ready = true;
			break;
		}
	}
	CHECK(ready &&
	      answerer.control.phase ==
		      PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY &&
	      v34_phase2_probe_turnaround_is_valid(&answerer),
	      "retried answerer turn did not accept the returning reversal");
	return true;
}

static bool test_bounded_failures(void)
{
	struct v34_phase2_ranging caller_ranging;
	struct v34_phase2_ranging answerer_ranging;
	struct v34_phase2_initial_probe caller;
	struct v34_phase2_initial_probe answerer;
	struct v34_phase2_initial_probe failed;
	struct v34_phase2_probe_sequence remote_probe;
	struct v34_phase2_initial_probe_event event;
	int16_t transmit;
	size_t samples;

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging),
	      "failure fixtures did not complete ranging");
	CHECK(v34_phase2_initial_probe_init(
		      &caller, &caller_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK,
	      "missing-probe fixture did not initialize");
	for (samples = 0; samples < 1000 &&
			  caller.control.phase !=
				  PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(v34_phase2_initial_probe_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_INITIAL_PROBE_OK,
		      "missing-probe wait failed at sample %zu", samples);
	CHECK(caller.control.phase == PHASE2_INITIAL_PROBE_FAILED &&
	      caller.control.failure ==
		      PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND &&
	      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED,
	      "missing remote probe did not fail explicitly");
	failed = caller;
	transmit = 1234;
	CHECK(v34_phase2_initial_probe_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      transmit == 0 &&
	      event.kind == PHASE2_INITIAL_PROBE_EVENT_NONE &&
	      memcmp(&caller, &failed, sizeof(caller)) == 0,
	      "failed controller did not remain silent and stable");

	CHECK(v34_phase2_initial_probe_init(
		      &answerer, &answerer_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK,
	      "transmit-limit fixture did not initialize");
	for (samples = 0; samples < 7000 &&
			  answerer.control.phase !=
				  PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(v34_phase2_initial_probe_sample(
			      &answerer, 0, &transmit, &event) ==
			      V34_PHASE2_INITIAL_PROBE_OK,
		      "probe transmit limit failed at sample %zu", samples);
	CHECK(answerer.control.phase == PHASE2_INITIAL_PROBE_FAILED &&
	      answerer.control.failure ==
		      PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
	      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED &&
	      event.transmit_event.kind ==
		      V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED,
	      "unanswered local probe did not reach its RTD-adjusted limit");

	CHECK(v34_phase2_initial_probe_init(
		      &caller, &caller_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK &&
	      v34_phase2_probe_sequence_init(
		      &remote_probe, TEST_AMPLITUDE,
		      caller_ranging.control.round_trip_delay_samples) ==
		      V34_PHASE2_PROBE_SEQUENCE_OK,
	      "tone-timeout fixture did not initialize");
	for (samples = 0; samples < 3000 &&
			  caller.control.phase ==
				  PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING;
	     ++samples) {
		struct v34_phase2_probe_sequence_event probe_event;
		int16_t received;

		CHECK(v34_phase2_probe_sequence_sample(
			      &remote_probe, false, &received, &probe_event) ==
			      V34_PHASE2_PROBE_SEQUENCE_OK &&
		      v34_phase2_initial_probe_sample(
			      &caller, received, &transmit, &event) ==
			      V34_PHASE2_INITIAL_PROBE_OK,
		      "tone-timeout probe failed at sample %zu", samples);
	}
	CHECK(caller.control.phase ==
		      PHASE2_INITIAL_PROBE_SIGNAL_B_TONE,
	      "caller did not begin Tone B after measuring the remote probe");
	for (samples = 0;
	     samples < PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
	     caller.control.phase != PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(v34_phase2_initial_probe_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_INITIAL_PROBE_OK,
		      "remote-tone timeout failed at sample %zu", samples);
	CHECK(samples + 1u ==
		      PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
	      caller.control.phase == PHASE2_INITIAL_PROBE_FAILED &&
	      caller.control.failure ==
		      PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT &&
	      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED,
	      "missing Tone A did not reach the exact bounded timeout");
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 local_capabilities =
		test_v34_phase2_capabilities(
			V34_CALLING_STATION);
	struct v34_phase2_ranging unready;
	struct v34_phase2_ranging caller_ranging;
	struct v34_phase2_ranging answerer_ranging;
	struct v34_phase2_initial_probe probe;
	struct v34_phase2_initial_probe before;
	struct v34_phase2_initial_probe invalid;
	struct v34_phase2_initial_probe_event event = {
		.kind = PHASE2_INITIAL_PROBE_EVENT_FAILED,
	};
	struct v34_phase2_initial_probe_event event_before = event;
	int16_t transmit = 1234;

	CHECK(v34_phase2_ranging_init(
		      &unready, V34_CALLING_STATION,
		      TEST_AMPLITUDE, &local_capabilities) ==
		      V34_PHASE2_RANGING_OK &&
	      completed_ranging_pair(&caller_ranging, &answerer_ranging),
	      "validation fixtures did not initialize");
	memset(&probe, 0xa5, sizeof(probe));
	before = probe;
	CHECK(v34_phase2_initial_probe_init(&probe, &unready, false) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      v34_phase2_initial_probe_init(NULL, &caller_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "invalid initialization changed caller-owned state");
	CHECK(v34_phase2_initial_probe_init(
		      &probe, &caller_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK,
	      "validation controller did not initialize");
	before = probe;
	CHECK(v34_phase2_initial_probe_sample(
		      &probe, 0, NULL, &event) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      v34_phase2_initial_probe_sample(
		      &probe, 0, &transmit, NULL) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      transmit == 1234,
	      "invalid sample output was not transactional");
	CHECK(v34_phase2_initial_probe_init(
		      &invalid, &caller_ranging, true) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "caller accepted concurrent answerer probe reception");
	invalid = probe;
	invalid.control.phase =
		PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING;
	CHECK(!v34_phase2_initial_probe_is_valid(&invalid) &&
	      v34_phase2_initial_probe_measure(&probe, NULL) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "validation contract changed");
	return true;
}

static bool test_turnaround_failures_and_validation(void)
{
	struct v34_phase2_initial_probe caller_probe;
	struct v34_phase2_initial_probe answerer_probe;
	struct v34_phase2_initial_probe unready_probe;
	struct v34_phase2_ranging caller_ranging;
	struct v34_phase2_ranging answerer_ranging;
	struct v34_phase2_probe_turnaround caller;
	struct v34_phase2_probe_turnaround answerer;
	struct v34_phase2_probe_turnaround before;
	struct v34_phase2_probe_turnaround invalid;
	struct v34_phase2_probe_turnaround_event event;
	struct v34_phase2_probe_turnaround_event event_before;
	int16_t transmit;
	size_t samples;

	CHECK(completed_initial_probe_states(&caller_probe, &answerer_probe) &&
	      v34_phase2_probe_turnaround_init(
		      &caller, &caller_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK &&
	      v34_phase2_probe_turnaround_init(
		      &answerer, &answerer_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK,
	      "turnaround failure fixtures did not initialize");
	for (samples = 0;
	     samples < PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	     caller.control.phase != PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	     ++samples)
		CHECK(v34_phase2_probe_turnaround_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "caller reversal timeout failed at sample %zu", samples);
	CHECK(samples ==
		      PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	      caller.control.phase == PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	      caller.control.failure ==
		      PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT &&
	      event.kind ==
		      PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
	      "caller did not report its exact bounded reversal timeout");
	before = caller;
	transmit = 1234;
	CHECK(v34_phase2_probe_turnaround_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK &&
	      transmit == 0 &&
	      event.kind == PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
	      memcmp(&caller, &before, sizeof(caller)) == 0,
	      "timed-out caller was not silent and stable");

	for (samples = 0;
	     samples <
		     PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			     PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES +
			     PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	     answerer.control.phase != PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	     ++samples)
		CHECK(v34_phase2_probe_turnaround_sample(
			      &answerer, 0, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "answerer reversal timeout failed at sample %zu", samples);
	CHECK(samples ==
		      PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			      PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES +
			      PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	      answerer.control.phase ==
		      PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	      answerer.control.failure ==
		      PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT &&
	      event.kind ==
		      PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
	      "answerer did not report its exact bounded reversal timeout");

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging) &&
	      v34_phase2_initial_probe_init(
		      &unready_probe, &caller_ranging, false) ==
		      V34_PHASE2_INITIAL_PROBE_OK,
	      "turnaround validation fixtures did not initialize");
	memset(&invalid, 0xa5, sizeof(invalid));
	before = invalid;
	CHECK(v34_phase2_probe_turnaround_init(
		      &invalid, &unready_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT &&
	      memcmp(&invalid, &before, sizeof(invalid)) == 0 &&
	      v34_phase2_probe_turnaround_init(NULL, &caller_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT,
	      "invalid turnaround initialization changed caller-owned state");
	CHECK(v34_phase2_probe_turnaround_init(
		      &caller, &caller_probe) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK,
	      "turnaround validation controller did not initialize");
	before = caller;
	event = (struct v34_phase2_probe_turnaround_event){
		.kind = PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		.failure =
			PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT,
	};
	event_before = event;
	transmit = 1234;
	CHECK(v34_phase2_probe_turnaround_sample(
		      &caller, 0, NULL, &event) ==
		      V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT &&
	      v34_phase2_probe_turnaround_sample(
		      &caller, 0, &transmit, NULL) ==
		      V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT &&
	      memcmp(&caller, &before, sizeof(caller)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      transmit == 1234,
	      "invalid turnaround sample output was not transactional");
	invalid = caller;
	invalid.control.phase =
		PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE;
	invalid.control.phase_samples_remaining =
		PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
	CHECK(!v34_phase2_probe_turnaround_is_valid(&invalid),
	      "turnaround validation contract changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
		} tests[] = {
			{ "bidirectional initial probe", test_bidirectional_turn },
			{ "answerer early Tone B",
			  test_answerer_accepts_early_tone_b },
			{ "answerer concurrent probe",
			  test_answerer_receives_concurrent_probe },
			{ "concurrent probe crosses turnaround",
			  test_concurrent_probe_crosses_turnaround },
			{ "bidirectional probe turnaround", test_probe_turnaround },
			{ "answerer reversal recovery",
			  test_answerer_reversal_recovery },
			{ "bounded failures", test_bounded_failures },
			{ "validation", test_validation },
			{ "turnaround failures and validation",
			  test_turnaround_failures_and_validation },
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
