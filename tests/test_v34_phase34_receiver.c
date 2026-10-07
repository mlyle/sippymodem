// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_mapping.h>
#include <v34_data_stream.h>
#include <v34_mapping_parameters.h>
#include <v34_phase34_receiver.h>
#include <v34_training_transmitter.h>

#include "test.h"
#include "v34_stream_fixture.h"
#include "v34_phase34_state_fixture.h"

#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
	PHASE3_TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 128,
	PHASE3_J_SYMBOLS = V34_PHASE3_J_PATTERN_SYMBOLS * 5,
	PHASE4_TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 384,
	SAMPLE_LIMIT = 40000,
};

struct line_source {
	struct v34_training_transmitter training;
	struct v34_data_stream_preparation preparation;
	struct v34_data_stream data;
	struct v34_data_direction_parameters parameters;
	struct v34_mp_parameters initial_mp;
	struct v34_mp_parameters final_mp;
	unsigned int completed_records;
	bool data_started;
};

static struct v34_data_direction_parameters data_parameters(uint8_t rate)
{
	return (struct v34_data_direction_parameters){
		.precoder_coefficients = {
			{ 0, 0 },
			{ 0, 0 },
			{ 0, 0 },
		},
		.trellis_selection = V34_TRELLIS_16_STATE,
		.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO,
		.primary_data_rate_2400_bps_units = rate,
	};
}

static bool profile(
	struct v34_phase3_profile *selected, enum v34_station_role role,
	enum v34_symbol_rate symbol_rate, uint8_t rate, bool high_carrier,
	uint8_t preemphasis_filter_index)
{
	const struct v34_info1_probe_result probe = {
		.high_carrier = high_carrier,
		.preemphasis_filter_index = preemphasis_filter_index,
		.projected_data_rate_2400_bps_units = rate,
	};

	return v34_phase3_profile_init(
		       selected, role, symbol_rate, &probe, 0, 0) ==
	       V34_PHASE3_PROFILE_OK;
}

static struct v34_mp_parameters mp_parameters(
	uint8_t rate, bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = rate,
		.maximum_answer_to_call_rate_2400_bps_units = rate,
		.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
		.acknowledges_peer_mp = acknowledges_peer,
		.data_rate_capability_mask = 0x1555,
	};
}

static bool received_all_ones(
	const struct v34_received_mapping_frame *received)
{
	for (size_t bit = 0; bit < received->primary_bit_count; ++bit)
		if (received->primary_bits[bit] != 1u)
			return false;
	return received->auxiliary_bit_count == 0u ||
	       received->auxiliary_bit == 1u;
}

static bool sample_preserves_state(
	const struct v34_phase34_receiver *receiver,
	enum v34_phase34_receiver_result expected)
{
	struct v34_phase34_receiver next = *receiver;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	CHECK(v34_phase34_receiver_sample(&next, 123, &output) == expected &&
	      v34_phase34_receivers_equal(&next, receiver) &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "terminal sample changed state or output (expected %u)", expected);
	return true;
}

static bool test_terminal_samples(const struct v34_phase34_receiver *receiver)
{
	struct v34_phase34_receiver next = *receiver;

	CHECK(v34_phase34_receiver_is_valid(&next),
	      "terminal fixture was not valid");
	if (next.stage == V34_PHASE34_RECEIVER_PHASE4) {
		CHECK(v34_phase34_receiver_is_complete(&next) &&
		      sample_preserves_state(&next, V34_PHASE34_RECEIVER_COMPLETE),
		      "completed training consumed another sample");
	}
	next.failed = true;
	CHECK(v34_phase34_receiver_has_failed(&next) &&
	      sample_preserves_state(&next, V34_PHASE34_RECEIVER_FAILED),
	      "failed receiver consumed a sample or reported completion");
	if (next.stage == V34_PHASE34_RECEIVER_PHASE4)
		next.semantic.phase4.mp_receiver.previous_clockwise_quarter_turns = 4;
	else
		next.semantic.data.data_receiver.channel_receiver.frame_decoder
			.channel_decoder.survivor.path_index =
			V34_SURVIVOR_TRACEBACK_INTERVALS;
	CHECK(!v34_phase34_receiver_is_valid(&next) &&
	      sample_preserves_state(&next, V34_PHASE34_RECEIVER_INVALID_STATE),
	      "failed receiver hid invalid active child state");
	next.failed = false;
	CHECK(sample_preserves_state(&next, V34_PHASE34_RECEIVER_INVALID_STATE),
	      "completion or data stage hid invalid active child state");
	return true;
}

static bool source_init(
	struct line_source *source,
	const struct v34_phase3_profile *transmitting_profile, uint8_t rate)
{
	int16_t nominal_peak_amplitude =
		transmitting_profile->symbol_rate == V34_SYMBOL_RATE_2800 &&
			!transmitting_profile->high_carrier ?
			7000 :
			5000;
	struct line_source initialized = {
		.parameters = data_parameters(rate),
		.initial_mp = mp_parameters(rate, false),
		.final_mp = mp_parameters(rate, true),
	};

	if (v34_training_transmitter_init(
		    &initialized.training, transmitting_profile,
		    V34_CONSTELLATION_16_POINT, SAMPLE_RATE,
		    nominal_peak_amplitude) !=
		    V34_TRAINING_TRANSMITTER_OK ||
	    v34_training_transmitter_select_phase4_constellation(
		    &initialized.training, V34_CONSTELLATION_16_POINT) !=
		    V34_TRAINING_TRANSMITTER_OK ||
	    v34_prepare_stream_fixture(
		    &initialized.preparation, transmitting_profile,
		    &initialized.parameters, false, SAMPLE_RATE) !=
		    V34_DATA_STREAM_OK)
		return false;
	*source = initialized;
	return true;
}

static bool prepare_source(struct line_source *source)
{
	struct v34_training_transmitter *training = &source->training;

	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    training->phase3.trn_symbols == PHASE3_TRN_SYMBOLS)
		return v34_training_transmitter_start_j(training) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_TRANSMITTER_J &&
	    training->phase3.j_symbols == PHASE3_J_SYMBOLS)
		return v34_training_transmitter_finish_j(training) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN &&
	    training->phase4_trn_symbols == PHASE4_TRN_SYMBOLS)
		return v34_training_transmitter_start_mp_type0(
			       training, &source->initial_mp) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (v34_training_transmitter_phase4_mp_record_complete(training)) {
		source->completed_records++;
		if (source->completed_records >= 3u)
			return v34_training_transmitter_start_e(training) ==
			       V34_TRAINING_TRANSMITTER_OK;
		return v34_training_transmitter_start_mp_type0(
			       training,
			       source->completed_records == 2u ?
				       &source->final_mp :
				       &source->initial_mp) ==
		       V34_TRAINING_TRANSMITTER_OK;
	}
	return true;
}

static bool source_sample(struct line_source *source, int16_t *sample)
{
	if (v34_training_transmitter_phase4_e_complete(&source->training)) {
		bool accepted;

		if (!source->data_started) {
			if (v34_data_stream_continue_prepared_b1(
				    &source->data, &source->training.line,
				    &source->preparation) != V34_DATA_STREAM_OK)
				return false;
			source->data_started = true;
		}
		return v34_data_stream_sample(
			       &source->data, NULL, NULL, sample, &accepted) ==
		       V34_DATA_STREAM_OK;
	}
	return prepare_source(source) &&
	       v34_training_transmitter_sample(&source->training, sample) ==
		       V34_TRAINING_TRANSMITTER_OK;
}

static bool test_validation(void)
{
	const struct v34_info1_probe_result reduced_probe = {
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
	};
	struct v34_phase3_profile valid;
	struct v34_phase3_profile invalid;
	struct v34_phase3_profile reduced;
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receiver saved;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	CHECK(profile(
		      &valid, V34_CALLING_STATION, V34_SYMBOL_RATE_3000, 8,
		      true, 3) &&
	      v34_phase3_profile_init(
		      &reduced, V34_CALLING_STATION, V34_SYMBOL_RATE_2800,
		      &reduced_probe, 0, 0) == V34_PHASE3_PROFILE_OK,
	      "validation profile did not initialize");
	invalid = valid;
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	invalid.symbol_rate_denominator = 0;
	CHECK(v34_phase34_receiver_init(
		      NULL, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_init(
		      &receiver, &invalid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receivers_equal(&receiver, &saved) &&
	      v34_phase34_receiver_init(
		      &receiver, &valid, (enum v34_constellation_size)8,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT, 0) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receivers_equal(&receiver, &saved),
	      "invalid initialization changed its destination");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      receiver.line.timing.gain.gain == TEST_INITIAL_RECEIVE_GAIN &&
	      receiver.line.timing.gain.mode == V34_GAIN_FROZEN,
	      "valid receiver did not initialize");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &reduced, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      receiver.line.timing.gain.gain == TEST_INITIAL_RECEIVE_GAIN,
	      "profile-independent receive gain changed");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "validation receiver did not restore");
	saved = receiver;
	memset(&output, 0x6b, sizeof(output));
	saved_output = output;
	for (unsigned int sample = 0; sample < 37u; ++sample)
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "leading silence failed at sample %u", sample);
	CHECK(!v34_receive_timing_is_acquired(&receiver.line.timing) &&
	      receiver.line.timing.gain.gain == TEST_INITIAL_RECEIVE_GAIN &&
	      receiver.line.timing.gain.mode == V34_GAIN_FROZEN &&
	      memcmp(&receiver.line.equalizer, &saved.line.equalizer,
		     sizeof(receiver.line.equalizer)) == 0 &&
	      memcmp(&receiver.semantic, &saved.semantic,
		     sizeof(receiver.semantic)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "leading silence advanced semantic reception or output");
	saved = receiver;
	CHECK(v34_phase34_receiver_sample(&receiver, 1, NULL) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_sample(
		      &receiver, 1,
		      (struct v34_phase34_receive_output *)&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_rearm_phase4_s(&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receiver_set_data_erasure(NULL, true) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receiver_set_data_erasure(&receiver, true) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receiver_continue_mp(&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receivers_equal(&receiver, &saved),
	      "invalid controls changed an incomplete receiver");
	return true;
}

static bool test_data_erasure(
	struct v34_phase34_receiver *receiver, struct line_source *source,
	enum v34_station_role role, uint8_t rate)
{
	struct v34_phase34_receiver saved;
	struct v34_phase34_receiver pending;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;
	struct v34_adaptive_equalizer_observation observation;

	saved = *receiver;
	CHECK(v34_phase34_receiver_set_data_erasure(receiver, false) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receivers_equal(receiver, &saved) &&
	      v34_phase34_receiver_set_data_erasure(receiver, true) ==
		      V34_PHASE34_RECEIVER_OK &&
	      receiver->data_erasure,
	      "role %u rate %u did not enter data erasure", role, rate);
	saved = *receiver;
	memset(&output, 0x3a, sizeof(output));
	saved_output = output;
	for (unsigned int index = 0; index < 320u; ++index) {
		int16_t sample;

		/* Lose 10 ms of input, then retain the erasure guard through
		 * the receive filters' tail before resuming adaptation. */
		CHECK(source_sample(source, &sample) &&
		      v34_phase34_receiver_sample(receiver, index < 80u ? 0 : sample, &output) ==
			      V34_PHASE34_RECEIVER_OK &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "role %u rate %u data erasure failed at sample %u", role,
		      rate, index);
	}
	CHECK(!v34_data_decision_receivers_equal(
		      &receiver->semantic.data, &saved.semantic.data) &&
	      receiver->tracking_symbols == saved.tracking_symbols &&
	      memcmp(&receiver->line.equalizer.feedback, &saved.line.equalizer.feedback,
		     sizeof(receiver->line.equalizer.feedback)) == 0 &&
	      memcmp(&receiver->line.equalizer.quality, &saved.line.equalizer.quality,
		     sizeof(receiver->line.equalizer.quality)) == 0 &&
	      memcmp(receiver->line.equalizer.equalizer.in_phase_coefficient,
		     saved.line.equalizer.equalizer.in_phase_coefficient,
		     sizeof(receiver->line.equalizer.equalizer
				    .in_phase_coefficient)) == 0 &&
	      memcmp(receiver->line.equalizer.equalizer.quadrature_coefficient,
		     saved.line.equalizer.equalizer.quadrature_coefficient,
		     sizeof(receiver->line.equalizer.equalizer
				    .quadrature_coefficient)) == 0 &&
	      memcmp(receiver->line.equalizer.equalizer.in_phase_residue,
		     saved.line.equalizer.equalizer.in_phase_residue,
		     sizeof(receiver->line.equalizer.equalizer.in_phase_residue)) == 0 &&
	      memcmp(receiver->line.equalizer.equalizer.quadrature_residue,
		     saved.line.equalizer.equalizer.quadrature_residue,
		     sizeof(receiver->line.equalizer.equalizer
				    .quadrature_residue)) == 0 &&
	      receiver->line.equalizer.phase_accumulator ==
		      saved.line.equalizer.phase_accumulator &&
	      receiver->line.equalizer.phase_error == saved.line.equalizer.phase_error &&
	      receiver->line.equalizer.rotate_in_phase ==
		      saved.line.equalizer.rotate_in_phase &&
	      receiver->line.equalizer.rotate_quadrature ==
		      saved.line.equalizer.rotate_quadrature &&
	      receiver->line.equalizer.carrier_phase_low ==
		      saved.line.equalizer.carrier_phase_low &&
	      receiver->line.equalizer.carrier_phase_fraction ==
		      saved.line.equalizer.carrier_phase_fraction &&
	      receiver->line.equalizer.epoch > saved.line.equalizer.epoch &&
	      receiver->line.equalizer.lineage != saved.line.equalizer.lineage &&
	      memcmp(&receiver->line.timing, &saved.line.timing,
		     sizeof(receiver->line.timing)) != 0 &&
	      !receiver->line.equalizer.decision_pending &&
	      v34_phase34_receiver_is_valid(receiver),
	      "role %u rate %u data erasure adapted or stopped the mapping clock",
	      role, rate);
	CHECK(v34_phase34_receiver_set_data_erasure(receiver, false) ==
		      V34_PHASE34_RECEIVER_OK &&
	      !receiver->data_erasure &&
	      v34_phase34_receiver_is_valid(receiver),
	      "role %u rate %u did not leave data erasure", role, rate);
	unsigned int recovered_frames = 0;
	for (unsigned int index = 0; index < 1600u; ++index) {
		int16_t sample;
		CHECK(source_sample(source, &sample), "erasure recovery source failed");
		enum v34_phase34_receiver_result result =
			v34_phase34_receiver_sample(receiver, sample, &output);
		CHECK(result == V34_PHASE34_RECEIVER_OK ||
		      result == V34_PHASE34_RECEIVER_INTERVAL_READY ||
		      result == V34_PHASE34_RECEIVER_MAPPING_FRAME_READY,
		      "role %u rate %u erasure recovery failed at sample %u: %u",
		      role, rate, index, result);
		if (index >= 800u && result == V34_PHASE34_RECEIVER_MAPPING_FRAME_READY) {
			CHECK(received_all_ones(&output.received),
			      "role %u rate %u lost framing after erasure", role, rate);
			recovered_frames++;
		}
	}
	CHECK(recovered_frames >= 10u, "erasure recovery produced no mapped data");

	pending = *receiver;
	while (pending.line.equalizer.inputs_since_observation <
	       V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION)
		CHECK(v34_adaptive_equalizer_push(
			      &pending.line.equalizer,
			      (struct v34_analytic_sample){ 0, 0 }) ==
			      V34_ADAPTIVE_EQUALIZER_OK,
		      "role %u rate %u could not prepare a pending decision", role,
		      rate);
	CHECK(v34_adaptive_equalizer_observe(
		      &pending.line.equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      v34_phase34_receiver_is_valid(&pending),
	      "role %u rate %u pending receiver became invalid", role, rate);
	saved = pending;
	CHECK(v34_phase34_receiver_set_data_erasure(&pending, true) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receivers_equal(&pending, &saved),
	      "role %u rate %u pending erasure changed state", role, rate);
	return true;
}

static bool test_tone_owned_acquisition(void)
{
	struct v34_phase3_profile transmitting_profile;
	struct v34_phase34_receiver receiver;
	struct v34_adaptive_equalizer saved_equalizer;
	union v34_phase34_receiver_semantic_state saved_semantic;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	CHECK(profile(
		      &transmitting_profile, V34_CALLING_STATION,
		      V34_SYMBOL_RATE_3000, 8, true, 3) &&
	      v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "tone-owned receiver did not initialize");
	saved_equalizer = receiver.line.equalizer;
	saved_semantic = receiver.semantic;
	memset(&output, 0x6d, sizeof(output));
	saved_output = output;
	for (unsigned int index = 0; index < 96u; ++index) {
		int16_t sample = (index & 1u) != 0u ? 400 : -400;

		CHECK(v34_phase34_receiver_sample(&receiver, sample, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "codec noise failed at sample %u", index);
	}
	CHECK(!v34_receive_timing_is_acquired(&receiver.line.timing) &&
	      memcmp(
		      &receiver.line.equalizer, &saved_equalizer,
		      sizeof(saved_equalizer)) == 0 &&
	      memcmp(
		      &receiver.semantic, &saved_semantic,
		      sizeof(saved_semantic)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      v34_phase34_receiver_is_valid(&receiver),
	      "codec noise advanced semantic reception");

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "impulse receiver did not initialize");
	for (unsigned int index = 0; index < 96u; ++index) {
		int16_t sample = index == 0u ? INT16_MAX : 0;

		CHECK(v34_phase34_receiver_sample(&receiver, sample, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "isolated impulse failed at sample %u", index);
	}
	CHECK(!v34_receive_timing_is_acquired(&receiver.line.timing) &&
	      v34_phase34_receiver_is_valid(&receiver),
	      "isolated impulse acquired S timing");

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_sample(&receiver, 400, &output) ==
		      V34_PHASE34_RECEIVER_OK,
	      "corrupt-state receiver did not initialize");
	receiver.line.timing.s_detector.carrier = V34_S_TONE_CARRIER_COUNT;
	{
		struct v34_phase34_receiver saved = receiver;

		CHECK(!v34_phase34_receiver_is_valid(&receiver) &&
		      v34_phase34_receiver_sample(&receiver, 400, &output) ==
			      V34_PHASE34_RECEIVER_INVALID_STATE &&
		      v34_phase34_receivers_equal(&receiver, &saved) &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "corrupt timing state was not rejected transactionally");
	}
	return true;
}

static bool test_training_case(
	enum v34_station_role role, enum v34_symbol_rate symbol_rate,
	uint8_t rate, bool high_carrier, uint8_t preemphasis_filter_index)
{
	struct v34_phase3_profile transmitting_profile;
	struct v34_phase34_receiver receiver;
	struct line_source source;
	unsigned int timing_events = 0;
	unsigned int phase3_s_events = 0;
	unsigned int pp_events = 0;
	unsigned int j_events = 0;
	unsigned int phase4_s_events = 0;
	unsigned int trn_events = 0;
	unsigned int initial_records = 0;
	unsigned int rejected_records = 0;
	unsigned int b1_frames = 0;
	unsigned int mapping_frames = 0;
	uint64_t phase3_preroll_epoch = 0;
	struct v34_adaptive_equalizer phase4_rearm_equalizer;
	struct v34_receive_timing phase4_rearm_timing;
	bool final_record = false;
	bool e_complete = false;
	bool b1_complete = false;
	bool phase4_equalizer_saved = false;

	CHECK(profile(
		      &transmitting_profile, role, symbol_rate, rate,
		      high_carrier, preemphasis_filter_index) &&
	      v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      source_init(&source, &transmitting_profile, rate),
	      "role %u rate %u training pair did not initialize", role, rate);
	for (unsigned int index = 0;
	     index < SAMPLE_LIMIT && mapping_frames < 3u; ++index) {
		struct v34_phase34_receive_output output;
		struct v34_phase34_receive_output saved_output;
		enum v34_phase34_receiver_result result;
		int16_t sample;

		CHECK(source_sample(&source, &sample),
		      "role %u rate %u source stopped at sample %u", role,
		      rate, index);
		memset(&output, 0x5d, sizeof(output));
		saved_output = output;
		result = v34_phase34_receiver_sample(&receiver, sample, &output);
		if (receiver.line.equalizer.epoch > 0u &&
		    receiver.line.equalizer.epoch <
			    V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH &&
		    receiver.stage == V34_PHASE34_RECEIVER_PHASE3 &&
		    receiver.semantic.phase3.stage ==
			    V34_PHASE3_DECISION_RECEIVER_S) {
			CHECK(receiver.semantic.phase3.s.buffered_decisions == 0u &&
			      !receiver.line.equalizer.decision_pending,
			      "role %u rate %u changed Phase 3 S during preroll",
			      role, rate);
			if (phase3_preroll_epoch != receiver.line.equalizer.epoch) {
				CHECK(receiver.line.equalizer.epoch ==
					      phase3_preroll_epoch + 1u,
				      "role %u rate %u skipped Phase 3 preroll epoch",
				      role, rate);
				phase3_preroll_epoch = receiver.line.equalizer.epoch;
			}
		}
		switch (result) {
		case V34_PHASE34_RECEIVER_OK:
			break;
		case V34_PHASE34_RECEIVER_TIMING_ACQUIRED:
			CHECK(receiver.stage == V34_PHASE34_RECEIVER_PHASE3 &&
			      receiver.line.timing.gain.mode == V34_GAIN_TRACKING &&
			      receiver.line.equalizer.epoch == 0u &&
			      receiver.line.equalizer.inputs_since_observation == 0u &&
			      !receiver.line.equalizer.decision_pending &&
			      receiver.semantic.phase3.stage ==
					V34_PHASE3_DECISION_RECEIVER_S &&
			      receiver.semantic.phase3.s.buffered_decisions == 0u,
			      "role %u rate %u adapted before S acquisition",
			      role, rate);
			timing_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE3_S_DETECTED:
			CHECK(timing_events == 1u,
			      "role %u rate %u accepted S before timing", role,
			      rate);
			phase3_s_events++;
			break;
		case V34_PHASE34_RECEIVER_PP_COMPLETE:
			pp_events++;
			break;
		case V34_PHASE34_RECEIVER_J_DETECTED:
			if (role == V34_ANSWERING_STATION) {
				phase4_rearm_equalizer = receiver.line.equalizer;
				phase4_rearm_timing = receiver.line.timing;
				phase4_equalizer_saved = true;
				CHECK(receiver.stage ==
					      V34_PHASE34_RECEIVER_PHASE4 &&
				      receiver.semantic.phase4.stage ==
					      V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
				      receiver.tracking_started &&
				      !receiver.phase4_s_rearmed &&
				      v34_phase34_receiver_rearm_phase4_s(
					      &receiver) ==
					      V34_PHASE34_RECEIVER_OK &&
				      receiver.phase4_s_rearmed &&
				      memcmp(&receiver.line.timing,
					     &phase4_rearm_timing,
					     sizeof(receiver.line.timing)) == 0 &&
				      memcmp(&receiver.line.equalizer,
					     &phase4_rearm_equalizer,
					     sizeof(receiver.line.equalizer)) == 0 &&
				      !receiver.tracking_started,
				      "role %u rate %u lost the Phase 4 receive lineage",
				      role, rate);
			}
			j_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE4_S_DETECTED:
			CHECK(receiver.tracking_started &&
			      receiver.tracking_symbols == 0u &&
			      v34_phase3_s_hunt_is_complete(
				      &receiver.semantic.phase4.answer_s),
			      "role %u rate %u did not start Phase 4 tracking",
			      role, rate);
			phase4_s_events++;
			break;
		case V34_PHASE34_RECEIVER_TRN_COMPLETE:
			trn_events++;
			break;
		case V34_PHASE34_RECEIVER_RECORD_READY:
			if (output.mp.type == V34_MP_RECEIVE_REJECTED) {
				rejected_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u rejected MP did not resume",
				      role, rate);
			} else if (output.mp.type == V34_MP_RECEIVE_TYPE0 &&
				   output.mp.frame_result == V34_MP_OK &&
				   v34_mp_parameters_are_equal(
					   &output.mp.parameters.type0,
					   &source.initial_mp)) {
				initial_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u initial MP did not resume",
				      role, rate);
			} else {
				CHECK(output.mp.type == V34_MP_RECEIVE_TYPE0 &&
				      output.mp.frame_result == V34_MP_OK &&
				      !final_record &&
				      v34_mp_parameters_are_equal(
					      &output.mp.parameters.type0,
					      &source.final_mp) &&
				      v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u final MP was not accepted",
				      role, rate);
				final_record = true;
			}
			continue;
		case V34_PHASE34_RECEIVER_E_COMPLETE: {
			struct v34_phase34_receiver saved = receiver;
			struct v34_phase34_receiver feedback_receiver = receiver;
			struct v34_phase34_receiver unstable_receiver = receiver;
			struct v34_q14_coefficient
				feedback_coefficients[V34_PRECODER_TAP_COUNT];
			struct v34_q14_coefficient
				zero_coefficients[V34_PRECODER_TAP_COUNT];
			struct v34_q14_coefficient
				unstable_coefficients[V34_PRECODER_TAP_COUNT];
			struct v34_q14_coefficient changed_coefficients[
				V34_PRECODER_TAP_COUNT];
			struct v34_receive_timing feedback_timing;
			struct v34_receive_timing zero_timing;
			struct v34_frame_geometry wrong =
				source.preparation.mapping.schedule.geometry;

			CHECK(test_terminal_samples(&receiver),
			      "role %u rate %u training terminal checks failed", role, rate);
			wrong.symbol_rate =
				source.preparation.mapping.schedule.geometry.symbol_rate ==
						V34_SYMBOL_RATE_2800 ?
					V34_SYMBOL_RATE_3000 :
					V34_SYMBOL_RATE_2800;
			feedback_receiver.line.equalizer.quality.decision_metric = 2;
			feedback_receiver.line.equalizer.quality.feedback_metric = 1;
			receiver.line.equalizer.quality.decision_metric = 1;
			receiver.line.equalizer.quality.feedback_metric = 1;
			unstable_receiver.line.equalizer.quality.decision_metric = 300;
			unstable_receiver.line.equalizer.quality.feedback_metric = 1;
			for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
				unstable_receiver.line.equalizer.feedback.coefficient_a[tap] =
					6000;
				unstable_receiver.line.equalizer.feedback.coefficient_b[tap] =
					6000;
			}
			saved = receiver;
			CHECK(v34_phase34_receiver_is_complete(&receiver) &&
			      v34_phase34_receiver_finish_feedback_training(
				      &receiver, false,
				      (struct v34_q14_coefficient *)&receiver) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver,
				      (const struct v34_power_normalizer *)&receiver,
				      &source.preparation.mapping.schedule.geometry, false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.preparation.mapping.normalizer,
				      (const struct v34_frame_geometry *)&receiver,
				      false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry, false,
				      (const struct v34_q14_coefficient *)&receiver,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receivers_equal(&receiver, &saved) &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_STATE &&
			      v34_phase34_receivers_equal(&receiver, &saved) &&
			      v34_phase34_receiver_finish_feedback_training(
				      &feedback_receiver, true,
				      feedback_coefficients) ==
				      V34_PHASE34_RECEIVER_OK &&
			      v34_phase34_receiver_finish_feedback_training(
				      &receiver, true, zero_coefficients) ==
				      V34_PHASE34_RECEIVER_OK &&
			      v34_phase34_receiver_finish_feedback_training(
				      &unstable_receiver, true,
				      unstable_coefficients) ==
				      V34_PHASE34_RECEIVER_OK,
			      "role %u rate %u did not freeze feedback at E", role,
			      rate);
			CHECK(memcmp(zero_coefficients,
				     source.parameters.precoder_coefficients,
				     sizeof(zero_coefficients)) == 0 &&
			      memcmp(unstable_coefficients,
				     source.parameters.precoder_coefficients,
				     sizeof(unstable_coefficients)) == 0 &&
			      !feedback_receiver.line.equalizer.decision_pending &&
			      feedback_receiver.feedback_training_complete &&
			      receiver.feedback_training_complete &&
			      unstable_receiver.feedback_training_complete,
			      "role %u rate %u selected unhelpful feedback", role,
			      rate);
			feedback_timing = feedback_receiver.line.timing;
			zero_timing = receiver.line.timing;
			changed_coefficients[0] = feedback_coefficients[0];
			changed_coefficients[1] = feedback_coefficients[1];
			changed_coefficients[2] = feedback_coefficients[2];
			changed_coefficients[0].real ^= 1;
			saved = feedback_receiver;
			CHECK(v34_phase34_receiver_start_data(
				      &feedback_receiver,
				      &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      changed_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receivers_equal(&feedback_receiver, &saved) &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.preparation.mapping.normalizer, &wrong,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &feedback_receiver,
				      &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      feedback_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK &&
			      feedback_receiver.line.equalizer.feedback_mode ==
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
			      memcmp(&feedback_receiver.line.timing, &feedback_timing,
				     sizeof(feedback_timing)) == 0 &&
			      v34_phase34_receiver_is_valid(
				      &feedback_receiver) &&
			      v34_phase34_receiver_start_data(
				      &receiver,
				      &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK &&
			      receiver.line.equalizer.feedback_mode ==
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
			      memcmp(&receiver.line.timing, &zero_timing,
				     sizeof(zero_timing)) == 0,
			      "role %u rate %u did not enter data at E", role,
			      rate);
			saved = receiver;
			CHECK(v34_phase34_receiver_start_data(
				      &receiver, &source.preparation.mapping.normalizer,
				      &source.preparation.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_STATE &&
			      v34_phase34_receivers_equal(&receiver, &saved),
			      "role %u rate %u repeated data start changed state",
			      role, rate);
			saved = receiver;
			CHECK(v34_phase34_receiver_set_data_timing_frozen(
				      &receiver, true) == V34_PHASE34_RECEIVER_OK &&
			      v34_phase34_receiver_set_data_timing_frozen(
				      &receiver, false) == V34_PHASE34_RECEIVER_OK &&
			      v34_phase34_receivers_equal(&receiver, &saved),
			      "role %u rate %u gated active timing acquisition",
			      role, rate);
			e_complete = true;
			break;
		}
		case V34_PHASE34_RECEIVER_INTERVAL_READY:
			continue;
		case V34_PHASE34_RECEIVER_B1_IN_PROGRESS:
			CHECK(received_all_ones(&output.received),
			      "role %u rate %u B1 frame changed after %u bits, "
			      "errors=%u",
			      role, rate,
			      receiver.semantic.data.data_receiver.data_receiver
				      .b1_received_bits,
			      receiver.semantic.data.data_receiver.data_receiver
				      .b1_error_bits);
			b1_frames++;
			continue;
		case V34_PHASE34_RECEIVER_B1_COMPLETE:
			CHECK(received_all_ones(&output.received),
			      "role %u rate %u final B1 frame changed", role,
			      rate);
			b1_frames++;
			b1_complete = true;
			continue;
		case V34_PHASE34_RECEIVER_MAPPING_FRAME_READY:
			CHECK(b1_complete && received_all_ones(&output.received),
			      "role %u rate %u emitted mapped data before B1",
			      role, rate);
			mapping_frames++;
			continue;
		default:
			CHECK(false,
			      "role %u rate %u training failed at sample %u, "
			      "result %u",
			      role, rate, index, result);
		}
		CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "role %u rate %u non-record result changed its event",
		      role, rate);
	}
	CHECK(phase3_preroll_epoch + 1u ==
		      V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH &&
	      (role != V34_ANSWERING_STATION || phase4_equalizer_saved),
	      "role %u rate %u did not preserve receive training: phase3=%llu",
	      role, rate, (unsigned long long)phase3_preroll_epoch);
	CHECK(e_complete && b1_complete && mapping_frames >= 3u &&
	      b1_frames ==
		      source.preparation.mapping.schedule.geometry
			      .mapping_frames_per_data_frame &&
	      timing_events == 1u &&
	      phase3_s_events == 1u && pp_events == 1u && j_events == 1u &&
	      phase4_s_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      trn_events == 1u &&
	      initial_records != 0u && final_record &&
	      source.completed_records >= 3u && source.data_started &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_PHASE34_RECEIVER_DATA &&
	      receiver.line.equalizer.feedback_mode ==
		      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	      !v34_phase34_receiver_is_complete(&receiver) &&
	      !v34_phase34_receiver_has_failed(&receiver),
	      "role %u symbol=%u rate=%u receiver did not reach E: "
	      "timing=%u S3=%u PP=%u "
	      "J=%u S4=%u TRN=%u initial=%u final=%u source=%u "
	      "rejects=%u B1=%u mapped=%u stage=%u failed=%u",
	      role, symbol_rate, rate, timing_events, phase3_s_events,
	      pp_events, j_events,
	      phase4_s_events, trn_events, initial_records, final_record,
	      source.completed_records, rejected_records, b1_frames,
	      mapping_frames, receiver.stage, receiver.failed);
	CHECK(v34_phase3_decision_handoff_is_valid(
		      &receiver.phase3_handoff) &&
	      receiver.tracking_started && receiver.tracking_symbols != 0u &&
	      receiver.line.equalizer.epoch != 0u &&
	      !receiver.line.equalizer.decision_pending,
	      "role %u rate %u receiver did not retain its physical lineage",
	      role, rate);
	CHECK(test_terminal_samples(&receiver),
	      "role %u rate %u data terminal checks failed", role, rate);
	receiver.rate_renegotiation.acquire_phase = true;
	CHECK(!v34_phase34_receiver_is_valid(&receiver),
	      "online receiver accepted training-only phase acquisition");
	receiver.rate_renegotiation.acquire_phase = false;
	return test_data_erasure(&receiver, &source, role, rate);
}

static bool test_late_s_entry(void)
{
	const int16_t offsets[] = { -30, 50 };
	struct v34_phase3_profile selected;

	CHECK(profile(&selected, V34_ANSWERING_STATION, V34_SYMBOL_RATE_3200,
		      12, true, 6), "late S profile did not initialize");
	for (size_t n = 0; n < ARRAY_LENGTH(offsets); ++n) {
		struct v34_phase34_receiver receiver;
		struct v34_training_transmitter transmitter;
		bool detected = false;

		CHECK(v34_training_transmitter_init(&transmitter, &selected,
			      V34_CONSTELLATION_4_POINT, SAMPLE_RATE, 5000) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      v34_phase34_receiver_init(&receiver, &selected,
			      V34_CONSTELLATION_4_POINT, TEST_INITIAL_RECEIVE_GAIN) ==
			      V34_PHASE34_RECEIVER_OK,
		      "late S pair did not initialize");
		for (unsigned int i = 0; i < SAMPLE_LIMIT && !detected; ++i) {
			struct v34_phase34_receive_output output;
			enum v34_phase34_receiver_result result;
			int16_t sample;

			if (transmitter.phase3.stage == V34_PHASE3_TRANSMITTER_TRN &&
			    transmitter.phase3.trn_symbols == PHASE3_TRN_SYMBOLS)
				CHECK(v34_training_transmitter_start_j(&transmitter) ==
					      V34_TRAINING_TRANSMITTER_OK,
				      "late S transmitter could not start J");
			CHECK(v34_training_transmitter_sample(&transmitter, &sample) ==
				      V34_TRAINING_TRANSMITTER_OK,
			      "late S transmitter stopped");
			/* Enter partway through S with a small constant DC bias. A
			 * timing restart at the later qualified edge loses this suffix. */
			if (i < 94u)
				continue;
			result = v34_phase34_receiver_sample(
				&receiver, (int16_t)(sample + offsets[n]), &output);
			CHECK(result < V34_PHASE34_RECEIVER_FAILED,
			      "late S receiver failed at sample %u", i);
			detected = result == V34_PHASE34_RECEIVER_J_DETECTED;
		}
		CHECK(detected, "DC offset %d prevented late S/J acquisition", offsets[n]);
	}
	return true;
}

static bool test_training(void)
{
	static const struct {
		enum v34_symbol_rate symbol_rate;
		bool high_carrier;
		bool caller_transmit_only;
		uint8_t preemphasis_filter_index;
	} cases[] = {
		{ V34_SYMBOL_RATE_2400, false, false, 0 },
		{ V34_SYMBOL_RATE_2400, true, false, 0 },
		{ V34_SYMBOL_RATE_2743, false, false, 0 },
		{ V34_SYMBOL_RATE_2743, true, false, 0 },
		{ V34_SYMBOL_RATE_2800, false, true, 0 },
		{ V34_SYMBOL_RATE_2800, true, false, 0 },
		{ V34_SYMBOL_RATE_3000, false, false, 2 },
		{ V34_SYMBOL_RATE_3000, true, false, 2 },
		{ V34_SYMBOL_RATE_3200, false, true, 0 },
		{ V34_SYMBOL_RATE_3200, true, false, 0 },
		{ V34_SYMBOL_RATE_3429, false, false, 7 },
	};
	bool success = true;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_frame_geometry geometry;
		uint8_t selected_rates[V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS];
		uint8_t selected_count = 0;

		for (uint8_t rate = 1; rate <= V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS;
		     ++rate) {
			if (v34_frame_geometry_init(
				    &geometry, cases[index].symbol_rate, rate,
				    false) != V34_FRAME_SCHEDULE_OK)
				continue;
			selected_rates[selected_count++] = rate;
		}
		CHECK(selected_count != 0,
		      "symbol rate %u has no supported data geometry",
		      cases[index].symbol_rate);
		/* Exercise only the profile this receiver advertises to its peer.
		 * The transmit owner independently covers every requestable
		 * profile. */
		for (uint8_t selection = 0; selection < selected_count;
		     ++selection)
			for (unsigned int role = V34_CALLING_STATION;
			     role <= V34_ANSWERING_STATION; ++role) {
				if (cases[index].caller_transmit_only &&
				    role != V34_CALLING_STATION)
					continue;
				if (!test_training_case(
					    (enum v34_station_role)role,
					    cases[index].symbol_rate,
					    selected_rates[selection],
					    cases[index].high_carrier,
					    cases[index].preemphasis_filter_index))
					success = false;
			}
	}
	return success;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "validation", test_validation },
		{ "tone-owned acquisition", test_tone_owned_acquisition },
		{ "late S entry", test_late_s_entry },
		{ "continuous Phase 3 through data", test_training },
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
