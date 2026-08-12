// SPDX-License-Identifier: BSD-3-Clause

#include "v90_trn2d_tx.h"

#include <string.h>

static bool pcm_symbol_is_valid(
	const struct v90_pcm_symbol *symbol, enum g711_law law)
{
	struct v90_pcm_symbol parsed;

	return v90_pcm_symbol_parse(&parsed, law, symbol->octet) &&
	       parsed.ucode == symbol->ucode &&
	       parsed.positive == symbol->positive &&
	       parsed.sample == symbol->sample;
}

static bool queued_symbol_is_zero(
	const struct v90_trn2d_symbol *symbol)
{
	return symbol->pcm.ucode == 0 && !symbol->pcm.positive &&
	       symbol->pcm.octet == 0 && symbol->pcm.sample == 0 &&
	       symbol->shaping_rule == V90_SPECTRAL_RULE_A &&
	       !symbol->shaping_rule_valid &&
	       !symbol->shaping_frame_start;
}

static bool queue_is_valid(const struct v90_trn2d_tx *transmitter)
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
		const struct v90_trn2d_symbol *symbol =
			&transmitter->queue[index];

		if (index >= transmitter->queue_count) {
			if (!queued_symbol_is_zero(symbol))
				return false;
			continue;
		}
		if (!pcm_symbol_is_valid(&symbol->pcm, mapping->law))
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
	const struct v90_trn2d_tx *transmitter)
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

bool v90_trn2d_tx_is_valid(const struct v90_trn2d_tx *transmitter)
{
	if (transmitter == NULL ||
	    !v90_pcm_frame_encoder_is_valid(&transmitter->encoder) ||
	    transmitter->encoder.mapping.kind != V90_CP_TRAINING ||
	    !v34_scrambler_is_valid(&transmitter->scrambler) ||
	    transmitter->scrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    (transmitter->encoder.finishing &&
	     !transmitter->finish_requested) ||
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

enum v90_trn2d_tx_result v90_trn2d_tx_init(
	struct v90_trn2d_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping)
{
	struct v90_trn2d_tx initialized = { 0 };

	if (transmitter == NULL || training_mapping == NULL)
		return V90_TRN2D_TX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING)
		return V90_TRN2D_TX_INVALID_PARAMETERS;
	if (v90_pcm_frame_encoder_init(
		    &initialized.encoder, training_mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return V90_TRN2D_TX_INVALID_PARAMETERS;
	if (!v90_trn2d_tx_is_valid(&initialized))
		return V90_TRN2D_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return V90_TRN2D_TX_OK;
}

bool v90_trn2d_tx_is_complete(const struct v90_trn2d_tx *transmitter)
{
	return v90_trn2d_tx_is_valid(transmitter) && transmitter->complete;
}

enum v90_trn2d_tx_result v90_trn2d_tx_request_finish(
	struct v90_trn2d_tx *transmitter)
{
	struct v90_trn2d_tx next;

	if (transmitter == NULL)
		return V90_TRN2D_TX_INVALID_ARGUMENT;
	if (!v90_trn2d_tx_is_valid(transmitter))
		return V90_TRN2D_TX_INVALID_STATE;
	if (transmitter->complete)
		return V90_TRN2D_TX_COMPLETE;
	next = *transmitter;
	next.finish_requested = true;
	if (!v90_trn2d_tx_is_valid(&next))
		return V90_TRN2D_TX_INTERNAL_ERROR;
	*transmitter = next;
	return V90_TRN2D_TX_OK;
}

static bool fill_queue(
	struct v90_trn2d_tx *transmitter,
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
		struct v90_trn2d_symbol queued = {
			.pcm = output->symbols[index],
		};

		if (!pcm_symbol_is_valid(&queued.pcm, mapping->law))
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

static enum v90_trn2d_tx_result generate_frame(
	struct v90_trn2d_tx *transmitter)
{
	struct v90_pcm_frame_output output;
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];
	size_t bit_count = transmitter->encoder.mapping.frame_bits;
	enum v90_pcm_frame_encoder_result result;

	if (transmitter->accepted_frames >=
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return V90_TRN2D_TX_OUT_OF_RANGE;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		if (v34_scramble_group(
			    &transmitter->scrambler, 1, 1,
			    &bits[bit]) != V34_FRAMING_OK)
			return V90_TRN2D_TX_INTERNAL_ERROR;
	}
	result = v90_pcm_frame_encoder_process(
		&transmitter->encoder, bits, bit_count, &output);
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return V90_TRN2D_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_BUFFERING &&
	    result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		return V90_TRN2D_TX_INTERNAL_ERROR;
	transmitter->accepted_frames++;
	if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	    !fill_queue(transmitter, &output))
		return V90_TRN2D_TX_INTERNAL_ERROR;
	return V90_TRN2D_TX_OK;
}

static enum v90_trn2d_tx_result drain_encoder(
	struct v90_trn2d_tx *transmitter)
{
	struct v90_pcm_frame_output output;
	enum v90_pcm_frame_encoder_result result =
		v90_pcm_frame_encoder_finish(
			&transmitter->encoder, &output);

	if (result == V90_PCM_FRAME_ENCODER_COMPLETE) {
		transmitter->complete = true;
		return V90_TRN2D_TX_COMPLETE;
	}
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return V90_TRN2D_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
	    !fill_queue(transmitter, &output))
		return V90_TRN2D_TX_INTERNAL_ERROR;
	return V90_TRN2D_TX_OK;
}

static struct v90_trn2d_symbol pop_symbol(
	struct v90_trn2d_tx *transmitter)
{
	struct v90_trn2d_symbol symbol = transmitter->queue[0];

	transmitter->queue_count--;
	memmove(&transmitter->queue[0], &transmitter->queue[1],
		transmitter->queue_count * sizeof(transmitter->queue[0]));
	memset(&transmitter->queue[transmitter->queue_count], 0,
	       sizeof(transmitter->queue[0]));
	transmitter->transmitted_symbols++;
	return symbol;
}

enum v90_trn2d_tx_result v90_trn2d_tx_next(
	struct v90_trn2d_tx *transmitter,
	struct v90_trn2d_symbol *symbol)
{
	struct v90_trn2d_tx next;
	struct v90_trn2d_symbol generated;
	enum v90_trn2d_tx_result result = V90_TRN2D_TX_OK;

	if (transmitter == NULL || symbol == NULL)
		return V90_TRN2D_TX_INVALID_ARGUMENT;
	if (!v90_trn2d_tx_is_valid(transmitter))
		return V90_TRN2D_TX_INVALID_STATE;
	if (transmitter->complete)
		return V90_TRN2D_TX_COMPLETE;
	next = *transmitter;
	if (next.queue_count == 0 && next.finish_requested) {
		result = drain_encoder(&next);
		if (result == V90_TRN2D_TX_COMPLETE) {
			if (!v90_trn2d_tx_is_valid(&next))
				return V90_TRN2D_TX_INTERNAL_ERROR;
			*transmitter = next;
			return V90_TRN2D_TX_COMPLETE;
		}
	} else if (next.queue_count == 0) {
		for (size_t attempt = 0;
		     attempt <= next.encoder.mapping.spectral_lookahead &&
		     next.queue_count == 0; ++attempt) {
			result = generate_frame(&next);
			if (result != V90_TRN2D_TX_OK)
				break;
		}
		if (result == V90_TRN2D_TX_OK && next.queue_count == 0)
			result = V90_TRN2D_TX_INTERNAL_ERROR;
	}
	if (result != V90_TRN2D_TX_OK)
		return result;
	generated = pop_symbol(&next);
	if (!v90_trn2d_tx_is_valid(&next))
		return V90_TRN2D_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return V90_TRN2D_TX_OK;
}

const char *v90_trn2d_tx_result_name(
	enum v90_trn2d_tx_result result)
{
	switch (result) {
	case V90_TRN2D_TX_OK:
		return "ok";
	case V90_TRN2D_TX_COMPLETE:
		return "complete";
	case V90_TRN2D_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_TRN2D_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_TRN2D_TX_INVALID_STATE:
		return "invalid state";
	case V90_TRN2D_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_TRN2D_TX_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_TRN2D_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
