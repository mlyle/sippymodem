// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_phase3_entry.h>
#include <v90_phase3_analog_station.h>
#include <v90_phase3_digital_station.h>
#include <v92_phase3_analog_station.h>
#include <v92_phase3_digital_station.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 6000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
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

static struct pcm_info1_selection conventional_selection(
	enum pcm_protocol protocol, enum v34_symbol_rate symbol_rate,
	bool high_carrier)
{
	struct v34_info1c digital = digital_info1();
	struct pcm_info1_selection selected = {
		.startup = {
			.protocol = protocol,
			.phase2 = PCM_PHASE2_FULL,
		},
		.digital = { .protocol = protocol },
		.analog.conventional = {
			.local_md_length_35_ms_units = 0,
			.training_ucode = 91,
			.upstream_symbol_rate = symbol_rate,
			.frequency_offset_1_50_hz_units = 13,
		},
		.upstream = PCM_UPSTREAM_V34,
	};

	digital.answer_transmit[symbol_rate].high_carrier = high_carrier;
	if (protocol == PCM_PROTOCOL_V90) {
		selected.digital.record.v90 = digital;
	} else {
		/* V.92 owns the otherwise-unused 3429-symbol/s carrier bit. */
		digital.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
		selected.digital.record.v92 = (struct v92_info1d){
			.v34 = digital,
			.pcm_upstream_supported = true,
		};
	}
	return selected;
}

static struct pcm_info1_selection pcm_upstream_selection(void)
{
	struct v34_info1c digital = digital_info1();

	digital.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
	return (struct pcm_info1_selection){
		.startup = {
			.protocol = PCM_PROTOCOL_V92,
			.phase2 = PCM_PHASE2_FULL,
		},
		.digital = {
			.record.v92 = {
				.v34 = digital,
				.pcm_upstream_supported = true,
			},
			.protocol = PCM_PROTOCOL_V92,
		},
		.analog.pcm = {
			.filter_sections = V92_PCM_FILTER_Z1_P1_Z2,
			.maximum_total_coefficients = 320,
			.maximum_section_coefficients = 192,
			.local_md_length_276_symbol_units = 7,
			.training_ucode = 91,
		},
		.upstream = PCM_UPSTREAM_PCM,
	};
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

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters v92_cpt_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 24u);
	add_ucode(&parameters.transmit_constellations[0], 80u);
	return parameters;
}

static struct v92_phase3_analog_station_configuration
v92_analog_configuration(
	enum g711_law law, const struct pcm_info1_selection *selection)
{
	return (struct v92_phase3_analog_station_configuration){
		.local_info1a = selection->analog.pcm,
		.cpt = v92_cpt_parameters(law),
		.upstream_rate_mask = 1u,
		.initial_trn1u_symbols =
			V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS,
		.law = law,
		.upstream_lu_ucode = 91u,
	};
}

static struct v92_jd v92_jd_parameters(void)
{
	return (struct v92_jd){
		.data_rate_capability_mask = UINT32_C(0x002a1555),
		.maximum_spectral_shaping_lookahead = 3,
	};
}

static struct v92_jp v92_jp_parameters(void)
{
	return (struct v92_jp){
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
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
					struct pcm_info1_selection selected =
						conventional_selection(
							PCM_PROTOCOL_V90,
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
					CHECK(pcm_phase3_entry_init(
						      &entry, role, law, &info0,
						      &selected) == PCM_PHASE3_ENTRY_OK &&
					      pcm_phase3_entry_is_valid(&entry) &&
					      profiles_equal(
						      &entry.conventional_profile,
						      &expected) &&
					      entry.law == law && entry.local_role == role &&
					      entry.selection.startup.protocol ==
						      PCM_PROTOCOL_V90 &&
					      entry.selection.startup.phase2 ==
						      PCM_PHASE2_FULL &&
					      entry.selection.upstream ==
						      PCM_UPSTREAM_V34,
					      "law %u role %u rate %u carrier %u entry changed",
					      law_index, role_index,
					      (unsigned int)symbol_rates[rate_index], high);

					memset(&analog, 0xa5, sizeof(analog));
					saved_analog = analog;
					memset(&digital, 0x5a, sizeof(digital));
					saved_digital = digital;
					if (role == V90_ANALOG_MODEM) {
						CHECK(pcm_phase3_entry_start_v90_analog(
							      &entry, NOMINAL_AMPLITUDE,
							      V90_DIL_PROFILE_NONE,
							      &analog) == PCM_PHASE3_ENTRY_OK &&
						      v90_phase3_analog_station_is_valid(
							      &analog) &&
						      pcm_phase3_entry_start_v90_digital(
							      &entry, &configuration,
							      TEST_INITIAL_RECEIVE_GAIN, &jd,
							      &digital) ==
							      PCM_PHASE3_ENTRY_ROLE_MISMATCH &&
						      memcmp(&digital, &saved_digital,
							     sizeof(digital)) == 0,
						      "analogue entry selected the wrong owner");
					} else {
						CHECK(pcm_phase3_entry_start_v90_digital(
							      &entry, &configuration,
							      TEST_INITIAL_RECEIVE_GAIN, &jd,
							      &digital) == PCM_PHASE3_ENTRY_OK &&
						      v90_phase3_digital_station_is_valid(
							      &digital) &&
						      pcm_phase3_entry_start_v90_analog(
							      &entry, NOMINAL_AMPLITUDE,
							      V90_DIL_PROFILE_NONE,
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
	struct pcm_info1_selection selected = conventional_selection(
		PCM_PROTOCOL_V90, V34_SYMBOL_RATE_3200, true);
	struct v90_info0d info0 = digital_info0(G711_LAW_PCMU);
	struct pcm_phase3_entry entry;
	struct pcm_phase3_entry saved;

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
	selected = conventional_selection(
		PCM_PROTOCOL_V92, V34_SYMBOL_RATE_3200, true);
	selected.startup.phase2 = PCM_PHASE2_SHORT;
	CHECK(pcm_info1_selection_is_valid(&selected) &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_SELECTION &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "unimplemented short Phase 2 entered Phase 3");

	selected = conventional_selection(
		PCM_PROTOCOL_V90, V34_SYMBOL_RATE_3200, true);
	selected.analog.conventional.local_md_length_35_ms_units = 1;
	CHECK(pcm_info1_selection_is_valid(&selected) &&
	      pcm_phase3_entry_init(
		      &entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_INVALID_SELECTION &&
	      memcmp(&entry, &saved, sizeof(entry)) == 0,
	      "unsupported MD interval changed the entry");

	selected = conventional_selection(
		PCM_PROTOCOL_V90, V34_SYMBOL_RATE_3200, true);
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
	struct pcm_info1_selection selected = conventional_selection(
		PCM_PROTOCOL_V90, V34_SYMBOL_RATE_3000, false);
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
	CHECK(pcm_phase3_entry_start_v90_analog(
		      NULL, NOMINAL_AMPLITUDE, V90_DIL_PROFILE_NONE, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_analog(
		      &analog_entry, 0, V90_DIL_PROFILE_NONE, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_analog(
		      &analog_entry, NOMINAL_AMPLITUDE,
		      V90_DIL_PROFILE_NONE, NULL) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_analog(
		      &analog_entry, INT16_MAX, V90_DIL_PROFILE_NONE,
		      &analog) ==
		      PCM_PHASE3_ENTRY_CLIPPING_RISK &&
	      memcmp(&analog, &saved_analog, sizeof(analog)) == 0,
	      "invalid or clipping analogue start changed state");
	CHECK(pcm_phase3_entry_start_v90_digital(
		      NULL, &configuration, TEST_INITIAL_RECEIVE_GAIN, &jd,
		      &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_digital(
		      &digital_entry, NULL, TEST_INITIAL_RECEIVE_GAIN, &jd,
		      &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_digital(
		      &digital_entry, &configuration, 0, &jd, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_digital(
		      &digital_entry, &configuration,
		      TEST_INITIAL_RECEIVE_GAIN, NULL, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v90_digital(
		      &digital_entry, &configuration,
		      TEST_INITIAL_RECEIVE_GAIN, &jd, NULL) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0,
	      "invalid digital start changed state");
	analog_entry.conventional_profile.md_length_35_ms_units = 1;
	analog_entry.conventional_profile.md_symbol_count = 105;
	CHECK(v34_phase3_profile_is_valid(&analog_entry.conventional_profile) &&
	      !pcm_phase3_entry_is_valid(&analog_entry) &&
	      pcm_phase3_entry_start_v90_analog(
		      &analog_entry, NOMINAL_AMPLITUDE,
		      V90_DIL_PROFILE_NONE, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT,
	      "entry accepted a conventional MD interval");
	return true;
}

static bool test_v90_branch_guards(void)
{
	struct v90_info0d info0 = digital_info0(G711_LAW_PCMU);
	struct v90_phase3_analog_rx_configuration configuration =
		receive_configuration();
	struct v90_jd_parameters jd = jd_parameters();

	for (unsigned int upstream_index = PCM_UPSTREAM_V34;
	     upstream_index <= PCM_UPSTREAM_PCM; ++upstream_index) {
		struct pcm_info1_selection selected =
			upstream_index == PCM_UPSTREAM_V34 ?
				conventional_selection(
					PCM_PROTOCOL_V92,
					V34_SYMBOL_RATE_3200, true) :
				pcm_upstream_selection();

		for (unsigned int role_index = V90_ANALOG_MODEM;
		     role_index <= V90_DIGITAL_MODEM; ++role_index) {
			enum v90_modem_role role =
				(enum v90_modem_role)role_index;
			struct pcm_phase3_entry entry;
			struct v90_phase3_analog_station analog;
			struct v90_phase3_analog_station saved_analog;
			struct v90_phase3_digital_station digital;
			struct v90_phase3_digital_station saved_digital;

			CHECK(pcm_phase3_entry_init(
				      &entry, role, G711_LAW_PCMU,
				      &info0, &selected) == PCM_PHASE3_ENTRY_OK &&
			      pcm_phase3_entry_is_valid(&entry) &&
			      entry.selection.startup.protocol == PCM_PROTOCOL_V92 &&
			      entry.selection.upstream ==
				      (enum pcm_upstream_mode)upstream_index &&
			      (upstream_index != PCM_UPSTREAM_PCM ||
			       entry.selection.analog.pcm.maximum_total_coefficients ==
				       selected.analog.pcm.maximum_total_coefficients),
			      "V.92 upstream %u role %u entry changed",
			      upstream_index, role_index);
			memset(&analog, 0xa5, sizeof(analog));
			saved_analog = analog;
			memset(&digital, 0x5a, sizeof(digital));
			saved_digital = digital;
			if (role == V90_ANALOG_MODEM) {
				CHECK(pcm_phase3_entry_start_v90_analog(
					      &entry, NOMINAL_AMPLITUDE,
					      V90_DIL_PROFILE_NONE,
					      &analog) ==
					      PCM_PHASE3_ENTRY_INVALID_SELECTION &&
				      memcmp(&analog, &saved_analog,
					     sizeof(analog)) == 0,
				      "V.92 upstream %u entered the V.90 analogue branch",
				      upstream_index);
			} else {
				CHECK(pcm_phase3_entry_start_v90_digital(
					      &entry, &configuration,
					      TEST_INITIAL_RECEIVE_GAIN, &jd,
					      &digital) ==
					      PCM_PHASE3_ENTRY_INVALID_SELECTION &&
				      memcmp(&digital, &saved_digital,
					     sizeof(digital)) == 0,
				      "V.92 upstream %u entered the V.90 digital branch",
				      upstream_index);
			}
		}
	}
	return true;
}

static bool test_v92_pcm_starts(void)
{
	struct pcm_info1_selection selected = pcm_upstream_selection();
	struct v90_info0d info0 = digital_info0(G711_LAW_PCMU);
	struct v92_phase3_analog_station_configuration configuration =
		v92_analog_configuration(G711_LAW_PCMU, &selected);
	struct v92_jd jd = v92_jd_parameters();
	struct v92_jp jp = v92_jp_parameters();
	struct pcm_phase3_entry analog_entry;
	struct pcm_phase3_entry digital_entry;
	struct v92_phase3_analog_station analog;
	struct v92_phase3_analog_station saved_analog;
	struct v92_phase3_analog_station untouched_analog;
	struct v92_phase3_digital_station digital;
	struct v92_phase3_digital_station saved_digital;
	struct v92_phase3_digital_station untouched_digital;

	CHECK(pcm_phase3_entry_init(
		      &analog_entry, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_OK &&
	      pcm_phase3_entry_init(
		      &digital_entry, V90_DIGITAL_MODEM, G711_LAW_PCMU,
		      &info0, &selected) == PCM_PHASE3_ENTRY_OK,
	      "V.92 PCM entries did not initialize");
	memset(&analog, 0xa5, sizeof(analog));
	saved_analog = analog;
	untouched_analog = saved_analog;
	memset(&digital, 0x5a, sizeof(digital));
	saved_digital = digital;
	untouched_digital = saved_digital;
	CHECK(pcm_phase3_entry_start_v92_analog(
		      &analog_entry, &configuration, &analog) ==
		      PCM_PHASE3_ENTRY_OK &&
	      v92_phase3_analog_station_is_valid(&analog) &&
	      analog.law == analog_entry.law &&
	      analog.downstream_training_ucode ==
		      selected.analog.pcm.training_ucode &&
	      pcm_phase3_entry_start_v92_analog(
		      &digital_entry, &configuration, &saved_analog) ==
		      PCM_PHASE3_ENTRY_ROLE_MISMATCH &&
	      memcmp(&saved_analog, &untouched_analog,
		     sizeof(saved_analog)) == 0,
	      "V.92 analogue entry selected the wrong owner");
	CHECK(pcm_phase3_entry_start_v92_digital(
		      &digital_entry, 0.0, 256u, &jd, &jp, &digital) ==
		      PCM_PHASE3_ENTRY_OK &&
	      v92_phase3_digital_station_is_valid(&digital) &&
	      digital.law == digital_entry.law &&
	      digital.training_ucode == selected.analog.pcm.training_ucode &&
	      pcm_phase3_entry_start_v92_digital(
		      &analog_entry, 0.0, 256u, &jd, &jp,
		      &saved_digital) == PCM_PHASE3_ENTRY_ROLE_MISMATCH &&
	      memcmp(&saved_digital, &untouched_digital,
		     sizeof(saved_digital)) == 0,
	      "V.92 digital entry selected the wrong owner");

	memset(&analog, 0xa5, sizeof(analog));
	saved_analog = analog;
	memset(&digital, 0x5a, sizeof(digital));
	saved_digital = digital;
	configuration.law = G711_LAW_PCMA;
	CHECK(pcm_phase3_entry_start_v92_analog(
		      NULL, &configuration, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v92_analog(
		      &analog_entry, NULL, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v92_analog(
		      &analog_entry, &configuration, &analog) ==
		      PCM_PHASE3_ENTRY_INVALID_SELECTION &&
	      memcmp(&analog, &saved_analog, sizeof(analog)) == 0,
	      "invalid V.92 analogue entry changed state");
	CHECK(pcm_phase3_entry_start_v92_digital(
		      NULL, 0.0, 256u, &jd, &jp, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v92_digital(
		      &digital_entry, NAN, 256u, &jd, &jp, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v92_digital(
		      &digital_entry, 0.0, 0u, &jd, &jp, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      pcm_phase3_entry_start_v92_digital(
		      &digital_entry, 0.0, 256u, NULL, &jp, &digital) ==
		      PCM_PHASE3_ENTRY_INVALID_ARGUMENT &&
	      memcmp(&digital, &saved_digital, sizeof(digital)) == 0,
	      "invalid V.92 digital entry changed state");
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
		{ "V.90 branch guards", test_v90_branch_guards },
		{ "V.92 PCM station starts", test_v92_pcm_starts },
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
