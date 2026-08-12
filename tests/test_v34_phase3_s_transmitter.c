// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_s_transmitter.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

struct exact_profile_rates {
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
};

struct marker_statistics {
	long double sum_squares;
	uint32_t samples;
	unsigned int stages_seen;
	int peak;
};

static struct exact_profile_rates profile_rates(
	enum v34_symbol_rate rate, bool high_carrier)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		return (struct exact_profile_rates){
			2400, 1, high_carrier ? 1800u : 1600u, 1,
		};
	case V34_SYMBOL_RATE_2743:
		return (struct exact_profile_rates){
			19200, 7, high_carrier ? 12800u : 11520u, 7,
		};
	case V34_SYMBOL_RATE_2800:
		return (struct exact_profile_rates){
			2800, 1, high_carrier ? 5600u : 1680u,
			high_carrier ? 3u : 1u,
		};
	case V34_SYMBOL_RATE_3000:
		return (struct exact_profile_rates){
			3000, 1, high_carrier ? 2000u : 1800u, 1,
		};
	case V34_SYMBOL_RATE_3200:
		return (struct exact_profile_rates){
			3200, 1, high_carrier ? 1920u : 12800u,
			high_carrier ? 1u : 7u,
		};
	case V34_SYMBOL_RATE_3429:
		return (struct exact_profile_rates){ 24000, 7, 96000, 49 };
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return (struct exact_profile_rates){ 0 };
}

static struct v34_phase3_profile profile_fixture(
	enum v34_symbol_rate rate, bool high_carrier,
	uint8_t power_reduction_db, uint8_t filter_index)
{
	struct exact_profile_rates rates = profile_rates(rate, high_carrier);

	return (struct v34_phase3_profile){
		.symbol_rate_numerator = rates.symbol_numerator,
		.symbol_rate_denominator = rates.symbol_denominator,
		.carrier_frequency_numerator = rates.carrier_numerator,
		.carrier_frequency_denominator = rates.carrier_denominator,
		.symbol_rate = rate,
		.local_role = V34_CALLING_STATION,
		.transmit_power_reduction_db = power_reduction_db,
		.preemphasis_filter_index = filter_index,
		.projected_data_rate_2400_bps_units = 1,
		.high_carrier = high_carrier,
	};
}

static uint32_t expected_samples(
	const struct v34_phase3_profile *profile,
	uint32_t sample_rate)
{
	uint64_t last_symbol = V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_PHASE3_S_TAIL_SYMBOLS - 1u;
	uint64_t numerator = last_symbol * sample_rate *
		profile->symbol_rate_denominator;
	uint64_t last_acceptance =
		(numerator + profile->symbol_rate_numerator - 1u) /
		profile->symbol_rate_numerator;

	return (uint32_t)(last_acceptance + 1u +
		V34_PHASE3_S_FILTER_TAIL_SAMPLES);
}

static bool render_marker(
	struct v34_phase3_s_transmitter *transmitter,
	struct marker_statistics *statistics)
{
	uint32_t limit = expected_samples(
		&transmitter->line.profile,
		transmitter->line.carrier.channel.sample_rate);

	memset(statistics, 0, sizeof(*statistics));
	while (!v34_phase3_s_transmitter_is_complete(transmitter)) {
		int16_t sample;
		int magnitude;

		statistics->stages_seen |= 1u << transmitter->stage;
		CHECK(statistics->samples < limit &&
		      v34_phase3_s_transmitter_sample(
			      transmitter, &sample) ==
			      V34_PHASE3_S_TRANSMITTER_OK,
		      "marker stopped at sample %u", statistics->samples);
		statistics->sum_squares +=
			(long double)sample * (long double)sample;
		magnitude = sample < 0 ? -(int)sample : sample;
		if (magnitude > statistics->peak)
			statistics->peak = magnitude;
		statistics->samples++;
	}
	statistics->stages_seen |= 1u << transmitter->stage;
	return true;
}

static bool test_all_line_profiles(void)
{
	const uint32_t sample_rate = 8000;
	unsigned int filter_index = 0;

	for (unsigned int rate_index = 0;
	     rate_index < V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum v34_symbol_rate rate =
			(enum v34_symbol_rate)rate_index;
		unsigned int carrier_count =
			rate == V34_SYMBOL_RATE_3429 ? 1u : 2u;

		for (unsigned int high = 0; high < carrier_count; ++high) {
			struct v34_phase3_profile profile =
				profile_fixture(
					rate, high != 0, 0,
					(uint8_t)filter_index);
			struct v34_phase3_s_transmitter transmitter;
			struct marker_statistics statistics;
			struct qam_modulator drained_carrier;
			struct v34_preemphasis drained_filter;
			const struct qam_point zero = { 0 };
			double carrier_sample;
			double filtered_sample;
			bool accepted;
			long double measured_rms;
			uint32_t count = expected_samples(&profile, sample_rate);

			CHECK(v34_phase3_profile_is_valid(&profile) &&
			      v34_phase3_s_transmitter_init(
				      &transmitter, &profile, sample_rate, 5000) ==
				      V34_PHASE3_S_TRANSMITTER_OK,
			      "profile %u/%u marker did not initialize",
			      rate_index, high);
			CHECK(transmitter.line.carrier.channel
				      .symbol_rate_numerator ==
				      profile.symbol_rate_numerator &&
			      transmitter.line.carrier.channel
				      .symbol_rate_denominator ==
				      profile.symbol_rate_denominator &&
			      transmitter.line.carrier.channel
				      .carrier_frequency_numerator ==
				      profile.carrier_frequency_numerator &&
			      transmitter.line.carrier.channel
				      .carrier_frequency_denominator ==
				      profile.carrier_frequency_denominator &&
			      transmitter.line.preemphasis.filter_index ==
				      filter_index,
			      "profile %u/%u settings were not connected",
			      rate_index, high);
			CHECK(render_marker(&transmitter, &statistics),
			      "profile %u/%u marker did not render",
			      rate_index, high);
			drained_carrier = transmitter.line.carrier;
			drained_filter = transmitter.line.preemphasis;
			measured_rms = sqrtl(
				statistics.sum_squares / statistics.samples);
			CHECK(statistics.samples == count &&
			      transmitter.line.generated_samples == count &&
			      transmitter.sequence.emitted_symbols ==
				      V34_PHASE3_S_SEQUENCE_SYMBOLS &&
			      transmitter.tail_symbols ==
				      V34_PHASE3_S_TAIL_SYMBOLS &&
			      transmitter.filter_tail_samples ==
				      V34_PHASE3_S_FILTER_TAIL_SAMPLES &&
			      statistics.stages_seen ==
				      (1u << (V34_PHASE3_S_TRANSMIT_COMPLETE +
					      1u)) -
					      1u &&
			      statistics.peak > 0 && statistics.peak <= INT16_MAX &&
			      fabsl(measured_rms - transmitter.line.target_rms) <
				      1.0L,
			      "profile %u/%u cadence or power changed",
			      rate_index, high);
			CHECK(qam_modulator_sample_f64(
				      &drained_carrier, &zero, &carrier_sample,
				      &accepted) == QAM_OK &&
			      v34_preemphasis_sample(
				      &drained_filter, carrier_sample,
				      &filtered_sample) ==
				      V34_PREEMPHASIS_OK &&
			      carrier_sample == 0.0 && filtered_sample == 0.0,
			      "profile %u/%u left an undrained line sample",
			      rate_index, high);
			filter_index++;
		}
	}
	CHECK(filter_index == 11,
	      "the legal line-profile coverage changed");
	return true;
}

static bool test_negotiated_power(void)
{
	struct v34_phase3_profile full_profile = profile_fixture(
		V34_SYMBOL_RATE_3000, true, 0, 8);
	struct v34_phase3_profile reduced_profile = full_profile;
	struct v34_phase3_s_transmitter full;
	struct v34_phase3_s_transmitter reduced;
	struct marker_statistics full_statistics;
	struct marker_statistics reduced_statistics;
	long double full_rms;
	long double reduced_rms;
	double expected_ratio = pow(10.0, -6.0 / 20.0);

	reduced_profile.transmit_power_reduction_db = 6;
	CHECK(v34_phase3_s_transmitter_init(
		      &full, &full_profile, 8000, 6000) ==
		      V34_PHASE3_S_TRANSMITTER_OK &&
	      v34_phase3_s_transmitter_init(
		      &reduced, &reduced_profile, 8000, 6000) ==
		      V34_PHASE3_S_TRANSMITTER_OK &&
	      full.line.raw_rms == reduced.line.raw_rms &&
	      fabs(reduced.line.output_gain / full.line.output_gain -
		   expected_ratio) <
		      1e-12,
	      "negotiated power did not scale the common raw waveform");
	CHECK(render_marker(&full, &full_statistics) &&
	      render_marker(&reduced, &reduced_statistics),
	      "power comparison marker did not render");
	full_rms = sqrtl(full_statistics.sum_squares /
			 full_statistics.samples);
	reduced_rms = sqrtl(reduced_statistics.sum_squares /
			    reduced_statistics.samples);
	CHECK(full_statistics.samples == reduced_statistics.samples &&
	      fabsl(full_rms - full.line.target_rms) < 1.0L &&
	      fabsl(reduced_rms - reduced.line.target_rms) < 1.0L &&
	      fabsl(reduced_rms / full_rms - expected_ratio) < 0.001L,
	      "rendered marker did not honor the negotiated power reduction");
	return true;
}

static bool test_shared_line_equivalence(void)
{
	struct v34_phase3_profile profile = profile_fixture(
		V34_SYMBOL_RATE_2743, true, 3, 6);
	struct v34_phase3_s_transmitter transmitter;
	struct v34_training_line line;
	struct v34_phase3_s_sequence sequence;
	uint32_t samples = 0;
	uint32_t limit = expected_samples(&profile, 8000);

	CHECK(v34_phase3_s_transmitter_init(
		      &transmitter, &profile, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_OK &&
	      v34_training_line_init(&line, &profile, 8000, 5000) ==
		      V34_TRAINING_LINE_OK &&
	      v34_phase3_s_sequence_init(&sequence) ==
		      V34_PHASE3_S_SEQUENCE_OK,
	      "shared-line comparison did not initialize");
	while (!v34_phase3_s_transmitter_is_complete(&transmitter)) {
		struct qam_point point = { 0 };
		const struct qam_point *point_pointer = NULL;
		enum v34_phase3_s_section section;
		int16_t expected;
		int16_t actual;
		bool accepted;

		CHECK(samples < limit,
		      "shared-line comparison exceeded its sample bound");
		if (v34_training_line_needs_point(&line)) {
			point_pointer = &point;
			if (!v34_phase3_s_sequence_is_complete(&sequence))
				CHECK(v34_phase3_s_sequence_next(
					      &sequence, &point, &section) ==
					      V34_PHASE3_S_SEQUENCE_OK,
				      "comparison sequence stopped early");
		}
		CHECK(v34_training_line_sample(
			      &line, point_pointer, &expected, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == (point_pointer != NULL) &&
		      v34_phase3_s_transmitter_sample(
			      &transmitter, &actual) ==
			      V34_PHASE3_S_TRANSMITTER_OK &&
		      actual == expected &&
		      transmitter.line.generated_samples ==
			      line.generated_samples &&
		      transmitter.line.carrier.carrier_phase ==
			      line.carrier.carrier_phase &&
		      transmitter.line.carrier.symbol_phase_units ==
			      line.carrier.symbol_phase_units &&
		      transmitter.line.preemphasis.next_history ==
			      line.preemphasis.next_history,
		      "finite marker diverged from the shared line at sample %u",
		      samples);
		samples++;
	}
	CHECK(samples == limit &&
	      transmitter.sequence.emitted_symbols == sequence.emitted_symbols,
	      "finite marker did not preserve exact line ownership");
	return true;
}

static bool test_copy_and_completion(void)
{
	struct v34_phase3_profile profile = profile_fixture(
		V34_SYMBOL_RATE_2743, true, 2, 7);
	struct v34_phase3_s_transmitter first;
	struct v34_phase3_s_transmitter second;

	CHECK(v34_phase3_s_transmitter_init(
		      &first, &profile, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_OK,
	      "copy fixture did not initialize");
	for (unsigned int index = 0; index < 137; ++index) {
		int16_t ignored;

		CHECK(v34_phase3_s_transmitter_sample(
			      &first, &ignored) ==
			      V34_PHASE3_S_TRANSMITTER_OK,
		      "copy fixture prefix stopped at %u", index);
	}
	second = first;
	while (!v34_phase3_s_transmitter_is_complete(&first)) {
		int16_t first_sample;
		int16_t second_sample;

		CHECK(v34_phase3_s_transmitter_sample(
			      &first, &first_sample) ==
			      V34_PHASE3_S_TRANSMITTER_OK &&
		      v34_phase3_s_transmitter_sample(
			      &second, &second_sample) ==
			      V34_PHASE3_S_TRANSMITTER_OK &&
		      first_sample == second_sample &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied marker diverged at sample %u",
		      (unsigned int)first.line.generated_samples);
	}
	{
		struct v34_phase3_s_transmitter before = first;
		int16_t sample = 1234;

		CHECK(v34_phase3_s_transmitter_sample(
			      &first, &sample) ==
			      V34_PHASE3_S_TRANSMITTER_COMPLETE &&
		      memcmp(&first, &before, sizeof(first)) == 0 &&
		      sample == 1234,
		      "completed marker mutated state or output");
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile profile = profile_fixture(
		V34_SYMBOL_RATE_3429, false, 0, 10);
	struct v34_phase3_profile invalid_profile = profile;
	struct v34_phase3_s_transmitter transmitter;
	struct v34_phase3_s_transmitter before;
	int16_t sample = 1234;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	invalid_profile.preemphasis_filter_index = 11;
	CHECK(v34_phase3_s_transmitter_init(
		      NULL, &profile, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_s_transmitter_init(
		      &transmitter, NULL, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_s_transmitter_init(
		      &transmitter, &invalid_profile, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_s_transmitter_init(
		      &transmitter, &profile, 7000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_s_transmitter_init(
		      &transmitter, &profile, 8000, 0) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "invalid marker initialization mutated its destination");
	CHECK(v34_phase3_s_transmitter_init(
		      &transmitter, &profile, 8000, INT16_MAX) ==
		      V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "clipping marker was accepted or mutated its destination");
	CHECK(v34_phase3_s_transmitter_init(
		      &transmitter, &profile, 8000, 5000) ==
		      V34_PHASE3_S_TRANSMITTER_OK &&
	      v34_phase3_s_transmitter_is_valid(&transmitter) &&
	      !v34_phase3_s_transmitter_is_valid(NULL) &&
	      !v34_phase3_s_transmitter_is_complete(NULL),
	      "valid marker state was not recognized");
	before = transmitter;
	CHECK(v34_phase3_s_transmitter_sample(
		      &transmitter, NULL) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "null marker output mutated state");
	transmitter.line.generated_samples = UINT64_MAX;
	before = transmitter;
	CHECK(!v34_phase3_s_transmitter_is_valid(&transmitter) &&
	      v34_phase3_s_transmitter_sample(
		      &transmitter, &sample) ==
		      V34_PHASE3_S_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      sample == 1234,
	      "invalid marker state mutated state or output");
	CHECK(strcmp(v34_phase3_s_transmitter_result_name(
			     V34_PHASE3_S_TRANSMITTER_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v34_phase3_s_transmitter_result_name(
			     (enum v34_phase3_s_transmitter_result)99),
		     "unknown") == 0,
	      "marker result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "all line profiles", test_all_line_profiles },
		{ "negotiated power", test_negotiated_power },
		{ "shared line equivalence", test_shared_line_equivalence },
		{ "copy and completion", test_copy_and_completion },
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
