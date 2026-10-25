// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <single_tone.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_ANSWER_HZ = 2100,
	TEST_CARRIER_HZ = 1800,
	TEST_WRONG_HZ = 2225,
	TEST_MIXED_LOW_HZ = 600,
	TEST_MIXED_HIGH_HZ = 3000,
	TEST_MIXED_LOW_AMPLITUDE = 6000,
	TEST_MIXED_HIGH_AMPLITUDE = 9000,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
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

static bool test_generation_and_reversal(void)
{
	struct single_tone_transmitter baseline;
	struct single_tone_transmitter reversed;

	CHECK(single_tone_transmitter_init(
		      &baseline, TEST_CARRIER_HZ, TEST_AMPLITUDE) ==
		      SINGLE_TONE_OK,
	      "single-tone transmitter did not initialize");
	for (size_t index = 0; index < 37; ++index) {
		int16_t discarded;

		CHECK(single_tone_transmitter_sample(
			      &baseline, &discarded) ==
			      SINGLE_TONE_OK,
		      "single-tone prelude failed");
	}
	reversed = baseline;
	CHECK(single_tone_transmitter_reverse(&reversed) ==
		      SINGLE_TONE_OK,
	      "single-tone transmitter rejected a phase reversal");
	for (size_t index = 0;
	     index < SINGLE_TONE_WINDOW_SAMPLES; ++index) {
		int16_t positive;
		int16_t negative;

		CHECK(single_tone_transmitter_sample(
			      &baseline, &positive) == SINGLE_TONE_OK &&
		      single_tone_transmitter_sample(
			      &reversed, &negative) ==
			      SINGLE_TONE_OK &&
		      positive + negative >= -1 &&
		      positive + negative <= 1,
		      "phase reversal did not invert sample %zu", index);
	}
	CHECK(baseline.phase ==
		      reversed.phase + UINT32_C(0x80000000),
	      "phase reversal disturbed the carrier clock");
	return true;
}

static bool feed(
	struct single_tone_detector *detector, int16_t sample,
	enum test_codec codec, size_t sample_index, size_t *started,
	size_t *reversed, size_t *ended, size_t *reversal_sample,
	struct single_tone_event *last_reversal)
{
	struct single_tone_event event;

	CHECK(single_tone_detector_sample(
		      detector, codec_round_trip(sample, codec), &event) ==
		      SINGLE_TONE_OK,
	      "single-tone detector failed at sample %zu", sample_index);
	switch (event.kind) {
	case SINGLE_TONE_NO_EVENT:
		break;
	case SINGLE_TONE_STARTED:
		(*started)++;
		break;
	case SINGLE_TONE_PHASE_REVERSED:
		(*reversed)++;
		*reversal_sample = sample_index;
		*last_reversal = event;
		break;
	case SINGLE_TONE_ENDED:
		(*ended)++;
		break;
	}
	return true;
}

static bool check_detection(unsigned int frequency_hz, enum test_codec codec)
{
	struct single_tone_transmitter transmitter;
	struct single_tone_detector detector;
	struct single_tone_measurement measurement;
	struct single_tone_event reversal = { 0 };
	size_t started = 0;
	size_t reversed = 0;
	size_t ended = 0;
	size_t reversal_edge;
	size_t reversal_sample = 0;
	size_t sample_index = 0;

	CHECK(single_tone_transmitter_init(
		      &transmitter, frequency_hz, TEST_AMPLITUDE) ==
		      SINGLE_TONE_OK &&
		      single_tone_detector_init(
			      &detector, frequency_hz) ==
			      SINGLE_TONE_OK,
	      "single-tone detection fixture did not initialize");
	for (; sample_index < 23; ++sample_index)
		CHECK(feed(
			      &detector, 0, codec, sample_index, &started,
			      &reversed, &ended, &reversal_sample, &reversal),
		      "leading silence failed");
	for (size_t count = 0; count < 400; ++count, ++sample_index) {
		int16_t sample;

		CHECK(single_tone_transmitter_sample(
			      &transmitter, &sample) ==
			      SINGLE_TONE_OK &&
		      feed(
			      &detector, sample, codec, sample_index, &started,
			      &reversed, &ended, &reversal_sample, &reversal),
		      "steady tone detection failed");
	}
	measurement = single_tone_detector_measure(&detector);
	CHECK(started == 1 && reversed == 0 && ended == 0 &&
		      measurement.ready && measurement.tone_present &&
		      measurement.line_power > 1000000.0 &&
		      measurement.carrier_share > 0.90 &&
		      measurement.carrier_share < 1.10,
	      "detector did not acquire one qualified tone");
	reversal_edge = sample_index;
	CHECK(single_tone_transmitter_reverse(&transmitter) ==
		      SINGLE_TONE_OK,
	      "detection fixture did not reverse");
	for (size_t count = 0; count < 180; ++count, ++sample_index) {
		int16_t sample;

		CHECK(single_tone_transmitter_sample(
			      &transmitter, &sample) ==
			      SINGLE_TONE_OK &&
		      feed(
			      &detector, sample, codec, sample_index, &started,
			      &reversed, &ended, &reversal_sample, &reversal),
		      "reversed tone detection failed");
	}
	CHECK(reversed == 1 && reversal.reversal_delay_samples != 0 &&
		      reversal_sample >= reversal_edge &&
		      reversal_sample - reversal_edge + 1 >=
			      (size_t)reversal.reversal_delay_samples - 2 &&
		      reversal_sample - reversal_edge + 1 <=
			      (size_t)reversal.reversal_delay_samples + 2,
	      "detector did not report one latency-corrected reversal");
	for (size_t count = 0; count < 240; ++count, ++sample_index)
		CHECK(feed(
			      &detector, 0, codec, sample_index, &started,
			      &reversed, &ended, &reversal_sample, &reversal),
		      "trailing silence failed");
	CHECK(started == 1 && reversed == 1 && ended == 1 &&
		      !single_tone_detector_measure(&detector)
			       .tone_present,
	      "detector did not release one qualified tone");
	return true;
}

static bool test_detection_and_rejection(void)
{
	static const unsigned int frequencies[] = {
		TEST_ANSWER_HZ,
		TEST_CARRIER_HZ,
	};

	for (size_t frequency = 0; frequency < ARRAY_LENGTH(frequencies);
	     ++frequency)
		for (enum test_codec codec = TEST_CODEC_LINEAR;
		     codec <= TEST_CODEC_PCMA; ++codec)
			CHECK(check_detection(frequencies[frequency], codec),
			      "tone %u failed codec %d",
			      frequencies[frequency], codec);
	{
		struct single_tone_transmitter transmitter;
		struct single_tone_detector detector;
		size_t events = 0;

		CHECK(single_tone_transmitter_init(
			      &transmitter, TEST_WRONG_HZ, TEST_AMPLITUDE) ==
			      SINGLE_TONE_OK &&
		      single_tone_detector_init(
			      &detector, TEST_ANSWER_HZ) ==
			      SINGLE_TONE_OK,
		      "wrong-tone fixture did not initialize");
		for (size_t index = 0; index < 800; ++index) {
			struct single_tone_event event;
			int16_t sample;

			CHECK(single_tone_transmitter_sample(
				      &transmitter, &sample) ==
				      SINGLE_TONE_OK &&
			      single_tone_detector_sample(
				      &detector, sample, &event) ==
				      SINGLE_TONE_OK,
			      "wrong-tone rejection failed");
			events += event.kind != SINGLE_TONE_NO_EVENT;
		}
		CHECK(events == 0 && !detector.tone_present,
		      "detector acquired a neighboring carrier");
	}
	return true;
}

static bool mixed_tone_sample(
	struct single_tone_transmitter *low,
	struct single_tone_transmitter *high,
	enum test_codec codec, int16_t *sample)
{
	int16_t low_sample;
	int16_t high_sample;

	if (single_tone_transmitter_sample(low, &low_sample) !=
		    SINGLE_TONE_OK ||
	    single_tone_transmitter_sample(high, &high_sample) !=
		    SINGLE_TONE_OK)
		return false;
	*sample = codec_round_trip(
		(int16_t)((int32_t)low_sample + high_sample), codec);
	return true;
}

static bool test_configurable_carrier_share(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		struct single_tone_transmitter low;
		struct single_tone_transmitter high;
		struct single_tone_detector strict;
		struct single_tone_detector mixed;
		struct single_tone_measurement measurement;
		size_t strict_events = 0;
		size_t mixed_started = 0;
		size_t mixed_reversed = 0;

		CHECK(single_tone_transmitter_init(
			      &low, TEST_MIXED_LOW_HZ,
			      TEST_MIXED_LOW_AMPLITUDE) ==
			      SINGLE_TONE_OK &&
		      single_tone_transmitter_init(
			      &high, TEST_MIXED_HIGH_HZ,
			      TEST_MIXED_HIGH_AMPLITUDE) ==
			      SINGLE_TONE_OK &&
		      single_tone_detector_init(
			      &strict, TEST_MIXED_HIGH_HZ) ==
			      SINGLE_TONE_OK &&
		      single_tone_detector_init_with_minimum_carrier_share(
			      &mixed, TEST_MIXED_HIGH_HZ, 0.60) ==
			      SINGLE_TONE_OK,
		      "mixed-tone fixture did not initialize");
		for (size_t sample_index = 0; sample_index < 480;
		     ++sample_index) {
			struct single_tone_event strict_event;
			struct single_tone_event mixed_event;
			int16_t sample;

			CHECK(mixed_tone_sample(&low, &high, codec, &sample),
			      "mixed-tone generation failed");
			CHECK(single_tone_detector_sample(
				      &strict, sample, &strict_event) ==
				      SINGLE_TONE_OK &&
			      single_tone_detector_sample(
				      &mixed, sample, &mixed_event) ==
				      SINGLE_TONE_OK,
			      "mixed-tone detection failed");
			strict_events += strict_event.kind !=
					 SINGLE_TONE_NO_EVENT;
			mixed_started += mixed_event.kind ==
					 SINGLE_TONE_STARTED;
		}
		measurement = single_tone_detector_measure(&mixed);
		CHECK(strict_events == 0 && !strict.tone_present &&
			      mixed_started == 1 && measurement.tone_present &&
			      measurement.carrier_share >= 0.60 &&
			      measurement.carrier_share < 0.70,
		      "configured detector did not distinguish a mixed carrier");
		CHECK(single_tone_transmitter_reverse(&low) ==
			      SINGLE_TONE_OK &&
		      single_tone_transmitter_reverse(&high) ==
			      SINGLE_TONE_OK,
		      "mixed carrier did not reverse");
		for (size_t sample_index = 0; sample_index < 180;
		     ++sample_index) {
			struct single_tone_event event;
			int16_t sample;

			CHECK(mixed_tone_sample(&low, &high, codec, &sample),
			      "reversed mixed-tone generation failed");
			CHECK(single_tone_detector_sample(
				      &mixed, sample, &event) ==
				      SINGLE_TONE_OK,
			      "reversed mixed-tone detection failed");
			mixed_reversed += event.kind ==
					  SINGLE_TONE_PHASE_REVERSED;
		}
		CHECK(mixed_reversed == 1 &&
			      single_tone_detector_reset(&mixed) ==
				      SINGLE_TONE_OK &&
			      mixed.profile.minimum_carrier_share == (double)0.60,
		      "mixed-carrier profile did not detect or survive reset");
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct single_tone_detector detector;
	struct single_tone_detector before;
	struct single_tone_detector_profile profile;
	struct single_tone_event event = {
		.kind = SINGLE_TONE_ENDED,
		.reversal_delay_samples = 17,
	};
	struct single_tone_transmitter transmitter;
	struct single_tone_transmitter transmitter_before;
	int16_t sample = 123;

	CHECK(single_tone_transmitter_init(
		      &transmitter, TEST_CARRIER_HZ, TEST_AMPLITUDE) ==
		      SINGLE_TONE_OK,
	      "single-tone transmitter validation fixture changed");
	transmitter_before = transmitter;
	CHECK(single_tone_transmitter_init(
		      &transmitter, 0, TEST_AMPLITUDE) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
		      single_tone_transmitter_init(
			      &transmitter,
			      SINGLE_TONE_SAMPLE_RATE / 2,
			      TEST_AMPLITUDE) ==
			      SINGLE_TONE_INVALID_ARGUMENT &&
		      single_tone_transmitter_init(
			      &transmitter, TEST_CARRIER_HZ, 0) ==
			      SINGLE_TONE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &transmitter_before,
			     sizeof(transmitter)) == 0 &&
		      single_tone_detector_init(
			      &detector, TEST_CARRIER_HZ) ==
			      SINGLE_TONE_OK,
	      "single-tone validation fixture changed");
	before = detector;
	CHECK(single_tone_detector_init_with_minimum_carrier_share(
		      &detector, TEST_CARRIER_HZ, 0.0) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
		      single_tone_detector_init_with_minimum_carrier_share(
			      &detector, TEST_CARRIER_HZ, 1.21) ==
			      SINGLE_TONE_INVALID_ARGUMENT &&
		      single_tone_detector_init_with_minimum_carrier_share(
			      &detector, TEST_CARRIER_HZ, NAN) ==
			      SINGLE_TONE_INVALID_ARGUMENT &&
		      memcmp(&detector, &before, sizeof(detector)) == 0,
	      "invalid carrier-share profiles changed the detector");
	profile = detector.profile;
	profile.window_samples = 1;
	CHECK(single_tone_detector_init_profile(&detector, NULL) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_init_profile(&detector, &profile) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &before, sizeof(detector)) == 0,
	      "invalid detector profiles changed caller-owned state");
	detector.history[0].in_phase = NAN;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "NaN in-phase observation was accepted");
	detector.history[0].in_phase = 0.0;
	detector.history[0].quadrature = -INFINITY;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "infinite quadrature observation was accepted");
	detector.history[0].quadrature = 0.0;
	detector.history[0].energy = INFINITY;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "infinite observation energy was accepted");
	detector.history[0].energy = -1.0;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "negative observation energy was accepted");
	detector.history[0].energy = 0.0;
	detector.history[0].in_phase = -(double)INT16_MIN + 1.0;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "high in-phase observation was accepted");
	detector.history[0].in_phase = 0.0;
	detector.history[0].quadrature = (double)INT16_MIN - 1.0;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "low quadrature observation was accepted");
	detector.history[0].quadrature = 0.0;
	detector.history[0].energy = (double)INT16_MIN * INT16_MIN + 1.0;
	CHECK(!single_tone_detector_is_valid(&detector),
	      "high observation energy was accepted");
	detector.history[0].energy = 0.0;
	transmitter.phase_step++;
	transmitter_before = transmitter;
	CHECK(single_tone_transmitter_sample(
		      &transmitter, &sample) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &transmitter_before,
			     sizeof(transmitter)) == 0 &&
		      sample == 123,
	      "invalid transmitter mutation was not transactional");
	detector.phase_step++;
	before = detector;
	CHECK(single_tone_detector_sample(
		      &detector, 1, &event) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
		      memcmp(&detector, &before, sizeof(detector)) == 0 &&
		      event.kind == SINGLE_TONE_ENDED &&
		      event.reversal_delay_samples == 17,
	      "invalid detector mutation was not transactional");
	return true;
}

static bool test_history_and_numeric_rejection(void)
{
	struct single_tone_detector detector;
	struct single_tone_detector before;
	struct single_tone_detector_profile profile;
	struct single_tone_event event = {
		.kind = SINGLE_TONE_ENDED,
		.reversal_delay_samples = 17,
	};

	CHECK(single_tone_detector_init(&detector, TEST_CARRIER_HZ) ==
		      SINGLE_TONE_OK,
	      "history rejection fixture failed");
	profile = detector.profile;
	profile.window_samples /= 2u;
	CHECK(single_tone_detector_init_profile(&detector, &profile) ==
		      SINGLE_TONE_OK,
	      "short history fixture failed");
	detector.history[SINGLE_TONE_MAX_WINDOW_SAMPLES - 1u].energy = NAN;
	before = detector;
	CHECK(single_tone_detector_sample(&detector, 0, &event) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_suppress_sample(&detector) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &before, sizeof(detector)) == 0 &&
	      event.kind == SINGLE_TONE_ENDED &&
	      event.reversal_delay_samples == 17,
	      "invalid inactive history was accepted or changed");

	CHECK(single_tone_detector_init(&detector, TEST_CARRIER_HZ) ==
		      SINGLE_TONE_OK,
	      "numeric rejection fixture failed");
	/* Individually bounded fields can still produce a negative energy
	 * when the next update removes an inconsistent retained observation. */
	detector.history_count = detector.profile.window_samples;
	detector.history[0].energy = 1.0;
	CHECK(single_tone_detector_is_valid(&detector),
	      "numeric rejection fixture is not range-valid");
	before = detector;
	CHECK(single_tone_detector_sample(&detector, 0, &event) ==
		      SINGLE_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &before, sizeof(detector)) == 0 &&
	      event.kind == SINGLE_TONE_ENDED &&
	      event.reversal_delay_samples == 17,
	      "rejected numeric update changed state or emitted an event");
	return true;
}

static bool test_owned_observation(void)
{
	for (unsigned window = 40; window <= 80; window += 40) {
		struct single_tone_detector reference, owned;
		struct single_tone_transmitter transmitter;
		unsigned started = 0, ended = 0, reversed = 0;

		CHECK(single_tone_detector_init(&reference, 1800) == SINGLE_TONE_OK &&
		      single_tone_transmitter_init(&transmitter, 1800, 7000) == SINGLE_TONE_OK,
		      "owned observation fixture failed");
		reference.profile.window_samples = (uint16_t)window;
		owned = reference;
		for (unsigned n = 0; n < 10000; ++n) {
			int16_t sample;
			struct single_tone_event a, b;
			struct single_tone_measurement expected, observed;

			if (n == 5000 || n == 6000)
				CHECK(single_tone_transmitter_reverse(&transmitter) == SINGLE_TONE_OK,
				      "owned reversal failed");
			CHECK(single_tone_transmitter_sample(&transmitter, &sample) == SINGLE_TONE_OK,
			      "owned sample generation failed");
			if (n % 8000 < 400 || n % 8000 >= 6800) sample = 0;
			if (n >= 5300 && n < 5400) {
				CHECK(single_tone_detector_suppress_sample(&reference) == SINGLE_TONE_OK &&
				      single_tone_detector_suppress_sample(&owned) == SINGLE_TONE_OK,
				      "owned suppression failed");
				continue;
			}
			if (n == 5400)
				CHECK(single_tone_detector_resume(&reference, 17) == SINGLE_TONE_OK &&
				      single_tone_detector_resume(&owned, 17) == SINGLE_TONE_OK,
				      "owned resumption failed");
			CHECK(single_tone_detector_is_valid(&owned) &&
			      single_tone_detector_sample(&reference, sample, &a) == SINGLE_TONE_OK &&
			      single_tone_detector_observe_owned(&owned, sample, &b, &observed) == SINGLE_TONE_OK,
			      "owned observation rejected sample %u", n);
			expected = single_tone_detector_measure(&reference);
			CHECK(memcmp(&reference, &owned, sizeof(owned)) == 0 &&
			      a.kind == b.kind && a.reversal_delay_samples == b.reversal_delay_samples &&
			      expected.line_power == observed.line_power &&
			      expected.carrier_power == observed.carrier_power &&
			      expected.carrier_share == observed.carrier_share &&
			      expected.ready == observed.ready && expected.tone_present == observed.tone_present,
			      "owned observation changed state, event, or measurement at %u", n);
			started += a.kind == SINGLE_TONE_STARTED;
			ended += a.kind == SINGLE_TONE_ENDED;
			reversed += a.kind == SINGLE_TONE_PHASE_REVERSED;
		}
		CHECK(started >= 2 && ended >= 1 && reversed >= 1,
		      "owned observation did not exercise acquisition, loss, and reversal");
	}
	return true;
}

static bool test_observation_output_contracts(void)
{
	struct single_tone_detector detector, before;
	struct single_tone_event event = { .kind = SINGLE_TONE_ENDED };
	struct single_tone_measurement measurement = { .line_power = 123 };
	union {
		struct single_tone_event event;
		struct single_tone_measurement measurement;
	} overlap;

	CHECK(single_tone_detector_init(&detector, 1800) == SINGLE_TONE_OK,
	      "observation contract fixture failed");
	before = detector;
	CHECK(single_tone_detector_sample(&detector, 0,
		      (struct single_tone_event *)&detector.phase) == SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_observe_owned(&detector, 0, NULL, &measurement) == SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_observe_owned(&detector, 0, &event, NULL) == SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_observe_owned(&detector, 0, &event,
		      (struct single_tone_measurement *)&detector.history[0]) == SINGLE_TONE_INVALID_ARGUMENT &&
	      single_tone_detector_observe_owned(&detector, 0, &overlap.event,
		      &overlap.measurement) == SINGLE_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &before, sizeof(before)) == 0 &&
	      event.kind == SINGLE_TONE_ENDED && measurement.line_power == 123,
	      "overlapping observation outputs were not rejected transactionally");
	/* Range-valid cached totals can still fail when removing an old sample.
	 * The owner entry point must retain that post-update numeric check. */
	detector.history_count = detector.profile.window_samples;
	detector.history[0].energy = 1;
	before = detector;
	CHECK(single_tone_detector_is_valid(&detector) &&
	      single_tone_detector_observe_owned(&detector, 0, &event, &measurement) == SINGLE_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &before, sizeof(before)) == 0 &&
	      event.kind == SINGLE_TONE_ENDED && measurement.line_power == 123,
	      "owned numeric rejection changed state or outputs");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generation and reversal", test_generation_and_reversal },
		{ "detection and rejection", test_detection_and_rejection },
		{ "configurable carrier share", test_configurable_carrier_share },
		{ "transactional validation", test_validation_is_transactional },
		{ "history and numeric rejection", test_history_and_numeric_rejection },
		{ "owned observation equivalence", test_owned_observation },
		{ "observation output contracts", test_observation_output_contracts },
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
