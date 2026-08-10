// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase3_profile.h>

#include "test.h"

#include <string.h>

struct expected_rates {
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
	uint32_t md_symbols_per_35_ms;
};

static struct expected_rates expected_rates(
	enum sippymodem_v34_symbol_rate rate, bool high_carrier)
{
	switch (rate) {
	case SIPPYMODEM_V34_SYMBOL_RATE_2400:
		return (struct expected_rates){
			2400, 1, high_carrier ? 1800u : 1600u, 1, 84,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_2743:
		return (struct expected_rates){
			19200, 7, high_carrier ? 12800u : 11520u, 7, 96,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_2800:
		return (struct expected_rates){
			2800, 1, high_carrier ? 5600u : 1680u,
			high_carrier ? 3u : 1u, 98,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3000:
		return (struct expected_rates){
			3000, 1, high_carrier ? 2000u : 1800u, 1, 105,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3200:
		return (struct expected_rates){
			3200, 1, high_carrier ? 1920u : 12800u,
			high_carrier ? 1u : 7u, 112,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3429:
		return (struct expected_rates){
			24000, 7, 96000, 49, 120,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_COUNT:
		break;
	}
	return (struct expected_rates){ 0 };
}

static struct sippymodem_v34_info1c info1c_fixture(void)
{
	struct sippymodem_v34_info1c info1c = {
		.common = {
			.minimum_peer_power_reduction_db = 3,
			.additional_peer_power_reduction_db = 2,
			.local_md_length_35_ms_units = 11,
			.frequency_offset_1_50_hz_units = -25,
		},
	};

	for (unsigned int index = 0;
	     index < SIPPYMODEM_V34_SYMBOL_RATE_COUNT; ++index) {
		info1c.answer_transmit[index] =
			(struct sippymodem_v34_info1_probe_result){
				.high_carrier = (index & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)index,
				.projected_data_rate_2400_bps_units =
					(uint8_t)(index + 1u),
			};
	}
	return info1c;
}

static struct sippymodem_v34_info1a info1a_fixture(void)
{
	return (struct sippymodem_v34_info1a){
		.common = {
			.minimum_peer_power_reduction_db = 5,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = 35,
		},
		.call_transmit = {
			.high_carrier = true,
			.preemphasis_filter_index = 9,
			.projected_data_rate_2400_bps_units = 13,
		},
		.answer_to_call_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_2400,
		.call_to_answer_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3200,
	};
}

static bool test_answerer_rates_and_carriers(void)
{
	for (unsigned int rate_index = 0;
	     rate_index < SIPPYMODEM_V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum sippymodem_v34_symbol_rate rate =
			(enum sippymodem_v34_symbol_rate)rate_index;

		for (unsigned int high = 0; high < 2; ++high) {
			struct sippymodem_v34_info1a local = info1a_fixture();
			struct sippymodem_v34_info1c peer = info1c_fixture();
			struct sippymodem_v34_phase3_profile profile;
			struct expected_rates expected =
				expected_rates(rate, high != 0);
			uint8_t md_units = (uint8_t)(rate_index + 1u);

			local.answer_to_call_symbol_rate = rate;
			local.common.local_md_length_35_ms_units = md_units;
			peer.answer_transmit[rate].high_carrier = high != 0;
			peer.answer_transmit[rate].preemphasis_filter_index =
				(uint8_t)(10u - rate_index);
			peer.answer_transmit[rate]
				.projected_data_rate_2400_bps_units =
				(uint8_t)(14u - rate_index);
			CHECK(sippymodem_v34_phase3_answerer_profile(
				      &profile, &local, &peer) ==
				      SIPPYMODEM_V34_PHASE3_PROFILE_OK &&
			      sippymodem_v34_phase3_profile_is_valid(&profile) &&
			      profile.local_role ==
				      SIPPYMODEM_V34_ANSWERING_STATION &&
			      profile.symbol_rate == rate &&
			      profile.symbol_rate_numerator ==
				      expected.symbol_numerator &&
			      profile.symbol_rate_denominator ==
				      expected.symbol_denominator &&
			      profile.carrier_frequency_numerator ==
				      expected.carrier_numerator &&
			      profile.carrier_frequency_denominator ==
				      expected.carrier_denominator &&
			      profile.high_carrier == (high != 0) &&
			      profile.transmit_power_reduction_db ==
				      peer.common
					      .minimum_peer_power_reduction_db &&
			      profile.preemphasis_filter_index ==
				      peer.answer_transmit[rate]
					      .preemphasis_filter_index &&
			      profile.projected_data_rate_2400_bps_units ==
				      peer.answer_transmit[rate]
					      .projected_data_rate_2400_bps_units &&
			      profile.md_length_35_ms_units == md_units &&
			      profile.md_symbol_count ==
				      expected.md_symbols_per_35_ms * md_units,
			      "answerer profile mismatch for rate %u carrier %u",
			      rate_index, high);
		}
	}
	return true;
}

static bool test_caller_field_ownership(void)
{
	struct sippymodem_v34_info1c local = info1c_fixture();
	struct sippymodem_v34_info1a peer = info1a_fixture();
	struct sippymodem_v34_phase3_profile profile;

	local.common.minimum_peer_power_reduction_db = 1;
	local.common.local_md_length_35_ms_units = 9;
	peer.common.minimum_peer_power_reduction_db = 6;
	peer.common.local_md_length_35_ms_units = 31;
	peer.answer_to_call_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3200;
	peer.call_to_answer_symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_2743;
	peer.call_transmit =
		(struct sippymodem_v34_info1_probe_result){ true, 10, 14 };
	CHECK(sippymodem_v34_phase3_caller_profile(
		      &profile, &local, &peer) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_OK &&
	      sippymodem_v34_phase3_profile_is_valid(&profile) &&
	      profile.local_role == SIPPYMODEM_V34_CALLING_STATION &&
	      profile.symbol_rate == SIPPYMODEM_V34_SYMBOL_RATE_2743 &&
	      profile.symbol_rate_numerator == 19200 &&
	      profile.symbol_rate_denominator == 7 &&
	      profile.carrier_frequency_numerator == 12800 &&
	      profile.carrier_frequency_denominator == 7 &&
	      profile.transmit_power_reduction_db == 6 &&
	      profile.preemphasis_filter_index == 10 &&
	      profile.projected_data_rate_2400_bps_units == 14 &&
	      profile.md_length_35_ms_units == 9 &&
	      profile.md_symbol_count == 9u * 96u &&
	      profile.high_carrier,
	      "caller profile did not preserve INFO1 field ownership");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_info1c local_c = info1c_fixture();
	struct sippymodem_v34_info1a local_a = info1a_fixture();
	struct sippymodem_v34_info1c peer_c = info1c_fixture();
	struct sippymodem_v34_info1a peer_a = info1a_fixture();
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_v34_phase3_profile invalid;
	unsigned char before[sizeof(profile)];

	memset(&profile, 0xa5, sizeof(profile));
	memcpy(before, &profile, sizeof(before));
	peer_a.call_transmit.projected_data_rate_2400_bps_units = 0;
	CHECK(sippymodem_v34_phase3_caller_profile(
		      &profile, &local_c, &peer_a) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_UNUSABLE_SELECTION &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "unusable caller selection mutated the profile");

	peer_c.answer_transmit[local_a.answer_to_call_symbol_rate]
		.projected_data_rate_2400_bps_units = 0;
	CHECK(sippymodem_v34_phase3_answerer_profile(
		      &profile, &local_a, &peer_c) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_UNUSABLE_SELECTION &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "unusable answerer selection mutated the profile");

	peer_a = info1a_fixture();
	local_c.common.local_md_length_35_ms_units = 128;
	CHECK(sippymodem_v34_phase3_caller_profile(
		      &profile, &local_c, &peer_a) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase3_caller_profile(
		      NULL, &local_c, &peer_a) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase3_answerer_profile(
		      &profile, NULL, &peer_c) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_INVALID_ARGUMENT &&
	      memcmp(&profile, before, sizeof(profile)) == 0,
	      "invalid profile input was accepted or mutated output");

	local_c = info1c_fixture();
	CHECK(sippymodem_v34_phase3_caller_profile(
		      &profile, &local_c, &peer_a) ==
		      SIPPYMODEM_V34_PHASE3_PROFILE_OK,
	      "valid profile fixture was rejected");
	invalid = profile;
	invalid.symbol_rate_numerator++;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid),
	      "profile accepted a mismatched symbol rate");
	invalid = profile;
	invalid.carrier_frequency_denominator++;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid),
	      "profile accepted a mismatched carrier rate");
	invalid = profile;
	invalid.md_symbol_count++;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid),
	      "profile accepted a mismatched MD length");
	invalid = profile;
	invalid.local_role = (enum sippymodem_v34_station_role)99;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid),
	      "profile accepted an invalid station role");
	invalid = profile;
	invalid.transmit_power_reduction_db = 8;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid),
	      "profile accepted an invalid power reduction");
	invalid = profile;
	invalid.projected_data_rate_2400_bps_units = 0;
	CHECK(!sippymodem_v34_phase3_profile_is_valid(&invalid) &&
	      !sippymodem_v34_phase3_profile_is_valid(NULL) &&
	      strcmp(sippymodem_v34_phase3_profile_result_name(
			     SIPPYMODEM_V34_PHASE3_PROFILE_UNUSABLE_SELECTION),
		     "unusable selection") == 0 &&
	      strcmp(sippymodem_v34_phase3_profile_result_name(
			     (enum sippymodem_v34_phase3_profile_result)99),
		     "unknown") == 0,
	      "profile validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "answerer rates and carriers",
		  test_answerer_rates_and_carriers },
		{ "caller field ownership", test_caller_field_ownership },
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
