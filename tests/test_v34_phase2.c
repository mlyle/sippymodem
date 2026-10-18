// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2.h>
#include <v34_training_transmitter.h>

#include "v34_phase2_test_pair.h"
#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SAMPLE_LIMIT = 40000,
	TEST_TONE_SAMPLES = 160,
	INITIAL_CONCURRENT_DELAY_SAMPLES = 1000,
	RETRAIN_PRIOR_RTD_SAMPLES = 1000,
	RETRAIN_LATE_ALIAS_SAMPLES = 5000,
};

struct owned_pair {
	struct test_pair channel;
	struct v34_phase2 caller;
	struct v34_phase2 answerer;
};

static struct v34_info1c caller_info1c(bool zero_md)
{
	struct v34_info1c info1c = {
		.common = {
			.local_md_length_35_ms_units = zero_md ? 0 : 2,
			.frequency_offset_1_50_hz_units = -10,
		},
	};

	info1c.answer_transmit[V34_SYMBOL_RATE_3200] =
		(struct v34_info1_probe_result){
			.high_carrier = true,
			.preemphasis_filter_index = 3,
			.projected_data_rate_2400_bps_units = 10,
		};
	return info1c;
}

static struct v34_info1a answerer_info1a(bool zero_md)
{
	return (struct v34_info1a){
		.common = {
			.local_md_length_35_ms_units = zero_md ? 0 : 3,
			.frequency_offset_1_50_hz_units = 11,
		},
		.call_transmit = {
			.preemphasis_filter_index = 4,
			.projected_data_rate_2400_bps_units = 9,
		},
		.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3200,
		.call_to_answer_symbol_rate = V34_SYMBOL_RATE_3000,
	};
}

static bool info1c_equal(
	const struct v34_info1c *first, const struct v34_info1c *second)
{
	uint8_t first_payload[V34_INFO1C_PAYLOAD_BYTES];
	uint8_t second_payload[V34_INFO1C_PAYLOAD_BYTES];

	return v34_info1c_encode(
		       first, first_payload, sizeof(first_payload)) ==
		       V34_INFO1_OK &&
	       v34_info1c_encode(
		       second, second_payload, sizeof(second_payload)) ==
		       V34_INFO1_OK &&
	       memcmp(first_payload, second_payload,
		      sizeof(first_payload)) == 0;
}

static bool info1a_equal(
	const struct v34_info1a *first, const struct v34_info1a *second)
{
	uint8_t first_payload[V34_INFO1A_PAYLOAD_BYTES];
	uint8_t second_payload[V34_INFO1A_PAYLOAD_BYTES];

	return v34_info1a_encode(
		       first, first_payload, sizeof(first_payload)) ==
		       V34_INFO1_OK &&
	       v34_info1a_encode(
		       second, second_payload, sizeof(second_payload)) ==
		       V34_INFO1_OK &&
	       memcmp(first_payload, second_payload,
		      sizeof(first_payload)) == 0;
}

static bool profiles_equal(
	const struct v34_phase3_profile *first,
	const struct v34_phase3_profile *second)
{
	return first->symbol_rate_numerator == second->symbol_rate_numerator &&
	       first->symbol_rate_denominator == second->symbol_rate_denominator &&
	       first->carrier_frequency_numerator ==
		       second->carrier_frequency_numerator &&
	       first->carrier_frequency_denominator ==
		       second->carrier_frequency_denominator &&
	       first->md_symbol_count == second->md_symbol_count &&
	       first->symbol_rate == second->symbol_rate &&
	       first->local_role == second->local_role &&
	       first->transmit_power_reduction_db ==
		       second->transmit_power_reduction_db &&
	       first->preemphasis_filter_index ==
		       second->preemphasis_filter_index &&
	       first->projected_data_rate_2400_bps_units ==
		       second->projected_data_rate_2400_bps_units &&
	       first->md_length_35_ms_units ==
		       second->md_length_35_ms_units &&
	       first->high_carrier == second->high_carrier;
}

static bool pair_init(
	struct owned_pair *pair, enum test_codec codec,
	size_t delay_samples)
{
	struct v34_info0 caller_info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 answerer_info0 = test_v34_phase2_capabilities(
		V34_ANSWERING_STATION);
	struct owned_pair initialized;

	memset(&initialized, 0, sizeof(initialized));
	if (!test_v34_phase2_pair_init(
		    &initialized.channel, codec, delay_samples) ||
	    v34_phase2_init(
		    &initialized.caller, V34_CALLING_STATION,
		    TEST_AMPLITUDE, &caller_info0) != V34_PHASE2_OK ||
	    v34_phase2_init(
		    &initialized.answerer, V34_ANSWERING_STATION,
		    TEST_AMPLITUDE, &answerer_info0) != V34_PHASE2_OK)
		return false;
	*pair = initialized;
	return true;
}

static bool pair_sample(
	struct owned_pair *pair, struct v34_phase2_event *caller_event,
	struct v34_phase2_event *answerer_event, int16_t *caller_transmit,
	int16_t *answerer_transmit)
{
	int16_t caller_receive;
	int16_t answerer_receive;

	return test_v34_phase2_channel_receive(
		       &pair->channel, &caller_receive, &answerer_receive) &&
	       v34_phase2_sample(
		       &pair->caller, caller_receive, caller_transmit,
		       caller_event) == V34_PHASE2_OK &&
	       v34_phase2_sample(
		       &pair->answerer, answerer_receive, answerer_transmit,
		       answerer_event) == V34_PHASE2_OK &&
	       test_v34_phase2_channel_transmit(
		       &pair->channel, *caller_transmit,
		       *answerer_transmit);
}

static bool record_event(
	const struct v34_phase2_event *event, uint32_t *events)
{
	uint32_t bit;

	if (event->kind == V34_PHASE2_EVENT_NONE)
		return true;
	if (event->kind == V34_PHASE2_EVENT_FAILED ||
	    (unsigned int)event->kind >= 32u)
		return false;
	bit = UINT32_C(1) << event->kind;
	if ((*events & bit) != 0)
		return false;
	*events |= bit;
	return true;
}

static bool measurement_is_finite(
	const struct v34_phase2_probe_receiver_measurement *measurement)
{
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index) {
		if (!isfinite(measurement->l1.harmonics[index].mean_square) ||
		    !isfinite(measurement->l2.harmonics[index].mean_square))
			return false;
	}
	return measurement->l1.sample_count ==
		       V34_PHASE2_PROBE_L1_SAMPLES &&
	       measurement->l2.sample_count ==
		       V34_PHASE2_PROBE_L1_SAMPLES;
}

static bool check_timeline(
	enum test_codec codec, size_t delay_samples, bool zero_md)
{
	const uint32_t common_events =
		(UINT32_C(1) << V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_READY_FOR_INFO1);
	const uint32_t caller_info1_events =
		(UINT32_C(1) << V34_PHASE2_EVENT_LOCAL_INFO1C_SENT) |
		(UINT32_C(1) << V34_PHASE2_EVENT_PEER_INFO1A_RECEIVED);
	const uint32_t answerer_info1_events =
		(UINT32_C(1) << V34_PHASE2_EVENT_PEER_INFO1C_RECEIVED) |
		(UINT32_C(1) << V34_PHASE2_EVENT_LOCAL_INFO1A_SENT);
	struct v34_info0 expected_caller_info0 =
		test_v34_phase2_capabilities(V34_CALLING_STATION);
	struct v34_info0 expected_answerer_info0 =
		test_v34_phase2_capabilities(V34_ANSWERING_STATION);
	struct v34_info0 caller_peer_info0;
	struct v34_info0 answerer_peer_info0;
	struct v34_info1c local_info1c = caller_info1c(zero_md);
	struct v34_info1c received_info1c;
	struct v34_info1a local_info1a = answerer_info1a(zero_md);
	struct v34_phase2_probe_receiver_measurement caller_measurement;
	struct v34_phase2_probe_receiver_measurement answerer_measurement;
	struct v34_phase3_profile caller_local_profile;
	struct v34_phase3_profile caller_peer_profile;
	struct v34_phase3_profile answerer_local_profile;
	struct v34_phase3_profile answerer_peer_profile;
	struct owned_pair pair;
	uint64_t answerer_tone_magnitude = 0;
	uint32_t caller_events = 0;
	uint32_t answerer_events = 0;
	int16_t caller_receive_gain;
	int16_t answerer_receive_gain;
	int16_t initial_caller_receive_gain;
	int16_t initial_answerer_receive_gain;
	bool answerer_replied = false;

	CHECK(pair_init(&pair, codec, delay_samples),
	      "Phase 2 pair did not initialize for codec %d delay %zu",
	      codec, delay_samples);
	{
		struct v34_phase2 caller = pair.caller, answerer = pair.answerer;

		CHECK(v34_phase2_start_caller_info1(&pair.caller, &local_info1c) ==
			      V34_PHASE2_INVALID_STATE &&
		      v34_phase2_start_answerer_info1(&pair.answerer) ==
			      V34_PHASE2_INVALID_STATE &&
		      memcmp(&caller, &pair.caller, sizeof(caller)) == 0 &&
		      memcmp(&answerer, &pair.answerer, sizeof(answerer)) == 0,
		      "Phase 2 permitted INFO1 before probing");
	}
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      record_event(&caller_event, &caller_events) &&
		      record_event(&answerer_event, &answerer_events),
		      "Phase 2 pair failed at sample %zu", index);
		if (v34_phase2_is_ready_for_info1(&pair.caller) &&
		    v34_phase2_is_ready_for_info1(&pair.answerer))
			break;
	}
	CHECK(v34_phase2_is_ready_for_info1(&pair.caller) &&
	      v34_phase2_is_ready_for_info1(&pair.answerer) &&
	      !v34_phase2_has_failed(&pair.caller) &&
	      !v34_phase2_has_failed(&pair.answerer) &&
	      (caller_events & common_events) == common_events &&
	      (answerer_events & common_events) == common_events &&
	      (answerer_events &
	       (UINT32_C(1) << V34_PHASE2_EVENT_PROBE_TURNAROUND_STARTED)) != 0 &&
	      (caller_events &
	       (UINT32_C(1) <<
		V34_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED)) != 0 &&
	      ((caller_events | answerer_events) &
	       (UINT32_C(1) << V34_PHASE2_EVENT_ROUND_TRIP_DELAY)) != 0,
	      "Phase 2 transition events were incomplete");
	CHECK(v34_phase2_peer_info0(
		      &pair.caller, &caller_peer_info0) == V34_PHASE2_OK &&
	      v34_phase2_peer_info0(
		      &pair.answerer, &answerer_peer_info0) == V34_PHASE2_OK &&
	      v34_info0_equal(&caller_peer_info0, &expected_answerer_info0) &&
	      v34_info0_equal(&answerer_peer_info0, &expected_caller_info0),
	      "Phase 2 did not retain reciprocal INFO0 records");
	CHECK(v34_phase2_measure_remote_probe(
		      &pair.caller, &caller_measurement) == V34_PHASE2_OK &&
	      v34_phase2_measure_remote_probe(
		      &pair.answerer, &answerer_measurement) == V34_PHASE2_OK &&
	      v34_phase2_receive_gain(
		      &pair.caller, &caller_receive_gain) == V34_PHASE2_OK &&
	      v34_phase2_receive_gain(
		      &pair.answerer, &answerer_receive_gain) == V34_PHASE2_OK &&
	      caller_receive_gain > 0 && answerer_receive_gain > 0 &&
	      measurement_is_finite(&caller_measurement) &&
	      measurement_is_finite(&answerer_measurement),
	      "Phase 2 did not retain both probe measurements");
	initial_caller_receive_gain = caller_receive_gain;
	initial_answerer_receive_gain = answerer_receive_gain;
	for (size_t index = 0; index < TEST_TONE_SAMPLES; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      caller_transmit == 0 &&
		      caller_event.kind == V34_PHASE2_EVENT_NONE &&
		      answerer_event.kind == V34_PHASE2_EVENT_NONE,
		      "ready Phase 2 endpoints changed terminal behavior");
		answerer_tone_magnitude += answerer_transmit < 0 ?
			(uint64_t)-(int64_t)answerer_transmit :
			(uint64_t)answerer_transmit;
	}
	CHECK(answerer_tone_magnitude != 0,
	      "answerer did not retain Tone A at the INFO1 boundary");
	{
		struct v34_info1c inconsistent = local_info1c;
		struct v34_phase2 saved = pair.caller;
		struct v34_phase2 saved_answerer = pair.answerer;

		CHECK(v34_phase2_start_caller_info1(&pair.answerer, &local_info1c) ==
			      V34_PHASE2_INVALID_STATE &&
		      v34_phase2_start_answerer_info1(&pair.caller) ==
			      V34_PHASE2_INVALID_STATE &&
		      memcmp(&pair.caller, &saved, sizeof(saved)) == 0 &&
		      memcmp(&pair.answerer, &saved_answerer, sizeof(saved_answerer)) == 0,
		      "Phase 2 permitted the wrong INFO1 station role");

		inconsistent.answer_transmit[V34_SYMBOL_RATE_3429]
			.projected_data_rate_2400_bps_units = 8;
		CHECK(v34_phase2_start_caller_info1(
			      &pair.caller, &inconsistent) ==
			      V34_PHASE2_INVALID_ARGUMENT &&
		      memcmp(&pair.caller, &saved, sizeof(saved)) == 0,
		      "inconsistent caller INFO1c changed Phase 2 state");
	}
	CHECK(v34_phase2_start_caller_info1(
		      &pair.caller, &local_info1c) == V34_PHASE2_OK &&
	      v34_phase2_start_answerer_info1(&pair.answerer) ==
		      V34_PHASE2_OK &&
	      v34_phase2_answerer_reply(
		      &pair.answerer, &local_info1a) ==
		      V34_PHASE2_INVALID_STATE,
	      "INFO1 did not start at the reciprocal-probe boundary");
	{
		struct v34_phase3_profile local = { .md_symbol_count = 17 };
		struct v34_phase3_profile peer = { .md_symbol_count = 19 };
		struct v34_phase3_profile saved_local = local;
		struct v34_phase3_profile saved_peer = peer;

		CHECK(v34_phase2_profiles(
			      &pair.caller, &local, &peer) ==
			      V34_PHASE2_INVALID_STATE &&
		      memcmp(&local, &saved_local, sizeof(local)) == 0 &&
		      memcmp(&peer, &saved_peer, sizeof(peer)) == 0,
		      "premature profile export changed its destinations");
	}

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      record_event(&caller_event, &caller_events) &&
		      record_event(&answerer_event, &answerer_events),
		      "INFO1 pair failed at sample %zu", index);
		if (answerer_event.kind ==
		    V34_PHASE2_EVENT_PEER_INFO1C_RECEIVED) {
			CHECK(!answerer_replied &&
			      v34_phase2_peer_info1c(
				      &pair.answerer, &received_info1c) ==
				      V34_PHASE2_OK &&
			      info1c_equal(&received_info1c, &local_info1c) &&
			      v34_phase2_answerer_reply(
				      &pair.answerer, &local_info1a) ==
				      V34_PHASE2_OK,
			      "answerer did not accept and answer INFO1c");
			answerer_replied = true;
		}
		if (caller_event.kind == V34_PHASE2_EVENT_PEER_INFO1A_RECEIVED) {
			struct v34_phase3_profile local, peer;

			CHECK(caller_transmit == 0 &&
			      pair.caller.state.info1.control.phase == PHASE2_INFO1_SILENCE &&
			      v34_phase2_is_ready_for_phase3(&pair.caller) &&
			      v34_phase2_profiles(&pair.caller, &local, &peer) == V34_PHASE2_OK,
			      "caller delayed Phase 3 reception after validated INFO1a");
		}
		if (pair.answerer.stage == V34_PHASE2_INFO1 &&
		    pair.answerer.state.info1.control.phase == PHASE2_INFO1_SILENCE)
			CHECK(!v34_phase2_is_ready_for_phase3(&pair.answerer),
			      "answerer shortened the required post-INFO1a silence");
		if (v34_phase2_is_ready_for_phase3(&pair.caller) &&
		    v34_phase2_is_ready_for_phase3(&pair.answerer))
			break;
	}
	CHECK(answerer_replied &&
	      v34_phase2_is_ready_for_phase3(&pair.caller) &&
	      v34_phase2_is_ready_for_phase3(&pair.answerer) &&
	      !v34_phase2_is_ready_for_info1(&pair.caller) &&
	      !v34_phase2_is_ready_for_info1(&pair.answerer) &&
	      (caller_events & caller_info1_events) == caller_info1_events &&
	      (answerer_events & answerer_info1_events) ==
		      answerer_info1_events &&
	      info1a_equal(
		      &pair.caller.state.info1.peer_parameters.info1a,
		      &local_info1a),
	      "INFO1 exchange did not complete with both records");
	CHECK(v34_phase2_profiles(
		      &pair.caller, &caller_local_profile,
		      &caller_peer_profile) == V34_PHASE2_OK &&
	      v34_phase2_profiles(
		      &pair.answerer, &answerer_local_profile,
		      &answerer_peer_profile) == V34_PHASE2_OK &&
	      profiles_equal(&caller_local_profile, &answerer_peer_profile) &&
	      profiles_equal(&caller_peer_profile, &answerer_local_profile) &&
	      caller_local_profile.symbol_rate == V34_SYMBOL_RATE_3000 &&
	      answerer_local_profile.symbol_rate == V34_SYMBOL_RATE_3200,
	      "INFO1 did not produce reciprocal Phase 3 profiles");
	{
		struct v34_phase2 selected = pair.caller;
		struct v34_phase3_profile selected_local;
		struct v34_phase3_profile selected_peer;

		selected.local_info1.info1c
			.answer_transmit[V34_SYMBOL_RATE_2400] =
				(struct v34_info1_probe_result){
					.projected_data_rate_2400_bps_units = 9,
				};
		selected.state.info1.peer_parameters.info1a
			.answer_to_call_symbol_rate = V34_SYMBOL_RATE_2400;
		selected.state.info1.peer_parameters.info1a
			.call_to_answer_symbol_rate = V34_SYMBOL_RATE_2400;
		CHECK(v34_phase2_is_valid(&selected) &&
		      selected.state.info1.peer_parameters.info1a
			      .answer_to_call_symbol_rate ==
			      V34_SYMBOL_RATE_2400 &&
		      selected.state.info1.peer_parameters.info1a
			      .call_to_answer_symbol_rate ==
			      V34_SYMBOL_RATE_2400 &&
		      v34_phase2_profiles(
			      &selected, &selected_local, &selected_peer) ==
			      V34_PHASE2_OK &&
		      selected_local.symbol_rate == V34_SYMBOL_RATE_2400 &&
		      selected_peer.symbol_rate == V34_SYMBOL_RATE_2400,
		      "INFO1a did not select its literal 2400-symbol rate pair");
	}
	if (zero_md) {
		struct v34_training_transmitter caller_phase3;
		struct v34_training_transmitter answerer_phase3;

		CHECK(v34_training_transmitter_init(
			      &caller_phase3, &caller_local_profile,
			      V34_CONSTELLATION_16_POINT, 8000,
			      TEST_AMPLITUDE) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      v34_training_transmitter_init(
			      &answerer_phase3, &answerer_local_profile,
			      V34_CONSTELLATION_16_POINT, 8000,
			      TEST_AMPLITUDE) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      profiles_equal(
			      &caller_phase3.line.profile,
			      &caller_local_profile) &&
		      profiles_equal(
			      &answerer_phase3.line.profile,
			      &answerer_local_profile) &&
		      caller_phase3.phase3.local_role == V34_CALLING_STATION &&
		      answerer_phase3.phase3.local_role ==
			      V34_ANSWERING_STATION,
		      "Phase 2 profiles did not initialize reciprocal Phase 3 lines");
	}
	CHECK(v34_phase2_measure_remote_probe(
		      &pair.caller, &caller_measurement) == V34_PHASE2_OK &&
	      v34_phase2_measure_remote_probe(
		      &pair.answerer, &answerer_measurement) == V34_PHASE2_OK &&
	      v34_phase2_receive_gain(
		      &pair.caller, &caller_receive_gain) == V34_PHASE2_OK &&
	      v34_phase2_receive_gain(
		      &pair.answerer, &answerer_receive_gain) == V34_PHASE2_OK &&
	      caller_receive_gain == initial_caller_receive_gain &&
	      answerer_receive_gain == initial_answerer_receive_gain &&
	      measurement_is_finite(&caller_measurement) &&
	      measurement_is_finite(&answerer_measurement),
	      "INFO1 handoff discarded probe measurements");
	for (size_t index = 0; index < TEST_TONE_SAMPLES; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit) &&
		      caller_transmit == 0 && answerer_transmit == 0 &&
		      caller_event.kind == V34_PHASE2_EVENT_NONE &&
		      answerer_event.kind == V34_PHASE2_EVENT_NONE,
		      "completed INFO1 owner was not silent and stable");
	}
	return true;
}

static bool test_generated_timelines(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
		bool zero_md;
	} cases[] = {
		{ TEST_CODEC_LINEAR, 1, true },
		{ TEST_CODEC_PCMU, 13, false },
		{ TEST_CODEC_PCMA, 47, false },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_timeline(
			      cases[index].codec,
			      cases[index].delay_samples,
			      cases[index].zero_md),
		      "generated Phase 2 case %zu failed", index);
	return true;
}

static bool check_retrain_timeline(bool late_alias)
{
	struct v34_info0 caller_info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 answerer_info0 = test_v34_phase2_capabilities(
		V34_ANSWERING_STATION);
	struct v34_info0 caller_peer_info0;
	struct v34_info0 answerer_peer_info0;
	struct owned_pair pair;
	uint16_t caller_rtd = 0;
	uint16_t answerer_rtd = 0;
	bool answerer_late_alias_injected = false;
	bool answerer_concurrent_receive_armed = false;
	bool caller_trial_rejected = false;
	bool ready = false;

	memset(&pair, 0, sizeof(pair));
	CHECK(test_v34_phase2_pair_init(
		      &pair.channel, TEST_CODEC_PCMA, 47) &&
	      v34_phase2_init_retrain(
			      &pair.caller, V34_CALLING_STATION, TEST_AMPLITUDE,
			      &caller_info0, &answerer_info0,
			      RETRAIN_PRIOR_RTD_SAMPLES) == V34_PHASE2_OK &&
	      v34_phase2_init_retrain(
			      &pair.answerer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
			      &answerer_info0, &caller_info0,
			      RETRAIN_PRIOR_RTD_SAMPLES) == V34_PHASE2_OK,
	      "Phase 2 retrain pair did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		if (late_alias && !answerer_late_alias_injected &&
		    pair.answerer.stage == V34_PHASE2_RANGING &&
		    pair.answerer.state.ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    pair.answerer.state.ranging.control
			    .measurement_elapsed_samples != 0) {
			pair.answerer.state.ranging.control
				.measurement_elapsed_samples +=
				RETRAIN_LATE_ALIAS_SAMPLES;
			CHECK(v34_phase2_is_valid(&pair.answerer),
			      "late-reversal alias invalidated the answerer");
			answerer_late_alias_injected = true;
		}
		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "Phase 2 retrain pair failed at sample %zu", index);
		if (caller_event.kind == V34_PHASE2_EVENT_INITIAL_PROBE_STARTED)
			caller_rtd = caller_event.round_trip_delay_samples;
		if (answerer_event.kind == V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
			answerer_rtd = answerer_event.round_trip_delay_samples;
			answerer_concurrent_receive_armed =
				pair.answerer.concurrent_probe_receive &&
				pair.answerer.state.initial_probe
					.concurrent_receive_enabled;
		}
		if (pair.caller.stage == V34_PHASE2_RANGING &&
		    pair.caller.state.ranging.retrain_probe.phase ==
			    V34_PHASE2_RETRAIN_PROBE_REJECTED)
			caller_trial_rejected = true;
		if (v34_phase2_is_ready_for_info1(&pair.caller) &&
		    v34_phase2_is_ready_for_info1(&pair.answerer)) {
			ready = true;
			break;
		}
	}
	CHECK(ready && answerer_late_alias_injected == late_alias &&
	      answerer_concurrent_receive_armed &&
	      caller_trial_rejected &&
	      caller_rtd == RETRAIN_PRIOR_RTD_SAMPLES &&
	      answerer_rtd == RETRAIN_PRIOR_RTD_SAMPLES &&
	      !v34_phase2_has_failed(&pair.caller) &&
	      !v34_phase2_has_failed(&pair.answerer) &&
	      v34_phase2_peer_info0(
		      &pair.caller, &caller_peer_info0) == V34_PHASE2_OK &&
	      v34_phase2_peer_info0(
		      &pair.answerer, &answerer_peer_info0) == V34_PHASE2_OK &&
	      v34_info0_equal(&caller_peer_info0, &answerer_info0) &&
	      v34_info0_equal(&answerer_peer_info0, &caller_info0),
	      "Phase 2 retrain did not preserve INFO0 through ranging");
	return true;
}

static bool test_retrain_timeline(void)
{
	/* The short generated line measures less than the retained RTD. Adding
	 * an extra reversal interval makes the opposite alias; both must retain
	 * the established delay and listen for an overlapping peer probe. */
	CHECK(check_retrain_timeline(false), "early retrain alias failed");
	CHECK(check_retrain_timeline(true), "late retrain alias failed");
	return true;
}

static bool check_concurrent_trial(bool retraining)
{
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_probe_sequence peer_probe;
	struct owned_pair pair;
	struct v34_phase2 empty_trial;
	uint16_t measured_round_trip_delay = 0;
	int16_t receive_gain = 0;
	bool delay_injected = false;
	bool armed = false;
	bool disabled = false;
	bool received = false;
	bool handed_off = false;

	CHECK(pair_init(&pair, TEST_CODEC_LINEAR, 1),
	      "concurrent-trial pair did not initialize");
	if (retraining) {
		struct v34_info0 caller = test_v34_phase2_capabilities(
			V34_CALLING_STATION);
		struct v34_info0 answerer = test_v34_phase2_capabilities(
			V34_ANSWERING_STATION);

		CHECK(v34_phase2_init_retrain(
			      &pair.caller, V34_CALLING_STATION, TEST_AMPLITUDE,
			      &caller, &answerer, RETRAIN_PRIOR_RTD_SAMPLES) ==
			      V34_PHASE2_OK &&
		      v34_phase2_init_retrain(
			      &pair.answerer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
			      &answerer, &caller, RETRAIN_PRIOR_RTD_SAMPLES) ==
			      V34_PHASE2_OK,
		      "concurrent retrain pair did not initialize");
	}
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		if (!retraining && !delay_injected &&
		    pair.answerer.stage == V34_PHASE2_RANGING &&
		    pair.answerer.state.ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    pair.answerer.state.ranging.control
			    .measurement_elapsed_samples != 0) {
			pair.answerer.state.ranging.control
				.measurement_elapsed_samples +=
				INITIAL_CONCURRENT_DELAY_SAMPLES;
			CHECK(v34_phase2_is_valid(&pair.answerer),
			      "initial delay invalidated the answerer");
			delay_injected = true;
		}
		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "initial concurrent trial stopped at sample %zu", index);
		if (answerer_event.kind ==
		    V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
			measured_round_trip_delay =
				answerer_event.round_trip_delay_samples;
			armed = pair.answerer.concurrent_probe_receive &&
				pair.answerer.state.initial_probe
					.concurrent_receive_enabled;
			break;
		}
	}
	CHECK(delay_injected == !retraining && armed &&
	      measured_round_trip_delay >=
		      V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES,
	      "answerer concurrent trial was not armed");
	empty_trial = pair.answerer;
	for (size_t index = 0;
	     index <= V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	     ++index) {
		struct v34_phase2_event event;
		int16_t transmit;

		CHECK(v34_phase2_sample(
			      &empty_trial, 0, &transmit, &event) ==
			      V34_PHASE2_OK,
		      "empty initial concurrent trial stopped at %zu", index);
		if (!empty_trial.concurrent_probe_receive) {
			disabled = true;
			break;
		}
	}
	CHECK(disabled == !retraining &&
	      empty_trial.state.initial_probe.concurrent_receive_enabled ==
		      retraining &&
	      !empty_trial.remote_probe_stored &&
	      v34_phase2_is_valid(&empty_trial) &&
	      !v34_phase2_has_failed(&empty_trial),
	      "empty concurrent trial did not preserve its receive policy");
	CHECK(v34_phase2_probe_sequence_init(
		      &peer_probe, TEST_AMPLITUDE,
		      measured_round_trip_delay) ==
		      V34_PHASE2_PROBE_SEQUENCE_OK,
	      "initial overlapping-probe source did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_probe_sequence_event peer_event;
		struct v34_phase2_probe_acquisition_event receive_event;
		int16_t peer_transmit;

		CHECK(v34_phase2_probe_sequence_sample(
			      &peer_probe, false, &peer_transmit,
			      &peer_event) == V34_PHASE2_PROBE_SEQUENCE_OK &&
		      v34_phase2_initial_probe_sample_concurrent(
			      &pair.answerer.state.initial_probe,
			      peer_transmit, &receive_event) ==
			      PHASE2_INITIAL_PROBE_OK,
		      "overlapping probe stopped at %zu", index);
		/* The initial-call trial must not adopt a later returned copy of
		 * the local probe after its echo-safe capture has expired. */
		if (!retraining)
			CHECK(v34_phase2_initial_probe_sample_concurrent(
				      &empty_trial.state.initial_probe,
				      peer_transmit, &receive_event) ==
				      PHASE2_INITIAL_PROBE_OK &&
			      !empty_trial.state.initial_probe.concurrent_receive_enabled &&
			      !empty_trial.remote_probe_stored,
			      "expired initial trial adopted delayed echo at %zu", index);
		if (pair.answerer.state.initial_probe.concurrent_receive.phase ==
		    V34_PHASE2_PROBE_ACQUISITION_COMPLETE) {
			received = true;
			break;
		}
	}
	CHECK(received && v34_phase2_is_valid(&pair.answerer),
	      "answerer did not retain its overlapping probe");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "initial concurrent handoff stopped at sample %zu", index);
		if (answerer_event.kind ==
		    V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED) {
			handed_off = true;
			break;
		}
	}
	CHECK(handed_off && pair.answerer.remote_probe_stored &&
	      v34_phase2_is_ready_for_info1(&pair.answerer) &&
	      v34_phase2_measure_remote_probe(
		      &pair.answerer, &measurement) == V34_PHASE2_OK &&
	      v34_phase2_receive_gain(
		      &pair.answerer, &receive_gain) == V34_PHASE2_OK &&
	      measurement_is_finite(&measurement) && receive_gain > 0,
	      "concurrent measurement did not cross the reciprocal handoff");
	return true;
}

static bool test_concurrent_trials(void)
{
	CHECK(check_concurrent_trial(false), "initial concurrent trial failed");
	CHECK(check_concurrent_trial(true), "short-alias concurrent trial failed");
	return true;
}

static bool check_retrain_probe_boundary(bool delayed_probe)
{
	struct v34_info0 caller_info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 answerer_info0 = test_v34_phase2_capabilities(
		V34_ANSWERING_STATION);
	struct v34_phase2_tone_transmitter answer_tone;
	struct v34_phase2_probe_transmitter answer_probe;
	struct v34_phase2 caller;
	size_t tail_samples = 0;
	bool first_reversed = false;
	bool second_reversed = false;
	bool first_trial_rejected = false;
	bool retry_started = false;
	bool probing = false;
	bool handed_off = false;

	CHECK(v34_phase2_init_retrain(
		      &caller, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &caller_info0, &answerer_info0,
		      RETRAIN_PRIOR_RTD_SAMPLES) == V34_PHASE2_OK &&
	      v34_phase2_tone_transmitter_init(
		      &answer_tone, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_TONE_OK &&
	      v34_phase2_probe_transmitter_init(
		      &answer_probe, TEST_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK,
	      "second-reversal retrain fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event event;
		int16_t received;
		int16_t transmit;

		if (!first_reversed && caller.stage == V34_PHASE2_RANGING &&
		    caller.state.ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &answer_tone) == V34_PHASE2_TONE_OK,
			      "retrain fixture did not reverse Tone A");
			first_reversed = true;
		}
		if (delayed_probe && !second_reversed &&
		    caller.stage == V34_PHASE2_RANGING &&
		    caller.state.ranging.control.phase ==
			    PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL &&
		    caller.state.ranging.retrain_probe.phase ==
			    V34_PHASE2_RETRAIN_PROBE_REJECTED) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &answer_tone) == V34_PHASE2_TONE_OK,
			      "delayed retrain fixture did not start its L1 edge");
			second_reversed = true;
		}
		if (!probing) {
			CHECK(v34_phase2_tone_transmitter_sample(
				      &answer_tone, &received) ==
				      V34_PHASE2_TONE_OK,
			      "retrain Tone A generation failed");
			if (!delayed_probe && first_reversed &&
			    ++tail_samples ==
				    PHASE2_RANGING_TONE_TAIL_SAMPLES)
				probing = true;
		} else {
			CHECK(v34_phase2_probe_transmitter_sample(
				      &answer_probe, V34_PHASE2_PROBE_L1,
				      &received) == V34_PHASE2_PROBE_OK,
			      "second-reversal L1 generation failed");
		}
		CHECK(v34_phase2_sample(
			      &caller, received, &transmit, &event) ==
			      V34_PHASE2_OK,
		      "retrain probe boundary stopped at sample %zu", index);
		if (caller.stage == V34_PHASE2_RANGING &&
		    caller.state.ranging.retrain_probe.phase ==
			    V34_PHASE2_RETRAIN_PROBE_REJECTED)
			first_trial_rejected = true;
		if (delayed_probe && second_reversed && !probing &&
		    caller.stage == V34_PHASE2_RANGING &&
		    caller.state.ranging.retrain_probe
			    .early_boundary_retry_used) {
			retry_started = true;
			probing = true;
		}
		if (event.kind == V34_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
			const struct v34_phase2_initial_probe *initial =
				&caller.state.initial_probe;

			CHECK(event.round_trip_delay_samples ==
				      RETRAIN_PRIOR_RTD_SAMPLES &&
			      transmit == 0 &&
			      initial->ranging.retrain_probe.phase ==
				      V34_PHASE2_RETRAIN_PROBE_ACQUIRED &&
			      initial->ranging.retrain_probe
				      .early_boundary_retry_used == delayed_probe &&
			      initial->control.path.receive.phase ==
				      V34_PHASE2_PROBE_ACQUISITION_RECEIVING &&
			      initial->control.path.receive.receiver.l1_spectrum
				      .samples_observed >=
				      V34_PHASE2_PROBE_PERIOD_SAMPLES,
			      "second reversal did not retain its L1 prefix");
			handed_off = true;
			break;
		}
	}
	CHECK(first_reversed && probing && handed_off &&
	      second_reversed == delayed_probe &&
	      first_trial_rejected == delayed_probe &&
	      retry_started == delayed_probe &&
	      !v34_phase2_has_failed(&caller),
	      "retrain probe boundary was not recovered (delayed=%d)",
	      delayed_probe);
	return true;
}

static bool test_retrain_probe_boundaries(void)
{
	CHECK(check_retrain_probe_boundary(false),
	      "first observed retrain boundary failed");
	CHECK(check_retrain_probe_boundary(true),
	      "delayed retrain boundary failed");
	return true;
}

static bool test_validation(void)
{
	struct v34_info0 info0 = test_v34_phase2_capabilities(
		V34_CALLING_STATION);
	struct v34_info0 invalid_info0 = info0;
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_probe_receiver_measurement saved_measurement;
	struct v34_phase2 phase2;
	struct v34_phase2 saved;
	struct v34_phase2_event event;
	struct v34_phase2_event saved_event;
	struct owned_pair pair;
	int16_t transmit = 1234;
	int16_t receive_gain = 4321;
	bool ranging_active = false;

	memset(&phase2, 0xa5, sizeof(phase2));
	saved = phase2;
	invalid_info0.maximum_symbol_rate_difference_steps = 8;
	CHECK(v34_phase2_init(
		      &phase2, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &invalid_info0) == V34_PHASE2_INVALID_ARGUMENT &&
	      memcmp(&phase2, &saved, sizeof(phase2)) == 0,
	      "invalid Phase 2 initialization changed its destination");
	CHECK(v34_phase2_init(
		      &phase2, V34_CALLING_STATION, TEST_AMPLITUDE,
		      &info0) == V34_PHASE2_OK,
	      "validation endpoint did not initialize");
	saved = phase2;
	phase2.concurrent_probe_receive = true;
	CHECK(!v34_phase2_is_valid(&phase2),
	      "initial caller accepted concurrent retrain reception");
	phase2 = saved;
	saved = phase2;
	memset(&event, 0x5a, sizeof(event));
	saved_event = event;
	memset(&measurement, 0x6b, sizeof(measurement));
	saved_measurement = measurement;
	CHECK(v34_phase2_sample(&phase2, INT16_MIN, NULL, &event) ==
		      V34_PHASE2_INVALID_ARGUMENT &&
	      v34_phase2_measure_remote_probe(&phase2, &measurement) ==
		      V34_PHASE2_INVALID_STATE &&
	      v34_phase2_receive_gain(&phase2, &receive_gain) ==
		      V34_PHASE2_INVALID_STATE &&
	      memcmp(&phase2, &saved, sizeof(phase2)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0 &&
	      memcmp(&measurement, &saved_measurement,
		     sizeof(measurement)) == 0 &&
	      receive_gain == 4321 &&
	      transmit == 1234,
	      "Phase 2 validation was not transactional");
	CHECK(pair_init(&pair, TEST_CODEC_LINEAR, 1),
	      "failure pair did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "failure pair stopped at %zu", index);
		if (pair.answerer.stage == V34_PHASE2_RANGING &&
		    pair.answerer.state.ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			ranging_active = true;
			break;
		}
	}
	CHECK(ranging_active, "failure pair did not enter active ranging");
	phase2 = pair.answerer;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		CHECK(v34_phase2_sample(&phase2, 0, &transmit, &event) ==
			      V34_PHASE2_OK,
		      "isolated ranging failed at %zu", index);
		CHECK(event.kind != V34_PHASE2_EVENT_FAILED,
		      "missing ranging response aborted instead of reacquiring Tone B");
	}
	CHECK(!v34_phase2_has_failed(&phase2) &&
	      phase2.stage == V34_PHASE2_RANGING &&
	      phase2.state.ranging.control.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
	      phase2.state.ranging.control.measurement_elapsed_samples == 0,
	      "Phase 2 did not retain the first-reversal recovery wait");
	return true;
}

static bool test_probe_recovery(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU; codec <= TEST_CODEC_PCMA; ++codec) {
		struct owned_pair pair;
		struct v34_phase2_event caller_event, event;
		int16_t caller_transmit, transmit;
		bool started = false;

		CHECK(pair_init(&pair, codec, 13u), "probe recovery pair did not initialize");
		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			CHECK(pair_sample(&pair, &caller_event, &event,
					  &caller_transmit, &transmit), "probe recovery setup failed");
			if (pair.answerer.stage == V34_PHASE2_INITIAL_PROBE) {
				started = true;
				break;
			}
		}
		CHECK(started, "probe recovery did not reach L1");
		uint16_t delay = pair.answerer.state.initial_probe.ranging.control
			.round_trip_delay_samples;
		struct v34_info0 peer = pair.answerer.peer_info0;
		uint32_t deadline = (160u + 600u) * 8u + delay;

		for (uint32_t elapsed = 1u; elapsed <= deadline; ++elapsed)
			CHECK(v34_phase2_sample(&pair.answerer, 0, &transmit, &event) ==
				      V34_PHASE2_OK && !v34_phase2_has_failed(&pair.answerer) &&
			      pair.answerer.stage == (elapsed < deadline ?
				      V34_PHASE2_INITIAL_PROBE : V34_PHASE2_RANGING),
			      "answerer probe recovery failed at %u/%u", elapsed, deadline);
		const struct v34_phase2_ranging *ranging = &pair.answerer.state.ranging;
		struct v34_phase2_tone_transmitter expected =
			ranging->info0_exchange.transport.tone_transmitter;

		CHECK(ranging->control.phase == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		      ranging->control.prior_round_trip_delay_samples == 0 &&
		      ranging->control.local_tone_samples == 0u &&
		      !ranging->control.round_trip_delay_valid &&
		      !ranging->info0_exchange.transport.tone_detector.carrier.tone_present &&
		      !pair.answerer.remote_probe_stored && pair.answerer.peer_info0_stored &&
		      memcmp(&peer, &pair.answerer.peer_info0, sizeof(peer)) == 0,
		      "answerer recovery lost INFO0 or retained a stale ranging epoch");
		for (size_t index = 0; index < 400u; ++index) {
			int16_t tone;

			CHECK(v34_phase2_tone_transmitter_sample(&expected, &tone) == V34_PHASE2_TONE_OK &&
			      v34_phase2_sample(&pair.answerer, 0, &transmit, &event) == V34_PHASE2_OK &&
			      transmit == tone, "answerer recovery did not send fresh Tone A");
		}
	}
	return true;
}

static bool test_info1_recovery(void)
{
	struct v34_info1c local_info1c = caller_info1c(false);
	struct owned_pair pair;
	struct v34_phase2_event event;
	int16_t transmit;
	bool ready = false;
	bool recovering = false;

	CHECK(pair_init(&pair, TEST_CODEC_LINEAR, 1),
	      "INFO1 timeout pair did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_event caller_event;
		struct v34_phase2_event answerer_event;
		int16_t caller_transmit;
		int16_t answerer_transmit;

		CHECK(pair_sample(
			      &pair, &caller_event, &answerer_event,
			      &caller_transmit, &answerer_transmit),
		      "INFO1 timeout pair stopped at %zu", index);
		if (v34_phase2_is_ready_for_info1(&pair.caller)) {
			ready = true;
			break;
		}
	}
	CHECK(ready && v34_phase2_start_caller_info1(
			       &pair.caller, &local_info1c) == V34_PHASE2_OK,
	      "caller did not reach the isolated INFO1 test");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		CHECK(v34_phase2_sample(
			      &pair.caller, 0, &transmit, &event) ==
			      V34_PHASE2_OK,
		      "isolated INFO1 failed at %zu", index);
		if (event.kind == V34_PHASE2_EVENT_INFO1_RECOVERY_STARTED) {
			recovering = true;
			break;
		}
	}
	CHECK(recovering && !v34_phase2_has_failed(&pair.caller) &&
	      pair.caller.state.info1.control.phase ==
		      PHASE2_INFO1_RECOVERING,
	      "Phase 2 did not enter INFO1 recovery at the peer deadline");
	for (size_t index = 0; index < V34_PHASE2_SAMPLE_RATE; ++index)
		CHECK(v34_phase2_sample(
			      &pair.caller, 0, &transmit, &event) ==
			      V34_PHASE2_OK &&
		      transmit == 0 && event.kind == V34_PHASE2_EVENT_NONE &&
		      !v34_phase2_has_failed(&pair.caller),
		      "INFO1 recovery was not silent and stable at %zu", index);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated probe timelines", test_generated_timelines },
		{ "concurrent probe trials", test_concurrent_trials },
		{ "generated retrain timeline", test_retrain_timeline },
		{ "retrain probe boundaries", test_retrain_probe_boundaries },
		{ "validation", test_validation },
		{ "initial probe ranging recovery", test_probe_recovery },
		{ "INFO1 recovery", test_info1_recovery },
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
