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
		enum v34_phase2_probe_turnaround_phase caller_phase =
			V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL;
		enum v34_phase2_probe_turnaround_phase answerer_phase =
			V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A;
		uint16_t caller_remaining = 0;

		if (caller_turning) {
			caller_phase = pair.caller.turnaround.phase;
			caller_remaining =
				pair.caller.turnaround.phase_samples_remaining;
		}
		if (answerer_turning)
			answerer_phase = pair.answerer.turnaround.phase;
		CHECK(test_v34_phase2_pair_sample(&pair, &event),
		      "turnaround failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);

		if (event.caller.entered_turnaround)
			observation.caller_entered = index;
		if (event.answerer.entered_turnaround)
			observation.answerer_entered = index;
		if (answerer_turning &&
		    answerer_phase ==
			    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A) {
			if (observation.answerer_first_a == SIZE_MAX)
				observation.answerer_first_a = index;
			observation.answerer_initial_a_samples++;
		} else if (answerer_turning &&
			   answerer_phase ==
				   V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL) {
			if (observation.answerer_first_reversed_a == SIZE_MAX)
				observation.answerer_first_reversed_a = index;
			observation.answerer_reversed_a_samples++;
		}
		if (caller_turning &&
		    (caller_phase ==
			     V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL ||
		     (caller_phase ==
			      V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY &&
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
		    V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY)
			observation.caller_ready = index;
		if (event.answerer.turnaround.kind ==
		    V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY)
			observation.answerer_ready = index;
		if (pair.caller.stage == TEST_ENDPOINT_PROBE_TURNAROUND &&
		    pair.answerer.stage == TEST_ENDPOINT_PROBE_TURNAROUND &&
		    pair.caller.turnaround.phase ==
			    V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY &&
		    pair.answerer.turnaround.phase ==
			    V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY)
			break;
	}

	CHECK(observation.caller_entered != SIZE_MAX &&
	      observation.answerer_entered != SIZE_MAX &&
	      observation.answerer_first_a == observation.answerer_entered + 1u &&
	      observation.answerer_initial_a_samples ==
		      V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
	      observation.answerer_first_reversed_a ==
		      observation.answerer_first_a +
			      V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES &&
	      observation.answerer_reversed_a_samples ==
		      V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES,
	      "answerer changed the exact Tone A intervals");
	CHECK(observation.caller_a_reversal_event != SIZE_MAX &&
	      samples_are_close(
		      observation.caller_estimated_a_reversal,
		      observation.answerer_first_reversed_a + delay_samples, 4) &&
	      observation.caller_first_reversed_b ==
		      observation.caller_estimated_a_reversal +
			      V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES,
	      "caller did not compensate the Tone A reversal deadline");
	CHECK(observation.caller_reversed_b_samples ==
		      V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES &&
	      observation.caller_ready ==
		      observation.caller_first_reversed_b +
			      V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
			      1u,
	      "caller changed the exact reversed Tone B tail");
	CHECK(observation.answerer_b_reversal_event != SIZE_MAX &&
	      samples_are_close(
		      observation.answerer_estimated_b_reversal,
		      observation.caller_first_reversed_b + delay_samples, 4),
	      "answerer did not estimate the returning Tone B reversal");
	{
		const size_t handoff_delay =
			V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES -
			V34_PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES;
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
			      V34_PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
		      event.answerer.turnaround.kind ==
			      V34_PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
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
		      &caller, &caller_ranging) ==
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
		      &answerer, &answerer_ranging) ==
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
		      &caller, &caller_ranging) ==
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
	CHECK(v34_phase2_initial_probe_init(&probe, &unready) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      v34_phase2_initial_probe_init(NULL, &caller_ranging) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "invalid initialization changed caller-owned state");
	CHECK(v34_phase2_initial_probe_init(
		      &probe, &caller_ranging) ==
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
	invalid = probe;
	invalid.control.phase =
		PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING;
	CHECK(!v34_phase2_initial_probe_is_valid(&invalid) &&
	      v34_phase2_initial_probe_measure(&probe, NULL) ==
		      V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      strcmp(
		      v34_phase2_initial_probe_result_name(
			      V34_PHASE2_INITIAL_PROBE_OK),
		      "ok") == 0 &&
	      strcmp(
		      v34_phase2_initial_probe_result_name(
			      (enum v34_phase2_initial_probe_result)99),
		      "unknown") == 0,
	      "validation or result-name contract changed");
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
	     samples < V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	     caller.phase != V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	     ++samples)
		CHECK(v34_phase2_probe_turnaround_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "caller reversal timeout failed at sample %zu", samples);
	CHECK(samples ==
		      V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	      caller.phase == V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	      caller.failure ==
		      V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT &&
	      event.kind ==
		      V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
	      "caller did not report its exact bounded reversal timeout");
	before = caller;
	transmit = 1234;
	CHECK(v34_phase2_probe_turnaround_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      V34_PHASE2_PROBE_TURNAROUND_OK &&
	      transmit == 0 &&
	      event.kind == V34_PHASE2_PROBE_TURNAROUND_EVENT_NONE &&
	      memcmp(&caller, &before, sizeof(caller)) == 0,
	      "timed-out caller was not silent and stable");

	for (samples = 0;
	     samples <
		     V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			     V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES +
			     V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	     answerer.phase != V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT;
	     ++samples)
		CHECK(v34_phase2_probe_turnaround_sample(
			      &answerer, 0, &transmit, &event) ==
			      V34_PHASE2_PROBE_TURNAROUND_OK,
		      "answerer reversal timeout failed at sample %zu", samples);
	CHECK(samples ==
		      V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES +
			      V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES +
			      V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES &&
	      answerer.phase ==
		      V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT &&
	      answerer.failure ==
		      V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT &&
	      event.kind ==
		      V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
	      "answerer did not report its exact bounded reversal timeout");

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging) &&
	      v34_phase2_initial_probe_init(
		      &unready_probe, &caller_ranging) ==
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
		.kind = V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		.failure =
			V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT,
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
	invalid.phase =
		V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A;
	invalid.phase_samples_remaining =
		V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES;
	CHECK(!v34_phase2_probe_turnaround_is_valid(&invalid) &&
	      strcmp(
		      v34_phase2_probe_turnaround_result_name(
			      V34_PHASE2_PROBE_TURNAROUND_OK),
		      "ok") == 0 &&
	      strcmp(
		      v34_phase2_probe_turnaround_result_name(
			      (enum v34_phase2_probe_turnaround_result)99),
		      "unknown") == 0,
	      "turnaround validation or result-name contract changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
		} tests[] = {
			{ "bidirectional initial probe", test_bidirectional_turn },
			{ "bidirectional probe turnaround", test_probe_turnaround },
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
