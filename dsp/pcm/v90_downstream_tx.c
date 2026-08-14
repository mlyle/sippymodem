// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_tx.h"

#include "packed_bits.h"

#include <string.h>

static bool stage_is_valid(enum v90_downstream_tx_stage stage)
{
	return stage == V90_DOWNSTREAM_TX_TRN2D ||
	       stage == V90_DOWNSTREAM_TX_MP ||
	       stage == V90_DOWNSTREAM_TX_MP_PRIME ||
	       stage == V90_DOWNSTREAM_TX_ED;
}

static struct v90_mp_parameters *record_common(
	struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

static bool encode_mp_sequence(
	const struct v90_mp_record *base, uint8_t frame_bits,
	bool acknowledges_cp, uint8_t *sequence, size_t *sequence_bits)
{
	struct v90_mp_record record = *base;
	uint8_t encoded[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t bit_count = 0;

	record_common(&record)->acknowledges_cp = acknowledges_cp;
	if (v90_mp_encode(
		    &record, frame_bits, encoded, sizeof(encoded),
		    &bit_count) != V90_MP_OK ||
	    bit_count == 0 || bit_count % frame_bits != 0)
		return false;
	memcpy(sequence, encoded, sizeof(encoded));
	*sequence_bits = bit_count;
	return true;
}

static bool mp_state_is_valid(const struct v90_downstream_tx *transmitter)
{
	static const struct v90_mp_record empty_record;
	static const uint8_t empty_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t expected[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t expected_bits = 0;
	bool prime;

	if (!stage_is_valid(transmitter->stage))
		return false;
	if (!transmitter->mp_configured)
		return transmitter->stage == V90_DOWNSTREAM_TX_TRN2D &&
		       !transmitter->mp_prime_requested &&
		       !transmitter->mp_prime_record_accepted &&
		       !transmitter->ed_requested &&
		       transmitter->ed_frames == 0 &&
		       transmitter->mp_sequence_bits == 0 &&
		       transmitter->mp_bit_index == 0 &&
		       memcmp(&transmitter->mp_record, &empty_record,
			      sizeof(empty_record)) == 0 &&
		       memcmp(transmitter->mp_sequence, empty_sequence,
			      sizeof(empty_sequence)) == 0;
	if (!v90_mp_record_is_valid(&transmitter->mp_record) ||
	    v90_mp_record_acknowledges_cp(&transmitter->mp_record) ||
	    transmitter->mp_sequence_bits == 0 ||
	    transmitter->mp_bit_index >= transmitter->mp_sequence_bits ||
	    transmitter->mp_bit_index %
		    transmitter->encoder.mapping.frame_bits != 0 ||
	    (transmitter->stage == V90_DOWNSTREAM_TX_TRN2D &&
	     transmitter->mp_bit_index != 0) ||
	    (transmitter->stage != V90_DOWNSTREAM_TX_TRN2D &&
	     transmitter->accepted_frames <=
		     V90_PHASE4_TRN2D_MINIMUM_FRAMES) ||
	    (transmitter->stage == V90_DOWNSTREAM_TX_MP_PRIME &&
	     !transmitter->mp_prime_requested) ||
	    (transmitter->stage == V90_DOWNSTREAM_TX_MP &&
	     transmitter->mp_prime_requested &&
	     transmitter->mp_bit_index == 0) ||
	    (transmitter->mp_prime_record_accepted &&
	     (!transmitter->mp_prime_requested ||
	      (transmitter->stage != V90_DOWNSTREAM_TX_MP_PRIME &&
	       transmitter->stage != V90_DOWNSTREAM_TX_ED))) ||
	    (transmitter->ed_requested &&
	     !transmitter->mp_prime_requested))
		return false;
	if (transmitter->stage == V90_DOWNSTREAM_TX_ED) {
		if (!transmitter->ed_requested ||
		    !transmitter->mp_prime_record_accepted ||
		    transmitter->mp_bit_index != 0 ||
		    transmitter->ed_frames > V90_PHASE4_ED_FRAMES ||
		    transmitter->finish_requested !=
			    (transmitter->ed_frames == V90_PHASE4_ED_FRAMES) ||
		    (transmitter->complete &&
		     !transmitter->finish_requested))
			return false;
	} else if (transmitter->ed_frames != 0 ||
		   transmitter->finish_requested || transmitter->complete ||
		   (transmitter->stage == V90_DOWNSTREAM_TX_MP_PRIME &&
		    transmitter->ed_requested &&
		    transmitter->mp_prime_record_accepted &&
		    transmitter->mp_bit_index == 0)) {
		return false;
	}
	prime = transmitter->stage == V90_DOWNSTREAM_TX_MP_PRIME ||
		transmitter->stage == V90_DOWNSTREAM_TX_ED;
	return encode_mp_sequence(
		       &transmitter->mp_record,
		       transmitter->encoder.mapping.frame_bits, prime,
		       expected, &expected_bits) &&
	       expected_bits == transmitter->mp_sequence_bits &&
	       memcmp(expected, transmitter->mp_sequence,
		      sizeof(expected)) == 0;
}

static bool queued_symbol_is_zero(
	const struct v90_downstream_symbol *symbol)
{
	return symbol->pcm.ucode == 0 && !symbol->pcm.positive &&
	       symbol->pcm.octet == 0 && symbol->pcm.sample == 0 &&
	       symbol->shaping_rule == V90_SPECTRAL_RULE_A &&
	       !symbol->shaping_rule_valid &&
	       !symbol->shaping_frame_start;
}

static bool queue_is_valid(const struct v90_downstream_tx *transmitter)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;

	if (transmitter->queue_count > V90_CP_FRAME_INTERVALS)
		return false;
	for (size_t index = 0;
	     index < V90_CP_FRAME_INTERVALS; ++index) {
		const struct v90_downstream_symbol *symbol =
			&transmitter->queue[index];

		if (index >= transmitter->queue_count) {
			if (!queued_symbol_is_zero(symbol))
				return false;
			continue;
		}
		if (!v90_pcm_symbol_is_valid(&symbol->pcm, mapping->law))
			return false;
		if (mapping->sign_redundancy == 0) {
			if (symbol->shaping_rule_valid ||
			    symbol->shaping_frame_start ||
			    symbol->shaping_rule != V90_SPECTRAL_RULE_A)
				return false;
		} else {
			uint64_t stream_index =
				transmitter->transmitted_symbols + index;

			if (!symbol->shaping_rule_valid ||
			    symbol->shaping_rule > V90_SPECTRAL_RULE_D ||
			    symbol->shaping_frame_start !=
				    (stream_index % shaping_length == 0) ||
			    (index != 0 && !symbol->shaping_frame_start &&
			     symbol->shaping_rule !=
				     transmitter->queue[index - 1u]
					     .shaping_rule))
				return false;
		}
	}
	return true;
}

static bool symbol_accounting_is_valid(
	const struct v90_downstream_tx *transmitter)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	uint64_t accepted_symbols;
	uint64_t delayed_symbols = 0;

	if (transmitter->accepted_frames >
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return false;
	accepted_symbols = transmitter->accepted_frames *
			   V90_CP_FRAME_INTERVALS;
	if (mapping->sign_redundancy != 0) {
		delayed_symbols =
			(uint64_t)transmitter->encoder.shaper.pending_count *
			(V90_CP_FRAME_INTERVALS /
			 mapping->sign_redundancy);
	}
	return transmitter->transmitted_symbols <= accepted_symbols &&
	       delayed_symbols <= accepted_symbols -
				  transmitter->transmitted_symbols &&
	       transmitter->queue_count <=
		       accepted_symbols - transmitter->transmitted_symbols -
		       delayed_symbols &&
	       transmitter->transmitted_symbols +
		       transmitter->queue_count + delayed_symbols ==
		       accepted_symbols;
}

bool v90_downstream_tx_is_valid(const struct v90_downstream_tx *transmitter)
{
	if (transmitter == NULL ||
	    !v90_pcm_frame_encoder_is_valid(&transmitter->encoder) ||
	    transmitter->encoder.mapping.kind != V90_CP_TRAINING ||
	    !v34_scrambler_is_valid(&transmitter->scrambler) ||
	    transmitter->scrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    (transmitter->encoder.finishing &&
	     !transmitter->finish_requested) ||
	    !mp_state_is_valid(transmitter) ||
	    !queue_is_valid(transmitter) ||
	    !symbol_accounting_is_valid(transmitter))
		return false;
	if (!transmitter->complete)
		return true;
	return transmitter->finish_requested &&
	       transmitter->encoder.finishing &&
	       transmitter->queue_count == 0 &&
	       (transmitter->encoder.mapping.sign_redundancy == 0 ||
		transmitter->encoder.shaper.pending_count == 0);
}

enum v90_downstream_tx_result v90_downstream_tx_init(
	struct v90_downstream_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping)
{
	struct v90_downstream_tx initialized = { 0 };

	if (transmitter == NULL || training_mapping == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING)
		return V90_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (v90_pcm_frame_encoder_init(
		    &initialized.encoder, training_mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (!v90_downstream_tx_is_valid(&initialized))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return V90_DOWNSTREAM_TX_OK;
}

bool v90_downstream_tx_is_complete(const struct v90_downstream_tx *transmitter)
{
	return v90_downstream_tx_is_valid(transmitter) && transmitter->complete;
}

static bool same_mp_record(
	const struct v90_downstream_tx *transmitter,
	const struct v90_mp_record *record)
{
	return v90_mp_record_parameters_equal(
		&transmitter->mp_record, record);
}

enum v90_downstream_tx_result v90_downstream_tx_start_mp(
	struct v90_downstream_tx *transmitter,
	const struct v90_mp_record *record)
{
	struct v90_downstream_tx next;
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t sequence_bits = 0;
	struct v90_mp_record canonical = { 0 };

	if (transmitter == NULL || record == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_downstream_tx_is_valid(transmitter) ||
	    transmitter->finish_requested)
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	if (!v90_mp_record_is_valid(record) ||
	    v90_mp_record_acknowledges_cp(record))
		return V90_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (transmitter->mp_configured)
		return same_mp_record(transmitter, record) ?
			       V90_DOWNSTREAM_TX_OK :
			       V90_DOWNSTREAM_TX_INVALID_STATE;
	if (!encode_mp_sequence(
		    record, transmitter->encoder.mapping.frame_bits, false,
		    sequence, &sequence_bits) ||
	    v90_mp_decode(
		    sequence, sequence_bits,
		    transmitter->encoder.mapping.frame_bits,
		    &canonical) != V90_MP_OK)
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	next = *transmitter;
	next.mp_record = canonical;
	memcpy(next.mp_sequence, sequence, sizeof(sequence));
	next.mp_sequence_bits = sequence_bits;
	next.mp_configured = true;
	if (!v90_downstream_tx_is_valid(&next))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_DOWNSTREAM_TX_OK;
}

enum v90_downstream_tx_result v90_downstream_tx_request_mp_prime(
	struct v90_downstream_tx *transmitter)
{
	struct v90_downstream_tx next;

	if (transmitter == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_downstream_tx_is_valid(transmitter) ||
	    !transmitter->mp_configured)
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->mp_prime_requested)
		return V90_DOWNSTREAM_TX_OK;
	next = *transmitter;
	next.mp_prime_requested = true;
	if (next.stage == V90_DOWNSTREAM_TX_MP &&
	    next.mp_bit_index == 0) {
		next.stage = V90_DOWNSTREAM_TX_MP_PRIME;
		if (!encode_mp_sequence(
			    &next.mp_record, next.encoder.mapping.frame_bits,
			    true, next.mp_sequence,
			    &next.mp_sequence_bits))
			return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	if (!v90_downstream_tx_is_valid(&next))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_DOWNSTREAM_TX_OK;
}

enum v90_downstream_tx_result v90_downstream_tx_request_ed(
	struct v90_downstream_tx *transmitter)
{
	struct v90_downstream_tx next;

	if (transmitter == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_downstream_tx_is_valid(transmitter) ||
	    !transmitter->mp_configured ||
	    !transmitter->mp_prime_requested)
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return V90_DOWNSTREAM_TX_COMPLETE;
	if (transmitter->ed_requested)
		return V90_DOWNSTREAM_TX_OK;
	next = *transmitter;
	next.ed_requested = true;
	if (next.stage == V90_DOWNSTREAM_TX_MP_PRIME &&
	    next.mp_prime_record_accepted && next.mp_bit_index == 0)
		next.stage = V90_DOWNSTREAM_TX_ED;
	if (!v90_downstream_tx_is_valid(&next))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_DOWNSTREAM_TX_OK;
}

enum v90_downstream_tx_result v90_downstream_tx_request_finish(
	struct v90_downstream_tx *transmitter)
{
	struct v90_downstream_tx next;

	if (transmitter == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_downstream_tx_is_valid(transmitter))
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return V90_DOWNSTREAM_TX_COMPLETE;
	if (transmitter->mp_configured)
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	next = *transmitter;
	next.finish_requested = true;
	if (!v90_downstream_tx_is_valid(&next))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_DOWNSTREAM_TX_OK;
}

static bool fill_queue(
	struct v90_downstream_tx *transmitter,
	const struct v90_pcm_frame_output *output)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;
	size_t rule_index = 0;
	enum v90_spectral_rule rule = V90_SPECTRAL_RULE_A;

	if (transmitter->queue_count != 0 || output->symbol_count == 0 ||
	    output->symbol_count > V90_CP_FRAME_INTERVALS)
		return false;
	memset(transmitter->queue, 0, sizeof(transmitter->queue));
	for (size_t index = 0; index < output->symbol_count; ++index) {
		struct v90_downstream_symbol queued = {
			.pcm = output->symbols[index],
		};

		if (!v90_pcm_symbol_is_valid(&queued.pcm, mapping->law))
			return false;

		if (mapping->sign_redundancy != 0) {
			bool starts =
				(transmitter->transmitted_symbols + index) %
					shaping_length == 0;

			if (starts) {
				if (rule_index >= output->rule_count)
					return false;
				rule = output->rules[rule_index++];
				if (rule > V90_SPECTRAL_RULE_D)
					return false;
			}
			queued.shaping_rule = rule;
			queued.shaping_rule_valid = true;
			queued.shaping_frame_start = starts;
		}
		transmitter->queue[index] = queued;
	}
	if (rule_index != output->rule_count)
		return false;
	transmitter->queue_count = output->symbol_count;
	return true;
}

static enum v90_downstream_tx_result generate_frame(
	struct v90_downstream_tx *transmitter)
{
	struct v90_pcm_frame_output output;
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];
	size_t bit_count = transmitter->encoder.mapping.frame_bits;
	enum v90_pcm_frame_encoder_result result;

	if (transmitter->accepted_frames >=
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return V90_DOWNSTREAM_TX_OUT_OF_RANGE;
	if (transmitter->stage == V90_DOWNSTREAM_TX_TRN2D &&
	    transmitter->mp_configured &&
	    transmitter->accepted_frames >=
		    V90_PHASE4_TRN2D_MINIMUM_FRAMES)
		transmitter->stage = V90_DOWNSTREAM_TX_MP;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		uint8_t source;

		if (transmitter->stage == V90_DOWNSTREAM_TX_TRN2D)
			source = 1;
		else if (transmitter->stage == V90_DOWNSTREAM_TX_ED)
			source = 0;
		else
			source = packed_msb_bit(
				transmitter->mp_sequence,
				transmitter->mp_bit_index + bit);

		if (v34_scramble_group(
			    &transmitter->scrambler, source, 1,
			    &bits[bit]) != V34_FRAMING_OK)
			return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	result = v90_pcm_frame_encoder_process(
		&transmitter->encoder, bits, bit_count, &output);
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return V90_DOWNSTREAM_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_BUFFERING &&
	    result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	transmitter->accepted_frames++;
	if (transmitter->stage == V90_DOWNSTREAM_TX_MP ||
	    transmitter->stage == V90_DOWNSTREAM_TX_MP_PRIME) {
		transmitter->mp_bit_index += bit_count;
		if (transmitter->mp_bit_index ==
		    transmitter->mp_sequence_bits) {
			transmitter->mp_bit_index = 0;
			if (transmitter->stage == V90_DOWNSTREAM_TX_MP &&
			    transmitter->mp_prime_requested) {
				transmitter->stage =
					V90_DOWNSTREAM_TX_MP_PRIME;
				if (!encode_mp_sequence(
					    &transmitter->mp_record,
					    transmitter->encoder.mapping.frame_bits,
					    true,
					    transmitter->mp_sequence,
					    &transmitter->mp_sequence_bits))
					return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
			} else if (transmitter->stage ==
				   V90_DOWNSTREAM_TX_MP_PRIME) {
				transmitter->mp_prime_record_accepted = true;
				if (transmitter->ed_requested)
					transmitter->stage =
						V90_DOWNSTREAM_TX_ED;
			}
		}
	} else if (transmitter->stage == V90_DOWNSTREAM_TX_ED) {
		transmitter->ed_frames++;
		if (transmitter->ed_frames == V90_PHASE4_ED_FRAMES)
			transmitter->finish_requested = true;
	}
	if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	    !fill_queue(transmitter, &output))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	return V90_DOWNSTREAM_TX_OK;
}

static enum v90_downstream_tx_result drain_encoder(
	struct v90_downstream_tx *transmitter)
{
	struct v90_pcm_frame_output output;
	enum v90_pcm_frame_encoder_result result =
		v90_pcm_frame_encoder_finish(
			&transmitter->encoder, &output);

	if (result == V90_PCM_FRAME_ENCODER_COMPLETE) {
		transmitter->complete = true;
		return V90_DOWNSTREAM_TX_COMPLETE;
	}
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return V90_DOWNSTREAM_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
	    !fill_queue(transmitter, &output))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	return V90_DOWNSTREAM_TX_OK;
}

static struct v90_downstream_symbol pop_symbol(
	struct v90_downstream_tx *transmitter)
{
	struct v90_downstream_symbol symbol = transmitter->queue[0];

	transmitter->queue_count--;
	memmove(&transmitter->queue[0], &transmitter->queue[1],
		transmitter->queue_count * sizeof(transmitter->queue[0]));
	memset(&transmitter->queue[transmitter->queue_count], 0,
	       sizeof(transmitter->queue[0]));
	transmitter->transmitted_symbols++;
	return symbol;
}

enum v90_downstream_tx_result v90_downstream_tx_next(
	struct v90_downstream_tx *transmitter,
	struct v90_downstream_symbol *symbol)
{
	struct v90_downstream_tx next;
	struct v90_downstream_symbol generated;
	enum v90_downstream_tx_result result = V90_DOWNSTREAM_TX_OK;

	if (transmitter == NULL || symbol == NULL)
		return V90_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_downstream_tx_is_valid(transmitter))
		return V90_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return V90_DOWNSTREAM_TX_COMPLETE;
	next = *transmitter;
	if (next.queue_count == 0 && next.finish_requested) {
		result = drain_encoder(&next);
		if (result == V90_DOWNSTREAM_TX_COMPLETE) {
			if (!v90_downstream_tx_is_valid(&next))
				return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
			*transmitter = next;
			return V90_DOWNSTREAM_TX_COMPLETE;
		}
	} else if (next.queue_count == 0) {
		for (size_t attempt = 0;
		     attempt <= next.encoder.mapping.spectral_lookahead &&
		     next.queue_count == 0 && !next.finish_requested;
		     ++attempt) {
			result = generate_frame(&next);
			if (result != V90_DOWNSTREAM_TX_OK)
				break;
		}
		if (result == V90_DOWNSTREAM_TX_OK &&
		    next.queue_count == 0 && next.finish_requested)
			result = drain_encoder(&next);
		else if (result == V90_DOWNSTREAM_TX_OK &&
			 next.queue_count == 0)
			result = V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	if (result == V90_DOWNSTREAM_TX_COMPLETE) {
		if (!v90_downstream_tx_is_valid(&next))
			return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
		*transmitter = next;
		return V90_DOWNSTREAM_TX_COMPLETE;
	}
	if (result != V90_DOWNSTREAM_TX_OK)
		return result;
	generated = pop_symbol(&next);
	if (!v90_downstream_tx_is_valid(&next))
		return V90_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return V90_DOWNSTREAM_TX_OK;
}

const char *v90_downstream_tx_result_name(
	enum v90_downstream_tx_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_TX_OK:
		return "ok";
	case V90_DOWNSTREAM_TX_COMPLETE:
		return "complete";
	case V90_DOWNSTREAM_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DOWNSTREAM_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DOWNSTREAM_TX_INVALID_STATE:
		return "invalid state";
	case V90_DOWNSTREAM_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_DOWNSTREAM_TX_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_DOWNSTREAM_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
