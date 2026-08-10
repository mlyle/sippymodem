// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_probe_sequence.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_NOMINAL_AMPLITUDE = 12000,
	TEST_EARLY_L2_SAMPLES = 317,
};

static bool advance_to_l2(
	struct sippymodem_v34_phase2_probe_sequence *sequence)
{
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES; ++index) {
		struct sippymodem_v34_phase2_probe_sequence_event event;
		int16_t sample;

		CHECK(sippymodem_v34_phase2_probe_sequence_sample(
			      sequence, false, &sample, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
		      "L1 failed at sample %zu", index);
		if (index + 1u == SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES) {
			CHECK(event.kind ==
				      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED &&
				      event.l2_samples == 0,
			      "L1 did not report its exact L2 boundary");
		} else {
			CHECK(event.kind ==
				      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE,
			      "L1 reported an early event at sample %zu", index);
		}
	}
	CHECK(sequence->phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 &&
		      sequence->samples_emitted ==
			      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		      sequence->waveform.period_index == 0,
	      "L1 did not leave a phase-aligned L2 state");
	return true;
}

static bool test_phase_continuity_and_early_finish(void)
{
	struct sippymodem_v34_phase2_probe_sequence sequence;
	struct sippymodem_v34_phase2_probe_sequence before;
	struct sippymodem_v34_phase2_probe_transmitter waveform;
	struct sippymodem_v34_phase2_probe_sequence_event event;
	int16_t generated;
	int16_t expected;

	CHECK(sippymodem_v34_phase2_probe_sequence_init(
		      &sequence, TEST_NOMINAL_AMPLITUDE, 37) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      sippymodem_v34_phase2_probe_transmitter_init(
			      &waveform, TEST_NOMINAL_AMPLITUDE) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_OK,
	      "sequence fixture did not initialize");
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES; ++index) {
		CHECK(sippymodem_v34_phase2_probe_transmitter_sample(
			      &waveform, SIPPYMODEM_V34_PHASE2_PROBE_L1,
			      &expected) == SIPPYMODEM_V34_PHASE2_PROBE_OK &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &generated, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      generated == expected,
		      "sequence changed L1 sample %zu", index);
		CHECK(event.kind ==
			      (index + 1u ==
				       SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES ?
				       SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED :
				       SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE),
		      "sequence reported the wrong L1 event at sample %zu", index);
	}
	CHECK(sequence.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 &&
		      sequence.samples_emitted ==
			      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		      memcmp(&sequence.waveform, &waveform, sizeof(waveform)) == 0,
	      "sequence lost waveform phase at the L1/L2 boundary");
	for (size_t index = 0; index < TEST_EARLY_L2_SAMPLES; ++index) {
		CHECK(sippymodem_v34_phase2_probe_transmitter_sample(
			      &waveform, SIPPYMODEM_V34_PHASE2_PROBE_L2,
			      &expected) == SIPPYMODEM_V34_PHASE2_PROBE_OK &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &generated, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      generated == expected &&
		      event.kind ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE,
		      "sequence changed L2 sample %zu", index);
	}
	CHECK(sequence.samples_emitted ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES +
			      TEST_EARLY_L2_SAMPLES,
	      "sequence did not count emitted L2 samples");
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, true, &generated, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      generated == 0 &&
		      event.kind ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_COMPLETED &&
		      event.l2_samples == TEST_EARLY_L2_SAMPLES &&
		      sequence.phase ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE &&
		      sequence.samples_emitted ==
			      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES +
				      TEST_EARLY_L2_SAMPLES &&
		      memcmp(&sequence.waveform, &waveform, sizeof(waveform)) == 0,
	      "normal L2 completion changed its count or waveform");
	before = sequence;
	generated = 1234;
	event.kind = SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED;
	event.l2_samples = 99;
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, true, &generated, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      generated == 0 &&
		      event.kind ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE &&
		      event.l2_samples == 0 &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0,
	      "complete sequence did not remain silent and stable");
	return true;
}

static bool check_l2_limit(uint16_t round_trip_delay_samples)
{
	struct sippymodem_v34_phase2_probe_sequence sequence;
	struct sippymodem_v34_phase2_probe_sequence before;
	uint32_t expected_limit =
		(uint32_t)SIPPYMODEM_V34_PHASE2_PROBE_L2_BASE_LIMIT_SAMPLES +
		round_trip_delay_samples;
	struct sippymodem_v34_phase2_probe_sequence_event event;
	int16_t sample;

	CHECK(sippymodem_v34_phase2_probe_sequence_init(
		      &sequence, TEST_NOMINAL_AMPLITUDE,
		      round_trip_delay_samples) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      advance_to_l2(&sequence),
	      "limit fixture failed for RTD %u", round_trip_delay_samples);
	for (uint32_t index = 0; index < expected_limit; ++index) {
		CHECK(sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &sample, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
		      "L2 limit fixture failed for RTD %u at sample %u",
		      round_trip_delay_samples, index);
		if (index + 1u == expected_limit) {
			CHECK(event.kind ==
				      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED &&
				      event.l2_samples == expected_limit &&
				      sequence.phase ==
					      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED,
			      "L2 did not report its RTD-adjusted limit");
		} else {
			CHECK(event.kind ==
				      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE &&
				      sequence.phase ==
					      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L2,
			      "L2 stopped before its RTD-adjusted limit");
		}
	}
	CHECK(sequence.samples_emitted ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES + expected_limit &&
		      sippymodem_v34_phase2_probe_sequence_is_valid(&sequence),
	      "limit terminal state is not self-consistent");
	before = sequence;
	sample = 1234;
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, false, &sample, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      sample == 0 &&
		      event.kind ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_NONE &&
		      event.l2_samples == 0 &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0,
	      "limit-reached sequence did not remain silent and stable");
	return true;
}

static bool test_l2_limits(void)
{
	static const uint16_t round_trip_delays[] = { 0, 47, UINT16_MAX };

	for (size_t index = 0; index < ARRAY_LENGTH(round_trip_delays); ++index)
		CHECK(check_l2_limit(round_trip_delays[index]),
		      "L2 limit failed for case %zu", index);
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase2_probe_sequence sequence;
	struct sippymodem_v34_phase2_probe_sequence before;
	struct sippymodem_v34_phase2_probe_sequence invalid;
	struct sippymodem_v34_phase2_probe_sequence_event event = {
		.kind = SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED,
		.l2_samples = 99,
	};
	struct sippymodem_v34_phase2_probe_sequence_event event_before = event;
	int16_t sample = 1234;

	memset(&sequence, 0xa5, sizeof(sequence));
	before = sequence;
	CHECK(sippymodem_v34_phase2_probe_sequence_init(
		      &sequence, INT16_MAX, 0) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      sippymodem_v34_phase2_probe_sequence_init(
			      NULL, TEST_NOMINAL_AMPLITUDE, 0) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT,
	      "invalid sequence initialization changed caller-owned state");
	CHECK(sippymodem_v34_phase2_probe_sequence_init(
		      &sequence, TEST_NOMINAL_AMPLITUDE, 0) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
	      "validation fixture did not initialize");
	before = sequence;
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, true, &sample, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      sample == 1234 &&
		      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "premature L2 finish was not rejected transactionally");
	sequence.phase = SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_PHASE_L2;
	before = sequence;
	CHECK(!sippymodem_v34_phase2_probe_sequence_is_valid(&sequence) &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &sample, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      sample == 1234 &&
		      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "L2 phase without completed L1 was not rejected");
	CHECK(sippymodem_v34_phase2_probe_sequence_init(
		      &sequence, TEST_NOMINAL_AMPLITUDE, 0) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
	      "validation fixture did not reset");
	CHECK(advance_to_l2(&sequence),
	      "finish validation did not reach L2");
	before = sequence;
	sample = 1234;
	event = event_before;
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, true, &sample, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      sample == 1234 &&
		      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "zero-length L2 was not rejected");

	sequence.samples_emitted++;
	invalid = sequence;
	CHECK(!sippymodem_v34_phase2_probe_sequence_is_valid(&sequence) &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &sample, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &invalid, sizeof(sequence)) == 0,
	      "inconsistent waveform position was not rejected");
	sequence = before;
	sequence.phase =
		(enum sippymodem_v34_phase2_probe_sequence_phase)99;
	invalid = sequence;
	CHECK(!sippymodem_v34_phase2_probe_sequence_is_valid(&sequence) &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &sample, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &invalid, sizeof(sequence)) == 0,
	      "unknown sequence phase was not rejected");
	sequence = before;
	sample = 1234;
	event = event_before;
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      &sequence, false, NULL, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      memcmp(&event, &event_before, sizeof(event)) == 0 &&
		      sippymodem_v34_phase2_probe_sequence_sample(
			      &sequence, false, &sample, NULL) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      memcmp(&sequence, &before, sizeof(sequence)) == 0 &&
		      sample == 1234,
	      "null output was not rejected transactionally");
	CHECK(sippymodem_v34_phase2_probe_sequence_sample(
		      NULL, false, &sample, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT &&
		      !sippymodem_v34_phase2_probe_sequence_is_valid(NULL),
	      "null sequence was accepted");
	CHECK(strcmp(
		      sippymodem_v34_phase2_probe_sequence_result_name(
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK),
		      "ok") == 0 &&
		      strcmp(
			      sippymodem_v34_phase2_probe_sequence_result_name(
				      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_INVALID_ARGUMENT),
			      "invalid argument") == 0 &&
		      strcmp(
			      sippymodem_v34_phase2_probe_sequence_result_name(
				      (enum sippymodem_v34_phase2_probe_sequence_result)99),
			      "unknown") == 0,
	      "sequence result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "phase continuity and early finish",
		  test_phase_continuity_and_early_finish },
		{ "L2 limits", test_l2_limits },
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
