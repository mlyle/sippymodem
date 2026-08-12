// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2.h>

#include "v34_phase2_test_pair.h"
#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SAMPLE_LIMIT = 40000,
	TEST_TONE_SAMPLES = 160,
};

struct owned_pair {
	struct test_pair channel;
	struct v34_phase2 caller;
	struct v34_phase2 answerer;
};

static bool pair_init(
	struct owned_pair *pair, enum test_codec codec,
	size_t delay_samples)
{
	struct v34_info0 caller_info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 answerer_info0 = test_v34_phase2_capabilities(
		V34_ANSWERING_STATION);
	struct owned_pair initialized;

	memset(&initialized, 0, sizeof(initialized));
	if (!test_v34_phase2_pair_init(
		    &initialized.channel, codec, delay_samples) ||
	    v34_phase2_init(
		    &initialized.caller, V34_CALLING_STATION,
		    TEST_AMPLITUDE, &caller_info0) != V34_PHASE2_OK ||
	    v34_phase2_init(
		    &initialized.answerer, V34_ANSWERING_STATION,
		    TEST_AMPLITUDE, &answerer_info0) != V34_PHASE2_OK)
		return false;
	*pair = initialized;
	return true;
}

static bool pair_sample(
	struct owned_pair *pair, struct v34_phase2_event *caller_event,
	struct v34_phase2_event *answerer_event, int16_t *caller_transmit,
	int16_t *answerer_transmit)
{
	int16_t caller_receive;
	int16_t answerer_receive;

	return test_v34_phase2_channel_receive(
		       &pair->channel, &caller_receive, &answerer_receive) &&
	       v34_phase2_sample(
		       &pair->caller, caller_receive, caller_transmit,
		       caller_event) == V34_PHASE2_OK &&
	       v34_phase2_sample(
		       &pair->answerer, answerer_receive, answerer_transmit,
		       answerer_event) == V34_PHASE2_OK &&
	       test_v34_phase2_channel_transmit(
		       &pair->channel, *caller_transmit,
		       *answerer_transmit);
}

static bool record_event(
	const struct v34_phase2_event *event, uint32_t *events)
{
	uint32_t bit;

	if (event->kind == V34_PHASE2_EVENT_NONE)
		return true;
	if (event->kind == V34_PHASE2_EVENT_FAILED ||
	    (unsigned int)event->kind >= 32u)
		return false;
	bit = UINT32_C(1) << event->kind;
	if ((*events & bit) != 0)
		return false;
	*events |= bit;
	return true;
}

static bool measurement_is_finite(
	const struct v34_phase2_probe_receiver_measurement *measurement)
{
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index) {
		if (!isfinite(measurement->l1.harmonics[index].mean_square) ||
		    !isfinite(measurement->l2.harmonics[index].mean_square))
			return false;
	}
	return measurement->l1.sample_count ==
		       V34_PHASE2_PROBE_L1_SAMPLES &&
	       measurement->l2.sample_count ==
		       V34_PHASE2_PROBE_L1_SAMPLES;
}

static bool check_timeline(
	enum test_codec codec, size_t delay_samples)
{
	const uint32_t common_events =
		(UINT32_C(1) << V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_PROBE_TURNAROUND_STARTED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_READY_FOR_INFO1);
	struct v34_phase2_probe_receiver_measurement caller_measurement;
	struct v34_phase2_probe_receiver_measurement answerer_measurement;
	struct owned_pair pair;
	uint64_t answerer_tone_magnitude = 0;
	uint32_t caller_events = 0;
	uint32_t answerer_events = 0;

	CHECK(pair_init(&pair, codec, delay_samples),
	      "Phase 2 pair did not initialize for codec %d delay %zu",
	      codec, delay_samples);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      record_event(&caller_event, &caller_events) &&
		      record_event(&answerer_event, &answerer_events),
		      "Phase 2 pair failed at sample %zu", index);
		if (v34_phase2_is_ready_for_info1(&pair.caller) &&
		    v34_phase2_is_ready_for_info1(&pair.answerer))
			break;
	}
	CHECK(v34_phase2_is_ready_for_info1(&pair.caller) &&
	      v34_phase2_is_ready_for_info1(&pair.answerer) &&
	      !v34_phase2_has_failed(&pair.caller) &&
	      !v34_phase2_has_failed(&pair.answerer) &&
	      (caller_events & common_events) == common_events &&
	      (answerer_events & common_events) == common_events &&
	      (caller_events &
	       (UINT32_C(1) <<
		V34_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED)) != 0 &&
	      ((caller_events | answerer_events) &
	       (UINT32_C(1) << V34_PHASE2_EVENT_ROUND_TRIP_DELAY)) != 0,
	      "Phase 2 transition events were incomplete");
	CHECK(v34_phase2_measure_remote_probe(
		      &pair.caller, &caller_measurement) == V34_PHASE2_OK &&
	      v34_phase2_measure_remote_probe(
		      &pair.answerer, &answerer_measurement) == V34_PHASE2_OK &&
	      measurement_is_finite(&caller_measurement) &&
	      measurement_is_finite(&answerer_measurement),
	      "Phase 2 did not retain both probe measurements");
	for (size_t index = 0; index < TEST_TONE_SAMPLES; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      caller_transmit == 0 &&
		      caller_event.kind == V34_PHASE2_EVENT_NONE &&
		      answerer_event.kind == V34_PHASE2_EVENT_NONE,
		      "ready Phase 2 endpoints changed terminal behavior");
		answerer_tone_magnitude += answerer_transmit < 0 ?
			(uint64_t)-(int64_t)answerer_transmit :
			(uint64_t)answerer_transmit;
	}
	CHECK(answerer_tone_magnitude != 0,
	      "answerer did not retain Tone A at the INFO1 boundary");
	return true;
}

static bool test_generated_timelines(void)
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
		CHECK(check_timeline(
			      cases[index].codec,
			      cases[index].delay_samples),
		      "generated Phase 2 case %zu failed", index);
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 invalid_info0 = info0;
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_probe_receiver_measurement saved_measurement;
	struct v34_phase2 phase2;
	struct v34_phase2 saved;
	struct v34_phase2_event event;
	struct v34_phase2_event saved_event;
	struct owned_pair pair;
	int16_t transmit = 1234;
	bool ranging_active = false;
	bool failed = false;

	memset(&phase2, 0xa5, sizeof(phase2));
	saved = phase2;
	invalid_info0.maximum_symbol_rate_difference_steps = 8;
	CHECK(v34_phase2_init(
		      &phase2, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &invalid_info0) == V34_PHASE2_INVALID_ARGUMENT &&
	      memcmp(&phase2, &saved, sizeof(phase2)) == 0,
	      "invalid Phase 2 initialization changed its destination");
	CHECK(v34_phase2_init(
		      &phase2, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &info0) == V34_PHASE2_OK,
	      "validation endpoint did not initialize");
	saved = phase2;
	memset(&event, 0x5a, sizeof(event));
	saved_event = event;
	memset(&measurement, 0x6b, sizeof(measurement));
	saved_measurement = measurement;
	CHECK(v34_phase2_sample(&phase2, INT16_MIN, NULL, &event) ==
		      V34_PHASE2_INVALID_ARGUMENT &&
	      v34_phase2_measure_remote_probe(&phase2, &measurement) ==
		      V34_PHASE2_INVALID_STATE &&
	      memcmp(&phase2, &saved, sizeof(phase2)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0 &&
	      memcmp(&measurement, &saved_measurement,
		     sizeof(measurement)) == 0 &&
	      transmit == 1234 &&
	      strcmp(v34_phase2_result_name(V34_PHASE2_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(v34_phase2_failure_name(
			     V34_PHASE2_FAILURE_INTERNAL),
		     "internal failure") == 0,
	      "Phase 2 validation was not transactional");
	CHECK(pair_init(&pair, TEST_CODEC_LINEAR, 1),
	      "failure pair did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "failure pair stopped at %zu", index);
		if (pair.answerer.stage == V34_PHASE2_RANGING &&
		    pair.answerer.state.ranging.phase ==
			    V34_PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			ranging_active = true;
			break;
		}
	}
	CHECK(ranging_active, "failure pair did not enter active ranging");
	phase2 = pair.answerer;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		CHECK(v34_phase2_sample(&phase2, 0, &transmit, &event) ==
			      V34_PHASE2_OK,
		      "isolated ranging failed at %zu", index);
		if (event.kind == V34_PHASE2_EVENT_FAILED) {
			failed = true;
			break;
		}
	}
	CHECK(failed && v34_phase2_has_failed(&phase2) &&
	      event.failure == V34_PHASE2_FAILURE_RANGING_TIMEOUT,
	      "Phase 2 did not retain a child ranging failure");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated probe timelines", test_generated_timelines },
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
