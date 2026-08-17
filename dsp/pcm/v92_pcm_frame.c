// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_frame.h"

#include "integer_math.h"

#include <limits.h>
#include <string.h>

enum {
	V92_PCM_TRELLIS_INTERVAL_SYMBOLS = 4,
	V92_PCM_GAIN_SHIFT = 18,
};

static bool bits_are_valid(const uint8_t *bits, size_t bit_count)
{
	if (bits == NULL)
		return false;
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (bits[bit] > 1u)
			return false;
	}
	return true;
}

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

static int positive_modulo(int value, unsigned int modulus)
{
	int reduced = value % (int)modulus;

	return reduced < 0 ? reduced + (int)modulus : reduced;
}

static uint8_t parity(int value)
{
	return (uint8_t)positive_modulo(value, 2u);
}

static size_t x_history_count(const struct v92_pcm_mapping *mapping)
{
	size_t count =
		mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK].count;
	size_t feed_forward =
		mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count;

	if (feed_forward > 1u && count < feed_forward - 1u)
		count = feed_forward - 1u;
	return count;
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

static bool equivalence_digit(
	const struct v92_pcm_mapping *mapping, size_t symbol, int eta,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t y0,
	uint8_t *digit)
{
	unsigned int modulus;
	int value;

	if (mapping == NULL || symbol >= V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    selected == NULL || digit == NULL)
		return false;
	modulus = mapping->cpd.modulus[symbol];
	if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS != 3u) {
		value = positive_modulo(eta, modulus);
	} else {
		size_t base = symbol - 3u;
		uint8_t offset = parity(
			selected[base] + selected[base + 1u] +
			selected[base + 2u] + y0);
		int adjusted = eta - offset;

		if (parity(adjusted) != 0u)
			return false;
		value = positive_modulo(adjusted / 2, modulus);
	}
	*digit = (uint8_t)value;
	return true;
}

static int32_t precoder_output(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter, int32_t u)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK];
	const int32_t *u_values = u_history(filter);
	const int32_t *x_values = x_history(mapping, filter);
	int64_t accumulator = (int64_t)u * INT64_C(32768);

	for (size_t tap = 0u; tap < feed_forward->count; ++tap)
		accumulator += (int64_t)u_values[tap] *
			       feed_forward->coefficient[tap];
	for (size_t tap = 0u; tap < feedback->count; ++tap)
		accumulator += (int64_t)x_values[tap] *
			       feedback->coefficient[tap] * 2;
	return round_shift_s64_to_s32_saturating(accumulator, 15u);
}

static int32_t prefilter_output(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter, int32_t x)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];
	const int32_t *x_values = x_history(mapping, filter);
	const int32_t *v_values = v_history(mapping, filter);
	int64_t accumulator = 0;

	if (feed_forward->count == 0u && feedback->count == 0u)
		return x;
	if (feed_forward->count != 0u)
		accumulator += (int64_t)x * feed_forward->coefficient[0];
	for (size_t tap = 1u; tap < feed_forward->count; ++tap)
		accumulator += (int64_t)x_values[tap - 1u] *
			       feed_forward->coefficient[tap];
	for (size_t tap = 0u; tap < feedback->count; ++tap)
		accumulator += (int64_t)v_values[tap] *
			       feedback->coefficient[tap] * 2;
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

	*x = precoder_output(mapping, filter, u);
	*v = prefilter_output(mapping, filter, *x);
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
	uint8_t constellation_index =
		mapping->cpd.frame_constellation[
			symbol % V92_PCM_MAPPING_PHASES];

	constellation = &mapping->cpd.constellation[constellation_index];
	for (int eta = -(int)constellation->count;
	     eta < (int)constellation->count; ++eta) {
		uint8_t digit;
		uint8_t ordinal;
		int16_t positive;
		int16_t u;
		int32_t x;
		int64_t score;

		if (!equivalence_digit(
			    mapping, symbol, eta, selected, y0, &digit) ||
		    digit != expected_digit)
			continue;
		ordinal = (uint8_t)(eta >= 0 ? eta : -eta - 1);
		if (ordinal >= constellation->count)
			continue;
		positive = constellation->positive_linear[ordinal];
		u = eta >= 0 ? positive : (int16_t)-positive;
		x = precoder_output(mapping, filter, u);
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

enum v92_pcm_frame_result v92_pcm_frame_encode(
	struct v92_pcm_frame_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame *frame)
{
	struct v92_pcm_frame_tx_state next;
	struct v92_pcm_frame encoded = { 0 };
	uint8_t scrambled[V92_PCM_MAPPING_MAX_BITS];

	if (transmitter == NULL || frame == NULL || bits == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_tx_is_valid(transmitter))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (bit_count != transmitter->mapping.data_bits_per_frame ||
	    !bits_are_valid(bits, bit_count))
		return V92_PCM_FRAME_OUT_OF_RANGE;
	next = transmitter->state;
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (v34_scramble_group(
			    &next.scrambler, bits[bit], 1u,
			    &scrambled[bit]) != V34_FRAMING_OK)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	if (v92_pcm_mapping_encode(
		    &transmitter->mapping, scrambled, bit_count,
		    &next.differential, encoded.digit,
		    &encoded.frame_sign) != V92_PCM_MAPPING_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		uint8_t y0;

		if (v34_convolutional_encoder_output(
			    &next.trellis, &y0) != V34_TRELLIS_OK ||
		    !select_point(
			    &transmitter->mapping, &next.filter, symbol,
			    encoded.digit[symbol], encoded.signed_index, y0,
			    &encoded.signed_index[symbol], &encoded.u[symbol]))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		encoded.y0[symbol] = y0;
		filter_step(
			&transmitter->mapping, &next.filter,
			encoded.u[symbol], &encoded.x[symbol],
			&encoded.v[symbol]);
		if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u &&
		    !advance_trellis(
			    &next.trellis, encoded.signed_index, symbol))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	transmitter->state = next;
	*frame = encoded;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_scale(
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	int32_t prefilter_output, int16_t *sample)
{
	int32_t scaled;

	if (mapping == NULL || sample == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (upstream_level <= 0)
		return V92_PCM_FRAME_OUT_OF_RANGE;
	scaled = round_shift_s64_to_s32_saturating(
		(int64_t)prefilter_output *
			mapping->cpd.four_times_gain_q0_16 * upstream_level,
		V92_PCM_GAIN_SHIFT);
	if (scaled > INT16_MAX)
		scaled = INT16_MAX;
	else if (scaled < INT16_MIN)
		scaled = INT16_MIN;
	*sample = (int16_t)scaled;
	return V92_PCM_FRAME_OK;
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
	}
	return "unknown";
}
