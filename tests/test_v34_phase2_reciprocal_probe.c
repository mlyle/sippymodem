// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_reciprocal_probe.h>

#include "v34_phase2_test_pair.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SAMPLE_LIMIT = 16000,
	TEST_STABLE_TONE_SAMPLES = 160,
};

struct probe_observation {
	size_t caller_entered;
	size_t answerer_entered;
	size_t caller_first_probe_sample;
	size_t answerer_first_probe_sample;
	size_t caller_l2_started;
	size_t answerer_b_reversal;
	size_t answerer_acquired;
	size_t answerer_l2_started;
	size_t answerer_measurement_ready;
	size_t answerer_probe_measured;
	size_t caller_probe_acknowledged;
	uint32_t answerer_info1c_elapsed_at_entry;
	uint32_t answerer_info1c_elapsed_at_measurement;
	uint32_t caller_l2_samples;
	uint32_t answerer_l2_samples;
	uint64_t answerer_tone_magnitude;
};

static int64_t sample_magnitude(int16_t sample)
{
	return sample < 0 ? -(int64_t)sample : sample;
}

static void initialize_observation(struct probe_observation *observation)
{
	*observation = (struct probe_observation){
		.caller_entered = SIZE_MAX,
		.answerer_entered = SIZE_MAX,
		.caller_first_probe_sample = SIZE_MAX,
		.answerer_first_probe_sample = SIZE_MAX,
		.caller_l2_started = SIZE_MAX,
		.answerer_b_reversal = SIZE_MAX,
		.answerer_acquired = SIZE_MAX,
		.answerer_l2_started = SIZE_MAX,
		.answerer_measurement_ready = SIZE_MAX,
		.answerer_probe_measured = SIZE_MAX,
		.caller_probe_acknowledged = SIZE_MAX,
	};
}

static void observe_probe(
	const struct test_pair *pair, const struct test_pair_event *event,
	bool caller_was_probing, bool answerer_was_probing, size_t sample_index,
	struct probe_observation *observation)
{
	if (event->answerer.turnaround.tone_event.kind ==
	    V34_PHASE2_TONE_PHASE_REVERSED) {
		uint16_t detector_delay = event->answerer.turnaround.tone_event
					      .reversal_delay_samples;

		if (sample_index >= detector_delay)
			observation->answerer_b_reversal =
				sample_index - detector_delay;
	}
	if (event->caller.entered_reciprocal_probe)
		observation->caller_entered = sample_index;
	if (event->answerer.entered_reciprocal_probe) {
		observation->answerer_entered = sample_index;
		observation->answerer_info1c_elapsed_at_entry =
			pair->answerer.reciprocal_probe
				.answerer_info1c_elapsed_samples;
	}
	if (caller_was_probing &&
	    observation->caller_first_probe_sample == SIZE_MAX)
		observation->caller_first_probe_sample = sample_index;
	if (answerer_was_probing &&
	    observation->answerer_first_probe_sample == SIZE_MAX)
		observation->answerer_first_probe_sample = sample_index;
	if (event->caller.reciprocal_probe.transmit_event.kind ==
	    V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED)
		observation->caller_l2_started = sample_index;
	switch (event->answerer.reciprocal_probe.receive_event.kind) {
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED:
		observation->answerer_acquired = sample_index;
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED:
		observation->answerer_l2_started = sample_index;
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY:
		observation->answerer_measurement_ready = sample_index;
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED:
		break;
	}
	if (event->answerer.reciprocal_probe.kind ==
	    V34_PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED) {
		observation->answerer_probe_measured = sample_index;
		observation->answerer_l2_samples =
			event->answerer.reciprocal_probe.receive_event.l2_samples;
		observation->answerer_info1c_elapsed_at_measurement =
			pair->answerer.reciprocal_probe
				.answerer_info1c_elapsed_samples;
	}
	if (event->caller.reciprocal_probe.kind ==
	    V34_PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED) {
		observation->caller_probe_acknowledged = sample_index;
		observation->caller_l2_samples =
			event->caller.reciprocal_probe.transmit_event.l2_samples;
	}
	if (pair->answerer.stage == TEST_ENDPOINT_RECIPROCAL_PROBE &&
	    pair->answerer.reciprocal_probe.phase ==
		    V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A)
		observation->answerer_tone_magnitude +=
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

static bool check_reciprocal_probe(
	enum test_codec codec, size_t delay_samples)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_reciprocal_probe caller_terminal;
	struct probe_observation observation;
	struct test_pair pair;
	size_t stable_samples = 0;

	CHECK(test_v34_phase2_pair_init(&pair, codec, delay_samples),
	      "reciprocal-probe endpoints did not initialize");
	pair.caller.continue_to_turnaround = true;
	pair.answerer.continue_to_turnaround = true;
	pair.caller.continue_to_reciprocal_probe = true;
	pair.answerer.continue_to_reciprocal_probe = true;
	initialize_observation(&observation);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;
		bool caller_was_probing =
			pair.caller.stage == TEST_ENDPOINT_RECIPROCAL_PROBE;
		bool answerer_was_probing =
			pair.answerer.stage == TEST_ENDPOINT_RECIPROCAL_PROBE;

		CHECK(test_v34_phase2_pair_sample(&pair, &event),
		      "reciprocal probe failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);
		observe_probe(
			&pair, &event, caller_was_probing, answerer_was_probing,
			index, &observation);
		if (pair.caller.stage == TEST_ENDPOINT_RECIPROCAL_PROBE &&
		    pair.answerer.stage == TEST_ENDPOINT_RECIPROCAL_PROBE &&
		    pair.caller.reciprocal_probe.phase ==
			    V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE &&
		    pair.answerer.reciprocal_probe.phase ==
			    V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A) {
			if (++stable_samples == TEST_STABLE_TONE_SAMPLES)
				break;
		} else {
			stable_samples = 0;
		}
	}

	CHECK(observation.caller_entered != SIZE_MAX &&
	      observation.answerer_entered != SIZE_MAX &&
	      observation.answerer_b_reversal != SIZE_MAX &&
	      observation.caller_first_probe_sample ==
		      observation.caller_entered + 1u &&
	      observation.answerer_first_probe_sample ==
		      observation.answerer_entered + 1u,
	      "turnaround handoff changed: caller=%zu/%zu answerer=%zu/%zu",
	      observation.caller_entered,
	      observation.caller_first_probe_sample, observation.answerer_entered,
	      observation.answerer_first_probe_sample);
	CHECK(observation.answerer_info1c_elapsed_at_entry ==
		      observation.answerer_entered -
			      observation.answerer_b_reversal &&
	      observation.answerer_info1c_elapsed_at_measurement ==
		      observation.answerer_probe_measured -
			      observation.answerer_b_reversal,
	      "answerer INFO1c epoch changed across probing: edge=%zu "
	      "entry=%zu/%u measured=%zu/%u",
	      observation.answerer_b_reversal, observation.answerer_entered,
	      observation.answerer_info1c_elapsed_at_entry,
	      observation.answerer_probe_measured,
	      observation.answerer_info1c_elapsed_at_measurement);
	CHECK(stable_samples == TEST_STABLE_TONE_SAMPLES,
	      "reciprocal probe did not settle: caller=%d/%d answerer=%d/%d "
	      "edges=%zu/%zu sync=%u/%.6f",
	      pair.caller.reciprocal_probe.phase,
	      pair.caller.reciprocal_probe.failure,
	      pair.answerer.reciprocal_probe.phase,
	      pair.answerer.reciprocal_probe.failure,
	      observation.caller_first_probe_sample + delay_samples,
	      observation.answerer_first_probe_sample,
	      pair.answerer.reciprocal_probe.path.receive.synchronizer
		      .leading_samples,
	      pair.answerer.reciprocal_probe.path.receive.synchronizer
		      .probe_energy_fraction);
	CHECK(observation.caller_l2_started ==
		      observation.caller_first_probe_sample +
			      V34_PHASE2_PROBE_L1_SAMPLES - 1u,
	      "caller changed the exact 160 ms L1 boundary");
	CHECK(observation.answerer_acquired != SIZE_MAX &&
	      observation.answerer_l2_started != SIZE_MAX &&
	      observation.answerer_measurement_ready != SIZE_MAX &&
	      observation.answerer_probe_measured ==
		      observation.answerer_measurement_ready + 1u &&
	      observation.answerer_probe_measured >
		      observation.caller_l2_started + delay_samples,
	      "answerer did not measure the reciprocal probe causally");
	CHECK(observation.caller_probe_acknowledged >
		      observation.answerer_probe_measured + delay_samples &&
	      observation.caller_l2_samples >=
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      observation.caller_l2_samples <
		      (uint32_t)
			      V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		      pair.caller.reciprocal_probe.turnaround.initial_probe.ranging
			      .round_trip_delay_samples,
	      "final Tone A did not end caller L2 within its RTD-adjusted bound");
	CHECK(v34_phase2_reciprocal_probe_measure(
		      &pair.answerer.reciprocal_probe, &measurement) ==
		      V34_PHASE2_RECIPROCAL_PROBE_OK &&
	      measurement.l1.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples == observation.answerer_l2_samples &&
	      measurement.l2_samples == V34_PHASE2_PROBE_L1_SAMPLES,
	      "answerer did not retain exact reciprocal-probe measurements");
	{
		double l1_power = measurement_power(&measurement.l1);
		double l2_power = measurement_power(&measurement.l2);

		CHECK(l2_power > 0.0 && l1_power / l2_power > 3.9 &&
		      l1_power / l2_power < 4.1,
		      "reciprocal-probe power ratio changed for codec %d: %.6f",
		      codec, l1_power / l2_power);
	}
	CHECK(observation.answerer_tone_magnitude != 0 &&
	      v34_phase2_reciprocal_probe_is_valid(
		      &pair.caller.reciprocal_probe) &&
	      v34_phase2_reciprocal_probe_is_valid(
		      &pair.answerer.reciprocal_probe),
	      "completed reciprocal-probe endpoints are not stable");
	caller_terminal = pair.caller.reciprocal_probe;
	{
		struct test_pair_event event;

		CHECK(test_v34_phase2_pair_sample(&pair, &event) &&
		      event.caller_transmit == 0 &&
		      event.caller.reciprocal_probe.kind ==
			      V34_PHASE2_RECIPROCAL_PROBE_EVENT_NONE &&
		      event.answerer.reciprocal_probe.kind ==
			      V34_PHASE2_RECIPROCAL_PROBE_EVENT_NONE &&
		      memcmp(&pair.caller.reciprocal_probe, &caller_terminal,
			     sizeof(caller_terminal)) == 0,
		      "reciprocal-probe terminal behavior changed");
	}
	return true;
}

static bool test_bidirectional_probe(void)
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
		CHECK(check_reciprocal_probe(
			      cases[index].codec, cases[index].delay_samples),
		      "reciprocal probe failed for case %zu", index);
	return true;
}

static bool completed_turnaround_states(
	struct v34_phase2_probe_turnaround *caller,
	struct v34_phase2_probe_turnaround *answerer)
{
	struct test_pair pair;
	bool caller_ready = false;
	bool answerer_ready = false;

	if (!test_v34_phase2_pair_init(&pair, TEST_CODEC_LINEAR, 1))
		return false;
	pair.caller.continue_to_turnaround = true;
	pair.answerer.continue_to_turnaround = true;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;

		if (!test_v34_phase2_pair_sample(&pair, &event))
			return false;
		if (!caller_ready &&
		    event.caller.turnaround.kind ==
			    V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY) {
			*caller = pair.caller.turnaround;
			caller_ready = true;
		}
		if (!answerer_ready &&
		    event.answerer.turnaround.kind ==
			    V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY) {
			*answerer = pair.answerer.turnaround;
			answerer_ready = true;
		}
		if (caller_ready && answerer_ready)
			return true;
	}
	return false;
}

static bool test_bounded_failures(void)
{
	struct v34_phase2_probe_turnaround caller_turnaround;
	struct v34_phase2_probe_turnaround answerer_turnaround;
	struct v34_phase2_reciprocal_probe caller;
	struct v34_phase2_reciprocal_probe answerer;
	struct v34_phase2_reciprocal_probe terminal;
	struct v34_phase2_reciprocal_probe_event event;
	int16_t transmit;
	size_t samples;
	uint32_t caller_l2_limit;

	CHECK(completed_turnaround_states(
		      &caller_turnaround, &answerer_turnaround) &&
	      v34_phase2_reciprocal_probe_init(
		      &caller, &caller_turnaround) ==
		      V34_PHASE2_RECIPROCAL_PROBE_OK &&
	      v34_phase2_reciprocal_probe_init(
		      &answerer, &answerer_turnaround) ==
		      V34_PHASE2_RECIPROCAL_PROBE_OK,
	      "reciprocal-probe failure fixtures did not initialize");
	caller_l2_limit =
		(uint32_t)V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		caller_turnaround.initial_probe.ranging.round_trip_delay_samples;
	for (samples = 0;
	     samples < V34_PHASE2_PROBE_L1_SAMPLES + caller_l2_limit &&
	     caller.phase != V34_PHASE2_RECIPROCAL_PROBE_FAILED;
	     ++samples)
		CHECK(v34_phase2_reciprocal_probe_sample(
			      &caller, 0, &transmit, &event) ==
			      V34_PHASE2_RECIPROCAL_PROBE_OK,
		      "caller transmit limit failed at sample %zu", samples);
	CHECK(samples ==
		      V34_PHASE2_PROBE_L1_SAMPLES + caller_l2_limit &&
	      caller.phase == V34_PHASE2_RECIPROCAL_PROBE_FAILED &&
	      caller.failure ==
		      V34_PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
	      event.kind ==
		      V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED &&
	      event.transmit_event.kind ==
		      V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED &&
	      event.transmit_event.l2_samples == caller_l2_limit,
	      "missing Tone A did not reach the exact local transmit limit");
	terminal = caller;
	transmit = 1234;
	CHECK(v34_phase2_reciprocal_probe_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      V34_PHASE2_RECIPROCAL_PROBE_OK &&
	      transmit == 0 &&
	      event.kind == V34_PHASE2_RECIPROCAL_PROBE_EVENT_NONE &&
	      memcmp(&caller, &terminal, sizeof(caller)) == 0,
	      "failed caller was not silent and stable");

	for (samples = 0;
	     samples < V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
	     answerer.phase != V34_PHASE2_RECIPROCAL_PROBE_FAILED;
	     ++samples)
		CHECK(v34_phase2_reciprocal_probe_sample(
			      &answerer, 0, &transmit, &event) ==
			      V34_PHASE2_RECIPROCAL_PROBE_OK,
		      "missing reciprocal probe failed at sample %zu", samples);
	CHECK(samples == V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES &&
	      answerer.phase == V34_PHASE2_RECIPROCAL_PROBE_FAILED &&
	      answerer.failure ==
		      V34_PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND &&
	      event.kind ==
		      V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED &&
	      event.receive_event.kind ==
		      V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND,
	      "missing reciprocal probe did not fail at the acquisition bound");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase2_probe_turnaround caller_turnaround;
	struct v34_phase2_probe_turnaround answerer_turnaround;
	struct v34_phase2_probe_turnaround unready;
	struct v34_phase2_reciprocal_probe probe;
	struct v34_phase2_reciprocal_probe before;
	struct v34_phase2_reciprocal_probe invalid;
	struct v34_phase2_reciprocal_probe_event event = {
		.kind = V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
	};
	struct v34_phase2_reciprocal_probe_event event_before = event;
	int16_t transmit = 1234;

	CHECK(completed_turnaround_states(
		      &caller_turnaround, &answerer_turnaround),
	      "reciprocal-probe validation fixtures did not initialize");
	unready = caller_turnaround;
	unready.phase =
		V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL;
	CHECK(v34_phase2_probe_turnaround_is_valid(&unready),
	      "unready turnaround fixture is invalid");
	memset(&probe, 0xa5, sizeof(probe));
	before = probe;
	CHECK(v34_phase2_reciprocal_probe_init(&probe, &unready) ==
		      V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      v34_phase2_reciprocal_probe_init(
		      NULL, &caller_turnaround) ==
		      V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
	      "invalid reciprocal-probe initialization changed caller state");
	CHECK(v34_phase2_reciprocal_probe_init(
		      &probe, &caller_turnaround) ==
		      V34_PHASE2_RECIPROCAL_PROBE_OK,
	      "reciprocal-probe validation controller did not initialize");
	before = probe;
	CHECK(v34_phase2_reciprocal_probe_sample(
		      &probe, 0, NULL, &event) ==
		      V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
	      v34_phase2_reciprocal_probe_sample(
		      &probe, 0, &transmit, NULL) ==
		      V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      transmit == 1234,
	      "invalid reciprocal-probe sample output was not transactional");
	invalid = probe;
	invalid.phase =
		V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_RECEIVING;
	CHECK(!v34_phase2_reciprocal_probe_is_valid(&invalid) &&
	      v34_phase2_reciprocal_probe_measure(&probe, NULL) ==
		      V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
	      strcmp(
		      v34_phase2_reciprocal_probe_result_name(
			      V34_PHASE2_RECIPROCAL_PROBE_OK),
		      "ok") == 0 &&
	      strcmp(
		      v34_phase2_reciprocal_probe_result_name(
			      (enum v34_phase2_reciprocal_probe_result)99),
		      "unknown") == 0,
	      "reciprocal-probe validation or result-name contract changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional reciprocal probe", test_bidirectional_probe },
		{ "bounded failures", test_bounded_failures },
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
