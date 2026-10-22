// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_frame.h"

#include "v92_observation.h"

#include "fir.h"
#include "integer_math.h"
#include "memory_region.h"
#include "packed_bits.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
	V92_PCM_TRELLIS_INTERVAL_SYMBOLS = 4,
	V92_PCM_GAIN_SHIFT = 18,
};

static bool trellis_selection(
	enum v92_cpd_trellis selected,
	enum v34_trellis_selection *trellis)
{
	if (trellis == NULL)
		return false;
	switch (selected) {
	case V92_CPD_TRELLIS_16_STATE:
		*trellis = V34_TRELLIS_16_STATE;
		return true;
	case V92_CPD_TRELLIS_32_STATE:
		*trellis = V34_TRELLIS_32_STATE;
		return true;
	case V92_CPD_TRELLIS_64_STATE:
		*trellis = V34_TRELLIS_64_STATE;
		return true;
	}
	return false;
}

static bool state_is_valid(
	const struct v92_pcm_frame_tx_state *state,
	enum v92_cpd_trellis selected)
{
	enum v34_trellis_selection expected;

	return state != NULL && state->differential <= 1u &&
	       v34_scrambler_is_valid(&state->scrambler) &&
	       state->scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       trellis_selection(selected, &expected) &&
	       v34_convolutional_encoder_is_valid(&state->trellis) &&
	       state->trellis.selection == expected;
}

static bool state_init(
	struct v92_pcm_frame_tx_state *state,
	enum v92_cpd_trellis selected)
{
	enum v34_trellis_selection trellis;

	if (state == NULL || !trellis_selection(selected, &trellis))
		return false;
	memset(state, 0, sizeof(*state));
	return v34_scrambler_init(
		       &state->scrambler, V34_POLYNOMIAL_GPA) ==
		       V34_FRAMING_OK &&
	       v34_convolutional_encoder_init(
		       &state->trellis, trellis) == V34_TRELLIS_OK;
}

bool v92_pcm_frame_tx_is_valid(
	const struct v92_pcm_frame_tx *transmitter)
{
	return transmitter != NULL &&
	       v92_pcm_mapping_is_valid(&transmitter->mapping) &&
	       state_is_valid(
		       &transmitter->state,
		       transmitter->mapping.cpd.trellis);
}

enum v92_pcm_frame_result v92_pcm_frame_tx_init(
	struct v92_pcm_frame_tx *transmitter,
	const struct v92_pcm_mapping *mapping)
{
	struct v92_pcm_frame_tx initialized;

	if (transmitter == NULL || mapping == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	memset(&initialized, 0, sizeof(initialized));
	initialized.mapping = *mapping;
	if (!state_init(&initialized.state, mapping->cpd.trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	*transmitter = initialized;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_tx_reset_b1u(
	struct v92_pcm_frame_tx *transmitter)
{
	struct v92_pcm_frame_tx_state reset;

	if (transmitter == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(&transmitter->mapping) ||
	    !state_init(&reset, transmitter->mapping.cpd.trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	transmitter->state = reset;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_tx_reset_control(
	struct v92_pcm_frame_tx *transmitter)
{
	if (transmitter == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_tx_is_valid(transmitter))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	transmitter->state.scrambler.history = 0u;
	transmitter->state.differential = 0u;
	return V92_PCM_FRAME_OK;
}

static int positive_modulo(int value, unsigned int modulus)
{
	int reduced = value % (int)modulus;

	return reduced < 0 ? reduced + (int)modulus : reduced;
}

static size_t x_history_length(size_t feedback, size_t feed_forward)
{
	if (feed_forward)
		--feed_forward;
	return feedback > feed_forward ? feedback : feed_forward;
}

static size_t x_history_count(const struct v92_pcm_mapping *mapping)
{
	return x_history_length(
		mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK].count,
		mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count);
}

static const int32_t *u_history(
	const struct v92_pcm_frame_filter_state *filter)
{
	return filter->history;
}

static const int32_t *x_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	return filter->history +
	       mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD].count;
}

static const int32_t *v_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	return x_history(mapping, filter) + x_history_count(mapping);
}

struct filter_history_span {
	size_t u, x, v;
};

static struct filter_history_span causal_history(
	const struct v92_pcm_frame_rx *receiver)
{
	const struct v92_cpd_filter *filter = receiver->mapping.cpd.filter;
	struct filter_history_span span = {
		.u = v92_cpd_filter_response_length(filter + V92_CPD_PRECODER_FEED_FORWARD),
		.x = x_history_length(
			v92_cpd_filter_response_length(filter + V92_CPD_PRECODER_FEEDBACK),
			v92_cpd_filter_response_length(filter + V92_CPD_PREFILTER_FEED_FORWARD)),
		.v = v92_cpd_filter_response_length(filter + V92_CPD_PREFILTER_FEEDBACK),
	};

	if (receiver->observation == V92_PCM_FRAME_CONSTELLATION) {
		/* A later frame may select any supported channel model. Preserve
		 * its x history even when the wire filter has zero tail taps. */
		size_t reserved = x_history_count(&receiver->mapping);

		if (reserved > V92_OBSERVATION_TAPS - 1u)
			reserved = V92_OBSERVATION_TAPS - 1u;
		if (span.x < reserved)
			span.x = reserved;
	}
	return span;
}

static bool same_filter_history(
	const struct v92_pcm_mapping *mapping,
	const struct filter_history_span *span,
	const struct v92_pcm_frame_filter_state *left,
	const struct v92_pcm_frame_filter_state *right)
{
	/* Storage offsets follow declared lengths, including their zero tails.
	 * Only the causal prefixes determine future filter/model predictions. */
	return (!span->u || memcmp(u_history(left), u_history(right),
			span->u * sizeof(int32_t)) == 0) &&
	       (!span->x || memcmp(x_history(mapping, left), x_history(mapping, right),
			span->x * sizeof(int32_t)) == 0) &&
	       (!span->v || memcmp(v_history(mapping, left), v_history(mapping, right),
			span->v * sizeof(int32_t)) == 0);
}

/* A validated path fixes the fourth-symbol coset before point enumeration.
 * Other symbols use every index; the fourth uses eta = offset + 2*k. */
static int equivalence_offset(size_t symbol,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t y0)
{
	if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS != 3u)
		return 0;
	size_t base = symbol - 3u;

	return positive_modulo(selected[base] + selected[base + 1u] +
		selected[base + 2u] + y0, 2u);
}

/* CPd permits at most 384 coefficients in total. Even full-width samples
 * and doubled Q14 products have absolute sum <= 384 * 2^47 < 2^56, so
 * regrouping each section before the feedback doubling remains exact. */
static int64_t precoder_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK];
	const int32_t *u_values = u_history(filter);
	const int32_t *x_values = x_history(mapping, filter);

	return fir_read_s32_s16(u_values, feed_forward->coefficient,
				feed_forward->count) +
	       2 * fir_read_s32_s16(x_values, feedback->coefficient,
				    feedback->count);
}

static int64_t prefilter_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];
	const int32_t *x_values = x_history(mapping, filter);
	const int32_t *v_values = v_history(mapping, filter);

	return fir_read_s32_s16(x_values, feed_forward->coefficient + 1,
				feed_forward->count ? feed_forward->count - 1u : 0u) +
	       2 * fir_read_s32_s16(v_values, feedback->coefficient,
				    feedback->count);
}

/* A path's past inputs are fixed across candidate points. Retain the full
 * accumulator: rounding the history first would change half-way cases when
 * the current input changes its sign, and can lose cancellation at clipping. */
static int32_t precoder_output(int64_t history, int32_t u)
{
	return round_shift_s64_to_s32_saturating(
		history + (int64_t)u * INT64_C(32768), 15u);
}

static int32_t prefilter_output(
	const struct v92_pcm_mapping *mapping, int64_t history, int32_t x)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];

	if (feed_forward->count == 0u && feedback->count == 0u)
		return x;
	int64_t accumulator = history;
	if (feed_forward->count != 0u)
		accumulator += (int64_t)x * feed_forward->coefficient[0];
	return round_shift_s64_to_s32_saturating(accumulator, 15u);
}

static void push_history(int32_t *history, size_t count, int32_t value)
{
	if (count == 0u)
		return;
	if (count > 1u)
		memmove(
			history + 1u, history,
			(count - 1u) * sizeof(history[0]));
	history[0] = value;
}

static void filter_step(
	const struct v92_pcm_mapping *mapping,
	struct v92_pcm_frame_filter_state *filter, int32_t u,
	int32_t *x, int32_t *v)
{
	const struct v92_cpd_filter *z1 =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD];
	const struct v92_cpd_filter *p2 =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];
	size_t x_count = x_history_count(mapping);
	int32_t *u_values = filter->history;
	int32_t *x_values = u_values + z1->count;
	int32_t *v_values = x_values + x_count;

	*x = precoder_output(precoder_history(mapping, filter), u);
	*v = prefilter_output(mapping, prefilter_history(mapping, filter), *x);
	push_history(u_values, z1->count, u);
	push_history(x_values, x_count, *x);
	push_history(v_values, p2->count, *v);
}

static bool select_point(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter,
	size_t symbol, uint8_t expected_digit,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t y0,
	int16_t *eta_out, int16_t *u_out)
{
	const struct v92_cpd_constellation *constellation;
	int64_t best_score = 0;
	int16_t best_eta = 0;
	int16_t best_u = 0;
	bool found = false;
	int64_t history = precoder_history(mapping, filter);
	uint8_t constellation_index =
		mapping->cpd.frame_constellation[
			symbol % V92_PCM_MAPPING_PHASES];

	constellation = &mapping->cpd.constellation[constellation_index];
	unsigned stride = symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u ? 2u : 1u;
	unsigned step = stride * mapping->cpd.modulus[symbol];
	int first = -(int)constellation->count;
	int offset = equivalence_offset(symbol, selected, y0) + (int)(stride * expected_digit);

	first += positive_modulo(offset - first, step);
	for (int eta = first; eta < (int)constellation->count; eta += (int)step) {
		uint8_t ordinal;
		int16_t positive;
		int16_t u;
		int32_t x;
		int64_t score;

		ordinal = (uint8_t)(eta >= 0 ? eta : -eta - 1);
		positive = constellation->positive_linear[ordinal];
		u = eta >= 0 ? positive : (int16_t)-positive;
		x = precoder_output(history, u);
		score = x < 0 ? -(int64_t)x : (int64_t)x;
		if (!found || score < best_score) {
			found = true;
			best_score = score;
			best_eta = (int16_t)eta;
			best_u = u;
		}
	}
	if (!found)
		return false;
	*eta_out = best_eta;
	*u_out = best_u;
	return true;
}

static bool advance_trellis(
	struct v34_convolutional_encoder *trellis,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], size_t last)
{
	struct v34_grid_point first = {
		.in_phase = (int16_t)(2 * selected[last - 3u] + 1),
		.quadrature = (int16_t)(2 * selected[last - 2u] + 1),
	};
	struct v34_grid_point second = {
		.in_phase = (int16_t)(2 * selected[last - 1u] + 1),
		.quadrature = (int16_t)(2 * selected[last] + 1),
	};
	struct v34_convolutional_input input;
	uint8_t first_subset;
	uint8_t second_subset;

	return v34_grid_subset_label(&first, &first_subset) ==
		       V34_TRELLIS_OK &&
	       v34_grid_subset_label(&second, &second_subset) ==
		       V34_TRELLIS_OK &&
	       v34_convolutional_input_from_subset_labels(
		       first_subset, second_subset, &input) ==
		       V34_TRELLIS_OK &&
	       v34_convolutional_encoder_advance(trellis, &input) ==
		       V34_TRELLIS_OK;
}

static int32_t scale_gain_product_unclipped(int32_t gain_level, int32_t v)
{
	return round_shift_s64_to_s32_saturating(
		(int64_t)v * gain_level, V92_PCM_GAIN_SHIFT);
}

static int16_t scale_gain_product(int32_t gain_level, int32_t v)
{
	return clamp_s16(scale_gain_product_unclipped(gain_level, v));
}

static int16_t scale_sample(
	uint16_t four_times_gain_q0_16, int16_t upstream_level,
	int32_t v)
{
	return scale_gain_product((int32_t)four_times_gain_q0_16 * upstream_level, v);
}

static uint64_t level_error(
	const struct v92_pcm_mapping *mapping, int16_t level,
	const struct v92_pcm_frame *frame, const int16_t *samples)
{
	uint64_t error = 0u;

	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int64_t residual = (int64_t)samples[symbol] - g711_quantize(
			mapping->law, scale_sample(mapping->cpd.four_times_gain_q0_16,
				level, frame->v[symbol]));

		error += (uint64_t)(residual * residual);
	}
	return error;
}

enum v92_pcm_frame_result v92_pcm_frame_rx_refine_level(
	struct v92_pcm_frame_rx *receiver, struct v92_pcm_level_fit *fit,
	const struct v92_pcm_frame *frame,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	if (receiver == NULL || fit == NULL || frame == NULL || samples == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), fit, sizeof(*fit)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), frame, sizeof(*frame)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), samples,
				   V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*samples)) ||
	    memory_regions_overlap(fit, sizeof(*fit), frame, sizeof(*frame)) ||
	    memory_regions_overlap(fit, sizeof(*fit), samples,
				   V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*samples)))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver) ||
	    receiver->observation != V92_PCM_FRAME_FILTER_OUTPUT || fit->frames >= V92_B1U_FRAMES)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	struct v92_pcm_level_fit next = *fit;
	int16_t lower = receiver->minimum_level, upper = receiver->maximum_level;

	if (fit->frames == 0u) {
		struct g711_interval interval;
		if (!g711_quantization_interval(receiver->mapping.law, receiver->upstream_level, &interval))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		lower = interval.lower < 1 ? 1 : interval.lower;
		upper = interval.upper;
		next = (struct v92_pcm_level_fit){ .minimum = lower, .maximum = upper };
	}
	if (next.minimum <= 0 || next.minimum > receiver->upstream_level ||
	    next.maximum < receiver->upstream_level ||
	    next.maximum - next.minimum >= V92_PCM_LEVEL_FIT_POINTS)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	int16_t preferred = receiver->upstream_level;
	uint64_t frame_error = UINT64_MAX;
	int nearest = INT_MAX;

	/* Use the current frame only to break cumulative-score ties, preferring
	 * the nearest estimate in the prior plateau. All original candidates
	 * keep accumulating evidence, including those outside that plateau. */
	for (int level = next.minimum; level <= next.maximum; ++level) {
		uint64_t error = level_error(&receiver->mapping, (int16_t)level, frame, samples);
		int distance = level < receiver->upstream_level ?
			receiver->upstream_level - level : level - receiver->upstream_level;
		if (level >= lower && level <= upper &&
		    (error < frame_error || (error == frame_error && distance < nearest))) {
			frame_error = error;
			nearest = distance;
			preferred = (int16_t)level;
		}
		if (next.error[level - next.minimum] > UINT64_MAX - error)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		next.error[level - next.minimum] += error;
	}
	int16_t best = preferred;
	uint64_t best_error = UINT64_MAX;
	for (int level = next.minimum; level <= next.maximum; ++level) {
		uint64_t error = next.error[level - next.minimum];
		if (error < best_error || (error == best_error && level == preferred)) {
			best_error = error;
			best = (int16_t)level;
		}
	}
	int16_t low = best, high = best;
	while (low > next.minimum && next.error[low - next.minimum - 1] == best_error)
		low--;
	while (high < next.maximum && next.error[high - next.minimum + 1] == best_error)
		high++;
	next.frames++;
	*fit = next;
	receiver->upstream_level = best;
	receiver->minimum_level = low;
	receiver->maximum_level = high;
	return V92_PCM_FRAME_OK;
}

static bool encode_digits(const struct v92_pcm_mapping *mapping,
	struct v92_pcm_frame_tx_state *state, struct v92_pcm_frame *frame)
{
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		uint8_t y0;

		if (v34_convolutional_encoder_output(&state->trellis, &y0) != V34_TRELLIS_OK ||
		    !select_point(mapping, &state->filter, symbol, frame->digit[symbol],
			frame->signed_index, y0, &frame->signed_index[symbol], &frame->u[symbol]))
			return false;
		frame->y0[symbol] = y0;
		filter_step(mapping, &state->filter, frame->u[symbol], &frame->x[symbol],
			&frame->v[symbol]);
		if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u &&
		    !advance_trellis(&state->trellis, frame->signed_index, symbol))
			return false;
	}
	return true;
}

enum v92_pcm_frame_result v92_pcm_frame_encode_rm(
	const struct v92_pcm_mapping *mapping,
	struct v92_pcm_frame_tx_state *state, bool prime, struct v92_pcm_frame *frame)
{
	struct v92_pcm_frame encoded = { 0 };

	if (mapping == NULL || state == NULL || frame == NULL ||
	    memory_regions_overlap(mapping, sizeof(*mapping), state, sizeof(*state)) ||
	    memory_regions_overlap(mapping, sizeof(*mapping), frame, sizeof(*frame)) ||
	    memory_regions_overlap(state, sizeof(*state), frame, sizeof(*frame)))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping) || !state_is_valid(state, mapping->cpd.trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	struct v92_pcm_frame_tx_state next = *state;

	/* V.92 Tables 25/26 specify digits at the modulus encoder output,
	 * bypassing scrambling and differential coding, not the channel coder. */
	for (size_t i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		encoded.digit[i] = (i % 4u < 2u) != prime ?
			(uint8_t)(mapping->cpd.modulus[i] - 1u) : 0u;
	if (!encode_digits(mapping, &next, &encoded))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	*state = next;
	*frame = encoded;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_encode(
	struct v92_pcm_frame_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame *frame)
{
	if (transmitter == NULL || frame == NULL || bits == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_tx_is_valid(transmitter))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	return v92_pcm_frame_encode_owned(&transmitter->mapping,
		&transmitter->state, bits, bit_count, frame);
}

enum v92_pcm_frame_result v92_pcm_frame_encode_owned(
	const struct v92_pcm_mapping *mapping,
	struct v92_pcm_frame_tx_state *state,
	const uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame *frame)
{
	struct v92_pcm_frame_tx_state next;
	struct v92_pcm_frame encoded = { 0 };
	uint8_t scrambled[V92_PCM_MAPPING_MAX_BITS];

	if (mapping == NULL || state == NULL || frame == NULL || bits == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (bit_count != mapping->data_bits_per_frame ||
	    !unpacked_bits_are_binary(bits, bit_count))
		return V92_PCM_FRAME_OUT_OF_RANGE;
	next = *state;
	if (v34_scramble_unpacked_bits(
		    &next.scrambler, bits, bit_count, scrambled) !=
	    V34_FRAMING_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (v92_pcm_mapping_encode_owned(
		    mapping, scrambled, bit_count,
		    &next.differential, encoded.digit,
		    &encoded.frame_sign) != V92_PCM_MAPPING_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (!encode_digits(mapping, &next, &encoded))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	*state = next;
	*frame = encoded;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_measure_b1u(
	const struct v92_cpd_parameters *parameters, enum g711_law law,
	struct v92_b1u_measurement *measurement)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx_state state;
	struct v92_b1u_measurement next = { 0 };
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];

	if (parameters == NULL || measurement == NULL ||
	    memory_regions_overlap(parameters, sizeof(*parameters),
		measurement, sizeof(*measurement)))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (v92_pcm_mapping_init(&mapping, law, parameters) != V92_PCM_MAPPING_OK ||
	    !state_init(&state, parameters->trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	memset(ones, 1, mapping.data_bits_per_frame);
	for (unsigned index = 0; index < V92_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;
		enum v92_pcm_frame_result result = v92_pcm_frame_encode_owned(
			&mapping, &state, ones,
			mapping.data_bits_per_frame, &frame);

		if (result != V92_PCM_FRAME_OK)
			return result;
		for (size_t symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			uint64_t square = (uint64_t)((int64_t)frame.v[symbol] * frame.v[symbol]);
			int point = frame.signed_index[symbol] + V92_CPD_MAX_CONSTELLATION_POINTS;

			if (point < 0 || point >= 2 * V92_CPD_MAX_CONSTELLATION_POINTS ||
			    square > UINT64_MAX - next.prefilter_energy)
				return V92_PCM_FRAME_OUT_OF_RANGE;
			/* 576 signed 16-bit points keep this sum below 2^40. */
			next.point_energy +=
				(uint64_t)((int64_t)frame.u[symbol] * frame.u[symbol]);
			next.prefilter_energy += square;
			next.signed_frequencies[point]++;
		}
	}
	*measurement = next;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_scale(
	uint16_t four_times_gain_q0_16, int16_t upstream_level,
	int32_t prefilter_output, int16_t *sample)
{
	if (sample == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (four_times_gain_q0_16 == 0u)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (upstream_level <= 0)
		return V92_PCM_FRAME_OUT_OF_RANGE;
	*sample = scale_sample(four_times_gain_q0_16, upstream_level, prefilter_output);
	return V92_PCM_FRAME_OK;
}

bool v92_pcm_frame_b1u_fits(
	const struct v92_pcm_mapping *mapping, int16_t upstream_level)
{
	struct v92_pcm_frame_tx_state state;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

	if (!v92_pcm_mapping_is_valid(mapping) || upstream_level <= 0 ||
	    !state_init(&state, mapping->cpd.trellis))
		return false;
	memset(bits, 1, mapping->data_bits_per_frame);
	int32_t gain_level = (int32_t)mapping->cpd.four_times_gain_q0_16 * upstream_level;

	for (unsigned index = 0; index < V92_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;

		if (v92_pcm_frame_encode_owned(mapping, &state, bits,
			    mapping->data_bits_per_frame, &frame) != V92_PCM_FRAME_OK)
			return false;
		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int32_t scaled = scale_gain_product_unclipped(gain_level, frame.v[symbol]);

			if (scaled < INT16_MIN || scaled > INT16_MAX)
				return false;
		}
	}
	return true;
}

static bool noise_is_valid(const struct v92_pcm_frame_noise *noise)
{
	if (noise->frames > V92_B1U_FRAMES ||
	    !isfinite(noise->relative_variance) || noise->relative_variance < 0.0 ||
	    noise->relative_variance > 65535.0 ||
	    (noise->frames < V92_B1U_FRAMES && noise->relative_variance != 0.0))
		return false;
	unsigned symbols = noise->frames > V92_B1U_FRAMES / 2u && noise->frames < V92_B1U_FRAMES ?
		(noise->frames - V92_B1U_FRAMES / 2u) * V92_PCM_MAPPING_FRAME_SYMBOLS : 0u;
	const double moments[] = { noise->signal_energy, noise->signal_fourth,
		noise->error_energy, noise->error_signal_energy };
	const double bounds[] = { 32768.0 * 32768.0, 32768.0 * 32768.0 * 32768.0 * 32768.0,
		65535.0 * 65535.0, 65535.0 * 65535.0 * 32768.0 * 32768.0 };

	for (size_t i = 0; i < sizeof(moments) / sizeof(moments[0]); ++i)
		if (!isfinite(moments[i]) || moments[i] < 0.0 || moments[i] > symbols * bounds[i])
			return false;
	return true;
}

bool v92_pcm_frame_rx_is_valid(const struct v92_pcm_frame_rx *receiver)
{
	enum v34_trellis_selection selected;
	bool normalized = false;

	if (receiver == NULL || !v92_pcm_mapping_is_valid(&receiver->mapping) ||
	    receiver->path_count == 0u ||
	    receiver->path_count > V92_PCM_RX_PATHS ||
	    receiver->pending_frames > V92_PCM_RX_DELAY_FRAMES ||
	    receiver->post_b1_frames > receiver->pending_frames ||
	    receiver->b1u_frames > V92_B1U_FRAMES ||
	    !v34_scrambler_is_valid(&receiver->b1u_scrambler) ||
	    receiver->b1u_scrambler.polynomial != V34_POLYNOMIAL_GPA ||
	    !trellis_selection(receiver->mapping.cpd.trellis, &selected))
		return false;
	switch (receiver->observation) {
	case V92_PCM_FRAME_FILTER_OUTPUT:
		if (!memory_region_is_zero(&receiver->noise, sizeof(receiver->noise)) ||
		    receiver->minimum_level <= 0 ||
		    receiver->minimum_level > receiver->upstream_level ||
		    receiver->maximum_level < receiver->upstream_level)
			return false;
		break;
	case V92_PCM_FRAME_CONSTELLATION:
		if (!noise_is_valid(&receiver->noise) ||
		    receiver->upstream_level != 0 || receiver->minimum_level != 0 ||
		    receiver->maximum_level != 0)
			return false;
		break;
	default:
		return false;
	}
	for (size_t index = 0u; index < receiver->path_count; ++index) {
		const struct v92_pcm_frame_rx_path *path = &receiver->paths[index];

		normalized |= path->metric == 0u;
		if (path->differential > 1u ||
		    !v34_scrambler_is_valid(&path->descrambler) ||
		    path->descrambler.polynomial != V34_POLYNOMIAL_GPA ||
		    !v34_convolutional_encoder_is_valid(&path->trellis) ||
		    path->trellis.selection != selected)
			return false;
		if ((path->ambiguous_frames | path->nondata_frames | path->b1u_code_mismatches) >> receiver->pending_frames ||
		    path->rm_frames > V92_PCM_RM_QUALIFY_FRAMES || path->rm_prime_frames > 2u ||
		    (path->rm_prime_frames && path->rm_frames != V92_PCM_RM_QUALIFY_FRAMES))
			return false;
		for (size_t frame = 0u; frame < V92_PCM_RX_DELAY_FRAMES; ++frame) {
			size_t used = frame < receiver->pending_frames ?
				receiver->mapping.data_bits_per_frame : 0u;

			if (!unpacked_bits_are_binary(path->pending[frame], used) ||
			    !memory_region_is_zero(path->pending[frame] + used,
				V92_PCM_MAPPING_MAX_BITS - used))
				return false;
		}
	}
	return normalized;
}

static enum v92_pcm_frame_result initialize_receiver(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	enum v92_pcm_frame_observation observation)
{
	struct v92_pcm_mapping accepted;
	struct v34_scrambler descrambler = { 0 };
	struct v34_convolutional_encoder trellis = { 0 };
	enum v34_trellis_selection selected;

	if (receiver == NULL || mapping == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping) ||
	    (observation == V92_PCM_FRAME_FILTER_OUTPUT && upstream_level <= 0) ||
	    !trellis_selection(mapping->cpd.trellis, &selected))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (v34_scrambler_init(&descrambler,
			       V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK ||
	    v34_convolutional_encoder_init(&trellis, selected) !=
		    V34_TRELLIS_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	/* All fallible work precedes replacement. Preserve a mapping that aliases
	 * this owner without staging the empty traceback histories. */
	accepted = *mapping;
	memset(receiver, 0, sizeof(*receiver));
	receiver->mapping = accepted;
	receiver->observation = observation;
	receiver->upstream_level = upstream_level;
	receiver->minimum_level = upstream_level;
	receiver->maximum_level = upstream_level;
	receiver->path_count = 1u;
	receiver->paths[0].descrambler = descrambler;
	receiver->b1u_scrambler = descrambler;
	receiver->paths[0].trellis = trellis;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_rx_init(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level)
{
	return initialize_receiver(receiver, mapping, upstream_level, V92_PCM_FRAME_FILTER_OUTPUT);
}

enum v92_pcm_frame_result v92_pcm_frame_rx_init_constellation(
	struct v92_pcm_frame_rx *receiver, const struct v92_pcm_mapping *mapping)
{
	return initialize_receiver(receiver, mapping, 0, V92_PCM_FRAME_CONSTELLATION);
}

enum v92_pcm_frame_result v92_pcm_frame_rx_reset_control(
	struct v92_pcm_frame_rx *receiver)
{
	if (receiver == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	/* Pending words retain their evidence; new control is not B1u. */
	receiver->b1u_frames = V92_B1U_FRAMES;
	for (size_t index = 0u; index < receiver->path_count; ++index) {
		receiver->paths[index].descrambler.history = 0u;
		receiver->paths[index].differential = 0u;
		receiver->paths[index].rm_frames = 0u;
		receiver->paths[index].rm_prime_frames = 0u;
	}
	return V92_PCM_FRAME_OK;
}

struct frame_path {
	uint8_t origin;
	struct v92_pcm_frame_rx_path state;
	struct v92_pcm_frame frame;
	uint64_t frame_metric;
	uint8_t output_bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t known_digits[V92_PCM_MAPPING_FRAME_SYMBOLS];
	bool output_ambiguous;
	bool output_nondata;
	bool output_b1u_code_mismatch;
	bool uncertain;
};

/* Pending payload is unchanged while extending the twelve symbols of a
 * frame. Keep its source path by origin and copy it only after the beam has
 * settled, instead of copying ten delayed frames at every symbol. */
static void copy_frame_path_without_pending(
	struct frame_path *destination, const struct frame_path *source)
{
	const size_t pending_offset = offsetof(struct frame_path, state) +
		offsetof(struct v92_pcm_frame_rx_path, pending);
	const size_t after_pending = pending_offset +
		sizeof(destination->state.pending);

	memcpy(destination, source, pending_offset);
	memcpy((char *)destination + after_pending,
	       (const char *)source + after_pending,
	       sizeof(*destination) - after_pending);
}

static enum v92_pcm_frame_rm rm_state(const struct v92_pcm_frame_rx_path *path)
{
	if (path->rm_frames < V92_PCM_RM_QUALIFY_FRAMES)
		return V92_PCM_FRAME_RM_NONE;
	return path->rm_prime_frames == 0u ? V92_PCM_FRAME_RM_SIGNAL :
		path->rm_prime_frames == 1u ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
}

/* Bit 0 matches RM; bit 1 matches RM-prime. A prefix may match either. */
static unsigned rm_prefix(const struct v92_pcm_mapping *mapping,
	const uint8_t *digits, size_t symbols)
{
	unsigned words = 3u;

	for (size_t i = 0u; i < symbols && words; ++i) {
		uint8_t high = (uint8_t)(mapping->cpd.modulus[i] - 1u);
		if (digits[i] != (i % 4u < 2u ? high : 0u)) words &= ~1u;
		if (digits[i] != (i % 4u < 2u ? 0u : high)) words &= ~2u;
	}
	return words;
}

static bool observe_rm(const struct v92_pcm_mapping *mapping, const uint8_t *digits,
	bool b1u, struct v92_pcm_frame_rx_path *path)
{
	unsigned words = b1u ? 0u : rm_prefix(mapping, digits, V92_PCM_MAPPING_FRAME_SYMBOLS);
	bool rm = (words & 1u) != 0u, prime = (words & 2u) != 0u;

	if (rm) {
		if (path->rm_prime_frames) path->rm_frames = 0u;
		if (path->rm_frames < V92_PCM_RM_QUALIFY_FRAMES) ++path->rm_frames;
		path->rm_prime_frames = 0u;
	} else if (prime && path->rm_frames == V92_PCM_RM_QUALIFY_FRAMES) {
		if (path->rm_prime_frames < 2u) ++path->rm_prime_frames;
	} else {
		path->rm_frames = 0u;
		path->rm_prime_frames = 0u;
	}
	return rm || (prime && path->rm_prime_frames != 0u);
}

struct frame_candidate {
	/* Heap rank includes any remaining-symbol lower bound; path_metric
	 * contains only observations already consumed by this candidate. */
	uint64_t metric;
	uint64_t path_metric;
	int16_t eta;
	uint8_t path;
	uint8_t digit;
	uint8_t y0;
};

/* Max-heap of the best bounded candidates. Equal metrics do not replace
 * retained candidates; the path/point enumeration makes ties deterministic. */
static void candidate_push(struct frame_candidate *candidates, size_t count)
{
	size_t child = count - 1u;
	struct frame_candidate added = candidates[child];

	while (child != 0u) {
		size_t parent = (child - 1u) / 2u;

		if (candidates[parent].metric >= added.metric)
			break;
		candidates[child] = candidates[parent];
		child = parent;
	}
	candidates[child] = added;
}

static void candidate_replace(
	struct frame_candidate *candidates, size_t count,
	const struct frame_candidate *replacement)
{
	size_t parent = 0u;
	struct frame_candidate added = *replacement;

	for (;;) {
		size_t left = 2u * parent + 1u;
		size_t right = left + 1u;
		size_t larger;

		if (left >= count)
			break;
		larger = right < count &&
			candidates[right].metric > candidates[left].metric ?
				right : left;
		if (added.metric >= candidates[larger].metric)
			break;
		candidates[parent] = candidates[larger];
		parent = larger;
	}
	candidates[parent] = added;
}

static int16_t signed_point(
	const struct v92_cpd_constellation *constellation, int eta)
{
	return eta >= 0 ? constellation->positive_linear[eta] :
		(int16_t)-constellation->positive_linear[-eta - 1];
}

static uint64_t constellation_weight(const struct v92_pcm_frame_rx *receiver, int16_t sample)
{
	double variance = 1.0 + receiver->noise.relative_variance * (double)sample * sample;
	uint64_t weight = (uint64_t)(65536.0 / variance);

	return weight ? weight : 1u;
}

static uint64_t cell_metric(const struct v92_pcm_frame_cell *cell,
	int16_t point, uint64_t weight, bool present)
{
	double distance = fmax(cell->lower - point, fmax(point - cell->upper, 0.0));
	double cost = present ? distance * distance * weight : 0.0;

	/* Compare with exactly representable 2^64 before converting to uint64_t. */
	return cost >= 0x1p64 ? UINT64_MAX : (uint64_t)cost;
}

static int16_t level_sample(const struct v92_pcm_frame_rx *receiver, int16_t level, int32_t v)
{
	return g711_quantize(receiver->mapping.law,
		scale_sample(receiver->mapping.cpd.four_times_gain_q0_16, level, v));
}

static int16_t predicted_sample(
	const struct v92_pcm_frame_rx *receiver, int32_t v, int16_t observed, bool b1u)
{
	if (b1u || receiver->minimum_level == receiver->maximum_level)
		return level_sample(receiver, receiver->upstream_level, v);
	int low = receiver->minimum_level;
	int high = receiver->maximum_level;
	int16_t low_sample = level_sample(receiver, (int16_t)low, v);
	int16_t high_sample = level_sample(receiver, (int16_t)high, v);

	if (low_sample == high_sample || (v >= 0 ? observed <= low_sample : observed >= low_sample))
		return low_sample;
	if (v >= 0 ? observed >= high_sample : observed <= high_sample)
		return high_sample;
	/* Quantized predictions are monotone in LU. Find the two reachable
	 * codewords surrounding the observation, not an interpolated level
	 * which the transmitter cannot produce. */
	while (high - low > 1) {
		int middle = low + (high - low) / 2;
		int16_t sample = level_sample(receiver, (int16_t)middle, v);

		if (v >= 0 ? sample < observed : sample > observed) {
			low = middle;
			low_sample = sample;
		} else {
			high = middle;
			high_sample = sample;
		}
	}
	int low_error = observed - low_sample;
	int high_error = observed - high_sample;

	return (int64_t)low_error * low_error <= (int64_t)high_error * high_error ?
		low_sample : high_sample;
}

static int16_t predicted_observation(
	const struct v92_pcm_frame_rx *receiver, int16_t u, int32_t v,
	int16_t observed, bool b1u)
{
	return receiver->observation == V92_PCM_FRAME_CONSTELLATION ? u :
		predicted_sample(receiver, v, observed, b1u);
}

/* Pre-codec noise moves the source within or between G.711 input cells.
 * Reconstructed-level distance can prefer a distant source merely because
 * its codeword is closer to the received codeword. Rank by input distance;
 * retain decoded-space predictions and residuals for line adaptation and
 * quality reporting. The caller has already validated the mapping and LU. */
static uint64_t input_cell_metric(
	const struct v92_pcm_frame_rx *receiver, int32_t v,
	const struct g711_interval *cell, int32_t minimum_gain, int32_t maximum_gain)
{
	int a = scale_gain_product(minimum_gain, v);
	int b = scale_gain_product(maximum_gain, v);
	int low = a < b ? a : b, high = a > b ? a : b;
	int64_t error = low > cell->upper ? low - cell->upper :
		high < cell->lower ? high - cell->lower : 0;

	if (!error && low < cell->lower && high > cell->upper) {
		/* LU is an integer, not a continuum. Large scale steps can skip
		 * a received cell even when the endpoints surround it. */
		int left = v < 0 ? receiver->maximum_level : receiver->minimum_level;
		int right = v < 0 ? receiver->minimum_level : receiver->maximum_level;

		while (right - left > 1 || left - right > 1) {
			int middle = left + (right - left) / 2;
			int sample = scale_sample(receiver->mapping.cpd.four_times_gain_q0_16,
				(int16_t)middle, v);

			if (sample < cell->lower) {
				left = middle;
				low = sample;
			} else if (sample > cell->upper) {
				right = middle;
				high = sample;
			} else {
				return 0;
			}
		}
		error = cell->lower - low < high - cell->upper ?
			cell->lower - low : high - cell->upper;
	}

	return (uint64_t)(error * error);
}

static inline uint64_t sample_metric(const struct v92_pcm_frame_rx *receiver,
	int16_t u, int32_t v, int16_t sample, bool b1u,
	const struct g711_interval *cell, int32_t minimum_gain, int32_t maximum_gain,
	uint64_t weight)
{
	if (cell)
		return input_cell_metric(receiver, v, cell, minimum_gain, maximum_gain);
	int64_t error = (int64_t)sample - predicted_observation(receiver, u, v, sample, b1u);

	return (uint64_t)(error * error) * weight;
}

static bool b1u_digits(
	const struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_frame_rx_path *path,
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	struct v34_scrambler scrambler = path->descrambler;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t differential = path->differential;
	uint8_t sign;

	memset(bits, 1, receiver->mapping.data_bits_per_frame);
	if (v34_scramble_unpacked_bits(
		    &scrambler, bits, receiver->mapping.data_bits_per_frame,
		    bits) != V34_FRAMING_OK)
		return false;
	return v92_pcm_mapping_encode(
		&receiver->mapping, bits, receiver->mapping.data_bits_per_frame,
		&differential, digits, &sign) == V92_PCM_MAPPING_OK;
}

static void train_noise(struct v92_pcm_frame_noise *noise,
	const int16_t *samples, const int16_t *expected)
{
	if (noise->frames == V92_B1U_FRAMES)
		return;
	/* Allow initial B1u gain acquisition to settle. Use only the remaining
	 * training interval; the caller qualifies its decoded bits. Payload and
	 * subsequent control exchanges must not update this estimate. */
	if (noise->frames >= V92_B1U_ACQUISITION_FRAMES) {
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			double signal = expected[i], error = samples[i] - signal;
			double square = signal * signal, residual = error * error;

			noise->signal_energy += square;
			noise->signal_fourth += square * square;
			noise->error_energy += residual;
			noise->error_signal_energy += residual * square;
		}
	}
	if (++noise->frames != V92_B1U_FRAMES)
		return;
	double count = V92_B1U_SYMBOLS / 2u;
	double determinant = count * noise->signal_fourth - noise->signal_energy * noise->signal_energy;
	double slope = determinant > 0.0 ?
		(count * noise->error_signal_energy - noise->error_energy * noise->signal_energy) / determinant : 0.0;
	double average_error = noise->error_energy / count;
	/* Do not extrapolate near-zero variance at low amplitudes from this
	 * short fit: retain the average B1u residual as a conservative noise
	 * floor. Constant-amplitude or amplitude-independent noise keeps equal
	 * weights. One sample and Q16 bound numerical precision, not quality. */
	double ratio = slope > 0.0 ? slope / (average_error > 1.0 ? average_error : 1.0) : 0.0;

	if (ratio > 65535.0)
		ratio = 65535.0;

	*noise = (struct v92_pcm_frame_noise){
		.frames = V92_B1U_FRAMES, .relative_variance = ratio,
	};
}

static enum v92_pcm_frame_result decode_frame(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame_decision *decision,
	const struct v92_pcm_frame_channel *channel,
	struct v92_pcm_frame_likelihood *likelihood, bool acquire_model)
{
	const struct v92_observation *model = channel ? channel->model : NULL;
	const struct v92_pcm_frame_cell *cells = channel ? channel->cells : NULL;
	const int16_t *levels = channel ? channel->levels : NULL;
	const double *offsets = channel ? channel->offsets : NULL;
	const bool *received = channel ? channel->received : NULL;
	struct frame_path banks[2][V92_PCM_RX_PATHS];
	struct frame_path *paths = banks[0];
	struct frame_path *next = banks[1];
	uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
	size_t path_count;
	size_t best = SIZE_MAX;
	const struct memory_region regions[] = {
		{ receiver, sizeof(*receiver) },
		{ samples, V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*samples) },
		{ bits, bit_count },
		{ decision, sizeof(*decision) },
		{ model, model ? sizeof(*model) : 0u },
		{ channel, channel ? sizeof(*channel) : 0u },
		{ likelihood, likelihood ? sizeof(*likelihood) : 0u },
		{ levels, levels ? V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*levels) : 0u },
		{ offsets, offsets ? V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*offsets) : 0u },
		{ received, received ? V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*received) : 0u },
		{ cells, cells ? V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*cells) : 0u },
	};

	if (receiver == NULL || samples == NULL || bits == NULL ||
	    decision == NULL ||
	    memory_region_set_overlaps(regions, sizeof(regions) / sizeof(regions[0])))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (cells) {
		if (model || levels || offsets || receiver->observation != V92_PCM_FRAME_CONSTELLATION)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			if (!isfinite(cells[i].lower) || !isfinite(cells[i].upper) ||
			    cells[i].lower > cells[i].upper)
				return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	if (acquire_model && (receiver->observation != V92_PCM_FRAME_CONSTELLATION ||
	    !b1u || receiver->noise.frames >= V92_B1U_ACQUISITION_FRAMES))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (offsets) {
		if (!model || !levels)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++)
			if (!isfinite(offsets[i]))
				return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	if (model && (!v92_observation_is_valid(model) ||
		model->law != receiver->mapping.law ||
		receiver->observation != V92_PCM_FRAME_CONSTELLATION ||
		x_history_count(&receiver->mapping) < model->taps - 1u))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (bit_count != receiver->mapping.data_bits_per_frame)
		return V92_PCM_FRAME_OUT_OF_RANGE;
	struct v34_scrambler b1u_scrambler = receiver->b1u_scrambler;
	uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
	bool expect_b1u = receiver->b1u_frames < V92_B1U_FRAMES;
	if (expect_b1u) {
		memset(expected, 1, bit_count);
		if (v34_scramble_unpacked_bits(&b1u_scrambler, expected, bit_count, expected) != V34_FRAMING_OK)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	/* LU bounds are fixed for this frame. Factor them out of the per-point
	 * cell score while retaining the exact fixed-point rounding. */
	int32_t minimum_gain = (int32_t)receiver->mapping.cpd.four_times_gain_q0_16 *
		receiver->minimum_level;
	int32_t maximum_gain = (int32_t)receiver->mapping.cpd.four_times_gain_q0_16 *
		receiver->maximum_level;
	const struct filter_history_span history_span = causal_history(receiver);
	path_count = receiver->path_count;
	struct v92_observation_cache cache;
	if (model)
		v92_observation_begin(&cache, model, receiver->noise.relative_variance);
	/* Linear constellation samples can acquire coding without forcing the
	 * initial B1u history. A wrong early constraint can otherwise outlive
	 * acquisition and bias the gain used by held-out training and payload.
	 * Codec-model acquisition explicitly requests known coordinates instead. */
	bool known = b1u && (acquire_model ||
		receiver->observation != V92_PCM_FRAME_CONSTELLATION);
	/* The fourth symbol must satisfy the group's parity. Ranking partial
	 * groups without its cost can fill the beam with cheap prefixes whose
	 * legal completion is expensive. Independent symbol scores permit an
	 * exact minimum remaining cost for each parity, computed backwards.
	 * Apply this to unconstrained constellation B1u too: acquisition cannot
	 * recover a pruned path by looking farther ahead. The bound orders the
	 * existing beam without adding future costs to history or changing
	 * filter-dependent scoring, known-bit constraints or residual units. */
	bool lookahead = !known && !model &&
		(cells || receiver->observation == V92_PCM_FRAME_CONSTELLATION);
	/* Constellation scores are independent of filter history, including B1u.
	 * Filter-output scores can share work when CPd has zero history taps:
	 * precoding is then x=u, with the prefilter's current-input scaling. */
	bool cache_points = lookahead || (!b1u && !model);
	if (cache_points && receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT)
		for (unsigned kind = 0; kind < V92_CPD_FILTER_SECTION_COUNT; ++kind) {
			const struct v92_cpd_filter *filter = &receiver->mapping.cpd.filter[kind];

			for (unsigned tap = kind == V92_CPD_PREFILTER_FEED_FORWARD;
			     tap < filter->count; ++tap)
				if (filter->coefficient[tap]) cache_points = false;
		}
	uint64_t tail[V92_PCM_MAPPING_FRAME_SYMBOLS][2] = { { 0 } };
	uint64_t point_cost[V92_PCM_MAPPING_FRAME_SYMBOLS]
		[2 * V92_CPD_MAX_CONSTELLATION_POINTS];
	/* Reuse the exact lookahead costs when the beam visits the same points. */
	if (lookahead) {
		for (unsigned symbol = V92_PCM_MAPPING_FRAME_SYMBOLS; symbol-- > 0;) {
			unsigned set = receiver->mapping.cpd.frame_constellation[symbol % V92_PCM_MAPPING_PHASES];
			const struct v92_cpd_constellation *constellation = &receiver->mapping.cpd.constellation[set];
			uint64_t minimum[2] = { UINT64_MAX, UINT64_MAX };
			/* Cells already describe companding uncertainty. After the B1u
			 * tail drains, score the remaining input distance uniformly. */
			uint64_t weight = cells && receiver->post_b1_frames == V92_PCM_RX_DELAY_FRAMES ?
				UINT64_C(65536) : constellation_weight(receiver, samples[symbol]);

			for (int eta = -(int)constellation->count; eta < constellation->count; ++eta) {
				int16_t u = signed_point(constellation, eta);
				uint64_t cost = cells ? cell_metric(cells + symbol, u,
					weight, !received || received[symbol]) :
					sample_metric(receiver, u, 0, samples[symbol], b1u,
						NULL, minimum_gain, maximum_gain, weight);
				point_cost[symbol][eta + constellation->count] = cost;
				unsigned parity = (unsigned)eta & 1u;

				if (cost < minimum[parity]) minimum[parity] = cost;
			}
			for (unsigned parity = 0; parity < 2; ++parity) {
				if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u)
					tail[symbol][parity] = minimum[parity];
				else {
					uint64_t even = add_u64_saturating(minimum[0], tail[symbol + 1][parity]);
					uint64_t odd = add_u64_saturating(minimum[1], tail[symbol + 1][parity ^ 1u]);
					tail[symbol][parity] = even < odd ? even : odd;
				}
			}
		}
	}

	for (size_t index = 0u; index < path_count; ++index) {
		paths[index] = (struct frame_path){
			.state = receiver->paths[index], .origin = (uint8_t)index,
		};
		if (known && !b1u_digits(receiver, &paths[index].state,
				      paths[index].known_digits))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	uint8_t maximum_digits[V92_PCM_MAPPING_FRAME_SYMBOLS];
	/* Apply payload bounds after delayed B1u validation has drained. */
	bool constrain_words = !b1u && receiver->post_b1_frames == V92_PCM_RX_DELAY_FRAMES;
	if (constrain_words) {
		uint8_t maximum_bits[V92_PCM_MAPPING_MAX_BITS], differential = 0u, sign;

		memset(maximum_bits, 1, bit_count);
		if (v92_pcm_mapping_encode_owned(&receiver->mapping, maximum_bits, bit_count,
			&differential, maximum_digits, &sign) != V92_PCM_MAPPING_OK)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		constrain_words = false;
		for (size_t i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			constrain_words |= maximum_digits[i] != receiver->mapping.cpd.modulus[i] - 1u;
	}
	bool initial = !known && b1u && receiver->noise.frames == 0u &&
		receiver->pending_frames == 0u;
	/* Inverting the four coordinates complements only convolutional input
	 * Y3. The 16/32-state codes ignore it; the 64-state recurrence instead
	 * preserves Y0 when delay bit 5 is complemented. Retain that alternative
	 * initial coding history without constraining the received B1u bits. */
	if (initial && path_count == 1u &&
	    receiver->mapping.cpd.trellis == V92_CPD_TRELLIS_64_STATE) {
		paths[1] = paths[0];
		paths[1].state.trellis.state ^= 32u;
		paths[1].state.differential ^= 1u;
		++path_count;
	}
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		uint8_t set = receiver->mapping.cpd.frame_constellation[
			symbol % V92_PCM_MAPPING_PHASES];
		const struct v92_cpd_constellation *constellation =
			&receiver->mapping.cpd.constellation[set];
		unsigned modulus = receiver->mapping.cpd.modulus[symbol];
		unsigned stride = symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u ? 2u : 1u;
		struct frame_candidate candidates[V92_PCM_RX_PATHS];
		size_t count = 0u;
		uint64_t weight = 1u;
		bool present = !received || received[symbol];
		bool input_metric = !b1u &&
			receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT;
		struct g711_interval cell;

		if (input_metric &&
		    !g711_quantization_interval(receiver->mapping.law, samples[symbol], &cell))
			return V92_PCM_FRAME_INVALID_PARAMETERS;

		if (model && present && !v92_observation_symbol_level(&cache,
			samples[symbol], levels ? levels + symbol : NULL,
			offsets ? offsets[symbol] : 0.))
			return V92_PCM_FRAME_INVALID_PARAMETERS;

		if (receiver->observation == V92_PCM_FRAME_CONSTELLATION && !model && !lookahead) {
			/* Companding noise grows with amplitude. Estimate reliability
			 * from the observation, independently of each candidate point.
			 * Compute once per symbol, outside the bounded path search. */
			weight = constellation_weight(receiver, samples[symbol]);
		}
		if (cache_points && !lookahead)
			for (int eta = -(int)constellation->count; eta < constellation->count; ++eta) {
				int16_t u = signed_point(constellation, eta);
				int32_t v = receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT ?
					prefilter_output(&receiver->mapping, 0, u) : 0;

				point_cost[symbol][eta + constellation->count] = sample_metric(receiver,
					u, v, samples[symbol], b1u, input_metric ? &cell : NULL,
					minimum_gain, maximum_gain, weight);
			}

		for (size_t path = 0u; path < path_count; ++path) {
			uint8_t y0;
			int64_t precoder = 0, prefilter = 0;
			struct v92_observation_point history[V92_OBSERVATION_TAPS - 1];

			bool filter_metric = !cache_points && !model && !cells &&
				receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT;
			/* Candidate and lookahead costs are nonnegative; ties cannot replace. */
			if (count == V92_PCM_RX_PATHS &&
			    paths[path].state.metric >= candidates[0].metric)
				continue;
			if (model || filter_metric)
				precoder = precoder_history(&receiver->mapping, &paths[path].state.filter);
			if (filter_metric)
				prefilter = prefilter_history(&receiver->mapping, &paths[path].state.filter);
			if (model) {
				const int32_t *previous = x_history(&receiver->mapping, &paths[path].state.filter);
				for (unsigned i = 1; i < model->taps; i++)
					history[i - 1] = v92_observation_point(&cache, previous[i - 1]);
			}

			if (v34_convolutional_encoder_output(
					&paths[path].state.trellis, &y0) !=
			    V34_TRELLIS_OK)
				return V92_PCM_FRAME_INVALID_PARAMETERS;
			unsigned parity = y0;
			if (lookahead)
				for (size_t previous = symbol - symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS;
				     previous < symbol; ++previous)
					parity ^= (unsigned)paths[path].frame.signed_index[previous] & 1u;
			int data_limit = (int)modulus - 1;
			unsigned requests = 0u;
			if (constrain_words && symbol + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS) {
				/* Restrict the completed word before the heap fills with
				 * unused codes. Compare its lower digits with 2^K-1 once
				 * per path; the final digit then needs only an integer bound.
				 * Differential reflection applies to every modulus digit. */
				data_limit = maximum_digits[symbol];
				for (size_t i = symbol; i-- > 0u;) {
					unsigned ordinary = paths[path].state.differential ?
						receiver->mapping.cpd.modulus[i] - 1u - paths[path].frame.digit[i] :
						paths[path].frame.digit[i];

					if (ordinary != maximum_digits[i]) {
						data_limit -= ordinary > maximum_digits[i];
						break;
					}
				}
				requests = rm_prefix(&receiver->mapping, paths[path].frame.digit, symbol);
				if (paths[path].state.rm_frames != V92_PCM_RM_QUALIFY_FRAMES)
					requests &= ~2u;
			}
			int first_eta = -(int)constellation->count, end_eta = constellation->count;
			/* A full heap supplies a budget which can only decrease as this
			 * path is visited. Trim only tails whose enlarged observation
			 * envelope cannot reach that budget; preserve eta/tie ordering. */
			if (model && count == V92_PCM_RX_PATHS && present) {
				uint64_t budget = candidates[0].metric - paths[path].state.metric;

				double radius = sqrt((budget + 1. + 1e-9 * budget) / cache.bound_scale);
				double minimum = cache.observed + cache.lower - radius;
				double maximum = cache.observed + cache.upper + radius;
				for (unsigned i = 1; i < model->taps; i++) {
					minimum -= history[i - 1].values.high[i];
					maximum -= history[i - 1].values.low[i];
				}
				int low = first_eta, high = end_eta;

				while (low < high) {
					int mid = low + (high - low) / 2;
					struct v92_observation_range point = v92_observation_range(&cache,
						precoder_output(precoder, signed_point(constellation, mid)));

					if (point.high < minimum) low = mid + 1;
					else high = mid;
				}
				first_eta = low;
				high = end_eta;
				while (low < high) {
					int mid = low + (high - low) / 2;
					struct v92_observation_range point = v92_observation_range(&cache,
						precoder_output(precoder, signed_point(constellation, mid)));

					if (point.low <= maximum) low = mid + 1;
					else high = mid;
				}
				end_eta = low;
			}
			int offset = equivalence_offset(symbol, paths[path].frame.signed_index, y0);
			unsigned step = known ? stride * modulus : stride;
			int residue = offset + (known ? (int)(stride * paths[path].known_digits[symbol]) : 0);

			first_eta += positive_modulo(residue - first_eta, step);
			unsigned next_digit = (unsigned)positive_modulo((first_eta - offset) / (int)stride, modulus);

			/* Known B1u visits one residue class; data cycles all digits. */
			for (int eta = first_eta; eta < end_eta; eta += (int)step) {
				uint8_t digit = (uint8_t)next_digit;
				uint64_t cost;
				struct frame_candidate candidate;

				next_digit += !known;
				if (next_digit == modulus)
					next_digit = 0u;
				if (constrain_words && symbol + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS) {
					int ordinary = paths[path].state.differential ? (int)modulus - 1 - digit : digit;

					if (ordinary > data_limit && !((requests & 1u) && digit == 0u) &&
					    !((requests & 2u) && digit == modulus - 1u))
						continue;
				}
				int16_t u = signed_point(constellation, eta);
				if (cache_points) {
					cost = point_cost[symbol][eta + constellation->count];
				} else if (model) {
					if (count == V92_PCM_RX_PATHS &&
						paths[path].state.metric >= candidates[0].metric)
						continue;
					uint64_t budget = count == V92_PCM_RX_PATHS ?
						candidates[0].metric - paths[path].state.metric : UINT64_MAX;

					cost = !present ? 0 :
						v92_observation_score(&cache, precoder_output(precoder, u), history, budget);
				} else if (cells) {
					cost = cell_metric(cells + symbol, u, weight, present);
				} else {
					int32_t v = receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT ?
						prefilter_output(&receiver->mapping, prefilter,
							precoder_output(precoder, u)) : 0;

					cost = sample_metric(receiver, u, v, samples[symbol], b1u,
						input_metric ? &cell : NULL, minimum_gain, maximum_gain, weight);
				}
				candidate = (struct frame_candidate){
					.metric = add_u64_saturating(paths[path].state.metric, cost),
					.eta = (int16_t)eta, .path = (uint8_t)path,
					.digit = digit, .y0 = y0,
				};
				candidate.path_metric = candidate.metric;
				if (lookahead && symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS != 3u)
					candidate.metric = add_u64_saturating(candidate.metric,
						tail[symbol + 1][parity ^ ((unsigned)eta & 1u)]);
				if (count < V92_PCM_RX_PATHS) {
					candidates[count++] = candidate;
					candidate_push(candidates, count);
				} else if (candidate.metric < candidates[0].metric) {
					candidate_replace(candidates, count, &candidate);
				}
			}
		}
		if (count == 0u)
			return V92_PCM_FRAME_NO_PATH;
		for (size_t index = 0u; index < count; ++index) {
			const struct frame_candidate *candidate = &candidates[index];
			struct v92_pcm_frame *frame = &next[index].frame;
			int64_t error;

			copy_frame_path_without_pending(
				&next[index], &paths[candidate->path]);
			next[index].state.metric = candidate->path_metric;
			frame->signed_index[symbol] = candidate->eta;
			frame->digit[symbol] = candidate->digit;
			frame->u[symbol] = signed_point(constellation, candidate->eta);
			frame->y0[symbol] = candidate->y0;
			filter_step(&receiver->mapping, &next[index].state.filter,
				    frame->u[symbol], &frame->x[symbol], &frame->v[symbol]);
			/* Reconstructed error reports residuals, not input-cell rank.
			 * Only retained paths need that quantized prediction. */
			error = (int64_t)samples[symbol] - predicted_observation(receiver,
				frame->u[symbol], frame->v[symbol], samples[symbol], b1u);
			next[index].frame_metric += (uint64_t)(error * error);
			if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u &&
			    !advance_trellis(&next[index].state.trellis,
					     frame->signed_index, symbol))
				return V92_PCM_FRAME_INVALID_PARAMETERS;
		}
		struct frame_path *swap = paths;

		paths = next;
		next = swap;
		path_count = count;
	}
	/* Input-cell distance is not a likelihood. Use the best symbol
	 * path's CURRENT-frame cost as an optimistic residual scale, excluding
	 * its inherited path penalty. Gaps within three mean symbol energies
	 * are weak evidence, not exact ambiguity or permission to discard bits.
	 * The call owner can qualify repeated uncertainty over time. */
	bool measure_confidence = !b1u && !model &&
		(cells || receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT);
	uint64_t resolution = 0u;
	if (measure_confidence) {
		size_t closest = 0u;

		for (size_t i = 1u; i < path_count; ++i)
			if (paths[i].state.metric < paths[closest].state.metric)
				closest = i;
		uint64_t cost = paths[closest].state.metric -
			receiver->paths[paths[closest].origin].metric;

		resolution = cost / (V92_PCM_MAPPING_FRAME_SYMBOLS / 3u);
	}
	/* Spare payload words remain invalid, except the exact RM sequences
	 * defined at the modulus encoder output rather than its bit input. */
	size_t valid = 0u;

	for (size_t index = 0u; index < path_count; ++index) {
		memcpy(paths[index].state.pending,
		       receiver->paths[paths[index].origin].pending,
		       sizeof(paths[index].state.pending));
		uint8_t candidate_bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame_rx_path *state = &paths[index].state;
		bool request_word = observe_rm(&receiver->mapping, paths[index].frame.digit, b1u, state);
		bool nondata = false;

		/* Frame entry validated this mapping, which candidates never change. */
		enum v92_pcm_mapping_result mapped = v92_pcm_mapping_decode_owned(
			&receiver->mapping, paths[index].frame.digit,
			&state->differential, candidate_bits, bit_count);
		/* The other codes share symbol paths under inversion, so defer the
		 * initial differential alternative until modulus decoding. A spare
		 * word must not discard the right path before coding can synchronize.
		 * Never relax payload, later B1u or known-history model acquisition. */
		if (mapped == V92_PCM_MAPPING_OUT_OF_RANGE && initial &&
		    receiver->mapping.cpd.trellis != V92_CPD_TRELLIS_64_STATE) {
			state->differential ^= 1u;
			mapped = v92_pcm_mapping_decode_owned(&receiver->mapping,
				paths[index].frame.digit, &state->differential, candidate_bits, bit_count);
		}
		if (mapped != V92_PCM_MAPPING_OK) {
			if (!request_word) continue;
			memset(candidate_bits, 0, bit_count);
			nondata = true;
		}
		/* Observe the modulus word before GPA history can carry an
		 * acquisition error into later source bits. Forced acquisition
		 * cannot independently certify its own choices. */
		bool code_mismatch = !expect_b1u || known || mapped != V92_PCM_MAPPING_OK ||
			memcmp(candidate_bits, expected, bit_count) != 0;
		if (mapped == V92_PCM_MAPPING_OK) {
			/* An isolated in-range RM-like word can be genuine payload.
			 * Preserve coding until the repeated request qualifies. */
			for (size_t bit = 0u; bit < bit_count;) {
				unsigned count = bit_count - bit > 8u ? 8u : (unsigned)(bit_count - bit);
				uint8_t group = 0u;

				for (unsigned i = 0u; i < count; ++i) {
					if (candidate_bits[bit + i] > 1u)
						return V92_PCM_FRAME_INVALID_PARAMETERS;
					group = (uint8_t)((group << 1) | candidate_bits[bit + i]);
				}
				if (v34_descramble_group(&state->descrambler, group,
							count, &group) != V34_FRAMING_OK)
					return V92_PCM_FRAME_INVALID_PARAMETERS;
				for (unsigned i = 0u; i < count; ++i)
					candidate_bits[bit + i] = (uint8_t)((group >> (count - 1u - i)) & 1u);
				bit += count;
			}
		}
		if (rm_state(state) != V92_PCM_FRAME_RM_NONE) {
			/* Qualification exceeds lookahead: every queued word belongs
			 * to the request, never to the preceding genuine data tail. */
			state->nondata_frames = (uint16_t)((1u << receiver->pending_frames) - 1u);
			nondata = true;
		}
		paths[index].frame.frame_sign = state->differential;
		memcpy(paths[index].output_bits, state->pending[0], bit_count);
		paths[index].output_ambiguous = (state->ambiguous_frames & 1u) != 0u;
		paths[index].output_nondata = (state->nondata_frames & 1u) != 0u;
		paths[index].output_b1u_code_mismatch = (state->b1u_code_mismatches & 1u) != 0u;
		unsigned pending_index = receiver->pending_frames;
		if (receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES) {
			state->ambiguous_frames >>= 1u;
			state->nondata_frames >>= 1u;
			state->b1u_code_mismatches >>= 1u;
			pending_index--;
			memmove(state->pending, state->pending + 1,
				(V92_PCM_RX_DELAY_FRAMES - 1u) * sizeof(state->pending[0]));
			memcpy(state->pending[V92_PCM_RX_DELAY_FRAMES - 1u],
				candidate_bits, bit_count);
		} else {
			memcpy(state->pending[receiver->pending_frames],
				candidate_bits, bit_count);
		}
		if (nondata) state->nondata_frames |= (uint16_t)(1u << pending_index);
		if (code_mismatch) state->b1u_code_mismatches |= (uint16_t)(1u << pending_index);
		/* Merge histories with identical future behavior. Different old
		 * payload decisions are resolved by their accumulated metric. */
		size_t survivor;

		for (survivor = 0u; survivor < valid; ++survivor) {
			const struct v92_pcm_frame_rx_path *prior = &paths[survivor].state;

			if (state->differential == prior->differential &&
			    state->rm_frames == prior->rm_frames &&
			    state->rm_prime_frames == prior->rm_prime_frames &&
			    state->descrambler.history == prior->descrambler.history &&
			    state->trellis.state == prior->trellis.state &&
			    same_filter_history(&receiver->mapping, &history_span,
				&state->filter, &prior->filter))
				break;
		}
		uint64_t gap = survivor == valid ? UINT64_MAX :
			(state->metric > paths[survivor].state.metric ?
			 state->metric - paths[survivor].state.metric :
			 paths[survivor].state.metric - state->metric);
		/* Once histories merge, future samples cannot improve this
		 * decision. Retain a low-margin disagreement for recovery. */
		bool unresolved = gap == 0u || (model && gap <= V92_PCM_RX_AMBIGUITY_COST);

		/* Once future-equivalent paths merge, later samples cannot settle
		 * their disputed payload. Tag either possible winner for this
		 * observation only; leave its delayed ambiguity tags unchanged. */
		if (measure_confidence && survivor != valid && gap <= resolution &&
		    (memcmp(paths[survivor].output_bits, paths[index].output_bits, bit_count) != 0 ||
		     memcmp(paths[survivor].state.pending, state->pending, sizeof(state->pending)) != 0)) {
			paths[survivor].uncertain = true;
			paths[index].uncertain = true;
		}
		if (survivor == valid)
			paths[valid++] = paths[index];
		else if (state->metric < paths[survivor].state.metric && !unresolved)
			paths[survivor] = paths[index];
		else if (unresolved) {
			struct frame_path *prior = &paths[survivor];

			if (state->metric < prior->state.metric) {
				struct frame_path swap = *prior;

				*prior = paths[index];
				paths[index] = swap;
				state = &paths[index].state;
			}
			prior->output_ambiguous |= paths[index].output_ambiguous ||
				prior->output_nondata != paths[index].output_nondata ||
				memcmp(prior->output_bits, paths[index].output_bits, bit_count) != 0;
			prior->state.ambiguous_frames |= state->ambiguous_frames |
				(prior->state.nondata_frames ^ state->nondata_frames);
			prior->output_b1u_code_mismatch |= paths[index].output_b1u_code_mismatch;
			prior->state.b1u_code_mismatches |= state->b1u_code_mismatches;
			for (size_t frame = 0u; frame < V92_PCM_RX_DELAY_FRAMES; ++frame) {
				if (memcmp(prior->state.pending[frame], state->pending[frame],
					   bit_count) != 0)
					prior->state.ambiguous_frames |= (uint16_t)(1u << frame);
			}
		}
	}
	for (size_t index = 0u; index < valid; ++index) {
		if (best == SIZE_MAX ||
		    paths[index].state.metric < paths[best].state.metric)
			best = index;
	}
	if (best == SIZE_MAX)
		return V92_PCM_FRAME_NO_PATH;
	memcpy(decoded, paths[best].output_bits, bit_count);
	bool ambiguous = paths[best].output_ambiguous;
	bool nondata = paths[best].output_nondata;
	bool uncertain = paths[best].uncertain;
	bool code_mismatch = paths[best].output_b1u_code_mismatch;
	enum v92_pcm_frame_rm rm = rm_state(&paths[best].state);

	for (size_t index = 0u; index < valid; ++index) {
		if (measure_confidence &&
		    paths[index].state.metric - paths[best].state.metric <= resolution &&
		    memcmp(decoded, paths[index].output_bits, bit_count) != 0)
			uncertain = true;
		if (paths[index].state.metric == paths[best].state.metric) {
			code_mismatch |= paths[index].output_b1u_code_mismatch;
			ambiguous |= paths[index].output_ambiguous ||
				nondata != paths[index].output_nondata ||
				memcmp(decoded, paths[index].output_bits, bit_count) != 0;
			if (rm_state(&paths[index].state) != rm) rm = V92_PCM_FRAME_RM_NONE;
		}
	}
	struct v92_pcm_frame_decision result = {
		.frame = paths[best].frame,
		.squared_error = paths[best].frame_metric,
		.uncertain = uncertain,
		.bits_ready = receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES && !nondata,
		.ambiguous = receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES && ambiguous,
		.rm = rm,
		.b1u_mismatches = code_mismatch ? V92_B1U_CODE_MISMATCH : 0u,
	};
	if (result.bits_ready)
		for (size_t bit = 0; bit < bit_count; bit++)
			if (decoded[bit] != 1u) {
				result.b1u_mismatches |= V92_B1U_SOURCE_MISMATCH;
				break;
			}
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		result.predicted_samples[symbol] = predicted_observation(receiver,
			result.frame.u[symbol], result.frame.v[symbol], samples[symbol], b1u);
	double estimated[V92_PCM_MAPPING_FRAME_SYMBOLS];
	for (size_t symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		estimated[symbol] = result.predicted_samples[symbol];
	if (model) {
		const int32_t *previous = x_history(&receiver->mapping,
			&receiver->paths[paths[best].origin].filter);
		for (size_t symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int32_t input[V92_OBSERVATION_TAPS];
			for (unsigned i = 0; i < model->taps; i++)
				input[i] = symbol >= i ? result.frame.x[symbol - i] : previous[i - symbol - 1];
			estimated[symbol] = v92_observation_predict(model, input, offsets ? offsets[symbol] : 0.);
			if (!isfinite(estimated[symbol]))
				return V92_PCM_FRAME_NO_PATH;
		}
	}
	/* No caller-visible output or receiver state changes before every
	 * quality prediction succeeds, including the final symbol. */
	if (result.bits_ready)
		memcpy(bits, decoded, bit_count);
	if (receiver->pending_frames < V92_PCM_RX_DELAY_FRAMES)
		receiver->pending_frames++;
	if (expect_b1u) {
		receiver->b1u_scrambler = b1u_scrambler;
		receiver->b1u_frames++;
	}
	if (b1u)
		receiver->post_b1_frames = 0u;
	else if (receiver->post_b1_frames < V92_PCM_RX_DELAY_FRAMES)
		receiver->post_b1_frames++;
	uint64_t minimum_metric = paths[best].state.metric;

	for (size_t index = 0u; index < valid; ++index) {
		receiver->paths[index] = paths[index].state;
		receiver->paths[index].metric -= minimum_metric;
	}
	memset(receiver->paths + valid, 0,
		(V92_PCM_RX_PATHS - valid) * sizeof(receiver->paths[0]));
	receiver->path_count = (uint8_t)valid;
	if (b1u && receiver->observation == V92_PCM_FRAME_CONSTELLATION)
		train_noise(&receiver->noise, samples, result.predicted_samples);
	*decision = result;
	if (likelihood) {
		likelihood->cost = minimum_metric;
		memcpy(likelihood->predicted_samples, estimated, sizeof(estimated));
	}
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_decode(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count, struct v92_pcm_frame_decision *decision)
{
	return decode_frame(receiver, samples, b1u, bits, bit_count, decision,
		NULL, NULL, false);
}

enum v92_pcm_frame_result v92_pcm_frame_acquire_model(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *bits, size_t bit_count, struct v92_pcm_frame_decision *decision)
{
	return decode_frame(receiver, samples, true, bits, bit_count, decision,
		NULL, NULL, true);
}

const char *v92_pcm_frame_result_name(
	enum v92_pcm_frame_result result)
{
	switch (result) {
	case V92_PCM_FRAME_OK:
		return "ok";
	case V92_PCM_FRAME_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_FRAME_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PCM_FRAME_OUT_OF_RANGE:
		return "out of range";
	case V92_PCM_FRAME_NO_PATH:
		return "no path";
	}
	return "unknown";
}

enum v92_pcm_frame_result v92_pcm_frame_decode_observed(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame_decision *decision,
	const struct v92_pcm_frame_channel *channel,
	struct v92_pcm_frame_likelihood *likelihood)
{
	if (!channel || (!channel->model && !channel->cells) || !likelihood)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	return decode_frame(receiver, samples, b1u, bits, bit_count, decision,
		channel, likelihood, false);
}
