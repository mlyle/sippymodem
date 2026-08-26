// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_selection.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

static struct v34_phase2_probe_receiver_measurement probe_measurement(
	double threshold_power, double source_power)
{
	struct v34_phase2_probe_receiver_measurement measurement = {
		.l1.sample_count = V34_PHASE2_PROBE_L1_SAMPLES,
		.l2.sample_count = V34_PHASE2_PROBE_L1_SAMPLES,
		.l2_samples = V34_PHASE2_PROBE_L1_SAMPLES,
	};

	measurement.l1.harmonics[4].mean_square = 1000000.0;
	measurement.l2.harmonics[4].mean_square = threshold_power;
	measurement.l2.harmonics[18].mean_square = source_power;
	measurement.l2.harmonics[19].mean_square = source_power;
	measurement.l2.harmonics[20].mean_square = source_power;
	measurement.l2.harmonics[22].mean_square = source_power;
	return measurement;
}

static bool test_measured_preemphasis(void)
{
	static const uint8_t expected[V34_SYMBOL_RATE_COUNT] = {
		8, 8, 8, 9, 9, 8,
	};
	struct v34_phase2_probe_receiver_measurement measurement =
		probe_measurement(4.0, 1.0);
	uint8_t selected;

	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		selected = 0xa5;
		CHECK(v34_phase2_select_preemphasis(
			      &measurement, (enum v34_symbol_rate)rate,
			      &selected) == V34_PHASE2_SELECTION_OK &&
		      selected == expected[rate],
		      "rate %u selected pre-emphasis %u, want %u", rate,
		      selected, expected[rate]);
	}
	measurement = probe_measurement(1.0, 1.0);
	CHECK(v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_3429, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      selected == 6,
	      "flat probe did not select the first shaped filter");
	measurement = probe_measurement(1.0, 0.0);
	CHECK(v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_2400, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      selected == 10,
	      "high-frequency null did not select maximum pre-emphasis");
	return true;
}

static bool test_preemphasis_validation(void)
{
	struct v34_phase2_probe_receiver_measurement measurement =
		probe_measurement(1.0, 1.0);
	uint8_t selected = 0xa5;

	CHECK(v34_phase2_select_preemphasis(
		      NULL, V34_SYMBOL_RATE_2400, &selected) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_2400, NULL) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_COUNT, &selected) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      selected == 0xa5,
	      "invalid pre-emphasis selection changed its output");
	measurement.l2.harmonics[18].mean_square = NAN;
	CHECK(v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_2400, &selected) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      selected == 0xa5,
	      "non-finite probe selected pre-emphasis");
	measurement = probe_measurement(1.0, DBL_MAX);
	CHECK(v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_2400, &selected) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      selected == 0xa5,
	      "overflowing probe selected pre-emphasis");
	measurement = probe_measurement(1.0, 1.0);
	CHECK(v34_phase2_select_preemphasis(
		      &measurement, V34_SYMBOL_RATE_2400,
		      (uint8_t *)&measurement) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT,
	      "aliased pre-emphasis output was accepted");
	return true;
}

static struct v34_info0 full_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 5,
		.constellation_1664_supported = true,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

static struct v34_info1c info1c_fixture(void)
{
	struct v34_info1c info1c = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 3,
			.local_md_length_35_ms_units = 5,
			.frequency_offset_1_50_hz_units = -17,
		},
	};

	for (unsigned int index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index) {
		info1c.answer_transmit[index] =
			(struct v34_info1_probe_result){
				.high_carrier = (index & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)index,
				.projected_data_rate_2400_bps_units = 12,
			};
	}
	return info1c;
}

static struct v34_info1a info1a_fixture(void)
{
	return (struct v34_info1a){
		.common = {
			.minimum_peer_power_reduction_db = 4,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = 23,
		},
		.call_transmit = {
			.high_carrier = true,
			.preemphasis_filter_index = 8,
			.projected_data_rate_2400_bps_units = 11,
		},
		.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3200,
		.call_to_answer_symbol_rate = V34_SYMBOL_RATE_3000,
	};
}

static void disable_transmit_configuration(
	struct v34_info0 *info0, enum v34_symbol_rate rate,
	bool high_carrier)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2400:
		break;
	case V34_SYMBOL_RATE_2743:
		info0->symbol_rate_2743_supported = false;
		break;
	case V34_SYMBOL_RATE_2800:
		info0->symbol_rate_2800_supported = false;
		break;
	case V34_SYMBOL_RATE_3000:
		if (high_carrier)
			info0->high_carrier_3000_supported = false;
		else
			info0->low_carrier_3000_supported = false;
		break;
	case V34_SYMBOL_RATE_3200:
		if (high_carrier)
			info0->high_carrier_3200_supported = false;
		else
			info0->low_carrier_3200_supported = false;
		break;
	case V34_SYMBOL_RATE_3429:
		info0->symbol_rate_3429_transmit_permitted = false;
		break;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
}

static bool rate_is_optional(enum v34_symbol_rate rate)
{
	return rate == V34_SYMBOL_RATE_2743 ||
	       rate == V34_SYMBOL_RATE_2800 ||
	       rate == V34_SYMBOL_RATE_3429;
}

static void disable_receive_capability(
	struct v34_info0 *info0, enum v34_symbol_rate rate)
{
	switch (rate) {
	case V34_SYMBOL_RATE_2743:
		info0->symbol_rate_2743_supported = false;
		break;
	case V34_SYMBOL_RATE_2800:
		info0->symbol_rate_2800_supported = false;
		break;
	case V34_SYMBOL_RATE_3429:
		info0->symbol_rate_3429_supported = false;
		break;
	case V34_SYMBOL_RATE_2400:
	case V34_SYMBOL_RATE_3000:
	case V34_SYMBOL_RATE_3200:
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
}

static bool test_rate_and_carrier_capabilities(void)
{
	for (unsigned int rate_index = 0;
	     rate_index < V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum v34_symbol_rate rate =
			(enum v34_symbol_rate)rate_index;

		for (unsigned int high = 0; high < 2; ++high) {
			struct v34_info0 caller = full_capabilities();
			struct v34_info0 answerer = full_capabilities();
			struct v34_info1c info1c = { 0 };
			struct v34_info1a info1a = info1a_fixture();
			bool high_carrier = high != 0;

			info1c.common.local_md_length_35_ms_units = 1;
			info1c.answer_transmit[rate] =
				(struct v34_info1_probe_result){
					.high_carrier = high_carrier,
					.preemphasis_filter_index = 3,
					.projected_data_rate_2400_bps_units = 12,
				};
			info1a.answer_to_call_symbol_rate = rate;
			info1a.call_to_answer_symbol_rate = rate;
			info1a.call_transmit =
				(struct v34_info1_probe_result){
					.high_carrier = high_carrier,
					.preemphasis_filter_index = 4,
					.projected_data_rate_2400_bps_units = 12,
				};
			CHECK(v34_phase2_info1c_is_consistent(
				      &caller, &answerer, &info1c) &&
			      v34_phase2_info1a_is_consistent(
				      &caller, &answerer, &info1c, &info1a),
			      "rate %u carrier %u rejected",
			      rate_index, high);

			if (rate != V34_SYMBOL_RATE_2400) {
				struct v34_info0 restricted = answerer;

				disable_transmit_configuration(
					&restricted, rate, high_carrier);
				CHECK(!v34_phase2_info1c_is_consistent(
					      &caller, &restricted, &info1c),
				      "answer transmitter restriction was ignored for %u/%u",
				      rate_index, high);
				restricted = caller;
				disable_transmit_configuration(
					&restricted, rate, high_carrier);
				CHECK(!v34_phase2_info1a_is_consistent(
					      &restricted, &answerer,
					      &info1c, &info1a),
				      "caller transmitter restriction was ignored for %u/%u",
				      rate_index, high);
			}
			if (rate_is_optional(rate)) {
				struct v34_info0 restricted = caller;

				disable_receive_capability(&restricted, rate);
				CHECK(!v34_phase2_info1c_is_consistent(
					      &restricted, &answerer,
					      &info1c),
				      "caller receive capability was ignored for %u",
				      rate_index);
				restricted = answerer;
				disable_receive_capability(&restricted, rate);
				CHECK(!v34_phase2_info1a_is_consistent(
					      &caller, &restricted,
					      &info1c, &info1a),
				      "answerer receive capability was ignored for %u",
				      rate_index);
			}
		}
	}
	return true;
}

static bool test_cross_record_constraints(void)
{
	struct v34_info0 caller = full_capabilities();
	struct v34_info0 answerer = full_capabilities();
	struct v34_info1c info1c = info1c_fixture();
	struct v34_info1a info1a = info1a_fixture();

	CHECK(v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "valid selection fixture was rejected");
	answerer.transmit_power_reduction_supported = false;
	CHECK(!v34_phase2_info1c_is_consistent(
		      &caller, &answerer, &info1c),
	      "INFO1c requested unsupported power reduction");
	info1c.common.minimum_peer_power_reduction_db = 0;
	info1c.common.additional_peer_power_reduction_db = 0;
	CHECK(v34_phase2_info1c_is_consistent(
		      &caller, &answerer, &info1c),
	      "zero INFO1c power request was rejected");

	caller.transmit_power_reduction_supported = false;
	CHECK(!v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "INFO1a requested unsupported power reduction");
	info1a.common.minimum_peer_power_reduction_db = 0;
	info1a.common.additional_peer_power_reduction_db = 0;
	CHECK(v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "zero INFO1a power request was rejected");

	caller = full_capabilities();
	answerer = full_capabilities();
	info1c = info1c_fixture();
	info1a = info1a_fixture();
	answerer.constellation_1664_supported = false;
	info1c.answer_transmit[V34_SYMBOL_RATE_2400]
		.projected_data_rate_2400_bps_units = 13;
	CHECK(!v34_phase2_info1c_is_consistent(
		      &caller, &answerer, &info1c),
	      "INFO1c exceeded the answerer's constellation capability");
	info1c.answer_transmit[V34_SYMBOL_RATE_2400]
		.projected_data_rate_2400_bps_units = 12;
	caller.constellation_1664_supported = false;
	info1a.call_transmit.projected_data_rate_2400_bps_units = 13;
	CHECK(!v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "INFO1a exceeded the caller's constellation capability");

	caller = full_capabilities();
	answerer = full_capabilities();
	info1c = info1c_fixture();
	info1a = info1a_fixture();
	info1a.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3429;
	info1a.call_to_answer_symbol_rate = V34_SYMBOL_RATE_2400;
	caller.maximum_symbol_rate_difference_steps = 4;
	CHECK(!v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "caller asymmetry limit was ignored");
	caller.maximum_symbol_rate_difference_steps = 5;
	answerer.maximum_symbol_rate_difference_steps = 4;
	CHECK(!v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "answerer asymmetry limit was ignored");
	answerer.maximum_symbol_rate_difference_steps = 5;
	info1c.answer_transmit[V34_SYMBOL_RATE_3429]
		.projected_data_rate_2400_bps_units = 0;
	CHECK(!v34_phase2_info1a_is_consistent(
		      &caller, &answerer, &info1c, &info1a),
	      "INFO1a selected an unusable INFO1c entry");
	return true;
}

static bool test_answerer_fallback_selection(void)
{
	static const uint8_t expected_limits[V34_SYMBOL_RATE_COUNT] = {
		9, 10, 11, 12, 13, 14,
	};
	struct v34_info0 caller = full_capabilities();
	struct v34_info0 answerer = full_capabilities();
	struct v34_info1c info1c = { 0 };
	struct v34_info1a preferred = {
		.call_transmit = {
			.preemphasis_filter_index = 7,
			.projected_data_rate_2400_bps_units = 14,
		},
		.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3429,
		.call_to_answer_symbol_rate = V34_SYMBOL_RATE_3429,
	};
	struct v34_info1a selected;
	struct v34_info1a saved;

	for (unsigned int rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate)
		CHECK(v34_phase2_projected_rate_limit(
			      (enum v34_symbol_rate)rate) ==
			      expected_limits[rate],
		      "rate %u has the wrong projected-rate limit", rate);
	CHECK(v34_phase2_projected_rate_limit(V34_SYMBOL_RATE_COUNT) == 0,
	      "invalid symbol rate has a projected-rate limit");

	info1c.answer_transmit[V34_SYMBOL_RATE_3429] =
		(struct v34_info1_probe_result){
			.preemphasis_filter_index = 8,
			.projected_data_rate_2400_bps_units = 14,
		};
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      memcmp(&selected, &preferred, sizeof(selected)) == 0,
	      "usable preferred INFO1a was changed");

	info1c.answer_transmit[V34_SYMBOL_RATE_3429] =
		(struct v34_info1_probe_result){ 0 };
	info1c.answer_transmit[V34_SYMBOL_RATE_3200] =
		(struct v34_info1_probe_result){
			.preemphasis_filter_index = 6,
			.projected_data_rate_2400_bps_units = 13,
		};
	caller.low_carrier_3200_supported = false;
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      selected.answer_to_call_symbol_rate == V34_SYMBOL_RATE_3200 &&
	      selected.call_to_answer_symbol_rate == V34_SYMBOL_RATE_3200 &&
	      selected.call_transmit.high_carrier &&
	      selected.call_transmit.preemphasis_filter_index == 7 &&
	      selected.call_transmit.projected_data_rate_2400_bps_units == 13,
	      "withdrawn preferred profile did not select 3200 fallback");

	info1c.answer_transmit[V34_SYMBOL_RATE_3200] =
		(struct v34_info1_probe_result){ 0 };
	info1c.answer_transmit[V34_SYMBOL_RATE_2400] =
		(struct v34_info1_probe_result){
			.projected_data_rate_2400_bps_units = 9,
		};
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred, &selected) ==
		      V34_PHASE2_SELECTION_OK &&
	      selected.answer_to_call_symbol_rate == V34_SYMBOL_RATE_2400 &&
	      selected.call_to_answer_symbol_rate == V34_SYMBOL_RATE_2400 &&
	      selected.call_transmit.projected_data_rate_2400_bps_units == 9,
	      "2400 fallback did not clamp its projected rate");

	info1c.answer_transmit[V34_SYMBOL_RATE_2400] =
		(struct v34_info1_probe_result){ 0 };
	memset(&selected, 0xa5, sizeof(selected));
	saved = selected;
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred, &selected) ==
		      V34_PHASE2_SELECTION_NO_COMMON_PROFILE &&
	      memcmp(&selected, &saved, sizeof(selected)) == 0,
	      "no-common fallback changed its output");
	caller.transmit_power_reduction_supported = false;
	preferred.common.minimum_peer_power_reduction_db = 1;
	CHECK(v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred, &selected) ==
		      V34_PHASE2_SELECTION_INCONSISTENT_INFO1A &&
	      memcmp(&selected, &saved, sizeof(selected)) == 0 &&
	      v34_phase2_select_answerer_info1a(
		      &caller, &answerer, &info1c, &preferred,
		      (struct v34_info1a *)&info1c) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      strcmp(v34_phase2_selection_result_name(
		     V34_PHASE2_SELECTION_NO_COMMON_PROFILE),
		     "no common profile") == 0,
	      "fallback validation was not transactional");
	return true;
}

static bool test_profile_selection_and_validation(void)
{
	struct v34_info0 caller = full_capabilities();
	struct v34_info0 answerer = full_capabilities();
	struct v34_info1c info1c = info1c_fixture();
	struct v34_info1a info1a = info1a_fixture();
	struct v34_phase3_profile caller_profile;
	struct v34_phase3_profile answerer_profile;
	struct v34_phase3_profile saved_caller;
	struct v34_phase3_profile saved_answerer;

	CHECK(v34_phase2_select_profiles(
		      &caller, &answerer, &info1c, &info1a,
		      &caller_profile, &answerer_profile) ==
		      V34_PHASE2_SELECTION_OK &&
	      v34_phase3_profile_is_valid(&caller_profile) &&
	      v34_phase3_profile_is_valid(&answerer_profile) &&
	      caller_profile.local_role == V34_CALLING_STATION &&
	      caller_profile.symbol_rate == V34_SYMBOL_RATE_3000 &&
	      caller_profile.preemphasis_filter_index == 8 &&
	      answerer_profile.local_role == V34_ANSWERING_STATION &&
	      answerer_profile.symbol_rate == V34_SYMBOL_RATE_3200 &&
	      answerer_profile.preemphasis_filter_index == 4,
	      "complete selection produced the wrong profiles");

	memset(&caller_profile, 0xa5, sizeof(caller_profile));
	memset(&answerer_profile, 0x5a, sizeof(answerer_profile));
	saved_caller = caller_profile;
	saved_answerer = answerer_profile;
	answerer.low_carrier_3200_supported = false;
	CHECK(v34_phase2_select_profiles(
		      &caller, &answerer, &info1c, &info1a,
		      &caller_profile, &answerer_profile) ==
		      V34_PHASE2_SELECTION_INCONSISTENT_INFO1C &&
	      memcmp(&caller_profile, &saved_caller,
		     sizeof(caller_profile)) == 0 &&
	      memcmp(&answerer_profile, &saved_answerer,
		     sizeof(answerer_profile)) == 0,
	      "inconsistent INFO1c mutated profile outputs");
	answerer = full_capabilities();
	caller.high_carrier_3000_supported = false;
	CHECK(v34_phase2_select_profiles(
		      &caller, &answerer, &info1c, &info1a,
		      &caller_profile, &answerer_profile) ==
		      V34_PHASE2_SELECTION_INCONSISTENT_INFO1A &&
	      memcmp(&caller_profile, &saved_caller,
		     sizeof(caller_profile)) == 0 &&
	      memcmp(&answerer_profile, &saved_answerer,
		     sizeof(answerer_profile)) == 0,
	      "inconsistent INFO1a mutated profile outputs");
	CHECK(v34_phase2_select_profiles(
		      &caller, &answerer, &info1c, &info1a,
		      &caller_profile, &caller_profile) ==
		      V34_PHASE2_SELECTION_INVALID_ARGUMENT &&
	      !v34_phase2_info1c_is_consistent(NULL, &answerer, &info1c) &&
	      !v34_phase2_info1a_is_consistent(
		      &caller, NULL, &info1c, &info1a) &&
	      strcmp(v34_phase2_selection_result_name(
		     V34_PHASE2_SELECTION_INCONSISTENT_INFO1C),
		     "inconsistent INFO1c") == 0 &&
	      strcmp(v34_phase2_selection_result_name(
		     (enum v34_phase2_selection_result)99),
		     "unknown") == 0,
	      "selection validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "measured pre-emphasis", test_measured_preemphasis },
		{ "pre-emphasis validation", test_preemphasis_validation },
		{ "rate and carrier capabilities",
		  test_rate_and_carrier_capabilities },
		{ "cross-record constraints", test_cross_record_constraints },
		{ "answerer INFO1 fallback", test_answerer_fallback_selection },
		{ "profile selection and validation",
		  test_profile_selection_and_validation },
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
