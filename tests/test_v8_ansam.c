// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v8_ansam.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_FRAGMENT_SAMPLES = 37,
};

static const double TEST_TWO_PI =
	6.283185307179586476925286766559;

static bool generate_discard(
	struct v8_ansam_generator *generator, size_t count)
{
	int16_t samples[64];

	while (count != 0) {
		size_t fragment = count < ARRAY_LENGTH(samples) ?
					  count : ARRAY_LENGTH(samples);

		if (v8_ansam_generate(
			    generator, samples, fragment) !=
		    V8_ANSAM_OK)
			return false;
		count -= fragment;
	}
	return true;
}

static bool test_generated_waveform(void)
{
	struct v8_ansam_generator fragmented;
	struct v8_ansam_generator single;
	struct v8_ansam_generator controlled;
	int16_t fragment[TEST_FRAGMENT_SAMPLES];
	int16_t sample = 0;
	int16_t previous = 0;
	unsigned int crossings = 0;
	unsigned int peak = 0;
	size_t total = 3u * V8_ANSAM_REVERSAL_SAMPLES;

	CHECK(v8_ansam_generator_init(
		      &fragmented, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      v8_ansam_generator_init(
		      &single, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK,
	      "ANSam generators did not initialize");
	for (size_t offset = 0; offset < total;) {
		size_t count = (offset % TEST_FRAGMENT_SAMPLES) + 1u;

		if (count > total - offset)
			count = total - offset;
		CHECK(v8_ansam_generate(
			      &fragmented, fragment, count) ==
			      V8_ANSAM_OK,
		      "fragmented ANSam generation failed");
		for (size_t index = 0; index < count; ++index) {
			int magnitude;

			CHECK(v8_ansam_generate(
				      &single, &sample, 1) ==
				      V8_ANSAM_OK &&
				      sample == fragment[index],
			      "ANSam changed across fragment %zu", offset + index);
			magnitude = sample < 0 ? -sample : sample;
			if ((unsigned int)magnitude > peak)
				peak = (unsigned int)magnitude;
		}
		offset += count;
	}
	CHECK(fragmented.carrier_phase == single.carrier_phase &&
		      fragmented.modulation_phase == single.modulation_phase &&
		      fragmented.samples_until_reversal ==
			      single.samples_until_reversal &&
		      peak <= (unsigned int)(TEST_AMPLITUDE * 6 / 5),
	      "ANSam generation was not bounded or chunk invariant");

	CHECK(v8_ansam_generator_init(
		      &controlled, 10000, false) ==
		      V8_ANSAM_OK,
	      "controlled ANSam generator did not initialize");
	controlled.carrier_phase = UINT32_C(0x40000000);
	controlled.modulation_phase = 0;
	CHECK(v8_ansam_generate(&controlled, &sample, 1) ==
		      V8_ANSAM_OK &&
		      sample == 12000,
	      "ANSam upper envelope was %d", sample);
	controlled.carrier_phase = UINT32_C(0x40000000);
	controlled.modulation_phase = UINT32_C(0x80000000);
	CHECK(v8_ansam_generate(&controlled, &sample, 1) ==
		      V8_ANSAM_OK &&
		      sample == 8000,
	      "ANSam lower envelope was %d", sample);

	CHECK(v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, false) ==
		      V8_ANSAM_OK,
	      "continuous ANSam generator did not initialize");
	for (size_t index = 0; index < V8_ANSAM_SAMPLE_RATE;
	     ++index) {
		CHECK(v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      V8_ANSAM_OK,
		      "continuous ANSam generation failed");
		if (previous <= 0 && sample > 0)
			crossings++;
		previous = sample;
	}
	CHECK(crossings == V8_ANSAM_CARRIER_HZ,
	      "ANSam carrier produced %u positive crossings", crossings);

	CHECK(v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      generate_discard(
		      &controlled,
		      V8_ANSAM_REVERSAL_SAMPLES - 1u) &&
	      controlled.samples_until_reversal == 1,
	      "ANSam phase-reversal boundary was not reached");
	{
		uint32_t phase = controlled.carrier_phase;

		CHECK(v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      V8_ANSAM_OK &&
			      controlled.carrier_phase ==
				      phase + controlled.carrier_phase_step +
					      UINT32_C(0x80000000) &&
			      controlled.samples_until_reversal ==
				      V8_ANSAM_REVERSAL_SAMPLES,
		      "ANSam did not reverse phase at 450 ms");
	}
	return true;
}

enum test_codec {
	TEST_LINEAR,
	TEST_PCMU,
	TEST_PCMA,
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_PCMU)
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	if (codec == TEST_PCMA)
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	return sample;
}

static bool detect_generated(enum test_codec codec)
{
	struct v8_ansam_generator generator;
	struct v8_ansam_detector detector;
	struct v8_ansam_detector fragmented;
	struct v8_ansam_measurement measurement;
	struct v8_ansam_measurement fragmented_measurement;
	int16_t samples[80];
	bool detected = false;
	bool fragmented_detected = false;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&detector) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&fragmented) ==
		      V8_ANSAM_OK,
	      "ANSam loopback did not initialize");
	for (size_t offset = 0;
	     offset < 2u * V8_ANSAM_DETECTOR_WINDOW_SAMPLES;
	     offset += ARRAY_LENGTH(samples)) {
		CHECK(v8_ansam_generate(
			      &generator, samples, ARRAY_LENGTH(samples)) ==
			      V8_ANSAM_OK,
		      "ANSam loopback generation failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
			samples[index] = codec_round_trip(samples[index], codec);
		CHECK(v8_ansam_detect(
			      &detector, samples, ARRAY_LENGTH(samples),
			      &detected) == V8_ANSAM_OK,
		      "ANSam loopback detection failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
			CHECK(v8_ansam_detect(
				      &fragmented, &samples[index], 1,
				      &fragmented_detected) ==
				      V8_ANSAM_OK,
			      "fragmented ANSam detection failed");
		}
	}
	measurement = v8_ansam_measure(&detector);
	fragmented_measurement = v8_ansam_measure(&fragmented);
	CHECK(detected && fragmented_detected && measurement.ready &&
		      measurement.detected &&
		      measurement.qualifying_windows ==
			      V8_ANSAM_REQUIRED_WINDOWS &&
		      measurement.carrier_share > 0.9 &&
		      measurement.modulation_ratio > 0.015 &&
		      measurement.modulation_ratio < 0.025 &&
		      measurement.carrier_share ==
			      fragmented_measurement.carrier_share &&
		      measurement.modulation_ratio ==
			      fragmented_measurement.modulation_ratio,
	      "ANSam codec %d measured carrier %.4f modulation %.4f",
	      codec, measurement.carrier_share, measurement.modulation_ratio);
	return true;
}

static bool run_synthetic_signal(
	unsigned int carrier_hz, unsigned int modulation_hz,
	double modulation_depth, int amplitude, bool *detected,
	struct v8_ansam_measurement *measurement)
{
	struct v8_ansam_detector detector;
	int16_t samples[80];
	size_t total = 4u * V8_ANSAM_DETECTOR_WINDOW_SAMPLES;

	CHECK(v8_ansam_detector_init(&detector) ==
		      V8_ANSAM_OK,
	      "synthetic ANSam detector did not initialize");
	*detected = false;
	for (size_t offset = 0; offset < total;
	     offset += ARRAY_LENGTH(samples)) {
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
			double sample_index = (double)(offset + index);
			double carrier = sin(
				TEST_TWO_PI * carrier_hz * sample_index /
				V8_ANSAM_SAMPLE_RATE);
			double envelope =
				1.0 + modulation_depth *
					      cos(TEST_TWO_PI * modulation_hz *
						  sample_index /
						  V8_ANSAM_SAMPLE_RATE);

			samples[index] =
				(int16_t)lround(amplitude * envelope * carrier);
		}
		CHECK(v8_ansam_detect(
			      &detector, samples, ARRAY_LENGTH(samples),
			      detected) == V8_ANSAM_OK,
		      "synthetic ANSam detection failed");
	}
	*measurement = v8_ansam_measure(&detector);
	return true;
}

static bool test_detection_and_rejection(void)
{
	struct v8_ansam_measurement measurement;
	bool detected;

	CHECK(detect_generated(TEST_LINEAR) && detect_generated(TEST_PCMU) &&
		      detect_generated(TEST_PCMA),
	      "ANSam was not codec-stable");
	CHECK(run_synthetic_signal(2100, 15, 0.0, TEST_AMPLITUDE,
				   &detected, &measurement) &&
		      !detected && measurement.modulation_ratio < 0.001,
	      "unmodulated answer tone was accepted");
	CHECK(run_synthetic_signal(2100, 20, 0.2, TEST_AMPLITUDE,
				   &detected, &measurement) &&
		      !detected,
	      "wrong envelope frequency was accepted");
	CHECK(run_synthetic_signal(2000, 15, 0.2, TEST_AMPLITUDE,
				   &detected, &measurement) &&
		      !detected,
	      "wrong carrier frequency was accepted");
	CHECK(run_synthetic_signal(2100, 15, 0.2, 128,
				   &detected, &measurement) &&
		      !detected,
	      "sub-threshold answer signal was accepted");
	return true;
}

static bool test_reversal_alignment(void)
{
	int16_t samples[80];

	for (size_t start = 0;
	     start < V8_ANSAM_REVERSAL_SAMPLES; start += 300) {
		struct v8_ansam_generator generator;
		struct v8_ansam_detector detector;
		bool detected = false;

		CHECK(v8_ansam_generator_init(
			      &generator, TEST_AMPLITUDE, true) ==
			      V8_ANSAM_OK &&
		      generate_discard(&generator, start) &&
		      v8_ansam_detector_init(&detector) ==
			      V8_ANSAM_OK,
		      "offset ANSam detector did not initialize");
		for (size_t offset = 0;
		     offset < 6u *
				      V8_ANSAM_DETECTOR_WINDOW_SAMPLES &&
		     !detected;
		     offset += ARRAY_LENGTH(samples)) {
			CHECK(v8_ansam_generate(
				      &generator, samples,
				      ARRAY_LENGTH(samples)) ==
				      V8_ANSAM_OK &&
			      v8_ansam_detect(
				      &detector, samples,
				      ARRAY_LENGTH(samples), &detected) ==
				      V8_ANSAM_OK,
			      "offset ANSam processing failed");
		}
		CHECK(detected,
		      "ANSam reversal alignment %zu prevented detection", start);
	}
	return true;
}

static bool test_validation(void)
{
	struct v8_ansam_generator generator;
	struct v8_ansam_detector detector;
	int16_t sample = 123;
	bool detected = true;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, false) ==
		      V8_ANSAM_OK &&
	      v8_ansam_generate(&generator, NULL, 0) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&detector) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detect(
		      &detector, NULL, 0, &detected) ==
		      V8_ANSAM_OK &&
	      !detected,
	      "ANSam zero-length operation failed");
	generator.carrier_phase_step = 0;
	detector.carrier.phase_step = 0;
	CHECK(v8_ansam_generator_init(NULL, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generator_init(&generator, 0, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generate(&generator, &sample, 1) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      sample == 123 &&
	      v8_ansam_detector_init(NULL) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(&detector, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      !v8_ansam_measure(&detector).ready &&
	      strcmp(v8_ansam_result_name(
			     V8_ANSAM_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "ANSam API accepted invalid state");
	v8_ansam_detector_reset(&detector);
	CHECK(!v8_ansam_measure(&detector).ready,
	      "ANSam detector reset retained a measurement");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated waveform", test_generated_waveform },
		{ "detection and rejection", test_detection_and_rejection },
		{ "phase-reversal alignment", test_reversal_alignment },
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
