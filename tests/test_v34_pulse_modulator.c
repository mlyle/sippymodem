// SPDX-License-Identifier: BSD-3-Clause

#include <v34_pulse_modulator.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SAMPLE_RATE = 8000,
	TEST_SAMPLES = 4096,
};

struct exact_rates {
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
};

static struct exact_rates rates_for(
	enum v34_symbol_rate rate, bool high_carrier)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		return (struct exact_rates){
			2400, 1, high_carrier ? 1800u : 1600u, 1,
		};
	case V34_SYMBOL_RATE_2743:
		return (struct exact_rates){
			19200, 7, high_carrier ? 12800u : 11520u, 7,
		};
	case V34_SYMBOL_RATE_2800:
		return (struct exact_rates){
			2800, 1, high_carrier ? 5600u : 1680u,
			high_carrier ? 3u : 1u,
		};
	case V34_SYMBOL_RATE_3000:
		return (struct exact_rates){
			3000, 1, high_carrier ? 2000u : 1800u, 1,
		};
	case V34_SYMBOL_RATE_3200:
		return (struct exact_rates){
			3200, 1, high_carrier ? 1920u : 12800u,
			high_carrier ? 1u : 7u,
		};
	case V34_SYMBOL_RATE_3429:
		return (struct exact_rates){ 24000, 7, 96000, 49 };
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return (struct exact_rates){ 0 };
}

static struct v34_phase3_profile profile_for(
	enum v34_symbol_rate rate, bool high_carrier)
{
	struct exact_rates rates = rates_for(rate, high_carrier);

	return (struct v34_phase3_profile){
		.symbol_rate_numerator = rates.symbol_numerator,
		.symbol_rate_denominator = rates.symbol_denominator,
		.carrier_frequency_numerator = rates.carrier_numerator,
		.carrier_frequency_denominator = rates.carrier_denominator,
		.symbol_rate = rate,
		.local_role = V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = 1,
		.high_carrier = high_carrier,
	};
}

static struct qam_channel channel_for(
	const struct v34_phase3_profile *profile)
{
	return (struct qam_channel){
		.sample_rate = TEST_SAMPLE_RATE,
		.symbol_rate_numerator = profile->symbol_rate_numerator,
		.symbol_rate_denominator = profile->symbol_rate_denominator,
		.carrier_frequency_numerator =
			profile->carrier_frequency_numerator,
		.carrier_frequency_denominator =
			profile->carrier_frequency_denominator,
		.rolloff = 0.1,
		.pulse_span_symbols = 14,
	};
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
	for (unsigned int byte = 0; byte < 8; ++byte) {
		hash ^= (value >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static uint32_t random_step(uint32_t state)
{
	return state * UINT32_C(1664525) + UINT32_C(1013904223);
}

static struct qam_point generated_point(uint32_t *random)
{
	static const double LEVELS[] = {
		-0.875, -0.625, -0.375, -0.125,
		0.125, 0.375, 0.625, 0.875,
	};
	struct qam_point point;

	*random = random_step(*random);
	point.in_phase = LEVELS[*random >> 29];
	*random = random_step(*random);
	point.quadrature = LEVELS[*random >> 29];
	return point;
}

static uint64_t waveform_hash(
	enum v34_symbol_rate rate, bool high_carrier,
	uint8_t preemphasis_filter_index, uint32_t *accepted_count)
{
	struct v34_phase3_profile profile = profile_for(rate, high_carrier);
	struct qam_channel channel = channel_for(&profile);
	struct v34_pulse_modulator modulator;
	uint32_t random = UINT32_C(0x34b19f2d) ^ (uint32_t)rate * 97u ^
		high_carrier;
	uint64_t hash = UINT64_C(1469598103934665603);
	uint32_t accepted_total = 0;

	profile.preemphasis_filter_index = preemphasis_filter_index;

	/* Exercise line-channel construction for every waveform profile. */
	if (!v34_line_channel_matches(&profile, &channel) ||
	    v34_line_channel_init(&profile, TEST_SAMPLE_RATE, &channel) !=
		    V34_LINE_CHANNEL_OK ||
	    v34_pulse_modulator_init(
		    &modulator, &profile, &channel, 2.0) !=
	    V34_PULSE_MODULATOR_OK)
		return 0;
	for (uint32_t index = 0; index < TEST_SAMPLES; ++index) {
		struct qam_point point;
		const struct qam_point *supplied = NULL;
		double sample;
		bool accepted;
		long long quantized;

		if (v34_pulse_modulator_needs_point(&modulator)) {
			point = generated_point(&random);
			supplied = &point;
		}
		if (v34_pulse_modulator_sample(
			    &modulator, supplied, &sample, &accepted) !=
		    V34_PULSE_MODULATOR_OK)
			return 0;
		quantized = llround(sample * 1048576.0);
		hash = hash_u64(hash, (uint64_t)quantized);
		hash = hash_u64(hash, accepted);
		accepted_total += accepted;
	}
	hash = hash_u64(hash, modulator.carrier_phase);
	hash = hash_u64(hash, modulator.symbol_phase_units);
	*accepted_count = accepted_total;
	return hash;
}

static bool test_all_profiles(void)
{
	static const uint64_t EXPECTED[11] = {
		UINT64_C(0x7e6155d5ed36855e),
		UINT64_C(0x6a17ab2ed8e42ace),
		UINT64_C(0x74dcb297da95de6b),
		UINT64_C(0xd5b6a1047f06c969),
		UINT64_C(0x4cfcea8ae9b29c0c),
		UINT64_C(0x58c89967703c9a05),
		UINT64_C(0x07bb09b38b179d81),
		UINT64_C(0x8cc78687ed5446e1),
		UINT64_C(0xcf7801c5687bc36d),
		UINT64_C(0x44299af826d84dab),
		UINT64_C(0xa6d374507b967fa7),
	};
	unsigned int configuration = 0;
	bool hashes_match = true;

	for (unsigned int rate_index = 0;
	     rate_index < V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum v34_symbol_rate rate = (enum v34_symbol_rate)rate_index;
		unsigned int carrier_count =
			rate == V34_SYMBOL_RATE_3429 ? 1u : 2u;

		for (unsigned int high = 0; high < carrier_count; ++high) {
			struct exact_rates rates = rates_for(rate, high != 0);
			uint32_t accepted = 0;
			uint32_t expected_accepted = (uint32_t)(
				((uint64_t)TEST_SAMPLES * rates.symbol_numerator +
				 TEST_SAMPLE_RATE * rates.symbol_denominator - 1u) /
				(TEST_SAMPLE_RATE * rates.symbol_denominator));
			uint64_t hash = waveform_hash(
				rate, high != 0, 0, &accepted);

			if (hash != EXPECTED[configuration]) {
				fprintf(stderr,
					"profile %u/%u hash %016llx\n",
					rate_index, high,
					(unsigned long long)hash);
				hashes_match = false;
			}
			CHECK(hash != 0 && accepted == expected_accepted,
			      "profile %u/%u cadence changed (%u/%u)",
			      rate_index, high, accepted, expected_accepted);
			configuration++;
		}
	}
	CHECK(configuration == ARRAY_LENGTH(EXPECTED) && hashes_match,
	      "V.34 pulse profile waveform changed");
	return true;
}

static bool test_fixed_line_oracle(void)
{
	uint32_t accepted = 0;
	uint64_t hash = waveform_hash(
		V34_SYMBOL_RATE_2400, true, 1, &accepted);

	CHECK(hash == UINT64_C(0xa58931810ae8059e) && accepted == 1229,
	      "fixed 2400-baud line oracle changed (%016llx/%u)",
	      (unsigned long long)hash, accepted);
	return true;
}

static bool test_preemphasis_profiles(void)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		for (uint8_t filter = 1;
		     filter <= V34_PREEMPHASIS_MAX_FILTER_INDEX; ++filter) {
			uint32_t accepted = 0;
			uint64_t waveform = waveform_hash(
				(enum v34_symbol_rate)rate,
				rate == V34_SYMBOL_RATE_3429, filter, &accepted);

			CHECK(waveform != 0 && accepted != 0,
			      "pre-emphasis profile %u/%u did not run",
			      rate, filter);
			hash = hash_u64(hash, waveform);
		}
	}
	CHECK(hash == UINT64_C(0x00e51937842ca4e8),
	      "integrated pre-emphasis profiles changed (%016llx)",
	      (unsigned long long)hash);
	return true;
}

static bool test_transactional_contract(void)
{
	struct v34_phase3_profile profile =
		profile_for(V34_SYMBOL_RATE_3000, true);
	struct qam_channel channel = channel_for(&profile);
	struct v34_pulse_modulator modulator;
	struct v34_pulse_modulator before;
	struct qam_point point = { 0.5, -0.25 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point excessive = { 1.5, 0.0 };
	double sample = 123.0;
	bool accepted = true;

	CHECK(v34_pulse_modulator_init(
		      &modulator, &profile, &channel, 2.0) ==
		      V34_PULSE_MODULATOR_OK &&
	      v34_pulse_modulator_needs_point(&modulator),
	      "transaction fixture did not initialize");
	before = modulator;
	CHECK(v34_pulse_modulator_sample(
		      NULL, &point, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_sample(
		      &modulator, NULL, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_sample(
		      &modulator, &invalid, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_sample(
		      &modulator, &excessive, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_sample(
		      &modulator, &point, NULL, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_sample(
		      &modulator, &point, &sample, NULL) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0 &&
	      sample == 123.0 && accepted,
	      "rejected point or output mutated state");
	CHECK(v34_pulse_modulator_sample(
		      &modulator, &point, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_OK && accepted,
	      "valid point was not accepted");
	before = modulator;
	sample = 123.0;
	accepted = true;
	CHECK(v34_pulse_modulator_sample(
		      &modulator, &point, &sample, &accepted) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0 &&
	      sample == 123.0 && accepted,
	      "early point mutated state or outputs");
	before = modulator;
	CHECK(v34_pulse_modulator_sample(
		      &modulator, NULL, (double *)(void *)&modulator,
		      &accepted) == V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&modulator, &before, sizeof(modulator)) == 0,
	      "owner/output overlap mutated state");
	return true;
}

static bool test_initialization_and_copy(void)
{
	struct v34_phase3_profile profile =
		profile_for(V34_SYMBOL_RATE_2743, false);
	struct v34_phase3_profile invalid_profile = profile;
	struct v34_phase3_profile invalid_preemphasis = profile;
	struct qam_channel channel = channel_for(&profile);
	struct qam_channel invalid_channel = channel;
	struct v34_pulse_modulator left;
	struct v34_pulse_modulator right;
	struct v34_pulse_modulator untouched;
	uint32_t random = UINT32_C(0xc08de739);

	memset(&left, 0xa5, sizeof(left));
	untouched = left;
	invalid_profile.symbol_rate = V34_SYMBOL_RATE_COUNT;
	invalid_preemphasis.preemphasis_filter_index =
		V34_PREEMPHASIS_MAX_FILTER_INDEX + 1u;
	invalid_channel.sample_rate--;
	CHECK(v34_pulse_modulator_init(
		      NULL, &profile, &channel, 2.0) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_init(
		      &left, &invalid_profile, &channel, 2.0) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_init(
		      &left, &invalid_preemphasis, &channel, 2.0) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_init(
		      &left, &profile, &invalid_channel, 2.0) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_init(
		      &left, &profile, &channel, NAN) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      v34_pulse_modulator_init(
		      &left, &profile, &channel, 64.0) ==
		      V34_PULSE_MODULATOR_INVALID_ARGUMENT &&
	      memcmp(&left, &untouched, sizeof(left)) == 0,
	      "invalid initialization mutated its destination");
	CHECK(v34_pulse_modulator_init(
		      &left, &profile, &channel, 2.0) ==
		      V34_PULSE_MODULATOR_OK,
	      "copy fixture did not initialize");
	right = left;
	for (unsigned int index = 0; index < 257; ++index) {
		struct qam_point point;
		const struct qam_point *supplied = NULL;
		double left_sample;
		double right_sample;
		bool left_accepted;
		bool right_accepted;

		if (v34_pulse_modulator_needs_point(&left)) {
			point = generated_point(&random);
			supplied = &point;
		}
		CHECK(v34_pulse_modulator_sample(
			      &left, supplied, &left_sample, &left_accepted) ==
			      V34_PULSE_MODULATOR_OK &&
		      v34_pulse_modulator_sample(
			      &right, supplied, &right_sample, &right_accepted) ==
			      V34_PULSE_MODULATOR_OK &&
		      left_sample == right_sample &&
		      left_accepted == right_accepted &&
		      memcmp(&left, &right, sizeof(left)) == 0,
		      "copied streams diverged at sample %u", index);
	}
	CHECK(v34_pulse_modulator_is_valid(&left) &&
	      !v34_pulse_modulator_is_valid(NULL) &&
	      !v34_pulse_modulator_needs_point(NULL),
	      "pulse owner validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all profiles", test_all_profiles },
		{ "fixed line oracle", test_fixed_line_oracle },
		{ "pre-emphasis profiles", test_preemphasis_profiles },
		{ "transactional contract", test_transactional_contract },
		{ "initialization and copy", test_initialization_and_copy },
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
