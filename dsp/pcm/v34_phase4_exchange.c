// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_exchange.h"

#include "memory_region.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static struct v34_mp_parameters *record_common(
	struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static const struct v34_mp_parameters *record_common_const(
	const struct v34_mp_record *record)
{
	return record->type == V34_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static void set_record_acknowledgment(
	struct v34_mp_record *record, bool acknowledged)
{
	record_common(record)->acknowledges_peer_mp = acknowledged;
}

static bool record_is_acknowledged(const struct v34_mp_record *record)
{
	return record_common_const(record)->acknowledges_peer_mp;
}

static bool encode_record(
	const struct v34_mp_record *record,
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES], size_t *bit_count)
{
	enum v34_mp_result result;

	if (!v34_mp_record_is_valid(record) || frame == NULL ||
	    bit_count == NULL)
		return false;
	memset(frame, 0, V34_MP_TYPE1_FRAME_BYTES);
	if (record->type == V34_MP_RECORD_TYPE0) {
		result = v34_mp_type0_encode(
			&record->parameters.type0, frame,
			V34_MP_TYPE1_FRAME_BYTES);
		*bit_count = V34_MP_TYPE0_FRAME_BITS;
	} else {
		result = v34_mp_type1_encode(
			&record->parameters.type1, frame,
			V34_MP_TYPE1_FRAME_BYTES);
		*bit_count = V34_MP_TYPE1_FRAME_BITS;
	}
	return result == V34_MP_OK;
}

static bool records_match(
	const struct v34_mp_record *left,
	const struct v34_mp_record *right)
{
	uint8_t left_frame[V34_MP_TYPE1_FRAME_BYTES];
	uint8_t right_frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t left_bits;
	size_t right_bits;

	return left->type == right->type &&
	       encode_record(left, left_frame, &left_bits) &&
	       encode_record(right, right_frame, &right_bits) &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static bool outputs_overlap(
	const struct v34_phase4_exchange *exchange,
	const int16_t *sample, const uint32_t *events,
	const struct v34_received_mapping_frame *received)
{
	return memory_regions_overlap(
		       exchange, sizeof(*exchange), sample, sizeof(*sample)) ||
	       memory_regions_overlap(
		       exchange, sizeof(*exchange), events, sizeof(*events)) ||
	       memory_regions_overlap(
		       exchange, sizeof(*exchange), received, sizeof(*received)) ||
	       memory_regions_overlap(
		       sample, sizeof(*sample), events, sizeof(*events)) ||
	       memory_regions_overlap(
		       sample, sizeof(*sample), received, sizeof(*received)) ||
	       memory_regions_overlap(
		       events, sizeof(*events), received, sizeof(*received));
}

static void note_ready(
	struct v34_phase4_exchange *exchange, uint32_t *events);

static bool trn_target_symbols(
	const struct v34_phase4_exchange *exchange, uint32_t *symbols)
{
	const struct v34_phase3_profile *profile;
	uint64_t two_seconds;

	if (exchange == NULL || symbols == NULL ||
	    exchange->transmit_data_active)
		return false;
	profile = &exchange->transmit.training.line.profile;
	if (!v34_phase3_profile_is_valid(profile))
		return false;
	if (profile->local_role == V34_CALLING_STATION) {
		*symbols = V34_PHASE4_TRN_MIN_SYMBOLS;
		return true;
	}
	/* The answerer uses the complete zero-RTD training allowance. This keeps
	 * the peer's line equalizer insensitive to the sample phase at which a
	 * full retrain reaches Phase 4. */
	two_seconds = (uint64_t)profile->symbol_rate_numerator * 2u /
		profile->symbol_rate_denominator;
	if (two_seconds < V34_PHASE4_TRN_MIN_SYMBOLS ||
	    two_seconds > UINT32_MAX)
		return false;
	*symbols = (uint32_t)two_seconds;
	return true;
}

static bool trn_target_complete(
	const struct v34_phase4_exchange *exchange)
{
	uint32_t target;

	return exchange->transmit_data_active ||
	       (trn_target_symbols(exchange, &target) &&
		exchange->transmit.training.phase4_trn_symbols >= target);
}

static bool profiles_are_equal(
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

static const struct v34_phase3_profile *local_transmit_profile(
	const struct v34_phase4_exchange *exchange)
{
	return exchange->transmit_data_active ?
	       &exchange->data_startup.transmit.transmitting_profile :
	       &exchange->transmit.training.line.profile;
}

static const struct v34_phase3_profile *peer_transmit_profile(
	const struct v34_phase4_exchange *exchange)
{
	return exchange->data_startup_prepared ?
	       &exchange->data_startup.receive.transmitting_profile :
	       &exchange->receive.training.state.phase4.transmitting_profile;
}

static uint32_t transmit_sample_rate(
	const struct v34_phase4_exchange *exchange)
{
	return exchange->transmit_data_active ?
	       exchange->transmit.data.transmitter.line.carrier.modulator.channel
		       .sample_rate :
	       exchange->transmit.training.line.carrier.channel.sample_rate;
}

static uint32_t receive_sample_rate(
	const struct v34_phase4_exchange *exchange)
{
	return exchange->receive_data_active ?
	       exchange->receive.data.demodulator.channel.sample_rate :
	       exchange->receive.training.state.phase4.demodulator.channel
		       .sample_rate;
}

static bool data_state_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_phase4_equalizer_receiver_stage receive_stage;
	bool record_pending;

	if (!isfinite(exchange->data_equalizer_adaptation) ||
	    exchange->data_equalizer_adaptation < 0.0 ||
	    exchange->data_equalizer_adaptation > 1.0)
		return false;
	if (!exchange->peer_mp_received)
		return !exchange->data_preparation_required &&
		       !exchange->data_startup_prepared &&
		       !exchange->peer_mp_prime_received &&
		       !exchange->transmit_data_active &&
		       !exchange->receive_data_active &&
		       !exchange->transmit_b1_complete &&
		       !exchange->receive_b1_complete &&
		       !exchange->online_announced;
	if (exchange->data_preparation_required) {
		record_pending = exchange->receive.training.state.phase4.control
					 .record_pending;
		return !exchange->data_startup_prepared && record_pending &&
		       !exchange->transmit_data_active &&
		       !exchange->receive_data_active;
	}
	if (!exchange->data_startup_prepared ||
	    !v34_data_startup_is_valid(&exchange->data_startup) ||
	    exchange->data_startup.receive.equalizer_adaptation !=
		    exchange->data_equalizer_adaptation)
		return false;
	if (exchange->transmit_data_active) {
		if (!v34_data_stream_is_valid(&exchange->transmit.data) ||
		    !profiles_are_equal(
			    &exchange->data_startup.transmit.transmitting_profile,
			    &exchange->transmit.data.transmitter.line.carrier.profile))
			return false;
	} else if (!profiles_are_equal(
			   &exchange->data_startup.transmit.transmitting_profile,
			   &exchange->transmit.training.line.profile)) {
		return false;
	}
	if (exchange->receive_data_active) {
		if (!v34_data_line_receiver_is_valid(&exchange->receive.data) ||
		    !profiles_are_equal(
			    &exchange->data_startup.receive.transmitting_profile,
			    &exchange->receive.data.transmitting_profile))
			return false;
	} else {
		receive_stage =
			exchange->receive.training.state.phase4.control.stage;
		record_pending = exchange->receive.training.state.phase4.control
					 .record_pending;
		if (receive_stage ==
			    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE ||
		    (record_pending && !exchange->failed) ||
		    exchange->peer_mp_prime_received !=
			    (receive_stage ==
			     V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E))
			return false;
	}
	if (exchange->transmit_b1_complete !=
		    (exchange->transmit_data_active &&
		     !v34_data_stream_is_transmitting_b1(
			     &exchange->transmit.data)) ||
	    exchange->receive_b1_complete !=
		    (exchange->receive_data_active &&
		     !v34_data_line_receiver_is_receiving_b1(
			     &exchange->receive.data)) ||
	    exchange->online_announced !=
		    (exchange->transmit_b1_complete &&
		     exchange->receive_b1_complete))
		return false;
	return true;
}

static bool transmitter_mp_matches(
	const struct v34_phase4_exchange *exchange)
{
	struct v34_mp_record expected = exchange->transmit_mp;
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t bit_count;

	if (exchange->transmit_data_active ||
	    exchange->transmit.training.stage !=
	    V34_TRAINING_TRANSMITTER_PHASE4_MP)
		return true;
	set_record_acknowledgment(
		&expected, exchange->transmit_mp_prime_started);
	return encode_record(&expected, frame, &bit_count) &&
	       exchange->transmit.training.phase4.mp.frame_bit_count ==
		       bit_count &&
	       memcmp(exchange->transmit.training.phase4.mp.frame, frame,
		      sizeof(frame)) == 0;
}

static bool transmitter_stage_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	enum v34_training_transmitter_stage stage;
	bool entered_trn;
	bool entered_mp;

	if (exchange->transmit_data_active)
		return exchange->transmit_trn_started &&
		       exchange->transmit_mp_started &&
		       exchange->transmit_mp_prime_started &&
		       exchange->transmit_mp_prime_records_completed ==
			       V34_PHASE4_MIN_MP_PRIME_RECORDS &&
		       exchange->peer_mp_prime_received &&
		       exchange->data_startup_prepared;
	stage = exchange->transmit.training.stage;
	entered_trn = stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN ||
		      stage == V34_TRAINING_TRANSMITTER_PHASE4_MP ||
		      stage == V34_TRAINING_TRANSMITTER_PHASE4_E;
	entered_mp = stage == V34_TRAINING_TRANSMITTER_PHASE4_MP ||
		     stage == V34_TRAINING_TRANSMITTER_PHASE4_E;

	if (exchange->transmit_trn_started != entered_trn ||
	    exchange->transmit_mp_started != entered_mp)
		return false;
	if (stage == V34_TRAINING_TRANSMITTER_PHASE4_E &&
	    (!exchange->transmit_mp_prime_started ||
	     exchange->transmit_mp_prime_records_completed !=
		     V34_PHASE4_MIN_MP_PRIME_RECORDS ||
	     !exchange->peer_mp_prime_received ||
	     !exchange->data_startup_prepared))
		return false;
	if (exchange->local_role == V34_CALLING_STATION)
		return stage == V34_TRAINING_TRANSMITTER_PHASE3 ||
		       stage == V34_TRAINING_TRANSMITTER_J_PRIME ||
		       entered_trn;
	return exchange->local_role == V34_ANSWERING_STATION &&
	       (stage == V34_TRAINING_TRANSMITTER_ANSWER_S || entered_trn);
}

bool v34_phase4_exchange_is_valid(
	const struct v34_phase4_exchange *exchange)
{
	if (exchange == NULL ||
	    (!exchange->transmit_data_active &&
	     !v34_training_transmitter_is_valid(&exchange->transmit.training)) ||
	    (!exchange->receive_data_active &&
	     !v34_training_receiver_is_valid(&exchange->receive.training)) ||
	    !v34_mp_record_is_valid(&exchange->transmit_mp) ||
	    record_is_acknowledged(&exchange->transmit_mp) ||
	    (!exchange->receive_data_active &&
	     exchange->receive.training.stage !=
		     V34_TRAINING_RECEIVER_PHASE4) ||
	    !transmitter_stage_is_valid(exchange) ||
	    !transmitter_mp_matches(exchange) || !data_state_is_valid(exchange))
		return false;
	if (exchange->local_role !=
		    local_transmit_profile(exchange)->local_role ||
	    exchange->local_role == peer_transmit_profile(exchange)->local_role ||
	    transmit_sample_rate(exchange) != receive_sample_rate(exchange) ||
	    (!exchange->receive_data_active &&
	     v34_training_receiver_has_failed(&exchange->receive.training) &&
	     !exchange->failed) ||
	    (exchange->receive_data_active &&
	     v34_data_line_receiver_has_failed(&exchange->receive.data) &&
	     !exchange->failed) ||
	    (exchange->transmit_mp_started && !trn_target_complete(exchange)) ||
	    (exchange->transmit_mp_prime_started &&
	     (!exchange->transmit_mp_started || !exchange->peer_mp_received)) ||
	    exchange->transmit_mp_prime_records_completed >
		    V34_PHASE4_MIN_MP_PRIME_RECORDS ||
	    (!exchange->transmit_mp_prime_started &&
	     exchange->transmit_mp_prime_records_completed != 0) ||
	    (exchange->peer_mp_prime_received &&
	     !exchange->peer_mp_received) ||
	    exchange->ready_announced !=
		    (exchange->transmit_mp_prime_started &&
		     exchange->transmit_mp_prime_records_completed ==
			     V34_PHASE4_MIN_MP_PRIME_RECORDS &&
		     exchange->peer_mp_prime_received &&
		     exchange->data_startup_prepared) ||
	    (exchange->peer_mp_received &&
	     (!v34_mp_record_is_valid(&exchange->peer_mp) ||
	      record_is_acknowledged(&exchange->peer_mp))))
		return false;
	if (exchange->local_role == V34_ANSWERING_STATION)
		return !exchange->finish_j_requested;
	if (exchange->local_role != V34_CALLING_STATION)
		return false;
	if (exchange->finish_j_requested)
		return !exchange->transmit_data_active &&
		       exchange->transmit.training.stage ==
		       V34_TRAINING_TRANSMITTER_PHASE3;
	return exchange->transmit_data_active ||
	       exchange->transmit.training.stage !=
		       V34_TRAINING_TRANSMITTER_PHASE3 ||
	       (!exchange->receive_data_active &&
		exchange->receive.training.state.phase4.control.stage ==
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S);
}

static enum v34_phase4_exchange_result transmit_result(
	enum v34_training_transmitter_result result)
{
	switch (result) {
	case V34_TRAINING_TRANSMITTER_OK:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_TRANSMITTER_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_TRAINING_TRANSMITTER_CLIPPING_RISK:
		return V34_PHASE4_EXCHANGE_CLIPPING_RISK;
	case V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT:
	case V34_TRAINING_TRANSMITTER_INVALID_STATE:
	case V34_TRAINING_TRANSMITTER_INVALID_PARAMETERS:
	case V34_TRAINING_TRANSMITTER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result data_transmit_result(
	enum v34_data_stream_result result)
{
	switch (result) {
	case V34_DATA_STREAM_OK:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_DATA_STREAM_SOURCE_FAILURE:
		return V34_PHASE4_EXCHANGE_SOURCE_FAILURE;
	case V34_DATA_STREAM_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_DATA_STREAM_CLIPPING_RISK:
		return V34_PHASE4_EXCHANGE_CLIPPING_RISK;
	case V34_DATA_STREAM_INVALID_ARGUMENT:
	case V34_DATA_STREAM_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

enum v34_phase4_exchange_result v34_phase4_exchange_init(
	struct v34_phase4_exchange *exchange,
	const struct v34_phase3_exchange_handoff *handoff,
	const struct v34_phase4_exchange_configuration *configuration)
{
	struct v34_phase4_exchange initialized = { 0 };
	enum v34_constellation_size local_transmit_constellation;
	enum v34_constellation_size peer_transmit_constellation;
	enum v34_training_transmitter_result transmit;
	enum v34_training_receiver_result receive;

	if (exchange == NULL ||
	    !v34_phase3_exchange_handoff_is_valid(handoff) ||
	    configuration == NULL ||
	    !v34_training_receiver_configuration_is_valid(
		    &configuration->receive) ||
	    !v34_mp_record_is_valid(&configuration->transmit_mp) ||
	    record_is_acknowledged(&configuration->transmit_mp) ||
	    !isfinite(configuration->data_equalizer_adaptation) ||
	    configuration->data_equalizer_adaptation < 0.0 ||
	    configuration->data_equalizer_adaptation > 1.0)
		return V34_PHASE4_EXCHANGE_INVALID_ARGUMENT;
	/* Each J selects the other station's Phase 4 transmitter. */
	local_transmit_constellation =
		handoff->receive.phase3.j.detection.sequence.remote_constellation;
	peer_transmit_constellation =
		handoff->phase3_transmit.remote_constellation;
	transmit = v34_training_transmitter_continue(
		&initialized.transmit.training, &handoff->transmit_line,
		&handoff->phase3_transmit, local_transmit_constellation);
	if (transmit != V34_TRAINING_TRANSMITTER_OK)
		return transmit_result(transmit);
	receive = v34_training_receiver_continue(
		&initialized.receive.training, &handoff->receive,
		peer_transmit_constellation,
		&configuration->receive);
	if (receive != V34_TRAINING_RECEIVER_OK)
		return receive == V34_TRAINING_RECEIVER_OUT_OF_RANGE ?
		       V34_PHASE4_EXCHANGE_OUT_OF_RANGE :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	initialized.transmit_mp = configuration->transmit_mp;
	initialized.data_equalizer_adaptation =
		configuration->data_equalizer_adaptation;
	initialized.local_role = handoff->transmit_line.profile.local_role;
	if (!v34_phase4_exchange_is_valid(&initialized))
		return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	*exchange = initialized;
	return V34_PHASE4_EXCHANGE_OK;
}

bool v34_phase4_exchange_has_failed(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && exchange->failed;
}

bool v34_phase4_exchange_needs_data_preparation(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && !exchange->failed &&
	       exchange->data_preparation_required;
}

bool v34_phase4_exchange_is_ready_for_e(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && !exchange->failed &&
	       exchange->ready_announced;
}

bool v34_phase4_exchange_is_online(
	const struct v34_phase4_exchange *exchange)
{
	return v34_phase4_exchange_is_valid(exchange) && !exchange->failed &&
	       exchange->online_announced;
}

bool v34_phase4_exchange_get_data_startup(
	const struct v34_phase4_exchange *exchange,
	struct v34_data_startup *startup)
{
	if (!v34_phase4_exchange_is_valid(exchange) || exchange->failed ||
	    !exchange->data_startup_prepared || startup == NULL)
		return false;
	*startup = exchange->data_startup;
	return true;
}

bool v34_phase4_exchange_get_peer_mp(
	const struct v34_phase4_exchange *exchange,
	struct v34_mp_record *record)
{
	if (!v34_phase4_exchange_is_valid(exchange) || exchange->failed ||
	    !exchange->peer_mp_received || record == NULL)
		return false;
	*record = exchange->peer_mp;
	return true;
}

static enum v34_phase4_exchange_result start_transmit_mp(
	struct v34_phase4_exchange *exchange, bool acknowledged)
{
	struct v34_mp_record record = exchange->transmit_mp;
	enum v34_training_transmitter_result result;

	set_record_acknowledgment(&record, acknowledged);
	result = record.type == V34_MP_RECORD_TYPE0 ?
		v34_training_transmitter_start_mp_type0(
			&exchange->transmit.training, &record.parameters.type0) :
		v34_training_transmitter_start_mp_type1(
			&exchange->transmit.training, &record.parameters.type1);
	return transmit_result(result);
}

static enum v34_phase4_exchange_result start_transmit_data(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	struct v34_data_stream data;
	enum v34_data_stream_result result =
		v34_data_stream_continue_prepared_b1(
			&data, &exchange->transmit.training.line,
			&exchange->data_startup.transmit);

	if (result != V34_DATA_STREAM_OK)
		return data_transmit_result(result);
	exchange->transmit.data = data;
	exchange->transmit_data_active = true;
	*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_STARTED;
	return V34_PHASE4_EXCHANGE_OK;
}

static enum v34_phase4_exchange_result prepare_transmitter(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	enum v34_phase4_exchange_result result;

	if (exchange->transmit_data_active)
		return V34_PHASE4_EXCHANGE_OK;
	if (exchange->transmit.training.stage ==
		    V34_TRAINING_TRANSMITTER_PHASE4_E) {
		if (!v34_training_transmitter_phase4_e_complete(
			    &exchange->transmit.training))
			return V34_PHASE4_EXCHANGE_OK;
		return start_transmit_data(exchange, events);
	}
	if (exchange->finish_j_requested &&
	    exchange->transmit.training.stage ==
		    V34_TRAINING_TRANSMITTER_PHASE3 &&
	    exchange->transmit.training.phase3.j_symbols >=
		    V34_PHASE3_J_PATTERN_SYMBOLS &&
	    v34_phase3_j_sequence_at_pattern_boundary(
		    &exchange->transmit.training.phase3.state.j)) {
		if (v34_training_transmitter_finish_j(
			    &exchange->transmit.training) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
		exchange->finish_j_requested = false;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_J_FINISHED;
	}
	if (!exchange->transmit_mp_started && trn_target_complete(exchange)) {
		result = start_transmit_mp(exchange, false);
		if (result != V34_PHASE4_EXCHANGE_OK)
			return result;
		exchange->transmit_mp_started = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_STARTED;
	}
	if (exchange->transmit_mp_started &&
	    v34_training_transmitter_phase4_mp_record_complete(
			    &exchange->transmit.training)) {
		bool acknowledged = exchange->peer_mp_received;

		if (exchange->transmit_mp_prime_started &&
		    exchange->transmit_mp_prime_records_completed <
			    V34_PHASE4_MIN_MP_PRIME_RECORDS)
			exchange->transmit_mp_prime_records_completed++;

		if (exchange->transmit_mp_prime_started &&
		    exchange->transmit_mp_prime_records_completed ==
			    V34_PHASE4_MIN_MP_PRIME_RECORDS &&
		    exchange->peer_mp_prime_received &&
		    exchange->data_startup_prepared) {
			result = transmit_result(v34_training_transmitter_start_e(
				&exchange->transmit.training));
			if (result == V34_PHASE4_EXCHANGE_OK)
				*events |=
					V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_E_STARTED;
			return result;
		}
		result = start_transmit_mp(exchange, acknowledged);
		if (result != V34_PHASE4_EXCHANGE_OK)
			return result;
		if (acknowledged && !exchange->transmit_mp_prime_started) {
			exchange->transmit_mp_prime_started = true;
			*events |=
				V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_MP_PRIME_STARTED;
		}
	}
	return V34_PHASE4_EXCHANGE_OK;
}

static void note_transmitter(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	if (!exchange->transmit_trn_started &&
	    exchange->transmit.training.stage ==
		    V34_TRAINING_TRANSMITTER_PHASE4_TRN) {
		exchange->transmit_trn_started = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_TRN_STARTED;
	}
}

static bool record_from_event(
	const struct v34_mp_receive_event *event,
	struct v34_mp_record *record)
{
	struct v34_mp_record decoded = { 0 };

	if (event->frame_result != V34_MP_OK)
		return false;
	if (event->type == V34_MP_RECEIVE_TYPE0) {
		decoded.type = V34_MP_RECORD_TYPE0;
		decoded.parameters.type0 = event->parameters.type0;
	} else if (event->type == V34_MP_RECEIVE_TYPE1) {
		decoded.type = V34_MP_RECORD_TYPE1;
		decoded.parameters.type1 = event->parameters.type1;
	} else {
		return false;
	}
	if (!v34_mp_record_is_valid(&decoded))
		return false;
	*record = decoded;
	return true;
}

static enum v34_phase4_exchange_result continue_mp_receiver(
	struct v34_phase4_exchange *exchange)
{
	enum v34_training_receiver_result result =
		v34_training_receiver_continue_mp(&exchange->receive.training);

	if (result == V34_TRAINING_RECEIVER_OK)
		return V34_PHASE4_EXCHANGE_OK;
	return result == V34_TRAINING_RECEIVER_FAILED ?
		       V34_PHASE4_EXCHANGE_FAILED :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result expect_e_receiver(
	struct v34_phase4_exchange *exchange)
{
	enum v34_training_receiver_result result =
		v34_training_receiver_expect_e(&exchange->receive.training);

	if (result == V34_TRAINING_RECEIVER_OK)
		return V34_PHASE4_EXCHANGE_OK;
	return result == V34_TRAINING_RECEIVER_FAILED ?
		       V34_PHASE4_EXCHANGE_FAILED :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result latch_failure(
	struct v34_phase4_exchange *exchange,
	enum v34_phase4_exchange_result result)
{
	struct v34_phase4_exchange failed = *exchange;

	failed.failed = true;
	if (!v34_phase4_exchange_is_valid(&failed)) {
		exchange->failed = true;
		return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	}
	*exchange = failed;
	return result;
}

enum v34_phase4_exchange_result v34_phase4_exchange_prepare_data_startup(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	struct v34_phase4_exchange next;
	struct v34_data_mode mode;
	struct v34_data_startup startup;
	enum v34_data_mode_result mode_result;
	enum v34_data_startup_result startup_result;
	enum v34_phase4_exchange_result result;
	uint32_t emitted = 0;

	if (!v34_phase4_exchange_is_valid(exchange) || events == NULL ||
	    memory_regions_overlap(
		    exchange, sizeof(*exchange), events, sizeof(*events)))
		return V34_PHASE4_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed)
		return V34_PHASE4_EXCHANGE_FAILED;
	if (!exchange->data_preparation_required)
		return V34_PHASE4_EXCHANGE_INVALID_STATE;
	mode_result = v34_data_mode_select_startup(
		exchange->local_role, &exchange->transmit_mp,
		&exchange->peer_mp, &mode);
	if (mode_result == V34_DATA_MODE_NO_COMMON_RATE)
		return latch_failure(
			exchange, V34_PHASE4_EXCHANGE_NO_COMMON_RATE);
	if (mode_result != V34_DATA_MODE_OK)
		return latch_failure(
			exchange, V34_PHASE4_EXCHANGE_NUMERIC_FAILURE);
	startup_result = v34_data_startup_prepare(
		&startup, &mode, &exchange->transmit.training.line.profile,
		&exchange->receive.training.state.phase4.transmitting_profile,
		exchange->receive.training.state.phase4.demodulator.channel
			.sample_rate,
		exchange->data_equalizer_adaptation);
	if (startup_result != V34_DATA_STARTUP_OK) {
		result = startup_result == V34_DATA_STARTUP_OUT_OF_RANGE ?
			V34_PHASE4_EXCHANGE_OUT_OF_RANGE :
			startup_result == V34_DATA_STARTUP_INVALID_ARGUMENT ?
				V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH :
				V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
		return latch_failure(exchange, result);
	}
	next = *exchange;
	next.data_startup = startup;
	next.data_preparation_required = false;
	next.data_startup_prepared = true;
	result = next.peer_mp_prime_received ?
		expect_e_receiver(&next) : continue_mp_receiver(&next);
	if (result != V34_PHASE4_EXCHANGE_OK) {
		next.failed = true;
		if (v34_phase4_exchange_is_valid(&next))
			*exchange = next;
		else
			exchange->failed = true;
		return result;
	}
	note_ready(&next, &emitted);
	if (!v34_phase4_exchange_is_valid(&next))
		return latch_failure(
			exchange, V34_PHASE4_EXCHANGE_NUMERIC_FAILURE);
	*exchange = next;
	*events |= emitted;
	return V34_PHASE4_EXCHANGE_OK;
}

static enum v34_phase4_exchange_result accept_mp_record(
	struct v34_phase4_exchange *exchange,
	const struct v34_mp_receive_event *event, uint32_t *events)
{
	struct v34_mp_record record;
	bool acknowledged;

	if (event->type == V34_MP_RECEIVE_REJECTED)
		return continue_mp_receiver(exchange);
	if (!record_from_event(event, &record))
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	acknowledged = record_is_acknowledged(&record);
	set_record_acknowledgment(&record, false);
	if (!exchange->peer_mp_received) {
		exchange->peer_mp = record;
		exchange->peer_mp_received = true;
		exchange->data_preparation_required = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_MP_RECEIVED;
		if (acknowledged) {
			exchange->peer_mp_prime_received = true;
			*events |=
				V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED;
		}
		return V34_PHASE4_EXCHANGE_OK;
	} else if (!records_match(&exchange->peer_mp, &record)) {
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	}
	if (acknowledged && !exchange->peer_mp_prime_received) {
		if (!exchange->data_startup_prepared)
			return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
		exchange->peer_mp_prime_received = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_MP_PRIME_RECEIVED;
		return expect_e_receiver(exchange);
	}
	return continue_mp_receiver(exchange);
}

static enum v34_phase4_exchange_result start_receive_data(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	struct v34_data_line_receiver data;
	enum v34_training_receiver_result result =
		v34_training_receiver_start_data(
			&exchange->receive.training, &data,
			&exchange->data_startup.receive);

	if (result != V34_TRAINING_RECEIVER_OK)
		return result == V34_TRAINING_RECEIVER_OUT_OF_RANGE ?
		       V34_PHASE4_EXCHANGE_OUT_OF_RANGE :
		       V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	exchange->receive.data = data;
	exchange->receive_data_active = true;
	*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_E_RECEIVED;
	return V34_PHASE4_EXCHANGE_OK;
}

static enum v34_phase4_exchange_result drive_training_receiver(
	struct v34_phase4_exchange *exchange, int16_t sample,
	uint32_t *events)
{
	struct v34_mp_receive_event event = { 0 };
	enum v34_training_receiver_result result =
		v34_training_receiver_sample(
			&exchange->receive.training, sample, &event);

	switch (result) {
	case V34_TRAINING_RECEIVER_OK:
	case V34_TRAINING_RECEIVER_SYMBOL:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_RECEIVER_S_DETECTED:
		if (exchange->local_role != V34_CALLING_STATION ||
		    exchange->transmit.training.stage !=
			    V34_TRAINING_TRANSMITTER_PHASE3 ||
		    exchange->finish_j_requested)
			return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
		exchange->finish_j_requested = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_S_DETECTED;
		return V34_PHASE4_EXCHANGE_OK;
	case V34_TRAINING_RECEIVER_FAILED:
		return V34_PHASE4_EXCHANGE_FAILED;
	case V34_TRAINING_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_TRAINING_RECEIVER_RECORD_READY:
		return accept_mp_record(exchange, &event, events);
	case V34_TRAINING_RECEIVER_E_COMPLETE:
		return start_receive_data(exchange, events);
	case V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH:
	case V34_TRAINING_RECEIVER_COMPLETE:
		return V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH;
	case V34_TRAINING_RECEIVER_TIMING_ACQUIRED:
	case V34_TRAINING_RECEIVER_MD_COMPLETE:
	case V34_TRAINING_RECEIVER_REPEATED_S_DETECTED:
	case V34_TRAINING_RECEIVER_PP_COMPLETE:
	case V34_TRAINING_RECEIVER_J_DETECTED:
	case V34_TRAINING_RECEIVER_INVALID_ARGUMENT:
	case V34_TRAINING_RECEIVER_INVALID_STATE:
	case V34_TRAINING_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result drive_data_receiver(
	struct v34_phase4_exchange *exchange, int16_t sample,
	uint32_t *events, struct v34_received_mapping_frame *received)
{
	struct v34_survivor_decision current;
	struct v34_received_mapping_frame decoded;
	enum v34_data_line_receiver_result result =
		v34_data_line_receiver_sample(
			&exchange->receive.data, sample, &current, &decoded);

	switch (result) {
	case V34_DATA_LINE_RECEIVER_OK:
	case V34_DATA_LINE_RECEIVER_INTERVAL_READY:
	case V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS:
		return V34_PHASE4_EXCHANGE_OK;
	case V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY:
		*received = decoded;
		*events |= V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED;
		return V34_PHASE4_EXCHANGE_OK;
	case V34_DATA_LINE_RECEIVER_B1_COMPLETE:
		exchange->receive_b1_complete = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_PEER_B1_RECEIVED;
		return V34_PHASE4_EXCHANGE_OK;
	case V34_DATA_LINE_RECEIVER_FAILED:
		return V34_PHASE4_EXCHANGE_FAILED;
	case V34_DATA_LINE_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE4_EXCHANGE_OUT_OF_RANGE;
	case V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT:
	case V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result drive_receiver(
	struct v34_phase4_exchange *exchange, int16_t sample,
	uint32_t *events, struct v34_received_mapping_frame *received)
{
	return exchange->receive_data_active ?
		drive_data_receiver(exchange, sample, events, received) :
		drive_training_receiver(exchange, sample, events);
}

static void note_ready(
	struct v34_phase4_exchange *exchange, uint32_t *events)
{
	if (!exchange->ready_announced &&
	    exchange->transmit_mp_prime_started &&
	    exchange->transmit_mp_prime_records_completed ==
		    V34_PHASE4_MIN_MP_PRIME_RECORDS &&
	    exchange->peer_mp_prime_received &&
	    exchange->data_startup_prepared) {
		exchange->ready_announced = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_READY_FOR_E;
	}
	if (!exchange->online_announced && exchange->transmit_b1_complete &&
	    exchange->receive_b1_complete) {
		exchange->online_announced = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_ONLINE;
	}
}

static bool terminal_result(enum v34_phase4_exchange_result result)
{
	return result == V34_PHASE4_EXCHANGE_FAILED ||
	       result == V34_PHASE4_EXCHANGE_NO_COMMON_RATE ||
	       result == V34_PHASE4_EXCHANGE_OUT_OF_RANGE ||
	       result == V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH ||
	       result == V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase4_exchange_result generate_transmit_sample(
	struct v34_phase4_exchange *exchange,
	v34_mapping_frame_source source, void *source_context,
	int16_t *sample, uint32_t *events)
{
	bool accepted;
	enum v34_phase4_exchange_result result;

	if (!exchange->transmit_data_active) {
		result = transmit_result(v34_training_transmitter_sample(
			&exchange->transmit.training, sample));
		if (result == V34_PHASE4_EXCHANGE_OK)
			note_transmitter(exchange, events);
		return result;
	}
	result = data_transmit_result(v34_data_stream_sample(
		&exchange->transmit.data, source, source_context, sample,
		&accepted));
	if (result != V34_PHASE4_EXCHANGE_OK)
		return result;
	if (!exchange->transmit_b1_complete &&
	    !v34_data_stream_is_transmitting_b1(&exchange->transmit.data)) {
		exchange->transmit_b1_complete = true;
		*events |= V34_PHASE4_EXCHANGE_EVENT_TRANSMIT_B1_COMPLETE;
	}
	return V34_PHASE4_EXCHANGE_OK;
}

enum v34_phase4_exchange_result v34_phase4_exchange_sample(
	struct v34_phase4_exchange *exchange, int16_t received_sample,
	v34_mapping_frame_source source, void *source_context,
	int16_t *transmit_sample, uint32_t *events,
	struct v34_received_mapping_frame *received)
{
	struct v34_phase4_exchange next;
	struct v34_received_mapping_frame decoded;
	enum v34_phase4_exchange_result result;
	int16_t generated;
	uint32_t emitted = 0;

	if (!v34_phase4_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || events == NULL || received == NULL ||
	    outputs_overlap(exchange, transmit_sample, events, received))
		return V34_PHASE4_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed)
		return V34_PHASE4_EXCHANGE_FAILED;
	if (exchange->data_preparation_required)
		return V34_PHASE4_EXCHANGE_INVALID_STATE;
	next = *exchange;
	result = prepare_transmitter(&next, &emitted);
	if (result != V34_PHASE4_EXCHANGE_OK) {
		exchange->failed = true;
		return result;
	}
	result = generate_transmit_sample(
		&next, source, source_context, &generated, &emitted);
	if (result == V34_PHASE4_EXCHANGE_SOURCE_FAILURE)
		return result;
	if (result != V34_PHASE4_EXCHANGE_OK) {
		exchange->failed = true;
		return result;
	}
	result = drive_receiver(&next, received_sample, &emitted, &decoded);
	if (result == V34_PHASE4_EXCHANGE_OK)
		result = prepare_transmitter(&next, &emitted);
	if (result == V34_PHASE4_EXCHANGE_OK)
		note_ready(&next, &emitted);
	if (terminal_result(result))
		next.failed = true;
	if (!v34_phase4_exchange_is_valid(&next)) {
		exchange->failed = true;
		return V34_PHASE4_EXCHANGE_NUMERIC_FAILURE;
	}
	*exchange = next;
	*transmit_sample = generated;
	*events = emitted;
	if ((emitted & V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) != 0)
		*received = decoded;
	return result;
}

const char *v34_phase4_exchange_result_name(
	enum v34_phase4_exchange_result result)
{
	switch (result) {
	case V34_PHASE4_EXCHANGE_OK:
		return "ok";
	case V34_PHASE4_EXCHANGE_SOURCE_FAILURE:
		return "mapping-frame source failure";
	case V34_PHASE4_EXCHANGE_FAILED:
		return "failed";
	case V34_PHASE4_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE4_EXCHANGE_INVALID_STATE:
		return "invalid state";
	case V34_PHASE4_EXCHANGE_NO_COMMON_RATE:
		return "no common rate";
	case V34_PHASE4_EXCHANGE_OUT_OF_RANGE:
		return "out of range";
	case V34_PHASE4_EXCHANGE_CLIPPING_RISK:
		return "clipping risk";
	case V34_PHASE4_EXCHANGE_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V34_PHASE4_EXCHANGE_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
