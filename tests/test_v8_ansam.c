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
		      fragmented.signed_amplitude == single.signed_amplitude &&
		      peak <= (unsigned int)(TEST_AMPLITUDE * 6 / 5 + 1),
	      "ANSam generation was not bounded or chunk invariant");

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
		int16_t amplitude = controlled.signed_amplitude;

		CHECK(v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      V8_ANSAM_OK &&
			      controlled.carrier_phase != phase &&
			      controlled.signed_amplitude == -amplitude &&
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
	struct v8_ansam_detector blocked;
	struct v8_ansam_detector samplewise;
	struct v8_line_conditioner blocked_line;
	struct v8_line_conditioner samplewise_line;
	static const uint16_t expected_energy[] = { 4680, 4637, 4684 };
	int16_t samples[73];
	bool blocked_detected = false;
	bool sample_detected = false;
	size_t detection_sample = 0;
	size_t total = 0;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&blocked) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&samplewise) ==
		      V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &blocked_line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK &&
	      v8_line_conditioner_init(
		      &samplewise_line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK,
	      "ANSam loopback did not initialize");
	while (!sample_detected && total < 2u * V8_ANSAM_SAMPLE_RATE) {
		CHECK(v8_ansam_generate(
			      &generator, samples, ARRAY_LENGTH(samples)) ==
			      V8_ANSAM_OK,
		      "ANSam loopback generation failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
			samples[index] = codec_round_trip(samples[index], codec);
		CHECK(v8_ansam_detect(
			      &blocked, &blocked_line, samples,
			      ARRAY_LENGTH(samples),
			      &blocked_detected) == V8_ANSAM_OK,
		      "ANSam loopback detection failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
			CHECK(v8_ansam_detect(
				      &samplewise, &samplewise_line,
				      &samples[index], 1,
				      &sample_detected) ==
				      V8_ANSAM_OK,
			      "samplewise ANSam detection failed");
			if (sample_detected && detection_sample == 0)
				detection_sample = total + index + 1u;
		}
		total += ARRAY_LENGTH(samples);
	}
	CHECK(blocked_detected && sample_detected && detection_sample == 8782 &&
	      memcmp(&blocked, &samplewise, sizeof(blocked)) == 0 &&
	      memcmp(&blocked_line, &samplewise_line,
		     sizeof(blocked_line)) == 0 &&
	      blocked.stage == V8_ANSAM_DETECTED &&
	      blocked.dwell_blocks ==
		      V8_ANSAM_POST_DETECT_DWELL_BLOCKS + 1u &&
	      blocked_line.gain == 406 && blocked.tone.counter == 85 &&
	      blocked.modulation_energy == expected_energy[codec],
	      "ANSam codec %d changed the fixed detector oracle at %zu/%u",
	      codec, detection_sample, blocked.modulation_energy);
	return true;
}

static bool run_synthetic_signal(
	unsigned int carrier_hz, unsigned int modulation_hz,
	double modulation_depth, int amplitude,
	struct v8_ansam_detector *detector, bool *detected)
{
	struct v8_line_conditioner line;
	int16_t samples[80];
	size_t total = 3u * V8_ANSAM_SAMPLE_RATE;

	CHECK(v8_ansam_detector_init(detector) == V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK,
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
			      detector, &line, samples, ARRAY_LENGTH(samples),
			      detected) == V8_ANSAM_OK,
		      "synthetic ANSam detection failed");
	}
	return true;
}

static bool test_detection_and_rejection(void)
{
	static const unsigned int tolerated_carriers[] = { 2080, 2120 };
	struct v8_ansam_detector detector;
	bool detected;

	CHECK(detect_generated(TEST_LINEAR) && detect_generated(TEST_PCMU) &&
		      detect_generated(TEST_PCMA),
	      "ANSam was not codec-stable");
	for (size_t index = 0;
	     index < ARRAY_LENGTH(tolerated_carriers); ++index) {
		CHECK(run_synthetic_signal(
			      tolerated_carriers[index], 15, 0.2,
			      TEST_AMPLITUDE, &detector, &detected) && detected,
		      "ANSam carrier tolerance rejected %u Hz",
		      tolerated_carriers[index]);
	}
	CHECK(run_synthetic_signal(2100, 15, 0.0, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected &&
		      detector.stage == V8_ANSAM_WAIT_MODULATION &&
		      detector.modulation_energy <= 0x018f,
	      "unmodulated answer tone was accepted");
	CHECK(run_synthetic_signal(2100, 20, 0.2, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected,
	      "wrong envelope frequency was accepted");
	CHECK(run_synthetic_signal(2000, 15, 0.2, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected,
	      "wrong carrier frequency was accepted");
	CHECK(run_synthetic_signal(2100, 15, 0.2, 128,
				   &detector, &detected) &&
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
		struct v8_line_conditioner line;
		bool detected = false;

		CHECK(v8_ansam_generator_init(
			      &generator, TEST_AMPLITUDE, true) ==
			      V8_ANSAM_OK &&
		      generate_discard(&generator, start) &&
		      v8_ansam_detector_init(&detector) ==
			      V8_ANSAM_OK &&
		      v8_line_conditioner_init(
			      &line, V8_CALLING_STATION) ==
			      V8_LINE_CONDITIONER_OK,
		      "offset ANSam detector did not initialize");
		for (size_t offset = 0;
		     offset < 2u * V8_ANSAM_SAMPLE_RATE &&
		     !detected;
		     offset += ARRAY_LENGTH(samples)) {
			CHECK(v8_ansam_generate(
				      &generator, samples,
				      ARRAY_LENGTH(samples)) ==
				      V8_ANSAM_OK &&
			      v8_ansam_detect(
				      &detector, &line, samples,
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
	struct v8_ansam_detector saved;
	struct v8_line_conditioner line;
	struct v8_line_conditioner saved_line;
	int16_t sample = 123;
	int16_t output_sample = 123;
	bool detected = true;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, false) ==
		      V8_ANSAM_OK &&
	      v8_ansam_generate(&generator, NULL, 0) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&detector) ==
		      V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK &&
	      v8_ansam_detect(
		      &detector, &line, NULL, 0, &detected) ==
		      V8_ANSAM_OK &&
	      !detected && v8_ansam_detector_is_valid(&detector),
	      "ANSam zero-length operation failed");
	generator.signed_amplitude = 0;
	saved = detector;
	saved_line = line;
	CHECK(v8_ansam_generator_init(NULL, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generator_init(&generator, 0, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generate(&generator, &sample, 1) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, true) == V8_ANSAM_OK &&
	      v8_ansam_generate(
		      &generator, (int16_t *)&generator, 1) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      sample == 123 &&
	      v8_ansam_detector_init(NULL) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(NULL, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(&detector, NULL, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, (const int16_t *)&detector, 1,
		      &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, (const int16_t *)&line, 1,
		      &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&detector) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&line) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&output_sample) == V8_ANSAM_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
	      memcmp(&line, &saved_line, sizeof(line)) == 0 &&
	      output_sample == 123 && detected == false,
	      "ANSam overlap handling changed state or output");
	detector.signal_ref_gain = 0;
	CHECK(!v8_ansam_detector_is_valid(&detector) &&
	      v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT,
	      "ANSam API accepted invalid state");
	detector = saved;
	line.gain = 0;
	CHECK(!v8_line_conditioner_is_valid(&line) &&
	      v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT,
	      "ANSam API accepted invalid line state");
	CHECK(v8_ansam_detector_init(&detector) == V8_ANSAM_OK &&
	      v8_ansam_detector_is_valid(&detector) &&
	      detector.stage == V8_ANSAM_WAIT_CARRIER,
	      "ANSam detector reset retained receive evidence");
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
