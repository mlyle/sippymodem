// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_phase3_entry.h>
#include <v90_phase3_analog_station.h>
#include <v90_phase3_digital_station.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 6000,
};

static const enum v34_symbol_rate symbol_rates[] = {
	V34_SYMBOL_RATE_3000,
	V34_SYMBOL_RATE_3200,
	V34_SYMBOL_RATE_3429,
};

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1 = {
		.common = {
			.minimum_peer_power_reduction_db = 3,
			.additional_peer_power_reduction_db = 2,
			.local_md_length_35_ms_units = 9,
			.frequency_offset_1_50_hz_units = -7,
		},
	};

	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		info1.answer_transmit[rate] =
			(struct v34_info1_probe_result){
				.high_carrier = (rate & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(rate + 1u),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(rate + 2u),
			};
	}
	return info1;
}

static struct pcm_info1_selection selection(
	enum v34_symbol_rate symbol_rate, bool high_carrier)
{
	struct v34_info1c digital = digital_info1();
	struct pcm_info1_selection selected = {
		.startup = {
			.protocol = PCM_PROTOCOL_V90,
			.phase2 = PCM_PHASE2_FULL,
		},
		.digital = { .protocol = PCM_PROTOCOL_V90 },
		.analog.conventional = {
			.local_md_length_35_ms_units = 7,
			.training_ucode = 91,
			.upstream_symbol_rate = symbol_rate,
			.frequency_offset_1_50_hz_units = 13,
		},
		.upstream = PCM_UPSTREAM_V34,
	};

	digital.answer_transmit[symbol_rate].high_carrier = high_carrier;
	selected.digital.record.v90 = digital;
	return selected;
}

static struct v90_info0d digital_info0(enum g711_law law)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_3429_supported = true,
			.maximum_symbol_rate_difference_steps = 2,
			.acknowledges_peer_info0 = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = law == G711_LAW_PCMA ?
				   V90_PCM_A_LAW : V90_PCM_MU_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static struct v90_phase3_analog_rx_configuration receive_configuration(void)
{
	return (struct v90_phase3_analog_rx_configuration){
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static bool profiles_equal(
	const struct v34_phase3_profile *left,
	const struct v34_phase3_profile *right)
{
	return left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->carrier_frequency_numerator ==
		       right->carrier_frequency_numerator &&
	       left->carrier_frequency_denominator ==
		       right->carrier_frequency_denominator &&
	       left->md_symbol_count == right->md_symbol_count &&
	       left->symbol_rate == right->symbol_rate &&
	       left->local_role == right->local_role &&
	       left->transmit_power_reduction_db ==
		       right->transmit_power_reduction_db &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units &&
	       left->md_length_35_ms_units == right->md_length_35_ms_units &&
	       left->high_carrier == right->high_carrier;
}

static bool test_generated_entries(void)
{
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();
	struct v90_jd_parameters jd = jd_parameters();

	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		enum g711_law law = (enum g711_law)law_index;
		struct v90_info0d info0 = digital_info0(law);

		for (unsigned int role_index = V90_ANALOG_MODEM;
		     role_index <= V90_DIGITAL_MODEM; ++role_index) {
			enum v90_modem_role role =
				(enum v90_modem_role)role_index;

			for (size_t rate_index = 0;
			     rate_index < ARRAY_LENGTH(symbol_rates); ++rate_index) {
				for (unsigned int high = 0; high < 2; ++high) {
					struct pcm_info1_selection selected = selection(
						symbol_rates[rate_index], high != 0);
					struct v34_phase3_profile expected;
					struct pcm_phase3_entry entry;
					struct v90_phase3_analog_station analog;
					struct v90_phase3_analog_station saved_analog;
					struct v90_phase3_digital_station digital;
					struct v90_phase3_digital_station saved_digital;

					CHECK(pcm_phase3_conventional_profile(
						      &selected, &expected) ==
						      V34_PHASE3_PROFILE_OK,
					      "entry fixture profile failed");
					expected.md_length_35_ms_units = 0;
					expected.md_symbol_count = 0;
					CHECK(pcm_phase3_entry_init(
						      &entry, role, law, &info0,
						      &selected) == PCM_PHASE3_ENTRY_OK &&
					      pcm_phase3_entry_is_valid(&entry) &&
					      profiles_equal(
						      &entry.analog_profile, &expected) &&
					      entry.law == law &&
					      entry.local_role == role &&
					      entry.training_ucode ==
						      selected.analog.conventional
							      .training_ucode,
					      "law %u role %u rate %u carrier %u entry changed",
					      law_index, role_index,
					      symbol_rates[rate_index], high);

					memset(&analog, 0xa5, sizeof(analog));
					saved_analog = analog;
					memset(&digital, 0x5a, sizeof(digital));
					saved_digital = digital;
					if (role == V90_ANALOG_MODEM) {
						CHECK(pcm_phase3_entry_start_analog(
							      &entry, NOMINAL_AMPLITUDE,
							      &analog) == PCM_PHASE3_ENTRY_OK &&
						      v90_phase3_analog_station_is_valid(
							      &analog) &&
						      pcm_phase3_entry_start_digital(
							      &entry, &configuration, &jd,
							      &digital) ==
							      PCM_PHASE3_ENTRY_ROLE_MISMATCH &&
						      memcmp(&digital, &saved_digital,
							     sizeof(digital)) == 0,
						      "analogue entry selected the wrong owner");
					} else {
						CHECK(pcm_phase3_entry_start_digital(
							      &entry, &configuration, &jd,
							      &digital) == PCM_PHASE3_ENTRY_OK &&
						      v90_phase3_digital_station_is_valid(
							      &digital) &&
						      pcm_phase3_entry_start_analog(
							      &entry, NOMINAL_AMPLITUDE,
							      &analog) ==
							      PCM_PHASE3_ENTRY_ROLE_MISMATCH &&
						      memcmp(&analog, &saved_analog,
							     sizeof(analog)) == 0,
						      "digital entry selected the wrong owner");
					}
				}
			}
		}
	}
	return true;
}

static bool test_selection_rejection(void)
{
	struct pcm_info1_selection selected = selection(
		V34_SYMBOL_RATE_3200, true);
	struct v90_info0d info0 = digital_info0(G711_LAW_PCMU);
	struct pcm_phase3_entry entry;
	struct pcm_phase3_entry saved;
	struct v34_info1c common;

	memset(&entry, 0xa5, sizeof(entry));
	saved = entry;
	CHECK(pcm_phase3_entry_init(
		      NULL, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_init(
		      &entry, (enum v90_modem_role)99, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, (enum g711_law)99,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      NULL, &selected) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, NULL) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "invalid entry arguments changed the destination");

	CHECK(pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMA,
		      &info0, &selected) == PCM_PHASE3_ENTRY_MEDIA_MISMATCH &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "media-law mismatch changed the entry");
	common = selected.digital.record.v90;
	/* V.92 assigns the otherwise unused 3429-symbol/s carrier bit to its
	 * PCM-upstream capability. */
	common.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
	selected.startup.protocol = PCM_PROTOCOL_V92;
	selected.digital.protocol = PCM_PROTOCOL_V92;
	selected.digital.record.v92 = (struct v92_info1d){
		.v34 = common,
		.pcm_upstream_supported = true,
	};
	CHECK(pcm_info1_selection_is_valid(&selected) &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_SELECTION &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "V.92 selection entered the V.90 owner");

	selected = selection(V34_SYMBOL_RATE_3200, true);
	selected.digital.record.v90
		.answer_transmit[V34_SYMBOL_RATE_3200]
		.projected_data_rate_2400_bps_units = 0;
	CHECK(pcm_info1_selection_is_valid(&selected) &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) ==
		      PCM_PHASE3_ENTRY_UNUSABLE_SELECTION &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "unusable line selection changed the entry");
	info0.pcm_law = (enum v90_pcm_law)99;
	CHECK(pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      !pcm_phase3_entry_is_valid(NULL) &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "invalid INFO0 changed the entry");
	return true;
}

static bool test_station_validation(void)
{
	struct pcm_info1_selection selected = selection(
		V34_SYMBOL_RATE_3000, false);
	struct v90_info0d info0 = digital_info0(G711_LAW_PCMU);
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();
	struct v90_jd_parameters jd = jd_parameters();
	struct pcm_phase3_entry analog_entry;
	struct pcm_phase3_entry digital_entry;
	struct v90_phase3_analog_station analog;
	struct v90_phase3_analog_station saved_analog;
	struct v90_phase3_digital_station digital;
	struct v90_phase3_digital_station saved_digital;

	CHECK(pcm_phase3_entry_init(
		      &analog_entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_OK &&
	      pcm_phase3_entry_init(
		      &digital_entry, V90_DIGITAL_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_OK,
	      "station validation entries did not initialize");
	memset(&analog, 0xa5, sizeof(analog));
	saved_analog = analog;
	memset(&digital, 0x5a, sizeof(digital));
	saved_digital = digital;
	CHECK(pcm_phase3_entry_start_analog(
		      NULL, NOMINAL_AMPLITUDE, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_analog(
		      &analog_entry, 0, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_analog(
		      &analog_entry, NOMINAL_AMPLITUDE, NULL) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_analog(
		      &analog_entry, INT16_MAX, &analog) ==
		      PCM_PHASE3_ENTRY_CLIPPING_RISK &&
	      memcmp(&analog, &saved_analog, sizeof(analog)) == 0,
	      "invalid or clipping analogue start changed state");
	CHECK(pcm_phase3_entry_start_digital(
		      NULL, &configuration, &jd, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_digital(
		      &digital_entry, NULL, &jd, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_digital(
		      &digital_entry, &configuration, NULL, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_digital(
		      &digital_entry, &configuration, &jd, NULL) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0,
	      "invalid digital start changed state");
	analog_entry.analog_profile.md_length_35_ms_units = 1;
	analog_entry.analog_profile.md_symbol_count = 105;
	CHECK(v34_phase3_profile_is_valid(&analog_entry.analog_profile) &&
	      !pcm_phase3_entry_is_valid(&analog_entry) &&
	      pcm_phase3_entry_start_analog(
		      &analog_entry, NOMINAL_AMPLITUDE, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT,
	      "entry accepted a conventional MD interval");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"invalid argument",
		"invalid selection",
		"invalid state",
		"unusable selection",
		"role mismatch",
		"media mismatch",
		"out of range",
		"clipping risk",
		"internal error",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index) {
		CHECK(strcmp(
			      pcm_phase3_entry_result_name(
				      (enum pcm_phase3_entry_result)index),
			      expected[index]) == 0,
		      "entry result name %zu changed", index);
	}
	CHECK(strcmp(pcm_phase3_entry_result_name(
		     (enum pcm_phase3_entry_result)99),
	     "unknown") == 0,
	      "unknown entry result name changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated role and media entries", test_generated_entries },
		{ "selection rejection", test_selection_rejection },
		{ "station validation", test_station_validation },
		{ "result names", test_result_names },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
