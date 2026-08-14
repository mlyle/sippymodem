// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_exchange.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>

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
	return v34_phase3_receiver_is_complete(&exchange->receive.points);
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
		       exchange->transmit.symbols.stage ==
			       V34_PHASE3_TRANSMITTER_J;
	return exchange->local_role == V34_ANSWERING_STATION &&
	       exchange->transmit_stopped;
}

bool v34_phase3_exchange_is_valid(
	const struct v34_phase3_exchange *exchange)
{
	uint32_t expected_silence;
	uint32_t sample_rate;

	if (exchange == NULL ||
	    !v34_phase3_line_transmitter_is_valid(&exchange->transmit) ||
	    !v34_phase3_line_receiver_is_valid(&exchange->receive) ||
	    !roles_are_reciprocal(&exchange->transmit.line.profile,
				 &exchange->receive.transmitting_profile) ||
	    exchange->local_role != exchange->transmit.line.profile.local_role ||
	    (v34_phase3_line_receiver_has_failed(&exchange->receive) &&
	     !exchange->failed))
		return false;
	sample_rate = exchange->transmit.line.carrier.channel.sample_rate;
	if (sample_rate != exchange->receive.demodulator.channel.sample_rate)
		return false;
	if (exchange->local_role == V34_CALLING_STATION) {
		if (exchange->answer_silence_samples != 0 ||
		    exchange->silence_samples_remaining != 0 ||
		    exchange->stop_transmit_requested ||
		    exchange->transmit_stopped ||
		    (exchange->transmit_started && !receiver_complete(exchange)))
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
		      exchange->transmit.symbols.stage !=
			      V34_PHASE3_TRANSMITTER_COMPLETE)))
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
	const struct v34_phase3_exchange_configuration *configuration)
{
	struct v34_phase3_exchange initialized = { 0 };
	enum v34_phase3_line_transmitter_result transmit_result;
	enum v34_phase3_line_receiver_result receive_result;

	if (exchange == NULL || configuration == NULL ||
	    !roles_are_reciprocal(local_transmit, peer_transmit) ||
	    !v34_phase3_line_receiver_configuration_is_valid(
		    &configuration->receive) ||
	    !v34_constellation_size_is_valid(
		    configuration->requested_remote_constellation))
		return V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	transmit_result = v34_phase3_line_transmitter_init(
		&initialized.transmit, local_transmit,
		configuration->requested_remote_constellation,
		configuration->sample_rate,
		configuration->nominal_peak_amplitude);
	if (transmit_result != V34_PHASE3_LINE_TRANSMITTER_OK) {
		if (transmit_result == V34_PHASE3_LINE_TRANSMITTER_OUT_OF_RANGE)
			return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
		if (transmit_result == V34_PHASE3_LINE_TRANSMITTER_CLIPPING_RISK)
			return V34_PHASE3_EXCHANGE_CLIPPING_RISK;
		return transmit_result ==
			       V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT ?
			       V34_PHASE3_EXCHANGE_INVALID_ARGUMENT :
			       V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	}
	receive_result = v34_phase3_line_receiver_init(
		&initialized.receive, peer_transmit,
		configuration->sample_rate, &configuration->receive);
	if (receive_result != V34_PHASE3_LINE_RECEIVER_OK)
		return receive_result == V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE ?
			       V34_PHASE3_EXCHANGE_OUT_OF_RANGE :
			       V34_PHASE3_EXCHANGE_INVALID_ARGUMENT;
	initialized.local_role = local_transmit->local_role;
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
	    !v34_training_line_is_valid(&handoff->transmit_line) ||
	    !v34_phase3_transmitter_is_valid(&handoff->phase3_transmit) ||
	    !v34_phase3_line_handoff_is_valid(&handoff->receive) ||
	    handoff->transmit_line.profile.local_role !=
		    handoff->phase3_transmit.local_role ||
	    !roles_are_reciprocal(&handoff->transmit_line.profile,
				 &handoff->receive.transmitting_profile))
		return false;
	return handoff->transmit_line.profile.local_role ==
		       V34_CALLING_STATION ?
		       handoff->phase3_transmit.stage ==
			       V34_PHASE3_TRANSMITTER_J :
		       handoff->phase3_transmit.stage ==
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
	enum v34_phase3_line_transmitter_result result;

	if (!exchange->transmit_started) {
		*sample = 0;
		if (exchange->local_role == V34_ANSWERING_STATION &&
		    --exchange->silence_samples_remaining == 0) {
			exchange->transmit_started = true;
			*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED;
		}
		return V34_PHASE3_EXCHANGE_OK;
	}
	if (exchange->transmit.symbols.stage ==
		    V34_PHASE3_TRANSMITTER_TRN &&
	    exchange->transmit.symbols.trn_symbols ==
		    V34_PHASE3_TRN_MIN_SYMBOLS) {
		if (v34_phase3_line_transmitter_start_j(&exchange->transmit) !=
		    V34_PHASE3_LINE_TRANSMITTER_OK)
			return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
		*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED;
	}
	if (exchange->stop_transmit_requested &&
	    exchange->transmit.symbols.stage ==
		    V34_PHASE3_TRANSMITTER_J &&
	    exchange->transmit.symbols.j_symbols >=
		    V34_PHASE3_J_PATTERN_SYMBOLS &&
	    exchange->transmit.symbols.j_symbols %
		    V34_PHASE3_J_PATTERN_SYMBOLS == 0 &&
	    v34_phase3_line_transmitter_finish_j(&exchange->transmit) !=
		    V34_PHASE3_LINE_TRANSMITTER_OK)
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	if (exchange->transmit.symbols.stage ==
	    V34_PHASE3_TRANSMITTER_COMPLETE) {
		if (!exchange->transmit_stopped) {
			exchange->transmit_stopped = true;
			*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STOPPED;
		}
		return drive_silence(&exchange->transmit.line, sample);
	}
	result = v34_phase3_line_transmitter_sample(&exchange->transmit, sample);
	if (result == V34_PHASE3_LINE_TRANSMITTER_OK)
		return V34_PHASE3_EXCHANGE_OK;
	return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
}

static enum v34_phase3_exchange_result drive_receiver(
	struct v34_phase3_exchange *exchange, int16_t sample,
	uint32_t *events)
{
	enum v34_phase3_line_receiver_result result;

	result = receiver_complete(exchange) ?
		v34_phase3_line_receiver_track_sample(
			&exchange->receive, sample) :
		v34_phase3_line_receiver_sample(&exchange->receive, sample);
	switch (result) {
	case V34_PHASE3_LINE_RECEIVER_OK:
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED:
		*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_TIMING_ACQUIRED;
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_S_DETECTED:
		*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED;
		if (exchange->local_role == V34_ANSWERING_STATION) {
			if (exchange->transmit.symbols.stage !=
			    V34_PHASE3_TRANSMITTER_J)
				return V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH;
			exchange->stop_transmit_requested = true;
		}
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_MD_COMPLETE:
		*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_MD_COMPLETE;
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_REPEATED_S_DETECTED:
		*events |=
			V34_PHASE3_EXCHANGE_EVENT_PEER_REPEATED_S_DETECTED;
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_PP_COMPLETE:
		*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_PP_COMPLETE;
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_J_DETECTED:
		*events |= V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED;
		if (exchange->local_role == V34_CALLING_STATION) {
			exchange->transmit_started = true;
			*events |= V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED;
		}
		return V34_PHASE3_EXCHANGE_OK;
	case V34_PHASE3_LINE_RECEIVER_FAILED:
		return V34_PHASE3_EXCHANGE_FAILED;
	case V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE:
		return V34_PHASE3_EXCHANGE_OUT_OF_RANGE;
	case V34_PHASE3_LINE_RECEIVER_COMPLETE:
	case V34_PHASE3_LINE_RECEIVER_INVALID_STATE:
	case V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT:
	case V34_PHASE3_LINE_RECEIVER_NUMERIC_FAILURE:
		break;
	}
	return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
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
	generated.transmit_line = exchange->transmit.line;
	generated.phase3_transmit = exchange->transmit.symbols;
	if (v34_phase3_line_receiver_handoff(
		    &exchange->receive, &generated.receive) !=
		    V34_PHASE3_LINE_RECEIVER_OK ||
	    !v34_phase3_exchange_handoff_is_valid(&generated))
		return V34_PHASE3_EXCHANGE_NUMERIC_FAILURE;
	*handoff = generated;
	return V34_PHASE3_EXCHANGE_OK;
}

const char *v34_phase3_exchange_result_name(
	enum v34_phase3_exchange_result result)
{
	switch (result) {
	case V34_PHASE3_EXCHANGE_OK:
		return "ok";
	case V34_PHASE3_EXCHANGE_FAILED:
		return "failed";
	case V34_PHASE3_EXCHANGE_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_EXCHANGE_INVALID_STATE:
		return "invalid state";
	case V34_PHASE3_EXCHANGE_OUT_OF_RANGE:
		return "out of range";
	case V34_PHASE3_EXCHANGE_CLIPPING_RISK:
		return "clipping risk";
	case V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V34_PHASE3_EXCHANGE_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
