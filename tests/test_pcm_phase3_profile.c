// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_phase3_profile.h>

#include "test.h"

#include <string.h>

struct expected_rate {
	enum v34_symbol_rate rate;
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t low_carrier_numerator;
	uint32_t low_carrier_denominator;
	uint32_t high_carrier_numerator;
	uint32_t high_carrier_denominator;
	uint32_t md_symbols_per_unit;
};

static const struct expected_rate rates[] = {
	{
		V34_SYMBOL_RATE_3000, 3000, 1,
		1800, 1, 2000, 1, 105,
	},
	{
		V34_SYMBOL_RATE_3200, 3200, 1,
		12800, 7, 1920, 1, 112,
	},
	{
		V34_SYMBOL_RATE_3429, 24000, 7,
		96000, 49, 96000, 49, 120,
	},
};

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1 = {
		.common = {
			.minimum_peer_power_reduction_db = 5,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 9,
			.frequency_offset_1_50_hz_units = -7,
		},
	};

	for (unsigned int index = 0;
	     index < V34_SYMBOL_RATE_COUNT; ++index) {
		info1.answer_transmit[index] =
			(struct v34_info1_probe_result){
				.high_carrier = false,
				.preemphasis_filter_index = (uint8_t)(index + 1u),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(index + 2u),
			};
	}
	return info1;
}

static struct v34_info1c *selection_digital_info1(
	struct pcm_info1_selection *selection)
{
	return selection->startup.protocol == PCM_PROTOCOL_V90 ?
		       &selection->digital.record.v90 :
		       &selection->digital.record.v92.v34;
}

static struct pcm_info1_selection conventional_selection(
	enum pcm_protocol protocol, enum v34_symbol_rate rate,
	bool high_carrier)
{
	struct v34_info1c digital = digital_info1();
	struct pcm_info1_selection selection = {
		.startup = {
			.protocol = protocol,
			.phase2 = PCM_PHASE2_FULL,
		},
		.digital = { .protocol = protocol },
		.analog.conventional = {
			.local_md_length_35_ms_units = (uint8_t)(rate + 3u),
			.training_ucode = 91,
			.upstream_symbol_rate = rate,
			.frequency_offset_1_50_hz_units = 13,
		},
		.upstream = PCM_UPSTREAM_V34,
	};

	digital.answer_transmit[rate].high_carrier = high_carrier;
	if (protocol == PCM_PROTOCOL_V90)
		selection.digital.record.v90 = digital;
	else
		selection.digital.record.v92 = (struct v92_info1d){
			.v34 = digital,
			.pcm_upstream_supported = true,
		};
	return selection;
}

static bool test_conventional_profile_matrix(void)
{
	for (unsigned int protocol = PCM_PROTOCOL_V90;
	     protocol <= PCM_PROTOCOL_V92; ++protocol) {
		for (size_t rate_index = 0;
		     rate_index < ARRAY_LENGTH(rates); ++rate_index) {
			for (unsigned int high = 0; high < 2; ++high) {
				const struct expected_rate *expected =
					&rates[rate_index];
				struct pcm_info1_selection selection =
					conventional_selection(
						(enum pcm_protocol)protocol,
						expected->rate, high != 0);
				struct v34_info1c *digital =
					selection_digital_info1(&selection);
				struct v34_phase3_profile profile;
				uint32_t carrier_numerator = high != 0 ?
					expected->high_carrier_numerator :
					expected->low_carrier_numerator;
				uint32_t carrier_denominator = high != 0 ?
					expected->high_carrier_denominator :
					expected->low_carrier_denominator;

				/* V.92 assigns this otherwise-irrelevant carrier bit
				 * to PCM-upstream capability. */
				if (protocol == PCM_PROTOCOL_V92 &&
				    expected->rate == V34_SYMBOL_RATE_3429) {
					digital->answer_transmit[expected->rate]
						.high_carrier = false;
					carrier_numerator =
						expected->low_carrier_numerator;
					carrier_denominator =
						expected->low_carrier_denominator;
				}
				CHECK(pcm_info1_selection_is_valid(&selection) &&
				      pcm_phase3_conventional_profile(
					      &selection, &profile) ==
					      V34_PHASE3_PROFILE_OK &&
				      v34_phase3_profile_is_valid(&profile) &&
				      profile.local_role == V34_ANSWERING_STATION &&
				      profile.symbol_rate == expected->rate &&
				      profile.symbol_rate_numerator ==
					      expected->symbol_numerator &&
				      profile.symbol_rate_denominator ==
					      expected->symbol_denominator &&
				      profile.carrier_frequency_numerator ==
					      carrier_numerator &&
				      profile.carrier_frequency_denominator ==
					      carrier_denominator &&
				      profile.high_carrier ==
					      digital->answer_transmit[expected->rate]
						      .high_carrier &&
				      profile.preemphasis_filter_index ==
					      digital->answer_transmit[expected->rate]
						      .preemphasis_filter_index &&
				      profile.projected_data_rate_2400_bps_units ==
					      digital->answer_transmit[expected->rate]
						      .projected_data_rate_2400_bps_units &&
				      profile.transmit_power_reduction_db ==
					      digital->common
						      .minimum_peer_power_reduction_db &&
				      profile.md_length_35_ms_units ==
					      selection.analog.conventional
						      .local_md_length_35_ms_units &&
				      profile.md_symbol_count ==
					      expected->md_symbols_per_unit *
						      profile.md_length_35_ms_units,
				      "protocol %u rate %u carrier %u profile changed",
				      protocol, expected->rate, high);
			}
		}
	}
	return true;
}

static bool test_rejection_is_transactional(void)
{
	struct pcm_info1_selection selection = conventional_selection(
		PCM_PROTOCOL_V90, V34_SYMBOL_RATE_3200, true);
	struct v34_phase3_profile profile;
	uint8_t before[sizeof(profile)];

	memset(&profile, 0xa5, sizeof(profile));
	memcpy(before, &profile, sizeof(before));
	selection_digital_info1(&selection)
		->answer_transmit[V34_SYMBOL_RATE_3200]
		.projected_data_rate_2400_bps_units = 0;
	CHECK(pcm_info1_selection_is_valid(&selection) &&
	      pcm_phase3_conventional_profile(&selection, &profile) ==
		      V34_PHASE3_PROFILE_UNUSABLE_SELECTION &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "unusable conventional selection changed output");
	selection = conventional_selection(
		PCM_PROTOCOL_V92, V34_SYMBOL_RATE_3200, false);
	selection.upstream = PCM_UPSTREAM_PCM;
	selection.analog.pcm = (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 256,
		.maximum_section_coefficients = 128,
		.local_md_length_276_symbol_units = 1,
		.training_ucode = 91,
	};
	CHECK(pcm_info1_selection_is_valid(&selection) &&
	      pcm_phase3_conventional_profile(&selection, &profile) ==
		      V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      pcm_phase3_conventional_profile(NULL, &profile) ==
		      V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      pcm_phase3_conventional_profile(&selection, NULL) ==
		      V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "PCM or invalid selection entered the conventional path");
	selection = (struct pcm_info1_selection){
		.startup = { PCM_PROTOCOL_V92, PCM_PHASE2_SHORT },
		.short_high_carrier = true,
		.analog.conventional = {
			.training_ucode = 91,
			.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
			.frequency_offset_1_50_hz_units = -512,
		},
		.upstream = PCM_UPSTREAM_V34,
	};
	CHECK(pcm_info1_selection_is_valid(&selection) &&
	      pcm_phase3_conventional_profile(&selection, &profile) ==
		      V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "short decision consumed an absent INFO1d as a full training profile");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "conventional profile matrix", test_conventional_profile_matrix },
		{ "transactional rejection", test_rejection_is_transactional },
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
