// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_exchange.h"

#include "memory_region.h"
#include "phase2_ranging.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE3_ECHO_TRAINING_SCALE_NUMERATOR = 15,
	V34_PHASE3_ECHO_TRAINING_SCALE_SHIFT = 3,
	V34_PHASE3_ECHO_TRAINING_OFFSET_SYMBOLS = 0x024c,
};

static bool roles_are_reciprocal(
	const struct v34_phase3_profile *local,
	const struct v34_phase3_profile *peer)
{
	return v34_phase3_profile_is_valid(local) &&
	       v34_phase3_profile_is_valid(peer) &&
	       local->local_role != peer->local_role;
}

static bool answer_silence_samples(
	uint32_t sample_rate, uint32_t *samples)
{
	uint64_t scaled;

	if (samples == NULL)
		return false;
	scaled = (uint64_t)sample_rate * V34_PHASE3_ANSWER_SILENCE_MS;
	/* Round to the nearest sample. The exact result remains inside the
	 * specified 70 +/- 5 ms interval at every supported line rate. */
	scaled = (scaled + 500u) / 1000u;
	if (scaled == 0 || scaled > UINT32_MAX)
		return false;
	*samples = (uint32_t)scaled;
	return true;
}

static bool receiver_complete(
	const struct v34_phase3_exchange *exchange)
{
	return exchange->receive.stage != V34_PHASE34_RECEIVER_PHASE3;
}

static bool zero_md_trn_target(
	const struct v34_phase3_profile *profile, uint32_t *symbols)
{
	uint64_t baud;
	uint64_t terminal_count;
	uint64_t pp_terminal_count = V34_PHASE3_PP_SEQUENCE_SYMBOLS - 1u;

	if (!v34_phase3_profile_is_valid(profile) || symbols == NULL ||
	    profile->md_symbol_count != 0)
		return false;
	if (profile->local_role == V34_CALLING_STATION) {
		*symbols = V34_TRAINING_SEQUENCE_MIN_SYMBOLS;
		return true;
	}
	/* The answerer's echo-training counter spans PP and TRN. PP leaves it
	 * one below its symbol count; zero measured delay ends it at 15/8 baud
	 * minus the fixed echo-training offset. */
	baud = ((uint64_t)profile->symbol_rate_numerator +
		profile->symbol_rate_denominator / 2u) /
		profile->symbol_rate_denominator;
	terminal_count = baud * V34_PHASE3_ECHO_TRAINING_SCALE_NUMERATOR >>
		V34_PHASE3_ECHO_TRAINING_SCALE_SHIFT;
	if (terminal_count <= V34_PHASE3_ECHO_TRAINING_OFFSET_SYMBOLS)
		return false;
	terminal_count -= V34_PHASE3_ECHO_TRAINING_OFFSET_SYMBOLS;
	if (terminal_count <= pp_terminal_count ||
	    terminal_count - pp_terminal_count > UINT32_MAX)
		return false;
	*symbols = (uint32_t)(terminal_count - pp_terminal_count);
	return *symbols >= V34_TRAINING_SEQUENCE_MIN_SYMBOLS;
}

static bool maximum_trn_target(
	const struct v34_phase3_profile *profile,
	uint16_t round_trip_delay_samples, uint32_t *symbols)
{
	uint64_t samples;
	uint64_t total_symbols;
	uint32_t prefix = V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_PHASE3_PP_SEQUENCE_SYMBOLS;

	if (round_trip_delay_samples >
	    PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES -
		    PHASE2_RANGING_TURNAROUND_SAMPLES)
		return false;
	/* V.34 11.3.1.1.6 permits 2000 ms + 2 RTD for the caller;
	 * 11.3.1.2.3 permits 2000 ms + RTD for the answerer. Count the
	 * entire zero-MD S/S-bar and PP prefix conservatively, and reserve
	 * one host sample for rounding the final waveform boundary. */
	samples = 2u * V34_RECEIVE_TIMING_SAMPLE_RATE - 1u +
		(uint64_t)round_trip_delay_samples *
		(profile->local_role == V34_CALLING_STATION ? 2u : 1u);
	total_symbols = samples * profile->symbol_rate_numerator /
		((uint64_t)V34_RECEIVE_TIMING_SAMPLE_RATE *
		 profile->symbol_rate_denominator);
	if (total_symbols < prefix + V34_TRAINING_SEQUENCE_MIN_SYMBOLS ||
	    total_symbols - prefix > UINT32_MAX)
		return false;
	*symbols = (uint32_t)(total_symbols - prefix);
	return true;
}

static bool trn_timing_is_valid(
	const struct v34_phase3_exchange *exchange)
{
	uint32_t expected;
	uint32_t limit;
	uint32_t target = exchange->trn_target_symbols;

	if (!zero_md_trn_target(
		    &exchange->transmit.line.profile, &expected) ||
	    !maximum_trn_target(&exchange->transmit.line.profile,
				exchange->round_trip_delay_samples, &limit) ||
	    (exchange->trn_limit_symbols != expected &&
	     exchange->trn_limit_symbols != limit) ||
	    target < V34_TRAINING_SEQUENCE_MIN_SYMBOLS ||
	    target > exchange->trn_limit_symbols)
		return false;
	switch (exchange->transmit.phase3.stage) {
	case V34_PHASE3_TRANSMITTER_S:
	case V34_PHASE3_TRANSMITTER_PP:
		return exchange->transmit.phase3.trn_symbols == 0 &&
		       target == exchange->trn_limit_symbols;
	case V34_PHASE3_TRANSMITTER_TRN:
		return exchange->transmit.phase3.trn_symbols <= target;
	case V34_PHASE3_TRANSMITTER_J:
	case V34_PHASE3_TRANSMITTER_COMPLETE:
		return exchange->transmit.phase3.trn_symbols == target;
	}
	return false;
}

static bool outputs_overlap(
	const int16_t *sample, const uint32_t *events)
{
	return memory_regions_overlap(
		sample, sizeof(*sample), events, sizeof(*events));
}

static bool handoff_ready(const struct v34_phase3_exchange *exchange)
{
	if (exchange->failed || !receiver_complete(exchange))
		return false;
	if (exchange->local_role == V34_CALLING_STATION)
		return exchange->transmit_started &&
		       exchange->transmit.stage ==
			       V34_TRAINING_TRANSMITTER_PHASE3 &&
		       exchange->transmit.phase3.stage ==
			       V34_PHASE3_TRANSMITTER_J;
	return exchange->local_role == V34_ANSWERING_STATION &&
	       exchange->transmit_stopped &&
	       exchange->transmit.stage ==
		       V34_TRAINING_TRANSMITTER_PHASE4_S;
}

bool v34_phase3_exchange_is_valid(
	const struct v34_phase3_exchange *exchange)
{
	uint32_t expected_silence;
	uint32_t sample_rate;

	if (exchange == NULL ||
	    !v34_training_transmitter_is_valid(&exchange->transmit) ||
	    exchange->transmit.rate_renegotiation ||
	    !v34_phase34_receiver_is_valid(&exchange->receive) ||
	    !trn_timing_is_valid(exchange) ||
	    !roles_are_reciprocal(&exchange->transmit.line.profile,
				 &exchange->receive.transmitting_profile) ||
	    exchange->local_role != exchange->transmit.line.profile.local_role ||
	    v34_constellation_size_is_valid(
		    exchange->transmit.phase4_constellation) !=
		    receiver_complete(exchange) ||
	    (receiver_complete(exchange) &&
	     exchange->transmit.phase4_constellation !=
		     exchange->receive.phase3_handoff.j.sequence
			     .remote_constellation) ||
	    (v34_phase34_receiver_has_failed(&exchange->receive) &&
	     !exchange->failed))
		return false;
	sample_rate = exchange->transmit.line.carrier.channel.sample_rate;
	if (sample_rate != V34_RECEIVE_TIMING_SAMPLE_RATE)
		return false;
	if (exchange->local_role == V34_CALLING_STATION) {
		if (exchange->answer_silence_samples != 0 ||
		    exchange->silence_samples_remaining != 0 ||
		    exchange->stop_transmit_requested ||
		    exchange->transmit_stopped ||
		    (exchange->transmit_started && !receiver_complete(exchange)) ||
		    exchange->receive.phase4_s_rearmed)
			return false;
	} else if (exchange->local_role == V34_ANSWERING_STATION) {
		if (!answer_silence_samples(sample_rate, &expected_silence) ||
		    exchange->answer_silence_samples != expected_silence ||
		    exchange->silence_samples_remaining > expected_silence ||
		    exchange->transmit_started !=
			    (exchange->silence_samples_remaining == 0) ||
		    (exchange->stop_transmit_requested &&
		     !exchange->transmit_started) ||
		    (exchange->transmit_stopped &&
		     (!exchange->stop_transmit_requested ||
		      exchange->transmit.stage !=
			      V34_TRAINING_TRANSMITTER_PHASE4_S)))
			return false;
	} else {
		return false;
	}
	if (!exchange->transmit_started &&
	    exchange->transmit.line.generated_samples != 0)
		return false;
	return !exchange->handoff_announced || handoff_ready(exchange);
}

enum v34_phase3_exchange_result v34_phase3_exchange_init(
	struct v34_phase3_exchange *exchange,
	const struct v34_phase3_profile *local_transmit,
	const struct v34_phase3_profile *peer_transmit,
	const struct v34_phase3_exchange_configuration *configuration,
	int16_t initial_receive_gain)
{
	struct v34_phase3_exchange initialized = { 0 };
	enum v34_training_transmitter_result transmit_result;
	uint32_t limit;

	if (exchange == NULL || configuration == NULL ||
	    !roles_are_reciprocal(local_transmit, peer_transmit) ||
	    initial_receive_gain <= 0 ||
	    configuration->sample_rate != V34_RECEIVE_TIMING_SAMPLE_RATE ||
	    !v34_constellation_size_is_valid(
		    configuration->requested_remote_constellation))
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	transmit_result = v34_training_transmitter_init(
		&initialized.transmit, local_transmit,
		configuration->requested_remote_constellation,
		configuration->sample_rate,
		configuration->nominal_peak_amplitude);
	if (transmit_result != V34_TRAINING_TRANSMITTER_OK) {
		if (transmit_result == V34_TRAINING_TRANSMITTER_OUT_OF_RANGE)
			return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
		if (transmit_result == V34_TRAINING_TRANSMITTER_CLIPPING_RISK)
			return V34_PHASE3_EXCHANGE_CLIPPING_RISK;
		return transmit_result ==
			       V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT ?
			       V34_PHASE3_EXCHANGE_INVALID_ARGUMENT :
			       V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	}
	if (v34_phase34_receiver_init(
		    &initialized.receive, peer_transmit,
		    configuration->requested_remote_constellation,
		    initial_receive_gain) !=
	    V34_PHASE34_RECEIVER_OK)
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	initialized.local_role = local_transmit->local_role;
	initialized.round_trip_delay_samples =
		configuration->round_trip_delay_samples;
	if (!zero_md_trn_target(
		    local_transmit, &initialized.trn_limit_symbols))
		return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
	if (!maximum_trn_target(local_transmit,
		initialized.round_trip_delay_samples, &limit))
		return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
	if (configuration->extend_training)
		initialized.trn_limit_symbols = limit;
	initialized.trn_target_symbols = initialized.trn_limit_symbols;
	if (initialized.local_role == V34_ANSWERING_STATION) {
		if (!answer_silence_samples(
			    configuration->sample_rate,
			    &initialized.answer_silence_samples))
			return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
		initialized.silence_samples_remaining =
			initialized.answer_silence_samples;
	}
	if (!v34_phase3_exchange_is_valid(&initialized))
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	*exchange = initialized;
	return V34_PHASE3_EXCHANGE_OK;
}

enum v34_phase3_exchange_result v34_phase3_exchange_finish_training(
	struct v34_phase3_exchange *exchange)
{
	if (!v34_phase3_exchange_is_valid(exchange))
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed || !exchange->transmit_started ||
	    exchange->transmit.stage != V34_TRAINING_TRANSMITTER_PHASE3 ||
	    exchange->transmit.phase3.stage != V34_PHASE3_TRANSMITTER_TRN ||
	    exchange->transmit.phase3.trn_symbols <
		    V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return V34_PHASE3_EXCHANGE_INVALID_STATE;
	exchange->trn_target_symbols = exchange->transmit.phase3.trn_symbols;
	return V34_PHASE3_EXCHANGE_OK;
}

bool v34_phase3_exchange_has_failed(
	const struct v34_phase3_exchange *exchange)
{
	return v34_phase3_exchange_is_valid(exchange) && exchange->failed;
}

bool v34_phase3_exchange_is_handoff_ready(
	const struct v34_phase3_exchange *exchange)
{
	return v34_phase3_exchange_is_valid(exchange) &&
	       handoff_ready(exchange);
}

bool v34_phase3_exchange_handoff_is_valid(
	const struct v34_phase3_exchange_handoff *handoff)
{
	if (handoff == NULL ||
	    !v34_training_transmitter_is_valid(&handoff->transmit) ||
	    handoff->transmit.rate_renegotiation ||
	    !v34_phase34_receiver_is_valid(&handoff->receive) ||
	    v34_phase34_receiver_has_failed(&handoff->receive) ||
	    handoff->receive.stage != V34_PHASE34_RECEIVER_PHASE4 ||
	    !v34_constellation_size_is_valid(
		    handoff->transmit.phase4_constellation) ||
	    handoff->transmit.phase4_constellation !=
		    handoff->receive.phase3_handoff.j.sequence
			    .remote_constellation ||
	    !roles_are_reciprocal(&handoff->transmit.line.profile,
				 &handoff->receive.transmitting_profile))
		return false;
	if (handoff->receive.phase4_s_rearmed)
		return false;
	return handoff->transmit.line.profile.local_role ==
		       V34_CALLING_STATION ?
		       handoff->transmit.stage ==
			       V34_TRAINING_TRANSMITTER_PHASE3 &&
		       handoff->transmit.phase3.stage ==
			       V34_PHASE3_TRANSMITTER_J :
		       handoff->transmit.stage ==
			       V34_TRAINING_TRANSMITTER_PHASE4_S &&
		       handoff->transmit.phase3.stage ==
			       V34_PHASE3_TRANSMITTER_COMPLETE;
}

static enum v34_phase3_exchange_result drive_silence(
	struct v34_training_line *line, int16_t *sample)
{
	static const struct qam_point zero;
	const struct qam_point *point = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting)
		point = &zero;
	if (v34_training_line_sample(line, point, sample, &accepted) !=
		    V34_TRAINING_LINE_OK ||
	    accepted != accepting)
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	return V34_PHASE3_EXCHANGE_OK;
}

static enum v34_phase3_exchange_result drive_transmitter(
	struct v34_phase3_exchange *exchange,
	int16_t *sample, uint32_t *events)
{
	enum v34_training_transmitter_result result;

	if (!exchange->transmit_started) {
		*sample = 0;
		if (exchange->local_role == V34_ANSWERING_STATION &&
		    --exchange->silence_samples_remaining == 0) {
			exchange->transmit_started = true;
			*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED;
		}
		return V34_PHASE3_EXCHANGE_OK;
	}
	if (exchange->transmit.phase3.stage ==
		    V34_PHASE3_TRANSMITTER_TRN &&
	    exchange->transmit.phase3.trn_symbols ==
		    exchange->trn_target_symbols) {
		if (v34_training_transmitter_start_j(&exchange->transmit) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
		*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED;
	}
	if (exchange->stop_transmit_requested &&
	    exchange->transmit.phase3.stage ==
		    V34_PHASE3_TRANSMITTER_J &&
	    exchange->transmit.phase3.j_symbols >=
		    V34_PHASE3_J_PATTERN_SYMBOLS &&
	    exchange->transmit.phase3.j_symbols %
		    V34_PHASE3_J_PATTERN_SYMBOLS == 0 &&
	    v34_training_transmitter_finish_j(&exchange->transmit) !=
		    V34_TRAINING_TRANSMITTER_OK)
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	if (exchange->transmit.stage != V34_TRAINING_TRANSMITTER_PHASE3) {
		if (!exchange->transmit_stopped) {
			exchange->transmit_stopped = true;
			*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STOPPED;
		}
		return drive_silence(&exchange->transmit.line, sample);
	}
	result = v34_training_transmitter_sample(&exchange->transmit, sample);
	if (result == V34_TRAINING_TRANSMITTER_OK)
		return V34_PHASE3_EXCHANGE_OK;
	return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase3_exchange_result discard_caller_wait_sample(
	struct v34_phase3_exchange *exchange, int16_t sample)
{
	struct v34_line_receiver *line = &exchange->receive.line;
	struct v34_adaptive_equalizer_observation observation;
	enum v34_line_receiver_result result;

	if (v34_receive_timing_set_feedback_held(&line->timing, true) !=
		    V34_RECEIVE_TIMING_OK ||
	    v34_receive_timing_freeze_gain(&line->timing) !=
		    V34_RECEIVE_TIMING_OK)
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	result = v34_line_receiver_sample(line, sample, &observation);
	if (result == V34_LINE_RECEIVER_OBSERVATION_READY)
		return v34_adaptive_equalizer_discard(
			       &line->equalizer, &observation) ==
			       V34_ADAPTIVE_EQUALIZER_OK ?
		       V34_PHASE3_EXCHANGE_OK :
		       V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	return result == V34_LINE_RECEIVER_OK ? V34_PHASE3_EXCHANGE_OK :
		V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase3_exchange_result drive_receiver(
	struct v34_phase3_exchange *exchange, int16_t sample,
	uint32_t *events)
{
	/* The answerer deliberately starts caller-S acquisition at its local J
	 * boundary. Earlier input belongs to the peer's prescribed silence and
	 * is discarded so the initialized receiver begins on that boundary. */
	if (exchange->local_role == V34_ANSWERING_STATION &&
	    exchange->transmit.phase3.stage < V34_PHASE3_TRANSMITTER_J)
		return V34_PHASE3_EXCHANGE_OK;
	/* The caller's answering peer cannot begin Phase 4 S before receiving
	 * local J. Discard decisions from the remaining peer stream and local
	 * echo while keeping the waveform clock continuous. Hold learned gain,
	 * coefficients and timing feedback until the Phase 4 rearm. */
	if (exchange->local_role == V34_CALLING_STATION &&
	    receiver_complete(exchange) &&
	    !exchange->receive.phase4_s_rearmed)
		return discard_caller_wait_sample(exchange, sample);
	{
		struct v34_phase34_receive_output output = { 0 };
		enum v34_phase34_receiver_result result =
			v34_phase34_receiver_sample(
				&exchange->receive, sample, &output);

		switch (result) {
		case V34_PHASE34_RECEIVER_OK:
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_TIMING_ACQUIRED:
			*events |=
				V34_PHASE3_EXCHANGE_EVENT_PEER_TIMING_ACQUIRED;
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_PHASE3_S_DETECTED:
			*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED;
			if (exchange->local_role == V34_ANSWERING_STATION) {
				if (exchange->transmit.phase3.stage !=
				    V34_PHASE3_TRANSMITTER_J)
					return V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH;
				exchange->stop_transmit_requested = true;
			}
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_MD_COMPLETE:
			*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_MD_COMPLETE;
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_REPEATED_S_DETECTED:
			*events |=
				V34_PHASE3_EXCHANGE_EVENT_PEER_REPEATED_S_DETECTED;
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_PP_COMPLETE:
			*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_PP_COMPLETE;
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_J_DETECTED:
			*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED;
			if (exchange->local_role == V34_CALLING_STATION) {
				exchange->transmit_started = true;
				*events |=
					V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED;
			}
			return V34_PHASE3_EXCHANGE_OK;
		case V34_PHASE34_RECEIVER_FAILED:
			return V34_PHASE3_EXCHANGE_FAILED;
		case V34_PHASE34_RECEIVER_OUT_OF_RANGE:
			return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
		case V34_PHASE34_RECEIVER_RATE_RENEGOTIATION:
		case V34_PHASE34_RECEIVER_PHASE4_S_DETECTED:
		case V34_PHASE34_RECEIVER_TRN_COMPLETE:
		case V34_PHASE34_RECEIVER_RECORD_READY:
		case V34_PHASE34_RECEIVER_E_COMPLETE:
		case V34_PHASE34_RECEIVER_INTERVAL_READY:
		case V34_PHASE34_RECEIVER_MAPPING_FRAME_READY:
		case V34_PHASE34_RECEIVER_B1_IN_PROGRESS:
		case V34_PHASE34_RECEIVER_B1_COMPLETE:
		case V34_PHASE34_RECEIVER_COMPLETE:
		case V34_PHASE34_RECEIVER_INVALID_ARGUMENT:
		case V34_PHASE34_RECEIVER_INVALID_STATE:
		case V34_PHASE34_RECEIVER_NUMERIC_FAILURE:
			break;
		}
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	}
}

static bool result_is_terminal(enum v34_phase3_exchange_result result)
{
	return result == V34_PHASE3_EXCHANGE_FAILED ||
	       result == V34_PHASE3_EXCHANGE_OUT_OF_RANGE ||
	       result == V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH ||
	       result == V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
}

enum v34_phase3_exchange_result v34_phase3_exchange_sample(
	struct v34_phase3_exchange *exchange, int16_t received_sample,
	int16_t *transmit_sample, uint32_t *events)
{
	struct v34_phase3_exchange next;
	enum v34_phase3_exchange_result result;
	int16_t generated = 0;
	uint32_t emitted = 0;

	if (!v34_phase3_exchange_is_valid(exchange) ||
	    transmit_sample == NULL || events == NULL ||
	    outputs_overlap(transmit_sample, events))
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed)
		return V34_PHASE3_EXCHANGE_FAILED;
	next = *exchange;
	result = drive_transmitter(&next, &generated, &emitted);
	if (result != V34_PHASE3_EXCHANGE_OK) {
		exchange->failed = true;
		return result;
	}
	result = drive_receiver(&next, received_sample, &emitted);
	if (result_is_terminal(result))
		next.failed = true;
	if (result != V34_PHASE3_EXCHANGE_OK && !result_is_terminal(result))
		return result;
	if (!next.failed && receiver_complete(&next) &&
	    !v34_constellation_size_is_valid(
		    next.transmit.phase4_constellation) &&
	    v34_training_transmitter_select_phase4_constellation(
		    &next.transmit,
		    next.receive.phase3_handoff.j.sequence
			    .remote_constellation) !=
		    V34_TRAINING_TRANSMITTER_OK) {
		next.failed = true;
		result = V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	}
	if (!next.failed && handoff_ready(&next) &&
	    !next.handoff_announced) {
		next.handoff_announced = true;
		emitted |= V34_PHASE3_EXCHANGE_EVENT_HANDOFF_READY;
	}
	if (!v34_phase3_exchange_is_valid(&next)) {
		exchange->failed = true;
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	}
	*exchange = next;
	*transmit_sample = generated;
	*events = emitted;
	return result;
}

enum v34_phase3_exchange_result v34_phase3_exchange_handoff(
	const struct v34_phase3_exchange *exchange,
	struct v34_phase3_exchange_handoff *handoff)
{
	struct v34_phase3_exchange_handoff generated;

	if (!v34_phase3_exchange_is_valid(exchange) || handoff == NULL)
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	if (exchange->failed)
		return V34_PHASE3_EXCHANGE_FAILED;
	if (!v34_phase3_exchange_is_handoff_ready(exchange))
		return V34_PHASE3_EXCHANGE_INVALID_STATE;
	generated.transmit = exchange->transmit;
	generated.receive = exchange->receive;
	if (!v34_phase3_exchange_handoff_is_valid(&generated))
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	*handoff = generated;
	return V34_PHASE3_EXCHANGE_OK;
}
