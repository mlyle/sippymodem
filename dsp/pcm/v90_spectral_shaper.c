// SPDX-License-Identifier: BSD-3-Clause

#include "v90_spectral_shaper.h"

#include <float.h>
#include <math.h>
#include <string.h>

struct spectral_filter_state {
	double x;
	double y;
	double v;
};

static size_t shaping_frame_length(uint8_t sign_redundancy)
{
	return sign_redundancy >= 1 && sign_redundancy <= 3 ?
		       V90_CP_FRAME_INTERVALS / sign_redundancy :
		       0;
}

static bool frame_is_canonical(
	const struct v90_spectral_frame *frame, size_t active_symbols)
{
	for (size_t symbol = 0;
	     symbol < V90_SPECTRAL_MAX_FRAME_SYMBOLS; ++symbol) {
		if (symbol < active_symbols) {
			if (frame->ucodes[symbol] > 127 ||
			    frame->initial_signs[symbol] > 1)
				return false;
		} else if (frame->ucodes[symbol] != 0 ||
			   frame->initial_signs[symbol] != 0) {
			return false;
		}
	}
	return true;
}

bool v90_spectral_shaper_is_valid(
	const struct v90_spectral_shaper *shaper)
{
	size_t frame_length;

	if (shaper == NULL || !g711_law_is_valid(shaper->law) ||
	    shaper->sign_redundancy < 1 ||
	    shaper->sign_redundancy > 3 ||
	    shaper->lookahead > V90_SPECTRAL_MAX_LOOKAHEAD ||
	    shaper->pending_count > shaper->lookahead ||
	    !pcm_shaping_coefficient_is_valid(shaper->shaping_a1_q1_6) ||
	    !pcm_shaping_coefficient_is_valid(shaper->shaping_a2_q1_6) ||
	    !pcm_shaping_coefficient_is_valid(shaper->shaping_b1_q1_6) ||
	    !pcm_shaping_coefficient_is_valid(shaper->shaping_b2_q1_6) ||
	    !isfinite(shaper->previous_x) ||
	    !isfinite(shaper->previous_y) ||
	    !isfinite(shaper->previous_v))
		return false;
	frame_length = shaping_frame_length(shaper->sign_redundancy);
	for (size_t frame = 0;
	     frame < V90_SPECTRAL_MAX_BUFFERED_FRAMES; ++frame) {
		if (!frame_is_canonical(
			    &shaper->pending[frame],
			    frame < shaper->pending_count ? frame_length : 0))
			return false;
	}
	return true;
}

enum v90_spectral_shaper_result v90_spectral_shaper_init(
	struct v90_spectral_shaper *shaper,
	const struct v90_pcm_mapping *mapping)
{
	struct v90_spectral_shaper initialized = { 0 };

	if (shaper == NULL || mapping == NULL)
		return V90_SPECTRAL_SHAPER_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping) ||
	    mapping->sign_redundancy == 0)
		return V90_SPECTRAL_SHAPER_INVALID_PARAMETERS;
	initialized.law = mapping->law;
	initialized.shaping_a1_q1_6 = mapping->shaping_a1_q1_6;
	initialized.shaping_a2_q1_6 = mapping->shaping_a2_q1_6;
	initialized.shaping_b1_q1_6 = mapping->shaping_b1_q1_6;
	initialized.shaping_b2_q1_6 = mapping->shaping_b2_q1_6;
	initialized.sign_redundancy = mapping->sign_redundancy;
	initialized.lookahead = mapping->spectral_lookahead;
	if (!v90_spectral_shaper_is_valid(&initialized))
		return V90_SPECTRAL_SHAPER_INVALID_PARAMETERS;
	*shaper = initialized;
	return V90_SPECTRAL_SHAPER_OK;
}

static bool filter_frame(
	const struct v90_spectral_shaper *shaper,
	const struct v90_spectral_frame *frame, const int16_t *magnitudes,
	enum v90_spectral_rule rule,
	struct spectral_filter_state *state, double *metric)
{
	double a1 = shaper->shaping_a1_q1_6 / 64.0;
	double a2 = shaper->shaping_a2_q1_6 / 64.0;
	double b1 = shaper->shaping_b1_q1_6 / 64.0;
	double b2 = shaper->shaping_b2_q1_6 / 64.0;
	size_t frame_length = shaping_frame_length(
		shaper->sign_redundancy);
	double accumulated = 0.0;

	for (size_t position = 0; position < frame_length; ++position) {
		bool positive = v90_spectral_rule_apply(
			frame->initial_signs[position], position, rule) != 0;
		double x;
		double y;
		double v;

		/* Branches change signs, never the linear PCM magnitudes. Keep
		 * negative-zero wire characters out of the filter calculation. */
		x = positive ? magnitudes[position] : -(double)magnitudes[position];
		y = x - b1 * state->x + a1 * state->y;
		v = y - b2 * state->y + a2 * state->v;
		accumulated += v * v;
		if (!isfinite(y) || !isfinite(v) ||
		    !isfinite(accumulated))
			return false;
		state->x = x;
		state->y = y;
		state->v = v;
	}
	*metric += accumulated;
	return isfinite(*metric);
}

static bool select_rule(
	const struct v90_spectral_shaper *shaper, size_t horizon,
	const int16_t magnitudes[V90_SPECTRAL_MAX_BUFFERED_FRAMES][V90_SPECTRAL_MAX_FRAME_SYMBOLS],
	enum v90_spectral_rule *selected)
{
	unsigned int sequence_count = 1u << horizon;
	double best_metric = DBL_MAX;
	unsigned int best_sequence = 0;

	/* Without lookahead the two paths share no filtering work. */
	if (horizon == 1u) {
		enum v90_spectral_rule first = shaper->trellis_state ?
			V90_SPECTRAL_RULE_C : V90_SPECTRAL_RULE_A;

		for (unsigned int branch = 0; branch < 2u; ++branch) {
			struct spectral_filter_state filter = {
				shaper->previous_x, shaper->previous_y, shaper->previous_v,
			};
			double metric = 0.0;

			if (!filter_frame(shaper, &shaper->pending[0], magnitudes[0],
				    (enum v90_spectral_rule)(first + branch), &filter, &metric))
				return false;
			if (metric < best_metric) {
				best_metric = metric;
				best_sequence = branch;
			}
		}
		*selected = (enum v90_spectral_rule)(first + best_sequence);
		return true;
	}
	struct spectral_filter_state prefix_filter[V90_SPECTRAL_MAX_BUFFERED_FRAMES];
	double prefix_metric[V90_SPECTRAL_MAX_BUFFERED_FRAMES];

	prefix_filter[0] = (struct spectral_filter_state) {
		.x = shaper->previous_x,
		.y = shaper->previous_y,
		.v = shaper->previous_v,
	};
	prefix_metric[0] = 0.0;

	for (unsigned int sequence = 0;
	     sequence < sequence_count; ++sequence) {
		size_t first_frame = 0;

		/* Consecutive binary paths share the prefix before the carry.
		 * Retain its filter and metric, evaluating only the changed suffix.
		 * Visit leaves in the same order to preserve exact metric ties. */
		if (sequence != 0) {
			first_frame = horizon - 1u;
			for (unsigned int carry = sequence;
			     (carry & 1u) == 0; carry >>= 1)
				first_frame--;
		}
		struct spectral_filter_state filter = prefix_filter[first_frame];
		double metric = prefix_metric[first_frame];

		for (size_t frame = first_frame; frame < horizon; ++frame) {
			unsigned int branch =
				(sequence >> (horizon - frame - 1u)) & 1u;
			bool state = frame == 0 ? shaper->trellis_state :
				((sequence >> (horizon - frame)) & 1u) != 0;
			enum v90_spectral_rule rule =
				(enum v90_spectral_rule)(
					(state ? V90_SPECTRAL_RULE_C :
						 V90_SPECTRAL_RULE_A) + branch);

			if (frame != first_frame) {
				prefix_filter[frame] = filter;
				prefix_metric[frame] = metric;
			}
			if (!filter_frame(
				    shaper, &shaper->pending[frame], magnitudes[frame],
				    rule, &filter, &metric))
				return false;
		}
		if (metric < best_metric) {
			best_metric = metric;
			best_sequence = sequence;
		}
	}
	*selected = (enum v90_spectral_rule)(
		(shaper->trellis_state ? V90_SPECTRAL_RULE_C :
					 V90_SPECTRAL_RULE_A) +
		(best_sequence >> (horizon - 1u)));
	return true;
}

static bool commit_first_frame(
	struct v90_spectral_shaper *shaper, const int16_t *magnitudes,
	enum v90_spectral_rule rule,
	struct v90_spectral_output *output)
{
	struct spectral_filter_state filter = {
		.x = shaper->previous_x,
		.y = shaper->previous_y,
		.v = shaper->previous_v,
	};
	struct v90_spectral_output generated = {
		.rule = rule,
		.symbol_count = (uint8_t)shaping_frame_length(
			shaper->sign_redundancy),
	};
	double ignored_metric = 0.0;

	if (!filter_frame(
		    shaper, &shaper->pending[0], magnitudes, rule,
		    &filter, &ignored_metric))
		return false;
	for (size_t position = 0;
	     position < generated.symbol_count; ++position) {
		if (!v90_pcm_symbol_make(
			    &generated.symbols[position], shaper->law,
			    shaper->pending[0].ucodes[position],
			    v90_spectral_rule_apply(
				    shaper->pending[0].initial_signs[position],
				    position, rule) != 0))
			return false;
	}
	/* Nothing above changes the shaper on failure. */
	shaper->previous_x = filter.x;
	shaper->previous_y = filter.y;
	shaper->previous_v = filter.v;
	shaper->trellis_state = (rule & 1u) != 0;
	shaper->pending_count--;
	memmove(&shaper->pending[0], &shaper->pending[1],
		shaper->pending_count * sizeof(shaper->pending[0]));
	memset(&shaper->pending[shaper->pending_count], 0,
	       sizeof(shaper->pending[0]));
	*output = generated;
	return true;
}

static enum v90_spectral_shaper_result emit_first(
	struct v90_spectral_shaper *shaper, size_t horizon,
	struct v90_spectral_output *output)
{
	enum v90_spectral_rule selected;
	int16_t magnitudes[V90_SPECTRAL_MAX_BUFFERED_FRAMES][V90_SPECTRAL_MAX_FRAME_SYMBOLS] = { 0 };
	size_t length = shaping_frame_length(shaper->sign_redundancy);

	/* The same codewords occur on every candidate path. Decode each once
	 * for this bounded horizon, including the selected frame's commit. */
	for (size_t frame = 0; frame < horizon; ++frame) {
		for (size_t symbol = 0; symbol < length; ++symbol) {
			struct v90_pcm_symbol magnitude;
			if (!v90_pcm_symbol_make(&magnitude, shaper->law,
				    shaper->pending[frame].ucodes[symbol], true))
				return V90_SPECTRAL_SHAPER_NUMERIC_FAILURE;
			magnitudes[frame][symbol] = magnitude.sample;
		}
	}
	if (!select_rule(shaper, horizon, magnitudes, &selected) ||
	    !commit_first_frame(shaper, magnitudes[0], selected, output))
		return V90_SPECTRAL_SHAPER_NUMERIC_FAILURE;
	return V90_SPECTRAL_SHAPER_FRAME_READY;
}

enum v90_spectral_shaper_result v90_spectral_shaper_push(
	struct v90_spectral_shaper *shaper,
	const uint8_t *ucodes, const uint8_t *initial_signs,
	size_t symbol_count, struct v90_spectral_output *output)
{
	struct v90_spectral_output generated;
	struct v90_spectral_frame frame = { 0 };
	size_t required;
	enum v90_spectral_shaper_result result;

	if (shaper == NULL || ucodes == NULL || initial_signs == NULL ||
	    output == NULL)
		return V90_SPECTRAL_SHAPER_INVALID_ARGUMENT;
	if (!v90_spectral_shaper_is_valid(shaper) || shaper->finishing)
		return V90_SPECTRAL_SHAPER_INVALID_STATE;
	required = shaping_frame_length(shaper->sign_redundancy);
	if (symbol_count != required)
		return V90_SPECTRAL_SHAPER_OUT_OF_RANGE;
	memcpy(frame.ucodes, ucodes, required);
	memcpy(frame.initial_signs, initial_signs, required);
	if (!frame_is_canonical(&frame, required))
		return V90_SPECTRAL_SHAPER_OUT_OF_RANGE;
	shaper->pending[shaper->pending_count++] = frame;
	if (shaper->pending_count <= shaper->lookahead)
		return V90_SPECTRAL_SHAPER_BUFFERING;
	result = emit_first(shaper, shaper->pending_count, &generated);
	if (result != V90_SPECTRAL_SHAPER_FRAME_READY) {
		shaper->pending_count--;
		memset(&shaper->pending[shaper->pending_count], 0,
		       sizeof(shaper->pending[0]));
		return result;
	}
	*output = generated;
	return V90_SPECTRAL_SHAPER_FRAME_READY;
}

enum v90_spectral_shaper_result v90_spectral_shaper_finish(
	struct v90_spectral_shaper *shaper,
	struct v90_spectral_output *output)
{
	struct v90_spectral_output generated;
	enum v90_spectral_shaper_result result;

	if (shaper == NULL || output == NULL)
		return V90_SPECTRAL_SHAPER_INVALID_ARGUMENT;
	if (!v90_spectral_shaper_is_valid(shaper))
		return V90_SPECTRAL_SHAPER_INVALID_STATE;
	if (shaper->pending_count == 0) {
		shaper->finishing = true;
		return V90_SPECTRAL_SHAPER_COMPLETE;
	}
	result = emit_first(shaper, shaper->pending_count, &generated);
	if (result != V90_SPECTRAL_SHAPER_FRAME_READY)
		return result;
	shaper->finishing = true;
	*output = generated;
	return V90_SPECTRAL_SHAPER_FRAME_READY;
}

const char *v90_spectral_shaper_result_name(
	enum v90_spectral_shaper_result result)
{
	switch (result) {
	case V90_SPECTRAL_SHAPER_OK:
		return "ok";
	case V90_SPECTRAL_SHAPER_FRAME_READY:
		return "frame ready";
	case V90_SPECTRAL_SHAPER_BUFFERING:
		return "buffering";
	case V90_SPECTRAL_SHAPER_COMPLETE:
		return "complete";
	case V90_SPECTRAL_SHAPER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_SPECTRAL_SHAPER_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_SPECTRAL_SHAPER_INVALID_STATE:
		return "invalid state";
	case V90_SPECTRAL_SHAPER_OUT_OF_RANGE:
		return "out of range";
	case V90_SPECTRAL_SHAPER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
