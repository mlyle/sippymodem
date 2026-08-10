// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v8_ansam.h>

#include "test.h"

#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_FRAGMENT_SAMPLES = 37,
};

static bool generate_discard(
	struct sippymodem_v8_ansam_generator *generator, size_t count)
{
	int16_t samples[64];

	while (count != 0) {
		size_t fragment = count < ARRAY_LENGTH(samples) ?
					  count : ARRAY_LENGTH(samples);

		if (sippymodem_v8_ansam_generate(
			    generator, samples, fragment) !=
		    SIPPYMODEM_V8_ANSAM_OK)
			return false;
		count -= fragment;
	}
	return true;
}

static bool test_generated_waveform(void)
{
	struct sippymodem_v8_ansam_generator fragmented;
	struct sippymodem_v8_ansam_generator single;
	struct sippymodem_v8_ansam_generator controlled;
	int16_t fragment[TEST_FRAGMENT_SAMPLES];
	int16_t sample = 0;
	int16_t previous = 0;
	unsigned int crossings = 0;
	unsigned int peak = 0;
	size_t total = 3u * SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES;

	CHECK(sippymodem_v8_ansam_generator_init(
		      &fragmented, TEST_AMPLITUDE, true) ==
		      SIPPYMODEM_V8_ANSAM_OK &&
	      sippymodem_v8_ansam_generator_init(
		      &single, TEST_AMPLITUDE, true) ==
		      SIPPYMODEM_V8_ANSAM_OK,
	      "ANSam generators did not initialize");
	for (size_t offset = 0; offset < total;) {
		size_t count = (offset % TEST_FRAGMENT_SAMPLES) + 1u;

		if (count > total - offset)
			count = total - offset;
		CHECK(sippymodem_v8_ansam_generate(
			      &fragmented, fragment, count) ==
			      SIPPYMODEM_V8_ANSAM_OK,
		      "fragmented ANSam generation failed");
		for (size_t index = 0; index < count; ++index) {
			int magnitude;

			CHECK(sippymodem_v8_ansam_generate(
				      &single, &sample, 1) ==
				      SIPPYMODEM_V8_ANSAM_OK &&
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

	CHECK(sippymodem_v8_ansam_generator_init(
		      &controlled, 10000, false) ==
		      SIPPYMODEM_V8_ANSAM_OK,
	      "controlled ANSam generator did not initialize");
	controlled.carrier_phase = UINT32_C(0x40000000);
	controlled.modulation_phase = 0;
	CHECK(sippymodem_v8_ansam_generate(&controlled, &sample, 1) ==
		      SIPPYMODEM_V8_ANSAM_OK &&
		      sample == 12000,
	      "ANSam upper envelope was %d", sample);
	controlled.carrier_phase = UINT32_C(0x40000000);
	controlled.modulation_phase = UINT32_C(0x80000000);
	CHECK(sippymodem_v8_ansam_generate(&controlled, &sample, 1) ==
		      SIPPYMODEM_V8_ANSAM_OK &&
		      sample == 8000,
	      "ANSam lower envelope was %d", sample);

	CHECK(sippymodem_v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, false) ==
		      SIPPYMODEM_V8_ANSAM_OK,
	      "continuous ANSam generator did not initialize");
	for (size_t index = 0; index < SIPPYMODEM_V8_ANSAM_SAMPLE_RATE;
	     ++index) {
		CHECK(sippymodem_v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      SIPPYMODEM_V8_ANSAM_OK,
		      "continuous ANSam generation failed");
		if (previous <= 0 && sample > 0)
			crossings++;
		previous = sample;
	}
	CHECK(crossings == SIPPYMODEM_V8_ANSAM_CARRIER_HZ,
	      "ANSam carrier produced %u positive crossings", crossings);

	CHECK(sippymodem_v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, true) ==
		      SIPPYMODEM_V8_ANSAM_OK &&
	      generate_discard(
		      &controlled,
		      SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES - 1u) &&
	      controlled.samples_until_reversal == 1,
	      "ANSam phase-reversal boundary was not reached");
	{
		uint32_t phase = controlled.carrier_phase;

		CHECK(sippymodem_v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      SIPPYMODEM_V8_ANSAM_OK &&
			      controlled.carrier_phase ==
				      phase + controlled.carrier_phase_step +
					      UINT32_C(0x80000000) &&
			      controlled.samples_until_reversal ==
				      SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES,
		      "ANSam did not reverse phase at 450 ms");
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v8_ansam_generator generator;
	int16_t sample = 123;

	CHECK(sippymodem_v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, false) ==
		      SIPPYMODEM_V8_ANSAM_OK &&
	      sippymodem_v8_ansam_generate(&generator, NULL, 0) ==
		      SIPPYMODEM_V8_ANSAM_OK,
	      "ANSam zero-length generation failed");
	generator.carrier_phase_step = 0;
	CHECK(sippymodem_v8_ansam_generator_init(NULL, TEST_AMPLITUDE, true) ==
		      SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT &&
	      sippymodem_v8_ansam_generator_init(&generator, 0, true) ==
		      SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT &&
	      sippymodem_v8_ansam_generate(&generator, &sample, 1) ==
		      SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT &&
	      sample == 123 &&
	      strcmp(sippymodem_v8_ansam_result_name(
			     SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "ANSam API accepted invalid state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated waveform", test_generated_waveform },
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
