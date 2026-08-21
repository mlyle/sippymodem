// SPDX-License-Identifier: BSD-3-Clause

#include <v34_s_tone_detector.h>

#include "test.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	TONE_AMPLITUDE = 12000,
	MAXIMUM_TONE_SAMPLES = 512,
};

static const double PI = 3.14159265358979323846264338327950288;

struct detector_case {
	enum v34_symbol_rate symbol_rate;
	bool high_carrier;
	enum v34_s_tone_carrier detector_carrier;
};

static const struct detector_case DETECTOR_CASES[] = {
	{ V34_SYMBOL_RATE_2400, false, V34_S_TONE_CARRIER_1600 },
	{ V34_SYMBOL_RATE_2400, true, V34_S_TONE_CARRIER_1800 },
	{ V34_SYMBOL_RATE_2743, false, V34_S_TONE_CARRIER_1680 },
	{ V34_SYMBOL_RATE_2743, true, V34_S_TONE_CARRIER_1829 },
	{ V34_SYMBOL_RATE_2800, false, V34_S_TONE_CARRIER_1680 },
	{ V34_SYMBOL_RATE_2800, true, V34_S_TONE_CARRIER_1867 },
	{ V34_SYMBOL_RATE_3000, false, V34_S_TONE_CARRIER_1800 },
	{ V34_SYMBOL_RATE_3000, true, V34_S_TONE_CARRIER_2000 },
	{ V34_SYMBOL_RATE_3200, false, V34_S_TONE_CARRIER_1829 },
	{ V34_SYMBOL_RATE_3200, true, V34_S_TONE_CARRIER_1920 },
	{ V34_SYMBOL_RATE_3429, false, V34_S_TONE_CARRIER_1959 },
	{ V34_SYMBOL_RATE_3429, true, V34_S_TONE_CARRIER_1959 },
};

static bool init_profile(
	struct v34_phase3_profile *profile,
	enum v34_symbol_rate symbol_rate, bool high_carrier)
{
	const struct v34_info1_probe_result probe = {
		.high_carrier = high_carrier,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 1,
	};

	return v34_phase3_profile_init(
		       profile, V34_CALLING_STATION, symbol_rate, &probe, 0, 1) ==
	       V34_PHASE3_PROFILE_OK;
}

static enum v34_s_tone_detector_result feed_tone(
	struct v34_s_tone_detector *detector,
	const struct v34_phase3_profile *profile, unsigned int sample_count,
	double frequency_hz, unsigned int *groups)
{
	uint64_t group_phase = 0;
	const uint64_t group_limit =
		(uint64_t)SAMPLE_RATE * profile->symbol_rate_denominator;
	enum v34_s_tone_detector_result result = V34_S_TONE_DETECTOR_OK;

	*groups = 0;
	for (unsigned int index = 0; index < sample_count; ++index) {
		double phase = 2.0 * PI * frequency_hz * index / SAMPLE_RATE;
		int16_t sample = (int16_t)lround(TONE_AMPLITUDE * sin(phase));

		result = v34_s_tone_detector_sample(detector, sample);
		if (result != V34_S_TONE_DETECTOR_OK &&
		    result != V34_S_TONE_DETECTOR_DETECTED)
			return result;
		group_phase += profile->symbol_rate_numerator;
		if (group_phase < group_limit)
			continue;
		group_phase -= group_limit;
		(*groups)++;
		result = v34_s_tone_detector_end_group(detector);
		if (result != V34_S_TONE_DETECTOR_OK)
			return result;
	}
	return result;
}

static bool test_carrier_profiles(void)
{
	for (size_t index = 0; index < ARRAY_LENGTH(DETECTOR_CASES); ++index) {
		const struct detector_case *test_case = &DETECTOR_CASES[index];
		struct v34_phase3_profile profile;
		struct v34_s_tone_detector detector;
		unsigned int groups;
		double frequency_hz;

		CHECK(init_profile(
			      &profile, test_case->symbol_rate,
			      test_case->high_carrier) &&
		      v34_s_tone_detector_init(&detector, &profile) ==
			      V34_S_TONE_DETECTOR_OK &&
		      detector.carrier == test_case->detector_carrier,
		      "V.34 S detector case %zu did not initialize", index);
		frequency_hz = (double)profile.carrier_frequency_numerator /
			       profile.carrier_frequency_denominator;
		CHECK(feed_tone(
			      &detector, &profile, MAXIMUM_TONE_SAMPLES,
			      frequency_hz, &groups) ==
			      V34_S_TONE_DETECTOR_DETECTED &&
		      detector.detected && groups >= 20u && groups <= 24u,
		      "V.34 S detector case %zu missed %.3f Hz after %u groups",
		      index, frequency_hz, groups);
		{
			struct v34_s_tone_detector saved = detector;

			CHECK(v34_s_tone_detector_sample(&detector, 1234) ==
				      V34_S_TONE_DETECTOR_DETECTED &&
			      v34_s_tone_detector_end_group(&detector) ==
				      V34_S_TONE_DETECTOR_DETECTED &&
			      memcmp(&detector, &saved, sizeof(detector)) == 0,
			      "V.34 S detector case %zu did not remain terminal",
			      index);
		}
	}
	return true;
}

static bool test_rejection_and_ownership(void)
{
	struct v34_phase3_profile profile;
	struct v34_s_tone_detector detector;
	struct v34_s_tone_detector copy;
	struct v34_s_tone_detector saved;
	struct v34_s_tone_detector invalid = { 0 };
	uint64_t group_phase = 0;
	uint32_t random = UINT32_C(0x9e3779b9);

	CHECK(init_profile(&profile, V34_SYMBOL_RATE_2743, false) &&
		      v34_s_tone_detector_init(&detector, &profile) ==
			      V34_S_TONE_DETECTOR_OK &&
		      v34_s_tone_detector_is_valid(&detector) &&
		      !v34_s_tone_detector_is_valid(NULL) &&
		      !v34_s_tone_detector_is_valid(&invalid),
	      "V.34 S detector lifecycle changed");
	saved = invalid;
	CHECK(v34_s_tone_detector_init(NULL, &profile) ==
		      V34_S_TONE_DETECTOR_INVALID_ARGUMENT &&
		      v34_s_tone_detector_init(&invalid, NULL) ==
			      V34_S_TONE_DETECTOR_INVALID_ARGUMENT &&
		      v34_s_tone_detector_sample(NULL, 0) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      v34_s_tone_detector_sample(&invalid, 0) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      v34_s_tone_detector_end_group(NULL) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      v34_s_tone_detector_end_group(&invalid) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      memcmp(&invalid, &saved, sizeof(invalid)) == 0,
	      "V.34 S detector rejection was not transactional");

	copy = detector;
	for (unsigned int index = 0; index < 256; ++index) {
		const uint64_t group_limit =
			(uint64_t)SAMPLE_RATE * profile.symbol_rate_denominator;
		int16_t sample;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		sample = (int16_t)(random >> 20);
		CHECK(v34_s_tone_detector_sample(&detector, sample) ==
			      V34_S_TONE_DETECTOR_OK &&
		      v34_s_tone_detector_sample(&copy, sample) ==
			      V34_S_TONE_DETECTOR_OK &&
		      memcmp(&detector, &copy, sizeof(detector)) == 0,
		      "V.34 S detector ownership sample %u failed", index);
		group_phase += profile.symbol_rate_numerator;
		if (group_phase >= group_limit) {
			group_phase -= group_limit;
			CHECK(v34_s_tone_detector_end_group(&detector) ==
				      V34_S_TONE_DETECTOR_OK &&
			      v34_s_tone_detector_end_group(&copy) ==
				      V34_S_TONE_DETECTOR_OK &&
			      memcmp(&detector, &copy, sizeof(detector)) == 0,
			      "V.34 S detector ownership group changed at %u",
			      index);
		}
	}
	CHECK(v34_s_tone_detector_is_valid(&copy) && !copy.detected,
	      "V.34 S detector accepted low-level noise");
	invalid = copy;
	invalid.warmup_groups = 0;
	saved = invalid;
	CHECK(!v34_s_tone_detector_is_valid(&invalid) &&
		      v34_s_tone_detector_sample(&invalid, 0) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      v34_s_tone_detector_end_group(&invalid) ==
			      V34_S_TONE_DETECTOR_INVALID_STATE &&
		      memcmp(&invalid, &saved, sizeof(invalid)) == 0,
	      "V.34 S detector corrupt state was not rejected");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "carrier profiles", test_carrier_profiles },
		{ "rejection and ownership", test_rejection_and_ownership },
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
